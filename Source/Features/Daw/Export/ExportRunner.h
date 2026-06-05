#pragma once
//==============================================================================
// PRD-0101: ExportRunner — the HEADLESS export orchestration the ExportDialog
// drives, MINUS the UI (PRD-0101 §1.5.8). It composes the PRD-0099
// OfflineRenderDriver and the PRD-0100 AudioExporter and adds NO new render or
// encode logic of its own.
//
// WHY A SEPARATE PIECE: the dialog (PRD-0101 §1.2) must run the render on a
// BACKGROUND thread, never the message thread (CLAUDE.md "The Audio Thread" rule
// applies to the live engine; the offline render is non-real-time but must still
// stay off the UI thread so the message thread never blocks). The same
// render/encode must also be callable directly by the capstone round-trip test
// (§1.5.8) without instantiating any juce::Component. ExportRunner is that shared
// core: a pure function over (snapshot, options, reader-provider) returning an
// ExportResult, communicating progress + cancellation through std::atomic only.
//
// THREADING: runBlocking() is meant to be invoked on a caller-owned background
// thread (the dialog's custom juce::Thread; the test's own thread or the calling
// thread). It allocates / does file I/O freely (offline code), but writes only an
// std::atomic<float> progress and reads only an std::atomic<bool> cancel flag —
// no locks on any path the live engine could touch.
//==============================================================================

#include <atomic>
#include <functional>
#include <memory>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "AudioExporter.h"
#include "ExportOptions.h"
#include "OfflineRenderDriver.h"
#include "../Playback/ArrangementSnapshot.h"

namespace Daw::Export
{

//==============================================================================
// ExportRunner — stateless composer of the driver + exporter.
//==============================================================================

class ExportRunner
{
public:
    //--------------------------------------------------------------------------
    // The render contract the dialog/test hands the runner. The snapshot and the
    // reader-provider are supplied by the caller so the runner never reaches into
    // any live subsystem: the dialog compiles the live daw branch into a snapshot
    // and wraps a ClipSourceResolver; the test supplies an in-memory snapshot and
    // an in-memory reader-provider (the SAME ReaderProvider seam PRD-0099 uses).
    //--------------------------------------------------------------------------
    struct Job
    {
        /// The compiled arrangement at the EXPORT sample rate (options.sampleRate).
        ArrangementSnapshot snapshot {};

        /// Per-clip reader supply (nullptr-returning => the driver renders that
        /// clip's span as silence and records its sourceFileId — PRD-0099 §1.5.8).
        OfflineRenderDriver::ReaderProvider readerProvider;

        /// The full export contract (format / rate / depth / range / normalize /
        /// outputFile). range is passed THROUGH to the driver and the exporter.
        ExportOptions options {};

        /// Offline block size. Defaults to the live engine block size so any
        /// block-boundary quantisation matches playback (PRD-0099 §1.5.5).
        int blockSize { OfflineRenderConfig::kDefaultBlockSize };
    };

    //--------------------------------------------------------------------------
    // Build the OfflineRenderConfig from a Job. Whole-arrangement is derived from
    // the snapshot when options.range is empty; otherwise the explicit half-open
    // [start,end) region is used. Pure; no side effects. Exposed so a test can
    // assert the config without running the render.
    //--------------------------------------------------------------------------
    static OfflineRenderConfig makeConfig (const Job& job)
    {
        OfflineRenderConfig cfg;
        cfg.snapshot         = job.snapshot;
        cfg.renderSampleRate = job.options.sampleRate;
        cfg.blockSize        = job.blockSize;

        if (job.options.range.isEmpty())
        {
            // Whole arrangement: the driver derives [0, arrangementEnd) from the
            // snapshot and applies the EngineSilenceCap tail (its default).
            cfg.rangeMode  = OfflineRenderConfig::RangeMode::WholeArrangement;
            cfg.tailPolicy = OfflineRenderConfig::TailPolicy::EngineSilenceCap;
        }
        else
        {
            // Explicit region: render exactly [start,end), no tail (region default).
            cfg.rangeMode        = OfflineRenderConfig::RangeMode::Region;
            cfg.rangeStartSample = job.options.range.startSample;
            cfg.rangeEndSample   = job.options.range.endSample;
            cfg.tailPolicy       = OfflineRenderConfig::TailPolicy::None;
        }

        return cfg;
    }

    //--------------------------------------------------------------------------
    // Run the full render + encode synchronously on the CALLING thread (which the
    // caller guarantees is a background thread). Writes `progress` (0..1) and
    // polls `cancel` between blocks. Returns the PRD-0100 ExportResult verbatim:
    //   - Cancelled  : cancel was set; the exporter closed + DELETED the partial
    //                  file (PRD-0100), so no artefact remains.
    //   - Failed     : encoder/disk/stream error; partial file deleted.
    //   - Unsupported: no backend for the format (e.g. MP3 without LAME).
    //   - InvalidOptions: capability validation rejected the options.
    //   - Completed  : the file exists, is readable, and matches the request.
    //
    // The `onProgress` callback (optional) is invoked from THIS background thread
    // with the same fraction written to the atomic; the dialog marshals it onto
    // the message thread via callAsync. Pass an empty function for the test path.
    //--------------------------------------------------------------------------
    static ExportResult runBlocking (const Job&                  job,
                                     std::atomic<float>&         progress,
                                     std::atomic<bool>&          cancel,
                                     std::function<void (float)> onProgress = {},
                                     AudioExporter*              exporterIn = nullptr)
    {
        // A clip with no reader is rendered as silence by the driver; the runner
        // exposes which sources were silent so the dialog can surface the
        // missing-source error (PRD-0097 funnels through here as well — the dialog
        // gates BEFORE calling this, but the driver's silent-clip list is the
        // defensive backstop if a source disappears mid-render).
        auto config = makeConfig (job);

        OfflineRenderDriver driver (config, job.readerProvider);

        AudioExporter  localExporter;
        AudioExporter& exporter = (exporterIn != nullptr) ? *exporterIn : localExporter;

        auto progressFn = [&progress, onProgress] (float f)
        {
            progress.store (f, std::memory_order_release);
            if (onProgress)
                onProgress (f);
        };

        return exporter.exportArrangement (job.options, driver, progressFn, cancel);
    }

private:
    ExportRunner() = delete;
};

} // namespace Daw::Export
