//==============================================================================
// PRD-0100: AudioExporterTests — drives the AudioExporter over a synthetic
// arrangement (via PRD-0099's OfflineRenderDriver with in-memory readers) and
// asserts every §1.4 acceptance criterion:
//
//   1. WAV round-trip at 16 / 24 / 32-bit-float (32f exact; 16/24 within the
//      format's quantisation step).
//   2. FLAC round-trip lossless at 16 / 24-bit.
//   3. Capability table contents + queryability; no-backend MP3 => Unsupported.
//   4. MP3 export (LAME present): Completed, file readable, duration ~ range.
//   5. Export range honouring: PCM output length == range length, content match.
//   6. Streaming: a multi-second export Completes with the correct length.
//   7. InvalidOptions: unsupported (format,bitDepth) / sample rate => no file.
//   8. Cancellation: Cancelled + partial file deleted.
//   9. Normalize: exported peak does not exceed the configured ceiling.
//  10. Clip-warning: integer-format export of >0 dBFS content sets clipped.
//
// REFERENCE STRATEGY: the OfflineRenderDriver is independently bit-identity
// tested (OfflineRenderDriverTests). Here we render the SAME config through the
// driver's buffer form to obtain the ground-truth float output, then compare the
// exported-and-read-back file against it. Lossless formats are asserted within
// their bit-depth quantisation tolerance; MP3 (lossy) is asserted for
// readability + approximate duration only.
//
// JUCE UnitTest, group "Audio Exporter", category "Sonik".
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../Source/Features/Daw/Export/AudioExporter.h"
#include "../Source/Features/Daw/Export/OfflineRenderDriver.h"
#include "../Source/Features/Daw/Playback/ArrangementSnapshot.h"
#include "../Source/Features/Daw/Import/ImportSourcePublisher.h"
#include "../Source/Features/AudioEngine/AudioBufferHolder.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

using namespace Daw;
using namespace Daw::Export;

namespace
{

constexpr int kBlock = OfflineRenderConfig::kDefaultBlockSize; // 512

//==============================================================================
// In-memory PCM library keyed by sourceFileId (render-rate holders => verbatim).
//==============================================================================
struct SourceLibrary
{
    double rate { 44100.0 };
    std::map<uint64_t, AudioBufferHolder::Ptr> holders;

    void add (uint64_t id, int numFrames,
              std::function<float (int)> fnL, std::function<float (int)> fnR)
    {
        juce::AudioBuffer<float> buf (2, numFrames);
        for (int i = 0; i < numFrames; ++i)
        {
            buf.setSample (0, i, fnL (i));
            buf.setSample (1, i, fnR (i));
        }
        holders[id] = new AudioBufferHolder (std::move (buf), rate, numFrames);
    }

    OfflineRenderDriver::ReaderProvider provider() const
    {
        const SourceLibrary* self = this;
        return [self] (const ClipEvent& ev) -> std::unique_ptr<juce::AudioFormatReader>
        {
            auto it = self->holders.find (ev.sourceFileId);
            if (it == self->holders.end())
                return nullptr;
            return std::make_unique<Daw::Import::BufferAudioFormatReader> (it->second);
        };
    }
};

struct TestClip
{
    uint64_t sourceFileId { 0 };
    int64_t  sourceStart  { 0 };
    int64_t  sourceEnd     { 0 };
    int64_t  timelineStart { 0 };
    float    gain          { 1.0f };
    int      lane          { 0 };

    int64_t timelineEnd() const { return timelineStart + (sourceEnd - sourceStart); }
};

ArrangementSnapshot makeSnapshot (const std::vector<TestClip>& clips, int laneCount)
{
    ArrangementSnapshot snap;
    snap.laneCount = laneCount;
    for (const auto& c : clips)
    {
        auto& ln = snap.lanes[(size_t) c.lane];
        auto& ev = ln.events[(size_t) ln.count++];
        ev.sourceFileId       = c.sourceFileId;
        ev.sourceStartSample   = c.sourceStart;
        ev.sourceEndSample     = c.sourceEnd;
        ev.timelineStartSample = c.timelineStart;
        ev.timelineEndSample   = c.timelineEnd();
        ev.gain                = c.gain;
        ev.laneIndex           = c.lane;
        ev.sourceReadHandle    = -1;
    }
    return snap;
}

// A region config with NO tail so the output length equals the range exactly.
OfflineRenderConfig makeConfig (ArrangementSnapshot snap, double rate,
                                int64_t start, int64_t end)
{
    OfflineRenderConfig cfg;
    cfg.snapshot         = std::move (snap);
    cfg.rangeMode        = OfflineRenderConfig::RangeMode::Region;
    cfg.rangeStartSample = start;
    cfg.rangeEndSample   = end;
    cfg.renderSampleRate = rate;
    cfg.blockSize        = kBlock;
    cfg.tailPolicy       = OfflineRenderConfig::TailPolicy::None;
    return cfg;
}

// Read a file back into a float AudioBuffer; reports rate / bit depth / length.
struct ReadBack
{
    bool ok { false };
    juce::AudioBuffer<float> buffer;
    double sampleRate { 0.0 };
    int bitsPerSample { 0 };
    int numChannels { 0 };
    int64_t lengthSamples { 0 };
};

ReadBack readFile (const juce::File& file)
{
    ReadBack rb;
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr)
        return rb;

    rb.ok            = true;
    rb.sampleRate    = reader->sampleRate;
    rb.bitsPerSample = (int) reader->bitsPerSample;
    rb.numChannels   = (int) reader->numChannels;
    rb.lengthSamples = reader->lengthInSamples;
    rb.buffer.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&rb.buffer, 0, (int) reader->lengthInSamples, 0, true, true);
    return rb;
}

float peakOf (const juce::AudioBuffer<float>& buf)
{
    float p = 0.0f;
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        p = juce::jmax (p, buf.getMagnitude (ch, 0, buf.getNumSamples()));
    return p;
}

// Ground-truth float render of a config via the driver's buffer form.
juce::AudioBuffer<float> renderReference (OfflineRenderConfig cfg,
                                          const SourceLibrary& lib)
{
    OfflineRenderDriver drv (std::move (cfg), lib.provider());
    juce::AudioBuffer<float> out;
    drv.render (out);
    return out;
}

} // namespace

//==============================================================================
class AudioExporterTests final : public juce::UnitTest
{
public:
    AudioExporterTests() : juce::UnitTest ("Audio Exporter", "Sonik") {}

    void runTest() override
    {
        testCapabilityTable();
        testNoBackendMp3Unsupported();
        testWavRoundTrip();
        testFlacRoundTrip();
        testMp3Export();
        testRangeHonouring();
        testStreamingLargeExport();
        testInvalidOptions();
        testCancellationDeletesPartial();
        testNormalizePeakCeiling();
        testClipWarningFlag();
    }

private:
    //--------------------------------------------------------------------------
    // Shared fixture: a single full-volume clip with smooth low-amplitude content
    // (a slow sine at 0.5 peak), block-aligned, on one lane.
    //--------------------------------------------------------------------------
    static constexpr uint64_t kSourceId = 0xA11CE5ull;

    SourceLibrary makeLib (double rate, int frames, float amp = 0.5f) const
    {
        SourceLibrary lib;
        lib.rate = rate;
        lib.add (kSourceId, frames,
                 [amp, rate] (int i) { return amp * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * (double) i / rate); },
                 [amp, rate] (int i) { return amp * std::sin (2.0 * juce::MathConstants<double>::pi * 330.0 * (double) i / rate); });
        return lib;
    }

    juce::File tempFile (const juce::String& ext) const
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("sonik_export_test_" + juce::String (juce::Random::getSystemRandom().nextInt64()))
                   .withFileExtension (ext);
    }

    void compareWithin (const juce::AudioBuffer<float>& ref,
                        const juce::AudioBuffer<float>& got,
                        float tol, const juce::String& label)
    {
        expect (got.getNumSamples() == ref.getNumSamples(),
                label + ": length " + juce::String (got.getNumSamples())
                + " != ref " + juce::String (ref.getNumSamples()));
        const int n = juce::jmin (got.getNumSamples(), ref.getNumSamples());
        const int chans = juce::jmin (got.getNumChannels(), ref.getNumChannels());
        float maxErr = 0.0f;
        for (int ch = 0; ch < chans; ++ch)
            for (int i = 0; i < n; ++i)
                maxErr = juce::jmax (maxErr, std::abs (got.getSample (ch, i) - ref.getSample (ch, i)));
        expect (maxErr <= tol, label + ": maxErr " + juce::String (maxErr) + " > tol " + juce::String (tol));
    }

    //--------------------------------------------------------------------------
    void testCapabilityTable()
    {
        beginTest ("capability table is the single source of truth");

        const auto& rates = AudioExporter::supportedSampleRates();
        expect (rates.size() == 3);
        expect (std::find (rates.begin(), rates.end(), 44100.0) != rates.end());
        expect (std::find (rates.begin(), rates.end(), 48000.0) != rates.end());
        expect (std::find (rates.begin(), rates.end(), 96000.0) != rates.end());

        AudioExporter exp; // default backend factory
        auto wav = exp.capabilitiesFor (Format::Wav);
        expect (wav.supported);
        expect (wav.allowsBitDepth (16) && wav.allowsBitDepth (24) && wav.allowsBitDepth (32));

        auto flac = exp.capabilitiesFor (Format::Flac);
        expect (flac.supported);
        expect (flac.allowsBitDepth (16) && flac.allowsBitDepth (24));
        expect (! flac.allowsBitDepth (32));

        auto mp3 = exp.capabilitiesFor (Format::Mp3);
        // This build links LAME, so MP3 must report supported with the bitrate set.
        expect (mp3.supported, "MP3 should be supported on the LAME build");
        expect (mp3.allowsBitrate (128) && mp3.allowsBitrate (192)
                && mp3.allowsBitrate (256) && mp3.allowsBitrate (320));
        expect (mp3.supportsVbr);
    }

    void testNoBackendMp3Unsupported()
    {
        beginTest ("no MP3 backend => Mp3 unsupported, no file created");

        // Inject a factory that yields no backend.
        AudioExporter exp ([] () -> std::unique_ptr<Mp3EncoderBackend> { return nullptr; });
        expect (! exp.capabilitiesFor (Format::Mp3).supported);

        const int frames = 4 * kBlock;
        auto lib = makeLib (44100.0, frames);
        auto cfg = makeConfig (makeSnapshot ({ { kSourceId, 0, frames, 0, 1.0f, 0 } }, 1),
                               44100.0, 0, frames);
        OfflineRenderDriver drv (cfg, lib.provider());

        ExportOptions opt;
        opt.format     = Format::Mp3;
        opt.sampleRate = 44100.0;
        opt.range      = { 0, frames };
        opt.outputFile = tempFile (".mp3");

        std::atomic<bool> cancel { false };
        auto r = exp.exportArrangement (opt, drv, nullptr, cancel);
        expect (r.status == ExportResult::Status::Unsupported);
        expect (! opt.outputFile.existsAsFile(), "no file should be created for Unsupported");
        opt.outputFile.deleteFile();
    }

    void testWavRoundTrip()
    {
        beginTest ("WAV round-trip at 16/24/32-bit-float");

        const double rate = 44100.0;
        const int frames = 8 * kBlock;
        auto lib = makeLib (rate, frames);
        auto cfg = makeConfig (makeSnapshot ({ { kSourceId, 0, frames, 0, 1.0f, 0 } }, 1),
                               rate, 0, frames);
        auto ref = renderReference (cfg, lib);

        struct Case { int bits; float tol; };
        // 32f: exact. 24-bit: ~1 LSB (2^-23 ~ 1.2e-7) plus rounding => 1e-6.
        // 16-bit: ~1 LSB (2^-15 ~ 3e-5) plus rounding/dither => 1.5e-4.
        for (auto c : { Case { 32, 0.0f }, Case { 24, 1.0e-6f }, Case { 16, 1.5e-4f } })
        {
            AudioExporter exp;
            OfflineRenderDriver drv (cfg, lib.provider());

            ExportOptions opt;
            opt.format     = Format::Wav;
            opt.sampleRate = rate;
            opt.bitDepth   = c.bits;
            opt.range      = { 0, frames };
            opt.outputFile = tempFile (".wav");

            std::atomic<bool> cancel { false };
            auto r = exp.exportArrangement (opt, drv, nullptr, cancel);
            expect (r.status == ExportResult::Status::Completed,
                    "WAV " + juce::String (c.bits) + "-bit export should complete");

            auto rb = readFile (opt.outputFile);
            expect (rb.ok, "WAV file should be readable");
            expect (juce::approximatelyEqual (rb.sampleRate, rate));
            expect (rb.numChannels == 2);
            compareWithin (ref, rb.buffer, c.tol, "WAV " + juce::String (c.bits) + "-bit");
            opt.outputFile.deleteFile();
        }
    }

    void testFlacRoundTrip()
    {
        beginTest ("FLAC round-trip lossless at 16/24-bit");

        const double rate = 44100.0;
        const int frames = 8 * kBlock;
        auto lib = makeLib (rate, frames);
        auto cfg = makeConfig (makeSnapshot ({ { kSourceId, 0, frames, 0, 1.0f, 0 } }, 1),
                               rate, 0, frames);
        auto ref = renderReference (cfg, lib);

        // FLAC is lossless at the integer resolution: readback == quantised ref.
        // 24-bit: within 1 LSB (2^-23). 16-bit: within 1 LSB (2^-15).
        struct Case { int bits; float tol; };
        for (auto c : { Case { 24, 2.0e-7f }, Case { 16, 4.0e-5f } })
        {
            AudioExporter exp;
            OfflineRenderDriver drv (cfg, lib.provider());

            ExportOptions opt;
            opt.format     = Format::Flac;
            opt.sampleRate = rate;
            opt.bitDepth   = c.bits;
            opt.range      = { 0, frames };
            opt.outputFile = tempFile (".flac");

            std::atomic<bool> cancel { false };
            auto r = exp.exportArrangement (opt, drv, nullptr, cancel);
            expect (r.status == ExportResult::Status::Completed,
                    "FLAC " + juce::String (c.bits) + "-bit export should complete");

            auto rb = readFile (opt.outputFile);
            expect (rb.ok, "FLAC file should be readable");
            expect (rb.lengthSamples == frames, "FLAC length should equal range");
            compareWithin (ref, rb.buffer, c.tol, "FLAC " + juce::String (c.bits) + "-bit");
            opt.outputFile.deleteFile();
        }
    }

    void testMp3Export()
    {
        beginTest ("MP3 export (LAME) completes, readable, duration ~ range");

        const double rate = 44100.0;
        const int frames = 16 * kBlock;
        auto lib = makeLib (rate, frames);
        auto cfg = makeConfig (makeSnapshot ({ { kSourceId, 0, frames, 0, 1.0f, 0 } }, 1),
                               rate, 0, frames);
        AudioExporter exp;
        OfflineRenderDriver drv (cfg, lib.provider());

        ExportOptions opt;
        opt.format         = Format::Mp3;
        opt.sampleRate     = rate;
        opt.mp3BitrateKbps = 320;
        opt.mp3Vbr         = false;
        opt.range          = { 0, frames };
        opt.outputFile     = tempFile (".mp3");

        std::atomic<bool> cancel { false };
        auto r = exp.exportArrangement (opt, drv, nullptr, cancel);
        expect (r.status == ExportResult::Status::Completed, "MP3 export should complete: " + r.message);
        expect (opt.outputFile.existsAsFile() && opt.outputFile.getSize() > 0, "MP3 file should be non-empty");

        auto rb = readFile (opt.outputFile);
        expect (rb.ok, "MP3 file should be readable by the decoder");
        if (rb.ok)
        {
            expect (juce::approximatelyEqual (rb.sampleRate, rate));
            // MP3 is lossy with encoder/decoder delay+padding: duration is close
            // to the range but not sample-exact. Allow up to ~3 frames (1152*3)
            // of delay/padding on either side; must be clearly non-empty.
            const int64_t tol = 1152 * 3;
            expect (rb.lengthSamples > frames - tol && rb.lengthSamples < frames + tol,
                    "MP3 decoded length " + juce::String (rb.lengthSamples)
                    + " not within tolerance of " + juce::String (frames));
        }
        opt.outputFile.deleteFile();
    }

    void testRangeHonouring()
    {
        beginTest ("export range is honoured (PCM length == range length)");

        const double rate = 44100.0;
        const int frames = 12 * kBlock;
        auto lib = makeLib (rate, frames);
        const int64_t start = 3 * kBlock;
        const int64_t end   = 9 * kBlock;
        auto cfg = makeConfig (makeSnapshot ({ { kSourceId, 0, frames, 0, 1.0f, 0 } }, 1),
                               rate, start, end);
        auto ref = renderReference (cfg, lib); // the [start,end) slice

        AudioExporter exp;
        OfflineRenderDriver drv (cfg, lib.provider());

        ExportOptions opt;
        opt.format     = Format::Wav;
        opt.sampleRate = rate;
        opt.bitDepth   = 32;
        opt.range      = { start, end };
        opt.outputFile = tempFile (".wav");

        std::atomic<bool> cancel { false };
        auto r = exp.exportArrangement (opt, drv, nullptr, cancel);
        expect (r.status == ExportResult::Status::Completed);

        auto rb = readFile (opt.outputFile);
        expect (rb.ok);
        expect (rb.lengthSamples == (end - start), "output length must equal range length");
        compareWithin (ref, rb.buffer, 0.0f, "range slice (32f exact)");
        opt.outputFile.deleteFile();
    }

    void testStreamingLargeExport()
    {
        beginTest ("a multi-second export completes with the correct length");

        const double rate = 44100.0;
        const int frames = (int) (rate * 3.0); // 3 seconds
        auto lib = makeLib (rate, frames);
        auto cfg = makeConfig (makeSnapshot ({ { kSourceId, 0, frames, 0, 1.0f, 0 } }, 1),
                               rate, 0, frames);
        AudioExporter exp;
        OfflineRenderDriver drv (cfg, lib.provider());

        ExportOptions opt;
        opt.format     = Format::Wav;
        opt.sampleRate = rate;
        opt.bitDepth   = 24;
        opt.range      = { 0, frames };
        opt.outputFile = tempFile (".wav");

        std::atomic<bool> cancel { false };
        float lastProgress = -1.0f;
        auto r = exp.exportArrangement (opt, drv, [&] (float f) { lastProgress = f; }, cancel);
        expect (r.status == ExportResult::Status::Completed);

        auto rb = readFile (opt.outputFile);
        expect (rb.ok);
        expect (rb.lengthSamples == frames, "large export length must equal range");
        expect (lastProgress >= 0.999f, "progress should reach ~1.0");
        opt.outputFile.deleteFile();
    }

    void testInvalidOptions()
    {
        beginTest ("invalid options return InvalidOptions and create no file");

        const int frames = 4 * kBlock;
        auto lib = makeLib (44100.0, frames);
        auto snap = makeSnapshot ({ { kSourceId, 0, frames, 0, 1.0f, 0 } }, 1);

        AudioExporter exp;

        // (a) FLAC + 32-bit-float is unsupported.
        {
            auto cfg = makeConfig (snap, 44100.0, 0, frames);
            OfflineRenderDriver drv (cfg, lib.provider());
            ExportOptions opt;
            opt.format     = Format::Flac;
            opt.sampleRate = 44100.0;
            opt.bitDepth   = 32;
            opt.range      = { 0, frames };
            opt.outputFile = tempFile (".flac");
            std::atomic<bool> cancel { false };
            auto r = exp.exportArrangement (opt, drv, nullptr, cancel);
            expect (r.status == ExportResult::Status::InvalidOptions, "FLAC+32f must be InvalidOptions");
            expect (! opt.outputFile.existsAsFile(), "no file for InvalidOptions (FLAC+32f)");
            opt.outputFile.deleteFile();
        }

        // (b) An unsupported sample rate.
        {
            auto cfg = makeConfig (snap, 44100.0, 0, frames);
            OfflineRenderDriver drv (cfg, lib.provider());
            ExportOptions opt;
            opt.format     = Format::Wav;
            opt.sampleRate = 22050.0; // not in {44100,48000,96000}
            opt.bitDepth   = 16;
            opt.range      = { 0, frames };
            opt.outputFile = tempFile (".wav");
            std::atomic<bool> cancel { false };
            auto r = exp.exportArrangement (opt, drv, nullptr, cancel);
            expect (r.status == ExportResult::Status::InvalidOptions, "unsupported rate must be InvalidOptions");
            expect (! opt.outputFile.existsAsFile(), "no file for InvalidOptions (rate)");
            opt.outputFile.deleteFile();
        }
    }

    void testCancellationDeletesPartial()
    {
        beginTest ("cancellation aborts and deletes the partial file");

        const double rate = 44100.0;
        const int frames = (int) (rate * 2.0);
        auto lib = makeLib (rate, frames);
        auto cfg = makeConfig (makeSnapshot ({ { kSourceId, 0, frames, 0, 1.0f, 0 } }, 1),
                               rate, 0, frames);
        AudioExporter exp;
        OfflineRenderDriver drv (cfg, lib.provider());

        ExportOptions opt;
        opt.format     = Format::Wav;
        opt.sampleRate = rate;
        opt.bitDepth   = 24;
        opt.range      = { 0, frames };
        opt.outputFile = tempFile (".wav");

        std::atomic<bool> cancel { true }; // cancel from the very first block
        auto r = exp.exportArrangement (opt, drv, nullptr, cancel);
        expect (r.status == ExportResult::Status::Cancelled, "should report Cancelled");
        expect (! opt.outputFile.existsAsFile(), "partial file must be deleted on cancel");
        opt.outputFile.deleteFile();
    }

    void testNormalizePeakCeiling()
    {
        beginTest ("normalize limits the exported peak to the ceiling");

        const double rate = 44100.0;
        const int frames = 8 * kBlock;
        auto lib = makeLib (rate, frames, 0.9f); // ~0.9 peak content
        auto cfg = makeConfig (makeSnapshot ({ { kSourceId, 0, frames, 0, 1.0f, 0 } }, 1),
                               rate, 0, frames);
        AudioExporter exp;
        OfflineRenderDriver drv (cfg, lib.provider());

        const float ceilingDb = -6.0f;
        const float ceilingLin = std::pow (10.0f, ceilingDb / 20.0f); // ~0.501

        ExportOptions opt;
        opt.format        = Format::Wav;
        opt.sampleRate    = rate;
        opt.bitDepth      = 32; // float => no quantisation in the peak check
        opt.range         = { 0, frames };
        opt.normalize     = true;
        opt.peakCeilingDb = ceilingDb;
        opt.outputFile    = tempFile (".wav");

        std::atomic<bool> cancel { false };
        auto r = exp.exportArrangement (opt, drv, nullptr, cancel);
        expect (r.status == ExportResult::Status::Completed);

        auto rb = readFile (opt.outputFile);
        expect (rb.ok);
        const float pk = peakOf (rb.buffer);
        expect (pk <= ceilingLin + 1.0e-3f, "peak " + juce::String (pk) + " exceeds ceiling " + juce::String (ceilingLin));
        // It should be brought UP to ~the ceiling (the source peak was below 0 dBFS scaling target).
        expect (pk > ceilingLin * 0.85f, "normalized peak " + juce::String (pk) + " unexpectedly low");
        opt.outputFile.deleteFile();
    }

    void testClipWarningFlag()
    {
        beginTest ("integer-format export flags clipping (and only when it clips)");

        const double rate = 44100.0;
        const int frames = 8 * kBlock;

        // Two overlapping full-volume clips on two lanes summing to ~1.6 => clips.
        {
            SourceLibrary lib; lib.rate = rate;
            lib.add (1ull, frames, [] (int) { return 0.8f; }, [] (int) { return 0.8f; });
            lib.add (2ull, frames, [] (int) { return 0.8f; }, [] (int) { return 0.8f; });
            auto snap = makeSnapshot ({ { 1ull, 0, frames, 0, 1.0f, 0 },
                                        { 2ull, 0, frames, 0, 1.0f, 1 } }, 2);
            auto cfg = makeConfig (snap, rate, 0, frames);
            AudioExporter exp;
            OfflineRenderDriver drv (cfg, lib.provider());

            ExportOptions opt;
            opt.format     = Format::Wav;
            opt.sampleRate = rate;
            opt.bitDepth   = 16; // integer => clip detectable
            opt.normalize  = false;
            opt.range      = { 0, frames };
            opt.outputFile = tempFile (".wav");
            std::atomic<bool> cancel { false };
            auto r = exp.exportArrangement (opt, drv, nullptr, cancel);
            expect (r.status == ExportResult::Status::Completed);
            expect (r.clipped, "summed content > 0 dBFS into 16-bit should set clipped");
            opt.outputFile.deleteFile();
        }

        // A quiet, non-clipping export should NOT flag clipping.
        {
            auto lib = makeLib (rate, frames, 0.3f);
            auto cfg = makeConfig (makeSnapshot ({ { kSourceId, 0, frames, 0, 1.0f, 0 } }, 1),
                                   rate, 0, frames);
            AudioExporter exp;
            OfflineRenderDriver drv (cfg, lib.provider());
            ExportOptions opt;
            opt.format     = Format::Wav;
            opt.sampleRate = rate;
            opt.bitDepth   = 16;
            opt.normalize  = false;
            opt.range      = { 0, frames };
            opt.outputFile = tempFile (".wav");
            std::atomic<bool> cancel { false };
            auto r = exp.exportArrangement (opt, drv, nullptr, cancel);
            expect (r.status == ExportResult::Status::Completed);
            expect (! r.clipped, "quiet content should not flag clipping");
            opt.outputFile.deleteFile();
        }
    }
};

static AudioExporterTests audioExporterTests;
