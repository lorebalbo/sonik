//==============================================================================
// PRD-0095: SessionSerializerTests — deep round-trip equality of a
// multi-lane / multi-clip / automation-bearing `daw` tree, future-version
// rejection, corrupt-file rejection, FileNotFound, unknown-node preservation,
// extension normalisation, and the atomic-write integrity guarantee.
//
// JUCE UnitTest, category "Sonik". Message-thread only; no audio-thread path.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "Features/Daw/State/DawState.h"
#include "Features/Daw/Model/ChannelGroup.h"
#include "Features/Daw/Model/DawClip.h"
#include "Features/Daw/Automation/AutomationIds.h"
#include "Features/Daw/Session/SessionSerializer.h"

using namespace Daw::Session;

class SessionSerializerTests final : public juce::UnitTest
{
public:
    SessionSerializerTests() : juce::UnitTest ("Session Serializer", "Sonik") {}

    void runTest() override
    {
        testRoundTripDeepEquality();
        testDawClipFieldsBitExact();
        testAutomationRoundTrip();
        testNoAudioEmbedded();
        testFutureVersionRejected();
        testCorruptFileRejected();
        testFileNotFound();
        testUnknownNodePreservation();
        testExtensionNormalisation();
        testAtomicWriteIntegrity();
    }

private:
    //==========================================================================
    // A temp directory that cleans itself up.
    //==========================================================================
    struct ScopedTempDir
    {
        juce::File dir;
        ScopedTempDir()
        {
            dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile ("sonik_session_test_" + juce::Uuid().toString());
            dir.createDirectory();
        }
        ~ScopedTempDir() { dir.deleteRecursively(); }
        juce::File child (const juce::String& name) const { return dir.getChildFile (name); }
    };

    //==========================================================================
    // Builds a representative `daw` branch: two channel groups, several clips
    // with non-trivial crop windows on multiple lanes, plus a continuous and a
    // boolean automation lane with breakpoints/steps.
    //==========================================================================
    static juce::ValueTree buildRepresentativeDaw()
    {
        auto daw = DawState::createDawBranch();

        auto addClip = [] (juce::ValueTree lane, juce::String sourceId,
                           std::int64_t srcStart, std::int64_t srcEnd,
                           std::int64_t timelineStart, std::int64_t srcLen, float gainDb)
        {
            DawClip c;
            c.clipId              = juce::Uuid();
            c.laneId              = juce::Uuid (lane.getProperty (DawIDs::laneId).toString());
            c.sourceFileId        = sourceId;
            c.sourceStartSample   = srcStart;
            c.sourceEndSample     = srcEnd;
            c.timelineStartSample = timelineStart;
            c.sourceLengthSamples = srcLen;
            c.gainDb              = gainDb;
            lane.getChildWithName (DawIDs::clips).addChild (DawClip::toValueTree (c), -1, nullptr);
        };

        auto t0 = DawState::ensureTrackForDeck (daw, 0);
        auto lanes0 = t0.getChildWithName (DawIDs::lanes);
        auto original0     = ChannelGroup::findLane (lanes0, ChannelGroup::LaneKind::Original);
        auto instrumental0 = ChannelGroup::findLane (lanes0, ChannelGroup::LaneKind::Instrumental);

        // Two clips sharing one source (the chop-into-regions case) + one on a
        // different lane referencing a different source.
        addClip (original0,     "lib:track-1234", 44100,   441000,  0,        9000000, -3.5f);
        addClip (original0,     "lib:track-1234", 882000,  1323000, 500000,   9000000, -3.5f);
        addClip (instrumental0, "stem:track-1234:vocal", 0, 220500, 250000,   220500,  0.0f);

        auto t1 = DawState::ensureTrackForDeck (daw, 1);
        auto lanes1 = t1.getChildWithName (DawIDs::lanes);
        auto vocal1 = ChannelGroup::findLane (lanes1, ChannelGroup::LaneKind::Vocal);
        addClip (vocal1, "ext:abcdef0123", 1000, 100000, 1200000, 100000, 2.25f);

        // Automation subtree (PRD-0087) — covered by structural copy.
        using namespace Daw;
        juce::ValueTree automation (AutomationIDs::automation);

        juce::ValueTree contLane (AutomationIDs::lane);
        contLane.setProperty (AutomationIDs::owner,       "A",           nullptr);
        contLane.setProperty (AutomationIDs::parameterId, "filter",      nullptr);
        contLane.setProperty (AutomationIDs::kind,        "continuous",  nullptr);
        contLane.setProperty (AutomationIDs::enabled,     true,          nullptr);
        for (auto bp : { std::tuple<std::int64_t, double, const char*> { 0,       0.0,  "linear" },
                          std::tuple<std::int64_t, double, const char*> { 220500,  0.75, "step" },
                          std::tuple<std::int64_t, double, const char*> { 441000, -0.5,  "linear" } })
        {
            juce::ValueTree b (AutomationIDs::breakpoint);
            b.setProperty (AutomationIDs::timelineSample, (juce::int64) std::get<0> (bp), nullptr);
            b.setProperty (AutomationIDs::value,          std::get<1> (bp),               nullptr);
            b.setProperty (AutomationIDs::interpolation,  juce::String (std::get<2> (bp)), nullptr);
            contLane.addChild (b, -1, nullptr);
        }
        automation.addChild (contLane, -1, nullptr);

        juce::ValueTree boolLane (AutomationIDs::lane);
        boolLane.setProperty (AutomationIDs::owner,       "master",  nullptr);
        boolLane.setProperty (AutomationIDs::parameterId, "keyLock", nullptr);
        boolLane.setProperty (AutomationIDs::kind,        "boolean", nullptr);
        boolLane.setProperty (AutomationIDs::enabled,     true,      nullptr);
        for (auto st : { std::pair<std::int64_t, bool> { 0, false },
                          std::pair<std::int64_t, bool> { 330000, true } })
        {
            juce::ValueTree s (AutomationIDs::step);
            s.setProperty (AutomationIDs::timelineSample, (juce::int64) st.first, nullptr);
            s.setProperty (AutomationIDs::value,          st.second,              nullptr);
            boolLane.addChild (s, -1, nullptr);
        }
        automation.addChild (boolLane, -1, nullptr);

        daw.addChild (automation, -1, nullptr);
        return daw;
    }

    static SessionMetadata buildMetadata()
    {
        SessionMetadata m;
        m.projectSampleRate = 44100.0;
        m.appVersion        = "0.1.0";
        m.masterGrid.bpm                = 128.0;
        m.masterGrid.downbeatSample     = 12345;
        m.masterGrid.timeSigNumerator   = 4;
        m.masterGrid.timeSigDenominator = 4;
        m.masterGrid.playheadSample     = 7777777;
        m.masterGrid.loopStartSample    = 100000;
        m.masterGrid.loopEndSample      = 900000;
        m.masterGrid.loopEnabled        = true;
        m.viewState.zoomSamplesPerPixel = 512.0;
        m.viewState.scrollStartSample   = 4096000;
        m.viewState.selectedClipId      = "selected-clip-uuid";
        return m;
    }

    //==========================================================================
    void testRoundTripDeepEquality()
    {
        beginTest ("load(save(x)) is deeply equal for daw tree + metadata");

        ScopedTempDir tmp;
        SessionSerializer s;

        auto daw  = buildRepresentativeDaw();
        auto meta = buildMetadata();
        auto target = tmp.child ("My Set.soniksession");

        auto saved = s.save (daw, meta, target);
        expect (saved.ok(), "save succeeded");
        expect (saved.writtenPath.existsAsFile(), "file written");

        auto loaded = s.load (target);
        expect (loaded.ok(), "load succeeded");

        expect (loaded.document.daw.isEquivalentTo (daw),
                "daw subtree is structurally identical after round-trip");

        expectEquals (loaded.document.projectSampleRate, 44100.0);
        expectEquals (loaded.document.masterGrid.bpm, 128.0);
        expectEquals ((int) loaded.document.masterGrid.downbeatSample, 12345);
        expectEquals (loaded.document.masterGrid.timeSigNumerator, 4);
        expectEquals ((int) loaded.document.masterGrid.playheadSample, 7777777);
        expectEquals ((int) loaded.document.masterGrid.loopStartSample, 100000);
        expectEquals ((int) loaded.document.masterGrid.loopEndSample, 900000);
        expect (loaded.document.masterGrid.loopEnabled);

        expect (loaded.document.viewState.zoomSamplesPerPixel.has_value());
        expectEquals (*loaded.document.viewState.zoomSamplesPerPixel, 512.0);
        expect (loaded.document.viewState.scrollStartSample.has_value());
        expectEquals ((int) *loaded.document.viewState.scrollStartSample, 4096000);
        expectEquals (loaded.document.viewState.selectedClipId, juce::String ("selected-clip-uuid"));

        expectEquals (loaded.document.loadedFromVersion, kCurrentSchemaVersion);
    }

    //==========================================================================
    void testDawClipFieldsBitExact()
    {
        beginTest ("all eight DawClip fields survive bit-exact");

        ScopedTempDir tmp;
        SessionSerializer s;

        auto daw  = buildRepresentativeDaw();
        auto meta = buildMetadata();
        auto target = tmp.child ("clips.soniksession");

        expect (s.save (daw, meta, target).ok());
        auto loaded = s.load (target);
        expect (loaded.ok());

        // Locate the first clip on deck-0 Original lane in both trees and compare
        // every field via the DawClip value object.
        auto firstClip = [] (const juce::ValueTree& d) -> DawClip
        {
            auto t0     = DawState::findTrackForDeck (d, 0);
            auto lanes  = t0.getChildWithName (DawIDs::lanes);
            auto lane   = ChannelGroup::findLane (lanes, ChannelGroup::LaneKind::Original);
            auto clips  = lane.getChildWithName (DawIDs::clips);
            return DawClip::fromValueTree (clips.getChild (0));
        };

        auto a = firstClip (daw);
        auto b = firstClip (loaded.document.daw);

        expect (a.clipId == b.clipId, "clipId");
        expect (a.laneId == b.laneId, "laneId");
        expectEquals (b.sourceFileId, a.sourceFileId);
        expectEquals ((int) b.sourceStartSample,   (int) a.sourceStartSample);
        expectEquals ((int) b.sourceEndSample,     (int) a.sourceEndSample);
        expectEquals ((int) b.timelineStartSample, (int) a.timelineStartSample);
        expectEquals ((int) b.sourceLengthSamples, (int) a.sourceLengthSamples);
        expectEquals (b.gainDb, a.gainDb);
    }

    //==========================================================================
    void testAutomationRoundTrip()
    {
        beginTest ("automation lanes + breakpoints/steps round-trip");

        ScopedTempDir tmp;
        SessionSerializer s;

        auto daw  = buildRepresentativeDaw();
        auto meta = buildMetadata();
        auto target = tmp.child ("autom.soniksession");

        expect (s.save (daw, meta, target).ok());
        auto loaded = s.load (target);
        expect (loaded.ok());

        auto autoIn  = daw.getChildWithName (Daw::AutomationIDs::automation);
        auto autoOut = loaded.document.daw.getChildWithName (Daw::AutomationIDs::automation);
        expect (autoOut.isValid(), "automation container present");
        expect (autoOut.isEquivalentTo (autoIn), "automation subtree identical");

        // Spot-check a breakpoint value/interpolation explicitly.
        auto contLaneOut = autoOut.getChild (0);
        expectEquals (contLaneOut.getProperty (Daw::AutomationIDs::parameterId).toString(),
                      juce::String ("filter"));
        auto bp1 = contLaneOut.getChild (1);
        expectEquals ((int) (juce::int64) bp1.getProperty (Daw::AutomationIDs::timelineSample), 220500);
        expectEquals ((double) bp1.getProperty (Daw::AutomationIDs::value), 0.75);
        expectEquals (bp1.getProperty (Daw::AutomationIDs::interpolation).toString(),
                      juce::String ("step"));
    }

    //==========================================================================
    void testNoAudioEmbedded()
    {
        beginTest ("no audio sample data embedded; file references ids only");

        ScopedTempDir tmp;
        SessionSerializer s;

        auto daw  = buildRepresentativeDaw();
        auto meta = buildMetadata();
        auto target = tmp.child ("tiny.soniksession");
        expect (s.save (daw, meta, target).ok());

        // A session referencing multi-second sources must remain a few KB of XML.
        expect (target.getSize() < 64 * 1024, "session file is tiny (no embedded audio)");

        auto xml = target.loadFileAsString();
        expect (xml.contains ("lib:track-1234"), "clip references source by id");
        expect (! xml.contains ("data:audio"), "no embedded audio payload");
    }

    //==========================================================================
    void testFutureVersionRejected()
    {
        beginTest ("schemaVersion > current is rejected with UnsupportedFutureVersion");

        ScopedTempDir tmp;
        SessionSerializer s;

        auto daw  = buildRepresentativeDaw();
        auto meta = buildMetadata();

        // Build the root, bump the version past current, write by hand.
        auto root = s.buildSessionTree (daw, meta);
        root.setProperty (IDs::schemaVersion, kCurrentSchemaVersion + 1, nullptr);
        auto target = tmp.child ("future.soniksession");
        target.replaceWithText (root.toXmlString());

        auto loaded = s.load (target);
        expect (! loaded.ok(), "load fails");
        expect (loaded.error == LoadError::UnsupportedFutureVersion, "typed future-version error");
    }

    //==========================================================================
    void testCorruptFileRejected()
    {
        beginTest ("malformed / wrong-root / missing-version files are CorruptFile");

        ScopedTempDir tmp;
        SessionSerializer s;

        auto garbage = tmp.child ("garbage.soniksession");
        garbage.replaceWithText ("this is not <<< xml at all");
        expect (s.load (garbage).error == LoadError::CorruptFile, "non-xml rejected");

        auto wrongRoot = tmp.child ("wrongroot.soniksession");
        wrongRoot.replaceWithText ("<NotASession foo=\"1\"/>");
        expect (s.load (wrongRoot).error == LoadError::CorruptFile, "wrong root rejected");

        auto noVersion = tmp.child ("noversion.soniksession");
        noVersion.replaceWithText ("<SONIK_SESSION projectSampleRate=\"44100\"/>");
        expect (s.load (noVersion).error == LoadError::CorruptFile, "missing version rejected");
    }

    //==========================================================================
    void testFileNotFound()
    {
        beginTest ("a missing file is FileNotFound");

        ScopedTempDir tmp;
        SessionSerializer s;
        auto missing = tmp.child ("does-not-exist.soniksession");
        expect (s.load (missing).error == LoadError::FileNotFound);
    }

    //==========================================================================
    void testUnknownNodePreservation()
    {
        beginTest ("unknown nodes/attributes in the daw subtree survive load+save");

        ScopedTempDir tmp;
        SessionSerializer s;

        auto daw  = buildRepresentativeDaw();

        // Inject a node and an attribute an older build would not understand, in
        // the place future arrangement data would live (inside the daw subtree).
        juce::ValueTree future ("FUTURE_FEATURE");
        future.setProperty ("someNewField", 42, nullptr);
        daw.addChild (future, -1, nullptr);

        auto t0    = DawState::findTrackForDeck (daw, 0);
        auto lane  = ChannelGroup::findLane (t0.getChildWithName (DawIDs::lanes),
                                             ChannelGroup::LaneKind::Original);
        auto clip0 = lane.getChildWithName (DawIDs::clips).getChild (0);
        clip0.setProperty ("unknownClipAttr", "future-value", nullptr);

        auto meta   = buildMetadata();
        auto target = tmp.child ("forward.soniksession");
        expect (s.save (daw, meta, target).ok());

        // Round-trip once...
        auto loaded1 = s.load (target);
        expect (loaded1.ok());
        expect (loaded1.document.daw.isEquivalentTo (daw), "unknown data preserved on load");

        // ...and re-save the reconstructed model, then reload: unknown data must
        // be re-emitted (forward compatibility).
        auto target2 = tmp.child ("forward2.soniksession");
        expect (s.save (loaded1.document.daw, meta, target2).ok());
        auto loaded2 = s.load (target2);
        expect (loaded2.ok());

        expect (loaded2.document.daw.getChildWithName ("FUTURE_FEATURE").isValid(),
                "unknown node re-emitted on next save");
        auto reClip = ChannelGroup::findLane (
                          DawState::findTrackForDeck (loaded2.document.daw, 0)
                              .getChildWithName (DawIDs::lanes),
                          ChannelGroup::LaneKind::Original)
                          .getChildWithName (DawIDs::clips).getChild (0);
        expectEquals (reClip.getProperty ("unknownClipAttr").toString(),
                      juce::String ("future-value"), "unknown attribute re-emitted");
    }

    //==========================================================================
    void testExtensionNormalisation()
    {
        beginTest ("save normalises target to .soniksession; load ignores extension");

        ScopedTempDir tmp;
        SessionSerializer s;
        auto daw  = buildRepresentativeDaw();
        auto meta = buildMetadata();

        // No extension -> appended.
        auto noExt = tmp.child ("project-without-ext");
        auto r1 = s.save (daw, meta, noExt);
        expect (r1.ok());
        expectEquals (r1.writtenPath.getFileName(), juce::String ("project-without-ext.soniksession"));

        // Wrong extension -> replaced (no double extension).
        auto wrongExt = tmp.child ("project.txt");
        auto r2 = s.save (daw, meta, wrongExt);
        expect (r2.ok());
        expectEquals (r2.writtenPath.getFileName(), juce::String ("project.soniksession"));

        // Load is content-authoritative: rename to an arbitrary extension and it
        // still loads.
        auto renamed = tmp.child ("renamed.bak");
        r2.writtenPath.copyFileTo (renamed);
        expect (s.load (renamed).ok(), "content-authoritative load ignores extension");
    }

    //==========================================================================
    void testAtomicWriteIntegrity()
    {
        beginTest ("a mid-write failure leaves the pre-existing session intact");

        ScopedTempDir tmp;
        SessionSerializer s;
        auto meta = buildMetadata();
        auto target = tmp.child ("important.soniksession");

        // Write a known-good session first.
        auto goodDaw = buildRepresentativeDaw();
        expect (s.save (goodDaw, meta, target).ok());
        const auto goodXml = target.loadFileAsString();
        expect (goodXml.isNotEmpty());

        // Now attempt a different save that fails after the temp write but before
        // the rename.
        s.setSimulateWriteFailureForTest (true);
        auto differentDaw = DawState::createDawBranch(); // empty, clearly different
        auto failed = s.save (differentDaw, meta, target);
        s.setSimulateWriteFailureForTest (false);

        expect (! failed.ok(), "the forced save reports failure");
        expect (failed.error == SaveError::IoFailure, "typed IoFailure");

        // The original file must be byte-for-byte intact.
        expect (target.existsAsFile(), "original session still exists");
        expectEquals (target.loadFileAsString(), goodXml, "original session unchanged");

        // And no stray temp files were left behind in the directory.
        auto strays = tmp.dir.findChildFiles (juce::File::findFiles, false, "*.tmp");
        expectEquals (strays.size(), 0, "no temp file leaked");
    }
};

static SessionSerializerTests sessionSerializerTests;
