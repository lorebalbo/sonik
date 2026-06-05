//==============================================================================
// PRD-0101 §1.5.8 CAPSTONE: the full DAW loop, end to end, headless.
//
//   build an arrangement -> SAVE it (.soniksession, PRD-0095/0096)
//                        -> REOPEN it from disk into a fresh model
//                        -> EXPORT it via the headless path (ExportJobBuilder +
//                           ExportRunner, the SAME render/encode the dialog drives
//                           minus the UI)
//   and assert the rendered file is non-empty, readable, and the expected
//   duration for the chosen range — proving save -> reopen -> export.
//
// The clip references an in-memory source by an "import:<hash>" id so the
// ClipSourceResolver (inside ExportJobBuilder) resolves it through the
// ImportSourcePublisher without needing real files in the library DB.
//
// JUCE UnitTest, group "Session Round Trip (PRD-0101)", category "Sonik".
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "Features/Daw/State/DawState.h"
#include "Features/Daw/Model/ChannelGroup.h"
#include "Features/Daw/Model/DawClip.h"
#include "Features/Daw/Session/SessionSerializer.h"
#include "Features/Daw/Export/ExportJobBuilder.h"
#include "Features/Daw/Export/ExportRunner.h"
#include "Features/Daw/Import/ImportSourcePublisher.h"
#include "Features/AudioEngine/AudioBufferHolder.h"
#include "Features/Deck/Database/TrackDatabase.h"

#include <atomic>
#include <memory>

using namespace Daw;
using namespace Daw::Session;
using namespace Daw::Export;

class SessionRoundTripTests final : public juce::UnitTest
{
public:
    SessionRoundTripTests() : juce::UnitTest ("Session Round Trip (PRD-0101)", "Sonik") {}

    void runTest() override
    {
        testBuildSaveReopenExport();
    }

private:
    struct ScopedTempDir
    {
        juce::File dir;
        ScopedTempDir()
        {
            dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile ("sonik_roundtrip_" + juce::Uuid().toString());
            dir.createDirectory();
        }
        ~ScopedTempDir() { dir.deleteRecursively(); }
        juce::File child (const juce::String& n) const { return dir.getChildFile (n); }
    };

    void testBuildSaveReopenExport()
    {
        beginTest ("build -> save -> reopen -> export produces a correct audio file");

        ScopedTempDir tmp;
        const juce::String kSourceId = "import:cap";
        const int frames = 44100; // 1 second @ project rate

        //----------------------------------------------------------------------
        // 1. Build a small arrangement: one clip on deck 0's Original lane,
        //    referencing an in-memory source by an "import:" id.
        //----------------------------------------------------------------------
        auto daw  = DawState::createDawBranch();
        auto track = DawState::ensureTrackForDeck (daw, 0);
        auto lanes = track.getChildWithName (DawIDs::lanes);
        auto original = ChannelGroup::findLane (lanes, ChannelGroup::LaneKind::Original);

        DawClip clip;
        clip.clipId              = juce::Uuid();
        clip.laneId              = juce::Uuid (original.getProperty (DawIDs::laneId).toString());
        clip.sourceFileId        = kSourceId;
        clip.sourceStartSample   = 0;
        clip.sourceEndSample     = frames;
        clip.timelineStartSample = 0;
        clip.sourceLengthSamples = frames;
        clip.gainDb              = 0.0f;
        original.getChildWithName (DawIDs::clips)
                .addChild (DawClip::toValueTree (clip), -1, nullptr);

        SessionMetadata meta;
        meta.projectSampleRate = DawState::kProjectSampleRate;
        meta.appVersion        = "0.1.0";

        //----------------------------------------------------------------------
        // 2. Save to a .soniksession, then 3. reopen from disk into a FRESH tree.
        //----------------------------------------------------------------------
        SessionSerializer serializer;
        auto target = tmp.child ("Capstone.soniksession");
        auto saved = serializer.save (daw, meta, target);
        expect (saved.ok(), "session saved");
        expect (saved.writtenPath.existsAsFile(), "session file written");

        auto loaded = serializer.load (target);
        expect (loaded.ok(), "session reopened");
        auto reopenedDaw = loaded.document.daw;
        expect (reopenedDaw.isValid(), "reopened daw tree valid");
        expect (reopenedDaw.isEquivalentTo (daw), "reopened tree structurally identical");

        //----------------------------------------------------------------------
        // Publish the in-memory source the reopened clip references.
        //----------------------------------------------------------------------
        juce::AudioBuffer<float> buf (2, frames);
        for (int i = 0; i < frames; ++i)
        {
            const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi
                                              * 440.0f * (float) i / 44100.0f);
            buf.setSample (0, i, v);
            buf.setSample (1, i, v);
        }
        AudioBufferHolder::Ptr holder = new AudioBufferHolder (std::move (buf), 44100.0, frames);
        Daw::Import::ImportSourcePublisher publisher;
        publisher.publish (kSourceId, holder);

        //----------------------------------------------------------------------
        // 4. Export via the headless path (ExportJobBuilder + ExportRunner).
        //----------------------------------------------------------------------
        auto dbFile = tmp.child ("lib.db");
        TrackDatabase db (dbFile);

        ExportJobBuilder builder (reopenedDaw, db, &publisher);

        ExportOptions options;
        options.format     = Format::Wav;
        options.sampleRate = DawState::kProjectSampleRate;
        options.bitDepth   = 24;
        options.range      = {}; // whole arrangement
        options.outputFile = tmp.child ("mix.wav");

        auto job = builder.buildJob (options);

        std::atomic<float> progress { 0.0f };
        std::atomic<bool>  cancel { false };
        auto result = ExportRunner::runBlocking (job, progress, cancel);

        //----------------------------------------------------------------------
        // 5. Assert the rendered file is non-empty, readable, and the expected
        //    duration (the 1s clip plus the whole-arrangement silence tail).
        //----------------------------------------------------------------------
        expect (result.status == ExportResult::Status::Completed,
                "export completed: " + result.message);
        expect (options.outputFile.existsAsFile() && options.outputFile.getSize() > 0,
                "exported file is non-empty");
        expect (juce::approximatelyEqual (progress.load(), 1.0f), "progress reached 1.0");

        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (options.outputFile));
        expect (reader != nullptr, "exported file is readable");
        if (reader != nullptr)
        {
            expect (juce::approximatelyEqual (reader->sampleRate, 44100.0), "rate matches");
            expect (reader->numChannels == 2, "stereo");
            // Whole-arrangement render = the 1s clip + a bounded silence tail.
            expect (reader->lengthInSamples >= frames,
                    "duration covers at least the clip length");
            expect (reader->lengthInSamples <= frames + 44100,
                    "tail is bounded (<= ~1s past the clip)");

            // The first ~second carries the clip's audio (non-silent).
            juce::AudioBuffer<float> check (2, frames);
            reader->read (&check, 0, frames, 0, true, true);
            expect (check.getMagnitude (0, 0, frames) > 0.1f,
                    "the clip's audio is present in the export (not silence)");
        }
    }
};

static SessionRoundTripTests sessionRoundTripTests;
