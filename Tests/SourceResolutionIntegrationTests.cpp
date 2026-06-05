//==============================================================================
// PRD-0097: SourceResolutionIntegrationTests — the INTEGRATION LAYER above the
// already-tested SourceIdResolver logic core (see SourceIdResolverTests.cpp,
// which is NOT duplicated here). This file covers the live-tree / live-subsystem
// seams added by PRD-0097:
//
//   1. ArrangementCompiler excludes `missingSource`-flagged clips from the engine
//      snapshot (resolved-only compile, §1.4) while keeping unflagged clips.
//   2. SessionSourceResolution writes the per-clip `missingSource` flag onto
//      EVERY clip whose distinct source is Missing, drives gating queries
//      (areAllSourcesResolved / missingSourceCount / missingClipIds), and CLEARS
//      the flag on a successful source relocate (driven through the real
//      TrackDatabase, §1.5.6).
//   3. The SOURCE_REFS builder round-trips: build refs from a daw tree, feed
//      them back into a fresh resolution pass.
//   4. ClipBlock's `missingSource` flag drives the DESIGN.md "Glitch" repaint
//      (offscreen render diff: ON renders more ink than OFF).
//   5. UnresolvedSourcesDialog's reachable surface (showUnresolvedSourcesStep)
//      constructs / dismisses without crashing.
//
// JUCE UnitTest, category "Sonik". Uses a real (temp-file) TrackDatabase and the
// real StemSeparationManager dependency stack so the integration object is built
// exactly as production builds it; no audio thread is touched.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <sqlite3.h>

#include "Features/Daw/State/DawState.h"
#include "Features/Daw/State/DawClipModel.h"
#include "Features/Daw/Model/ChannelGroup.h"
#include "Features/Daw/Model/DawClip.h"
#include "Features/Daw/Session/SessionSchema.h"
#include "Features/Daw/Session/SourceIdResolver.h"
#include "Features/Daw/Session/SessionSourceResolution.h"
#include "Features/Daw/Session/Ui/UnresolvedSourcesDialog.h"
#include "Features/Daw/Playback/ArrangementSnapshot.h"
#include "Features/Daw/Playback/ArrangementCompiler.h"
#include "Features/Daw/Ui/Atoms/ClipBlock.h"
#include "Features/Daw/Transform/TimelineTransform.h"
#include "Features/Waveform/WaveformData.h"

#include "Features/Deck/Database/TrackDatabase.h"
#include "Features/Deck/DeckStateManager.h"
#include "Features/Deck/DeckIdentifiers.h"
#include "Features/StemSeparation/StemSeparationManager.h"
#include "Features/StemSeparation/ModelManager.h"
#include "Features/AudioEngine/AudioEngine.h"

using namespace Daw::Session;

//==============================================================================
namespace
{
    juce::ValueTree laneOf (juce::ValueTree daw, int deck, ChannelGroup::LaneKind k)
    {
        auto t = DawState::ensureTrackForDeck (daw, deck);
        return ChannelGroup::findLane (t.getChildWithName (DawIDs::lanes), k);
    }

    // Adds a clip to a lane; returns the clip node so a test can read its id.
    juce::ValueTree addClip (juce::ValueTree lane, const juce::String& sourceId,
                             std::int64_t tStart)
    {
        DawClip c;
        c.clipId              = juce::Uuid();
        c.laneId              = juce::Uuid (lane.getProperty (DawIDs::laneId).toString());
        c.sourceFileId        = sourceId;
        c.sourceStartSample   = 0;
        c.sourceEndSample     = 44100;
        c.timelineStartSample = tStart;
        c.sourceLengthSamples = 44100;
        auto node = DawClip::toValueTree (c);
        lane.getChildWithName (DawIDs::clips).addChild (node, -1, nullptr);
        return node;
    }

    // The full real dependency stack SessionSourceResolution binds to, built the
    // same way production (and StemSeparationUiTests) builds it. The DB is a temp
    // file we can seed with library_tracks rows to drive resolution / relocation.
    struct ResolutionFixture
    {
        juce::ValueTree root { ::IDs::SonikState };
        juce::File dbFile;
        std::unique_ptr<TrackDatabase>         db;
        std::unique_ptr<DeckStateManager>      deckState;
        std::unique_ptr<ModelManager>          modelManager;
        std::unique_ptr<AudioEngine>           engine;
        std::unique_ptr<StemSeparationManager> stemManager;

        ResolutionFixture()
        {
            dbFile       = juce::File::createTempFile ("sonik_srcres_test.db");
            db           = std::make_unique<TrackDatabase> (dbFile);
            deckState    = std::make_unique<DeckStateManager> (*db);
            modelManager = std::make_unique<ModelManager> (root);
            engine       = std::make_unique<AudioEngine> (root);
            stemManager  = std::make_unique<StemSeparationManager> (*deckState, *db,
                                                                    *modelManager, *engine);
        }

        ~ResolutionFixture()
        {
            stemManager.reset();
            engine.reset();
            modelManager.reset();
            deckState.reset();
            db.reset();
            dbFile.deleteFile();
        }

        // Insert a library_tracks row binding `contentHash` -> `filePath`.
        void insertTrack (const juce::String& contentHash, const juce::String& filePath)
        {
            auto* h = db->getDbHandle();
            sqlite3_stmt* stmt = nullptr;
            // date_added is NOT NULL with no default in the library_tracks schema,
            // so it must be supplied or the insert fails (and the row would never
            // be found by getFilePathForContentHash).
            const char* sql =
                "INSERT INTO library_tracks (file_path, content_hash, title, date_added) "
                "VALUES (?,?,?,?);";
            const int prc = sqlite3_prepare_v2 (h, sql, -1, &stmt, nullptr);
            jassert (prc == SQLITE_OK);
            juce::ignoreUnused (prc);
            if (stmt != nullptr)
            {
                sqlite3_bind_text  (stmt, 1, filePath.toRawUTF8(),    -1, SQLITE_TRANSIENT);
                sqlite3_bind_text  (stmt, 2, contentHash.toRawUTF8(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text  (stmt, 3, "t",                     -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64 (stmt, 4, (sqlite3_int64) juce::Time::currentTimeMillis());
                const int rc = sqlite3_step (stmt);
                jassert (rc == SQLITE_DONE);
                juce::ignoreUnused (rc);
                sqlite3_finalize (stmt);
            }
        }

        std::unique_ptr<SessionSourceResolution> make (juce::ValueTree dawBranch)
        {
            return std::make_unique<SessionSourceResolution> (dawBranch, *db, *stemManager);
        }
    };

    // Counts how many clips in a daw tree carry missingSource == true.
    int countFlaggedClips (const juce::ValueTree& daw)
    {
        int n = 0;
        auto tracks = daw.getChildWithName (DawIDs::tracks);
        for (int t = 0; t < tracks.getNumChildren(); ++t)
        {
            auto lanes = tracks.getChild (t).getChildWithName (DawIDs::lanes);
            for (int l = 0; l < lanes.getNumChildren(); ++l)
            {
                auto clips = lanes.getChild (l).getChildWithName (DawIDs::clips);
                for (int c = 0; c < clips.getNumChildren(); ++c)
                    if (static_cast<bool> (clips.getChild (c)
                            .getProperty (DawClipIDs::missingSource)))
                        ++n;
            }
        }
        return n;
    }

    // Total ClipEvents across all lanes in a compiled snapshot.
    int totalEvents (const Daw::ArrangementSnapshot& snap)
    {
        int n = 0;
        for (int l = 0; l < snap.laneCount; ++l)
            n += snap.lanes[l].count;
        return n;
    }

    // A timeline transform (120 BPM @ 44.1 kHz) for ClipBlock placement.
    Daw::TimelineTransform makeTransform()
    {
        Daw::TimelineTransform::GridSnapshot grid;
        grid.samplesPerBeat    = 22050.0;
        grid.phaseOriginSample = 0;
        const std::int64_t contentEnd = static_cast<std::int64_t> (1000.0 * 22050.0);
        return Daw::TimelineTransform (grid, 100.0, 0, 2000.0, contentEnd);
    }

    // Counts dark (ink) pixels in an offscreen render of a ClipBlock — the metric
    // that distinguishes a clean clip from a Glitch-treated one.
    int countInkPixels (const juce::Image& img)
    {
        int dark = 0;
        for (int y = 0; y < img.getHeight(); ++y)
            for (int x = 0; x < img.getWidth(); ++x)
            {
                const auto p = img.getPixelAt (x, y);
                if (p.getBrightness() < 0.4f && p.getAlpha() > 128)
                    ++dark;
            }
        return dark;
    }
}

//==============================================================================
class SourceResolutionIntegrationTests final : public juce::UnitTest
{
public:
    SourceResolutionIntegrationTests()
        : juce::UnitTest ("Source Resolution Integration (PRD-0097)", "Sonik") {}

    void runTest() override
    {
        testCompilerExcludesMissingFlaggedClips();
        testCompilerAdmitsClipAfterFlagCleared();
        testFlagAppliedPerSourceAcrossAllClips();
        testGatingQueries();
        testRelocateClearsFlagAndResolves();
        testSourceRefsBuilderRoundTrips();
        testClipBlockGlitchFlagDrivesRepaint();
        testUnresolvedDialogReachableSurface();
    }

private:
    //==========================================================================
    // 1. ArrangementCompiler resolved-only compile (§1.4) — highest value.
    //==========================================================================
    void testCompilerExcludesMissingFlaggedClips()
    {
        beginTest ("ArrangementCompiler excludes missingSource-flagged clips");

        juce::ValueTree root ("SonikState");
        auto daw  = DawState::getOrCreateDawBranch (root);
        auto lane = laneOf (daw, 0, ChannelGroup::LaneKind::Original);

        auto good1   = addClip (lane, "lib:ok-1", 0);
        auto missing = addClip (lane, "lib:gone", 100000);
        auto good2   = addClip (lane, "lib:ok-2", 200000);

        // Flag only the middle clip as missing.
        missing.setProperty (DawClipIDs::missingSource, true, nullptr);

        Daw::ArrangementCompiler compiler;
        Daw::ArrangementSnapshot snap;
        compiler.compile (daw, snap);

        expectEquals (totalEvents (snap), 2, "flagged clip must NOT enter the snapshot");

        // The two admitted events must be the unflagged sources, never the flagged one.
        const uint64_t goneHash = hashOf ("lib:gone");
        bool sawGone = false;
        for (int l = 0; l < snap.laneCount; ++l)
            for (int c = 0; c < snap.lanes[l].count; ++c)
                if (snap.lanes[l].events[c].sourceFileId == goneHash)
                    sawGone = true;
        expect (! sawGone, "the Missing source's clip is absent from every lane");
    }

    void testCompilerAdmitsClipAfterFlagCleared()
    {
        beginTest ("ArrangementCompiler re-admits a clip once its flag is cleared");

        juce::ValueTree root ("SonikState");
        auto daw  = DawState::getOrCreateDawBranch (root);
        auto lane = laneOf (daw, 0, ChannelGroup::LaneKind::Original);

        auto clip = addClip (lane, "lib:x", 0);
        clip.setProperty (DawClipIDs::missingSource, true, nullptr);

        Daw::ArrangementCompiler compiler;
        Daw::ArrangementSnapshot snap;

        compiler.compile (daw, snap);
        expectEquals (totalEvents (snap), 0, "flagged: excluded");

        clip.setProperty (DawClipIDs::missingSource, false, nullptr);
        compiler.compile (daw, snap);
        expectEquals (totalEvents (snap), 1, "cleared: admitted");
    }

    //==========================================================================
    // 2. Flag application per SOURCE, across every clip sharing the id (§1.5.6).
    //==========================================================================
    void testFlagAppliedPerSourceAcrossAllClips()
    {
        beginTest ("missingSource flag is applied to ALL clips of a Missing source");

        ResolutionFixture fx;

        auto daw  = DawState::getOrCreateDawBranch (fx.root);
        auto lane = laneOf (daw, 0, ChannelGroup::LaneKind::Original);

        // Three clips share one missing source; one clip uses a present source.
        fx.insertTrack ("present", juce::File::createTempFile ("present").getFullPathName());
        auto presentFile = fx.db->getFilePathForContentHash ("present");
        // Materialise the present file so File::existsAsFile() / DB lookup agree.
        juce::File (presentFile).create();

        addClip (lane, "missing", 0);
        addClip (lane, "missing", 100000);
        addClip (lane, "missing", 200000);
        addClip (lane, "present", 300000);

        auto res = fx.make (daw);
        res->runResolutionPass (juce::ValueTree (Daw::Session::IDs::SOURCE_REFS));

        // Exactly the three clips of the Missing source are flagged.
        expectEquals (countFlaggedClips (daw), 3,
                      "all three clips of the missing source flagged; the present one is not");

        // missingClipIds() reports exactly those three clip ids.
        expectEquals (res->missingClipIds().size(), 3);

        juce::File (presentFile).deleteFile();
    }

    //==========================================================================
    // 3. Gating queries (§1.5.7).
    //==========================================================================
    void testGatingQueries()
    {
        beginTest ("gating: false while any source Missing; counts track distinct sources");

        ResolutionFixture fx;
        auto daw  = DawState::getOrCreateDawBranch (fx.root);
        auto lane = laneOf (daw, 0, ChannelGroup::LaneKind::Original);

        // Two distinct missing sources (each referenced twice) -> count 2, not 4.
        addClip (lane, "gone-a", 0);
        addClip (lane, "gone-a", 50000);
        addClip (lane, "gone-b", 100000);
        addClip (lane, "gone-b", 150000);

        auto res = fx.make (daw);
        res->runResolutionPass (juce::ValueTree (Daw::Session::IDs::SOURCE_REFS));

        expect (! res->areAllSourcesResolved(), "blocked while sources missing");
        expectEquals (res->missingSourceCount(), 2, "two DISTINCT missing sources");
        expectEquals ((int) res->missingSources().size(), 2);
    }

    //==========================================================================
    // 4. Relocation through the integration object flips Missing -> Resolved,
    //    clears the flag on ALL referencing clips, and satisfies gating.
    //==========================================================================
    void testRelocateClearsFlagAndResolves()
    {
        beginTest ("relocateSource binds path, clears flag on all clips, ungates");

        ResolutionFixture fx;
        auto daw  = DawState::getOrCreateDawBranch (fx.root);
        auto lane = laneOf (daw, 0, ChannelGroup::LaneKind::Original);

        // One missing source shared by two clips. Seed a DB row whose path is
        // broken so the source resolves to Missing on the first pass.
        const juce::String hash = "relocate-me";
        fx.insertTrack (hash, "/nonexistent/old.wav");
        addClip (lane, hash, 0);
        addClip (lane, hash, 60000);

        auto res = fx.make (daw);
        res->runResolutionPass (juce::ValueTree (Daw::Session::IDs::SOURCE_REFS));

        expectEquals (countFlaggedClips (daw), 2, "both clips flagged before relocate");
        expect (! res->areAllSourcesResolved());

        // Relocate to a real file on disk.
        auto replacement = juce::File::createTempFile ("relocated.wav");
        replacement.create();

        bool dedupRejected = true; // must be cleared to false on success
        const bool ok = res->relocateSource (hash, replacement, &dedupRejected);

        expect (ok, "relocation succeeds");
        expect (! dedupRejected, "not a dedup rejection");
        expectEquals (countFlaggedClips (daw), 0, "flag cleared on every referencing clip");
        expect (res->areAllSourcesResolved(), "all sources resolved after the relocate");
        expectEquals (res->missingSourceCount(), 0);

        // A relocation to a non-existent file is rejected.
        expect (! res->relocateSource (hash, juce::File ("/nope/missing.wav")),
                "non-existent replacement rejected");

        replacement.deleteFile();
    }

    //==========================================================================
    // 5. SOURCE_REFS builder round-trip: build refs from the tree, then feed
    //    them back into a fresh resolution pass and confirm classification
    //    survives (one ref per distinct source, kind preserved).
    //==========================================================================
    void testSourceRefsBuilderRoundTrips()
    {
        beginTest ("buildSourceRefs emits one ref per distinct source and round-trips");

        ResolutionFixture fx;
        auto daw = DawState::getOrCreateDawBranch (fx.root);

        // A library source (known in DB) on an Original lane, and a stem source
        // on a Vocal lane.
        const juce::String libHash = "lib-known";
        fx.insertTrack (libHash, juce::File::createTempFile ("lib").getFullPathName());
        auto libFile = fx.db->getFilePathForContentHash (libHash);
        juce::File (libFile).create();

        auto orig  = laneOf (daw, 0, ChannelGroup::LaneKind::Original);
        addClip (orig, libHash, 0);
        addClip (orig, libHash, 100000); // duplicate -> still ONE ref

        auto vocal = laneOf (daw, 0, ChannelGroup::LaneKind::Vocal);
        addClip (vocal, "stem-parent", 0);

        auto res  = fx.make (daw);
        auto refs = res->buildSourceRefs();

        expect (refs.hasType (Daw::Session::IDs::SOURCE_REFS));
        expectEquals (refs.getNumChildren(), 2, "one ref per DISTINCT source id");

        // Locate each ref and confirm its kind classification.
        auto kindFor = [&] (const juce::String& id) -> juce::String
        {
            for (int i = 0; i < refs.getNumChildren(); ++i)
            {
                auto r = refs.getChild (i);
                if (r.getProperty (Daw::Session::IDs::sourceFileId).toString() == id)
                    return r.getProperty (Daw::Session::IDs::sourceKind).toString();
            }
            return {};
        };
        expectEquals (kindFor (libHash), juce::String (SourceKindStrings::kLibrary),
                      "DB-known source on an Original lane classifies as Library");
        expectEquals (kindFor ("stem-parent"), juce::String (SourceKindStrings::kStemCache),
                      "source on a stem lane classifies as StemCache");

        // Round-trip: feed the built refs back into a fresh pass over a fresh
        // resolution object. The library source resolves (file exists) and the
        // stem source is Missing (no cache artefact) -> one missing source.
        auto res2 = fx.make (daw);
        res2->runResolutionPass (refs);
        expectEquals (res2->missingSourceCount(), 1,
                      "after round-trip: only the stem source is unresolved");

        juce::File (libFile).deleteFile();
    }

    //==========================================================================
    // 6. ClipBlock Glitch: the missingSource flag drives the offscreen repaint.
    //==========================================================================
    void testClipBlockGlitchFlagDrivesRepaint()
    {
        beginTest ("ClipBlock renders MORE ink when missingSource flag is set (Glitch)");

        auto transform = makeTransform();

        DawClip c;
        c.clipId              = juce::Uuid();
        c.laneId              = juce::Uuid();
        c.sourceFileId        = "hashGlitch";
        c.sourceStartSample   = 0;
        c.sourceEndSample     = 88200;
        c.timelineStartSample = 22050;
        c.sourceLengthSamples = 88200;
        auto node = DawClip::toValueTree (c);

        // No waveform data (placeholder path) so the only difference between the
        // two renders is the Glitch overlay driven by the flag.
        Daw::ClipBlock block (node, transform,
                              [] (const juce::String&) { return WaveformData::Ptr(); });
        block.setBounds (0, 0, 200, 48);

        auto renderInk = [&]
        {
            juce::Image img (juce::Image::ARGB, 200, 48, true);
            juce::Graphics g (img);
            block.paintEntireComponent (g, false);
            return countInkPixels (img);
        };

        // Flag OFF.
        node.setProperty (DawClipIDs::missingSource, false, nullptr);
        const int inkOff = renderInk();

        // Flag ON.
        node.setProperty (DawClipIDs::missingSource, true, nullptr);
        const int inkOn = renderInk();

        expect (inkOn > inkOff,
                "the Glitch overlay (flag ON) lays down strictly more ink than OFF");
        // Sanity: the dense (~42%) glitch should be a large fraction of pixels.
        expect (inkOn > (200 * 48) / 5, "glitch overlay is visibly dense");
    }

    //==========================================================================
    // 7. UnresolvedSourcesDialog reachable surface: showUnresolvedSourcesStep.
    //==========================================================================
    void testUnresolvedDialogReachableSurface()
    {
        beginTest ("showUnresolvedSourcesStep: no-missing early-return fires onClosed");

        ResolutionFixture fx;
        auto daw = DawState::getOrCreateDawBranch (fx.root);
        // No clips at all -> zero missing sources.
        auto res = fx.make (daw);
        res->runResolutionPass (juce::ValueTree (Daw::Session::IDs::SOURCE_REFS));
        expectEquals (res->missingSourceCount(), 0);

        bool closed = false;
        Ui::showUnresolvedSourcesStep (nullptr, *res, [&] { closed = true; });
        expect (closed, "with nothing missing the step returns immediately and notifies");

        beginTest ("UnresolvedSourcesDialog: builds + paints a real dialog (rows) headlessly");

        // A session WITH missing sources. We build the SAME dialog class production
        // presents, but through the non-modal test seam (modal presentation does
        // grabKeyboardFocus + enterModalState, which need a real on-screen window /
        // run loop and is NOT headlessly automatable on macOS). The seam exercises
        // the dialog's full construction, per-source row layout, and paint surface.
        auto daw2  = DawState::getOrCreateDawBranch (fx.root);
        auto lane  = laneOf (daw2, 1, ChannelGroup::LaneKind::Original);
        addClip (lane, "missing-a", 0);
        addClip (lane, "missing-a", 50000);  // shared source -> one row, count 2
        addClip (lane, "missing-b", 100000); // a second distinct source -> one row

        auto res2 = fx.make (daw2);
        res2->runResolutionPass (juce::ValueTree (Daw::Session::IDs::SOURCE_REFS));
        expectEquals (res2->missingSourceCount(), 2, "two distinct missing sources -> two rows");

        {
            auto dialog = Ui::createUnresolvedSourcesStepForTest (*res2);
            dialog->setSize (600, 460);
            // Force a layout pass so the row sub-components are positioned.
            dialog->setBounds (dialog->getBounds());

            juce::Image img (juce::Image::ARGB, 600, 460, true);
            juce::Graphics g (img);
            dialog->paintEntireComponent (g, false);

            // The dialog paints ink (title, count line, rows, dithered shadow); a
            // blank render would mean nothing drew. This both proves it did not
            // crash and that the monochrome surface produced content.
            expect (countInkPixels (img) > 0, "the batch dialog rendered visible content");
        }
        // `dialog` is plain-owned (never modal, never on desktop): the unique_ptr
        // destructor deletes it directly with no async self-delete to drain.
    }

    //==========================================================================
    static uint64_t hashOf (const juce::String& s)
    {
        // Mirrors ArrangementCompiler's djb2-like sourceFileId hash so a test can
        // identify a specific source's ClipEvent.
        uint64_t h = 0;
        const auto* bytes = reinterpret_cast<const unsigned char*> (s.toRawUTF8());
        for (int i = 0; bytes[i] != 0; ++i)
            h = h * 31u + bytes[i];
        return h;
    }
};

static SourceResolutionIntegrationTests sourceResolutionIntegrationTests;
