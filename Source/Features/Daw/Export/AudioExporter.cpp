//==============================================================================
// PRD-0100: AudioExporter implementation. See AudioExporter.h for the contract.
//
// The exporter is a PURE CONSUMER of PRD-0099's render blocks: it constructs the
// driver-agnostic writer for the chosen format, then drives the driver's
// streaming render() overload, writing each yielded block immediately. Peak
// memory is one render block plus the writer's encode buffer — independent of
// arrangement length (PRD-0100 §1.5.3).
//==============================================================================

#include "AudioExporter.h"

#include <cmath>

namespace Daw::Export
{

//==============================================================================
// Capability table — THE single source of truth (PRD-0100 §1.5.2).
//==============================================================================

const std::vector<double>& AudioExporter::supportedSampleRates()
{
    static const std::vector<double> rates { 44100.0, 48000.0, 96000.0 };
    return rates;
}

FormatCapabilities AudioExporter::staticCapabilitiesFor (Format format)
{
    FormatCapabilities c;
    c.format = format;

    switch (format)
    {
        case Format::Wav:
            c.supported   = true;
            c.bitDepths   = { 16, 24, 32 }; // 32 == 32-bit float
            break;

        case Format::Flac:
            c.supported   = true;
            c.bitDepths   = { 16, 24 };     // integer only, no float
            break;

        case Format::Mp3:
            c.supported   = false;          // resolved per-instance (backend probe)
            c.bitDepths   = {};             // n/a (perceptual codec)
            c.mp3Bitrates = { 128, 192, 256, 320 };
            c.supportsVbr = true;
            break;
    }

    return c;
}

FormatCapabilities AudioExporter::capabilitiesFor (Format format) const
{
    FormatCapabilities c = staticCapabilitiesFor (format);

    // MP3 support depends on whether THIS exporter's factory yields a backend
    // (a no-LAME build returns nullptr; a fake-injecting test returns one).
    if (format == Format::Mp3)
    {
        auto backend = mp3Factory_ ? mp3Factory_() : nullptr;
        c.supported = (backend != nullptr);
    }

    return c;
}

//==============================================================================
// Construction
//==============================================================================

AudioExporter::AudioExporter()
    : mp3Factory_ ([] { return makeMp3EncoderBackend(); })
{
}

AudioExporter::AudioExporter (Mp3BackendFactory mp3Factory)
    : mp3Factory_ (std::move (mp3Factory))
{
    if (! mp3Factory_)
        mp3Factory_ = [] { return makeMp3EncoderBackend(); };
}

//==============================================================================
// Validation
//==============================================================================

namespace
{
    bool sampleRateSupported (double sr)
    {
        for (double v : AudioExporter::supportedSampleRates())
            if (std::abs (v - sr) < 0.5)
                return true;
        return false;
    }
}

std::optional<ExportResult> AudioExporter::validate (const ExportOptions& options) const
{
    // Stereo only (PRD-0100 §1.5.5).
    if (options.layout != ChannelLayout::Stereo)
        return ExportResult::invalid ("Only stereo export is supported.");

    if (! sampleRateSupported (options.sampleRate))
        return ExportResult::invalid ("Unsupported sample rate: "
                                      + juce::String (options.sampleRate));

    if (options.outputFile.getFullPathName().isEmpty())
        return ExportResult::invalid ("No output file specified.");

    const FormatCapabilities caps = capabilitiesFor (options.format);

    switch (options.format)
    {
        case Format::Wav:
        case Format::Flac:
            if (! caps.allowsBitDepth (options.bitDepth))
                return ExportResult::invalid ("Unsupported bit depth "
                                              + juce::String (options.bitDepth)
                                              + " for the chosen format.");
            break;

        case Format::Mp3:
            if (! caps.supported)
                return ExportResult::unsupported (
                    "MP3 export is not available: no MP3 encoder backend is "
                    "compiled into this build.");
            if (! caps.allowsBitrate (options.mp3BitrateKbps))
                return ExportResult::invalid ("Unsupported MP3 bitrate "
                                              + juce::String (options.mp3BitrateKbps)
                                              + " kbps.");
            break;
    }

    return std::nullopt;
}

bool AudioExporter::isSupported (const ExportOptions& options) const
{
    return ! validate (options).has_value();
}

//==============================================================================
// Public entry point
//==============================================================================

ExportResult AudioExporter::exportArrangement (const ExportOptions&        options,
                                               OfflineRenderDriver&        driver,
                                               std::function<void (float)> progress,
                                               std::atomic<bool>&          cancelRequested)
{
    // 1. Validate BEFORE any file is created (PRD-0100 §1.5.2 / §1.5.5).
    if (auto invalid = validate (options))
        return *invalid;

    // 2. Route to the format path.
    if (options.format == Format::Mp3)
        return exportMp3 (options, driver, std::move (progress), cancelRequested);

    return exportPcm (options, driver, std::move (progress), cancelRequested);
}

//==============================================================================
// Shared helpers
//==============================================================================

namespace
{
    // Metadata map for juce::AudioFormat::createWriterFor. WAV uses the INFO
    // chunk keys; FLAC uses Vorbis-comment keys (PRD-0100 §1.5.8).
    juce::StringPairArray buildMetadata (Format format, const ExportMetadata& md)
    {
        juce::StringPairArray m;

        if (format == Format::Wav)
        {
            if (md.title.isNotEmpty())   m.set (juce::WavAudioFormat::riffInfoTitle,   md.title);
            if (md.artist.isNotEmpty())  m.set (juce::WavAudioFormat::riffInfoArtist,  md.artist);
            if (md.comment.isNotEmpty()) m.set (juce::WavAudioFormat::riffInfoComment, md.comment);
        }
        else // FLAC -> Vorbis comments
        {
            if (md.title.isNotEmpty())   m.set ("TITLE",   md.title);
            if (md.artist.isNotEmpty())  m.set ("ARTIST",  md.artist);
            if (md.comment.isNotEmpty()) m.set ("COMMENT", md.comment);
        }

        return m;
    }

    // Compute the linear gain that maps a measured peak to the ceiling. Only
    // attenuates (or boosts toward the ceiling); a zero/silent render yields 1.0.
    float normaliseGain (float measuredPeak, float ceilingDb)
    {
        if (measuredPeak <= 1.0e-9f)
            return 1.0f;
        const float ceilingLinear = juce::Decibels::decibelsToGain (ceilingDb);
        return ceilingLinear / measuredPeak;
    }

    // First pass: render the range through the driver to measure the true peak.
    // Used only when normalize == true (PRD-0100 §1.5.4). Honours cancellation.
    float measurePeak (OfflineRenderDriver& driver, std::atomic<bool>& cancel)
    {
        std::atomic<float> peak { 0.0f };
        OfflineRenderDriver::BlockSink sink =
            [&peak] (const juce::AudioBuffer<float>& block, int numSamples, int64_t)
            {
                float p = 0.0f;
                for (int ch = 0; ch < block.getNumChannels(); ++ch)
                    p = juce::jmax (p, block.getMagnitude (ch, 0, numSamples));
                float cur = peak.load (std::memory_order_relaxed);
                while (p > cur && ! peak.compare_exchange_weak (cur, p)) {}
            };
        driver.render (sink, {}, &cancel);
        return peak.load (std::memory_order_relaxed);
    }
}

//==============================================================================
// PCM path (WAV / FLAC) via juce::AudioFormatWriter
//==============================================================================

ExportResult AudioExporter::exportPcm (const ExportOptions&        options,
                                       OfflineRenderDriver&        driver,
                                       std::function<void (float)> progress,
                                       std::atomic<bool>&          cancelRequested)
{
    const bool isFloat = (options.format == Format::Wav && options.bitDepth == 32);
    const bool isInteger = ! isFloat;

    // ── Optional normalise: first analysis pass to find the true peak.
    float normGain = 1.0f;
    if (options.normalize)
    {
        const float peak = measurePeak (driver, cancelRequested);
        if (cancelRequested.load (std::memory_order_relaxed))
        {
            ExportResult r; r.status = ExportResult::Status::Cancelled;
            r.message = "Cancelled during normalisation analysis."; return r;
        }
        normGain = normaliseGain (peak, options.peakCeilingDb);
    }

    // ── Build the format + writer.
    std::unique_ptr<juce::AudioFormat> format;
    if (options.format == Format::Wav)
        format = std::make_unique<juce::WavAudioFormat>();
    else
        format = std::make_unique<juce::FlacAudioFormat>();

    options.outputFile.deleteFile();
    auto outStream = options.outputFile.createOutputStream();
    if (outStream == nullptr)
        return ExportResult::failed ("Could not open output file for writing: "
                                     + options.outputFile.getFullPathName());

    juce::StringPairArray metadata;
    if (options.metadata.has_value() && ! options.metadata->isEmpty())
        metadata = buildMetadata (options.format, *options.metadata);

    // For 32-bit-float WAV we ask JUCE for a float writer (no bit-depth
    // reduction => no dither, fully bit-faithful). For integer depths JUCE's
    // writer applies the format's STANDARD DITHER on conversion — the exporter
    // never pre-quantises the float buffer (PRD-0100 §1.5.4).
    const int writerBitDepth = options.bitDepth; // 16 / 24 / 32
    std::unique_ptr<juce::AudioFormatWriter> writer (
        format->createWriterFor (outStream.get(),
                                 options.sampleRate,
                                 2,                 // stereo
                                 writerBitDepth,
                                 metadata,
                                 0));               // default quality

    if (writer == nullptr)
        return ExportResult::failed ("Could not create writer for the chosen "
                                     "format / sample rate / bit depth.");

    // createWriterFor takes ownership of the stream on success.
    outStream.release();

    // ── Streaming render -> write loop. Each block is gained (if normalising)
    //    into a reusable scratch buffer then written immediately.
    ExportResult result;
    result.status = ExportResult::Status::Completed;

    bool       writeError = false;
    bool       clipped    = false;
    juce::AudioBuffer<float> scratch (2, 0); // resized lazily to block size

    OfflineRenderDriver::BlockSink sink =
        [&] (const juce::AudioBuffer<float>& block, int numSamples, int64_t)
        {
            if (writeError)
                return;

            const float* const* writeData = block.getArrayOfReadPointers();

            // Apply normalise gain and/or detect integer clipping into scratch.
            const bool needScratch = (normGain != 1.0f) || isInteger;
            if (needScratch)
            {
                if (scratch.getNumSamples() < numSamples)
                    scratch.setSize (2, numSamples, false, false, true);

                for (int ch = 0; ch < 2; ++ch)
                {
                    const float* src = block.getReadPointer (ch);
                    float*       dst = scratch.getWritePointer (ch);
                    for (int i = 0; i < numSamples; ++i)
                    {
                        float v = src[i] * normGain;
                        if (isInteger && (v > 1.0f || v < -1.0f))
                            clipped = true;
                        dst[i] = v;
                    }
                }
                writeData = scratch.getArrayOfReadPointers();
            }

            if (! writer->writeFromFloatArrays (writeData, 2, numSamples))
                writeError = true;
        };

    std::function<void (float)> progressCb;
    if (progress)
        progressCb = [progress] (float f) { progress (f); };

    RenderResult rr = driver.render (sink, progressCb, &cancelRequested);

    // ── Finalise: flush + close the writer (release the stream) BEFORE we judge
    //    success, so a partial/failed file is fully closed before deletion.
    writer.reset();

    if (rr.status == RenderResult::Status::Cancelled)
    {
        options.outputFile.deleteFile(); // never leave a valid-looking partial
        result.status = ExportResult::Status::Cancelled;
        result.message = "Export cancelled by user.";
        result.samplesWritten = 0;
        return result;
    }

    if (writeError)
    {
        options.outputFile.deleteFile();
        return ExportResult::failed ("Write error during export (disk full?).");
    }

    result.clipped        = clipped;
    result.samplesWritten = rr.samplesRendered;
    return result;
}

//==============================================================================
// MP3 path via the pluggable backend
//==============================================================================

ExportResult AudioExporter::exportMp3 (const ExportOptions&        options,
                                       OfflineRenderDriver&        driver,
                                       std::function<void (float)> progress,
                                       std::atomic<bool>&          cancelRequested)
{
    auto backend = mp3Factory_ ? mp3Factory_() : nullptr;
    if (backend == nullptr)
        return ExportResult::unsupported (
            "MP3 export is not available: no MP3 encoder backend is compiled "
            "into this build.");

    // ── Optional normalise analysis pass.
    float normGain = 1.0f;
    if (options.normalize)
    {
        const float peak = measurePeak (driver, cancelRequested);
        if (cancelRequested.load (std::memory_order_relaxed))
        {
            ExportResult r; r.status = ExportResult::Status::Cancelled;
            r.message = "Cancelled during normalisation analysis."; return r;
        }
        normGain = normaliseGain (peak, options.peakCeilingDb);
    }

    Mp3EncoderConfig cfg;
    cfg.sampleRate  = options.sampleRate;
    cfg.numChannels = 2;
    cfg.bitrateKbps = options.mp3BitrateKbps;
    cfg.vbr         = options.mp3Vbr;
    cfg.metadata    = options.metadata;

    if (! backend->initialise (options.outputFile, cfg))
    {
        options.outputFile.deleteFile();
        return ExportResult::failed ("MP3 encoder init failed: " + backend->getLastError());
    }

    bool encodeError = false;
    juce::AudioBuffer<float> scratch (2, 0);

    OfflineRenderDriver::BlockSink sink =
        [&] (const juce::AudioBuffer<float>& block, int numSamples, int64_t)
        {
            if (encodeError)
                return;

            const float* const* encData = block.getArrayOfReadPointers();

            if (normGain != 1.0f)
            {
                if (scratch.getNumSamples() < numSamples)
                    scratch.setSize (2, numSamples, false, false, true);
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float* src = block.getReadPointer (ch);
                    float*       dst = scratch.getWritePointer (ch);
                    for (int i = 0; i < numSamples; ++i)
                        dst[i] = src[i] * normGain;
                }
                encData = scratch.getArrayOfReadPointers();
            }

            if (! backend->encodeBlock (encData, numSamples))
                encodeError = true;
        };

    std::function<void (float)> progressCb;
    if (progress)
        progressCb = [progress] (float f) { progress (f); };

    RenderResult rr = driver.render (sink, progressCb, &cancelRequested);

    if (rr.status == RenderResult::Status::Cancelled)
    {
        backend->finish();               // flush/close so the file handle frees
        options.outputFile.deleteFile(); // then remove the partial file
        ExportResult r; r.status = ExportResult::Status::Cancelled;
        r.message = "Export cancelled by user.";
        return r;
    }

    if (encodeError)
    {
        const juce::String err = backend->getLastError();
        backend->finish();
        options.outputFile.deleteFile();
        return ExportResult::failed ("MP3 encode error: " + err);
    }

    if (! backend->finish())
    {
        const juce::String err = backend->getLastError();
        options.outputFile.deleteFile();
        return ExportResult::failed ("MP3 finalise error: " + err);
    }

    ExportResult result;
    result.status         = ExportResult::Status::Completed;
    result.clipped        = false;       // MP3 has no integer-clip notion
    result.samplesWritten = rr.samplesRendered;
    return result;
}

} // namespace Daw::Export
