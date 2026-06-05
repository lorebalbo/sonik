//==============================================================================
// PRD-0101: AudioExportTests — the ExportDialog's headless seams (option mapping,
// format-driven enable/disable, on-Export validation, last-used persistence) and
// the ExportRunner cancel-deletes-partial behaviour. The interactive modal flow
// (live progress bar, FileChooser, missing-source gate routing) is the manual
// plan (§1.6); everything verifiable headlessly is asserted here.
//
// JUCE UnitTest, group "Audio Export (PRD-0101)", category "Sonik".
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../Source/Features/Daw/Export/Ui/ExportDialog.h"
#include "../Source/Features/Daw/Export/ExportRunner.h"
#include "../Source/Features/Daw/Export/OfflineRenderDriver.h"
#include "../Source/Features/Daw/Playback/ArrangementSnapshot.h"
#include "../Source/Features/Daw/Import/ImportSourcePublisher.h"
#include "../Source/Features/AudioEngine/AudioBufferHolder.h"

#include <atomic>
#include <memory>

using namespace Daw;
using namespace Daw::Export;
using namespace Daw::Export::Ui;

namespace
{

juce::File tmp (const juce::String& ext)
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory)
               .getChildFile ("sonik_pr0101_" + juce::Uuid().toString())
               .withFileExtension (ext);
}

// A minimal context with no live subsystems; fakes supplied per test.
ExportContext baseContext()
{
    ExportContext ctx;
    ctx.projectSampleRate = 44100.0;
    ctx.hasSelectedRegion = [] { return false; };
    ctx.areAllSourcesResolved = [] { return true; };
    return ctx;
}

} // namespace

//==============================================================================
class AudioExportTests final : public juce::UnitTest
{
public:
    AudioExportTests() : juce::UnitTest ("Audio Export (PRD-0101)", "Sonik") {}

    void runTest() override
    {
        testFormatDrivenControlState();
        testSelectedRegionAvailability();
        testValidation();
        testBuildOptionsMappingAndExtension();
        testLastUsedPersistenceRestore();
        testRunnerCancelDeletesPartial();
        testRunnerMissingSourceRendersSilenceAndCompletes();
    }

private:
    //--------------------------------------------------------------------------
    void testFormatDrivenControlState()
    {
        beginTest ("format drives bit-depth vs MP3-bitrate enablement (§1.5.4)");

        auto dlg = createExportDialogForTest (baseContext());

        dlg->setFormatForTest (Format::Wav);
        {
            auto s = dlg->computeControlState();
            expect (s.bitDepthEnabled,   "WAV enables bit depth");
            expect (! s.mp3BitrateEnabled, "WAV disables MP3 bitrate");
            expect (! s.mp3VbrEnabled,     "WAV disables MP3 VBR");
        }

        dlg->setFormatForTest (Format::Flac);
        {
            auto s = dlg->computeControlState();
            expect (s.bitDepthEnabled,   "FLAC enables bit depth");
            expect (! s.mp3BitrateEnabled, "FLAC disables MP3 bitrate");
        }

        dlg->setFormatForTest (Format::Mp3);
        {
            auto s = dlg->computeControlState();
            expect (! s.bitDepthEnabled, "MP3 disables bit depth");
            expect (s.mp3BitrateEnabled, "MP3 enables bitrate");
            expect (s.mp3VbrEnabled,     "MP3 enables VBR");
        }
    }

    void testSelectedRegionAvailability()
    {
        beginTest ("selected-region radio tracks hasSelectedRegion (§1.4)");

        {
            auto ctx = baseContext();
            ctx.hasSelectedRegion = [] { return false; };
            auto dlg = createExportDialogForTest (std::move (ctx));
            expect (! dlg->computeControlState().selectedRegionEnabled,
                    "no selection => region disabled");
        }
        {
            auto ctx = baseContext();
            ctx.hasSelectedRegion = [] { return true; };
            ctx.selectedRegion = [] (double) { return ExportRange { 0, 44100 }; };
            auto dlg = createExportDialogForTest (std::move (ctx));
            expect (dlg->computeControlState().selectedRegionEnabled,
                    "selection present => region enabled");
        }
    }

    void testValidation()
    {
        beginTest ("on-Export validation: empty path / region-without-selection (§1.5.4)");

        // Empty output path => invalid.
        {
            auto dlg = createExportDialogForTest (baseContext());
            dlg->setFormatForTest (Format::Wav);
            expect (! dlg->validateForExport().ok, "empty path must fail validation");
        }

        // Valid writable path + whole arrangement => good.
        {
            auto dlg = createExportDialogForTest (baseContext());
            dlg->setFormatForTest (Format::Wav);
            dlg->setRangeWholeForTest (true);
            dlg->setOutputFileForTest (tmp (".wav"));
            expect (dlg->validateForExport().ok, "writable path + whole range should validate");
        }

        // "Selected region" chosen but no region selected => invalid.
        {
            auto ctx = baseContext();
            ctx.hasSelectedRegion = [] { return false; };
            auto dlg = createExportDialogForTest (std::move (ctx));
            dlg->setFormatForTest (Format::Wav);
            dlg->setOutputFileForTest (tmp (".wav"));
            dlg->setRangeWholeForTest (false); // request region
            expect (! dlg->validateForExport().ok,
                    "region range with no selection must fail validation");
        }
    }

    void testBuildOptionsMappingAndExtension()
    {
        beginTest ("buildExportOptions reflects controls + reconciles extension");

        auto dlg = createExportDialogForTest (baseContext());
        dlg->setFormatForTest (Format::Flac);
        dlg->setSampleRateForTest (48000.0);
        dlg->setBitDepthForTest (16);
        dlg->setNormalizeForTest (true);
        dlg->setRangeWholeForTest (true);
        // Give a path with a deliberately wrong extension to test reconciliation.
        dlg->setOutputFileForTest (tmp (".wav"));

        auto opt = dlg->buildExportOptions();
        expect (opt.format == Format::Flac);
        expect (juce::approximatelyEqual (opt.sampleRate, 48000.0));
        expectEquals (opt.bitDepth, 16);
        expect (opt.normalize);
        expectEquals (opt.outputFile.getFileExtension(), juce::String (".flac"),
                      "extension reconciled to the chosen format");
        expect (opt.range.isEmpty(), "whole arrangement => empty range");
    }

    void testLastUsedPersistenceRestore()
    {
        beginTest ("last-used options restore from the PropertiesFile (§1.5.6)");

        auto settings = tmp (".settings");
        juce::PropertiesFile::Options o;
        o.applicationName = "sonik_export_test";
        o.filenameSuffix  = "settings";
        juce::PropertiesFile props (settings, o);

        using K = ExportDialog::PersistenceKeys;
        props.setValue (K::format,     (int) Format::Flac);
        props.setValue (K::sampleRate, 48000);
        props.setValue (K::bitDepth,   16);
        props.setValue (K::normalize,  true);
        props.setValue (K::rangeWhole, true);
        props.saveIfNeeded();

        auto ctx = baseContext();
        ctx.properties = &props;
        auto dlg = createExportDialogForTest (std::move (ctx));

        auto opt = dlg->buildExportOptions();
        expect (opt.format == Format::Flac, "persisted format restored");
        expect (juce::approximatelyEqual (opt.sampleRate, 48000.0), "persisted rate restored");
        expectEquals (opt.bitDepth, 16, "persisted bit depth restored");
        expect (opt.normalize, "persisted normalize restored");

        settings.deleteFile();
    }

    //--------------------------------------------------------------------------
    // ExportRunner — the headless path the dialog drives (§1.5.8).
    //--------------------------------------------------------------------------
    static ExportRunner::Job makeJob (uint64_t sourceId, int frames,
                                      const Daw::Import::ImportSourcePublisher* pub,
                                      const juce::File& out)
    {
        ExportRunner::Job job;
        job.snapshot.laneCount = 1;
        auto& ln = job.snapshot.lanes[0];
        auto& ev = ln.events[ln.count++];
        ev.sourceFileId       = sourceId;
        ev.sourceStartSample   = 0;
        ev.sourceEndSample     = frames;
        ev.timelineStartSample = 0;
        ev.timelineEndSample   = frames;
        ev.gain                = 1.0f;
        ev.laneIndex           = 0;
        ev.sourceReadHandle    = -1;

        job.readerProvider = [pub] (const ClipEvent& e) -> std::unique_ptr<juce::AudioFormatReader>
        {
            if (pub == nullptr) return nullptr;
            // The publisher keys on the string id; the snapshot carries only the
            // hash, so we look up by a single known id here (test has one source).
            return pub->makeReader ("import:cap");
        };

        job.options.format     = Format::Wav;
        job.options.sampleRate = 44100.0;
        job.options.bitDepth   = 24;
        job.options.range      = { 0, frames };
        job.options.outputFile = out;
        return job;
    }

    void testRunnerCancelDeletesPartial()
    {
        beginTest ("ExportRunner cancel aborts and deletes the partial file (§1.5.2)");

        const int frames = (int) (44100.0 * 2.0);
        juce::AudioBuffer<float> buf (2, frames);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < frames; ++i)
                buf.setSample (ch, i, 0.25f);
        AudioBufferHolder::Ptr holder = new AudioBufferHolder (std::move (buf), 44100.0, frames);

        Daw::Import::ImportSourcePublisher pub;
        pub.publish ("import:cap", holder);

        auto out = tmp (".wav");
        // sourceFileId hash is irrelevant here (the provider ignores it), use any.
        auto job = makeJob (123ull, frames, &pub, out);

        std::atomic<float> progress { 0.0f };
        std::atomic<bool>  cancel { true }; // cancel from the first block
        auto r = ExportRunner::runBlocking (job, progress, cancel);
        expect (r.status == ExportResult::Status::Cancelled, "should report Cancelled");
        expect (! out.existsAsFile(), "partial file must be deleted on cancel");
        out.deleteFile();
    }

    void testRunnerMissingSourceRendersSilenceAndCompletes()
    {
        beginTest ("ExportRunner with a missing source still completes (driver backstop)");

        const int frames = 4 * OfflineRenderConfig::kDefaultBlockSize;
        auto out = tmp (".wav");
        // No publisher => the provider returns nullptr => the driver renders the
        // clip's span as silence and the export still completes (the dialog's
        // PRD-0097 gate blocks this up-front in the UI; the runner is the backstop).
        auto job = makeJob (123ull, frames, nullptr, out);

        std::atomic<float> progress { 0.0f };
        std::atomic<bool>  cancel { false };
        auto r = ExportRunner::runBlocking (job, progress, cancel);
        expect (r.status == ExportResult::Status::Completed, "render completes despite missing source");
        expect (out.existsAsFile(), "a file is produced");

        // The produced file should be readable and the expected length (silence).
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (out));
        expect (reader != nullptr, "output readable");
        if (reader != nullptr)
            expect (reader->lengthInSamples == frames, "length equals range");
        out.deleteFile();
    }
};

static AudioExportTests audioExportTests;
