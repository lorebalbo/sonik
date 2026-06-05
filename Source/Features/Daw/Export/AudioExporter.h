#pragma once
//==============================================================================
// PRD-0100: AudioExporter — encodes the summed arrangement (driven by PRD-0099's
// OfflineRenderDriver) to a single audio file (WAV / FLAC / MP3) with the
// caller-chosen format / rate / bit depth / range / normalisation.
//
// WHAT THIS OWNS:
//   - the per-format CAPABILITY TABLE (single source of truth, queryable so
//     PRD-0101's dialog reads the same table — PRD-0100 §1.5.2),
//   - up-front OPTIONS VALIDATION (returns InvalidOptions before any file),
//   - WRITER construction per format (WavAudioFormat / FlacAudioFormat / the
//     pluggable Mp3EncoderBackend),
//   - the STREAMING render->write loop: it drives the injected
//     OfflineRenderDriver with options.range and writes each yielded block one
//     buffer at a time (never the whole render in RAM — PRD-0100 §1.5.3),
//   - progress forwarding, cancel/error handling (close + DELETE partial file),
//     the optional normalise peak-limit pass, dither delegated to the JUCE
//     writer, metadata pass-through, and the integer-format clip-warning flag.
//
// WHAT THIS DOES NOT OWN: the render loop / seek / playhead (PRD-0099), the
// concrete MP3 encoder (Mp3EncoderBackend seam), and any UI (PRD-0101).
//
// THREADING (CLAUDE.md): exportArrangement() runs on a caller-supplied BACKGROUND
// thread. It is offline code and may freely allocate, lock, and do file I/O. It
// NEVER touches the audio device or processBlock.
//
// NO SINGLETONS: the MP3 backend is obtained through an injectable factory
// (defaulting to makeMp3EncoderBackend()) so a test can supply a fake backend.
//==============================================================================

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "ExportOptions.h"
#include "Mp3EncoderBackend.h"
#include "OfflineRenderDriver.h"

namespace Daw::Export
{

//==============================================================================
// FormatCapabilities — what a single Format supports. Read by PRD-0101 to build
// its controls and by exportArrangement to validate (defensive backstop).
//==============================================================================

struct FormatCapabilities
{
    Format            format {};
    bool              supported { false };          // false => no backend (MP3)
    std::vector<int>  bitDepths;                    // {16,24,32}/{16,24}/{} for MP3
    std::vector<int>  mp3Bitrates;                  // {128,192,256,320} for MP3
    bool              supportsVbr { false };

    bool allowsBitDepth (int bd) const
    {
        for (int v : bitDepths) if (v == bd) return true;
        return false;
    }
    bool allowsBitrate (int kbps) const
    {
        for (int v : mp3Bitrates) if (v == kbps) return true;
        return false;
    }
};

//==============================================================================
// AudioExporter
//==============================================================================

class AudioExporter
{
public:
    /// Factory for an MP3 backend. Injected so tests can supply a fake; defaults
    /// to the build-selected makeMp3EncoderBackend() registration seam.
    using Mp3BackendFactory = std::function<std::unique_ptr<Mp3EncoderBackend>()>;

    AudioExporter();
    explicit AudioExporter (Mp3BackendFactory mp3Factory);

    //--------------------------------------------------------------------------
    // Capability table — the SINGLE SOURCE OF TRUTH (PRD-0100 §1.5.2). Static so
    // PRD-0101 can query without an instance; the supported sample-rate set is
    // shared across all formats.
    //--------------------------------------------------------------------------

    static const std::vector<double>& supportedSampleRates();

    /// The capabilities for one format. MP3.supported reflects whether THIS
    /// exporter's backend factory yields a backend (so a fake-injecting test or a
    /// no-LAME build both report correctly).
    FormatCapabilities capabilitiesFor (Format format) const;

    /// Convenience: true iff a (format, sampleRate, bitDepth/bitrate) request is
    /// valid for this exporter. Pure query; never creates a file.
    bool isSupported (const ExportOptions& options) const;

    //--------------------------------------------------------------------------
    // The export entry point. Validates options, builds the writer, and STREAMS
    // PRD-0099's render blocks into it. BACKGROUND THREAD.
    //
    //   options          : the export contract (range is passed THROUGH to the
    //                      driver; the exporter never seeks the timeline).
    //   driver           : the PRD-0099 driver, already constructed for the same
    //                      sample rate and the matching range. The exporter is a
    //                      pure consumer of its blocks.
    //   progress         : receives a fraction in [0,1] (forwarded from driver).
    //   cancelRequested  : polled by the driver between blocks; on cancel the
    //                      exporter closes + deletes the partial file.
    //--------------------------------------------------------------------------
    ExportResult exportArrangement (const ExportOptions&         options,
                                    OfflineRenderDriver&         driver,
                                    std::function<void (float)>  progress,
                                    std::atomic<bool>&           cancelRequested);

private:
    // Build the capability table entry (no backend probing — pure table).
    static FormatCapabilities staticCapabilitiesFor (Format format);

    // Validate options against the capability table. Returns an InvalidOptions /
    // Unsupported result (with message) iff invalid; std::nullopt if valid.
    std::optional<ExportResult> validate (const ExportOptions& options) const;

    // PCM (WAV/FLAC) export path via juce::AudioFormatWriter.
    ExportResult exportPcm (const ExportOptions&        options,
                            OfflineRenderDriver&        driver,
                            std::function<void (float)> progress,
                            std::atomic<bool>&          cancelRequested);

    // MP3 export path via the pluggable backend.
    ExportResult exportMp3 (const ExportOptions&        options,
                            OfflineRenderDriver&        driver,
                            std::function<void (float)> progress,
                            std::atomic<bool>&          cancelRequested);

    Mp3BackendFactory mp3Factory_;
};

} // namespace Daw::Export
