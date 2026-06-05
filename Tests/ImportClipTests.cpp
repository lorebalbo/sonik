//==============================================================================
// PRD-0098: ImportClipTests — sample-rate reconciliation, content-hash source
// de-duplication + ref counting, crop-window initialisation to the full file,
// grid snapping of the drop position, single undoable transaction, and
// sequential multi-file placement. JUCE UnitTest, category "Sonik". Headless;
// no decoding, no audio thread.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "Features/Daw/State/DawState.h"
#include "Features/Daw/Model/ChannelGroup.h"
#include "Features/Daw/Model/DawClip.h"
#include "Features/Daw/Import/ImportSource.h"
#include "Features/Daw/Import/ImportClipPlacer.h"

using namespace Daw::Import;

class ImportClipTests final : public juce::UnitTest
{
public:
    ImportClipTests() : juce::UnitTest ("Import Clip", "Sonik") {}

    void runTest() override
    {
        testSampleRateReconciliation();
        testRegistryDedupAndRefCount();
        testPlaceClipCropAndSnap();
        testSingleUndoableTransaction();
        testSequentialMultiFile();
        testMultiFileAppendsPastExisting();
    }

private:
    static ImportedSourceDescriptor makeSource (const juce::String& hash,
                                                std::int64_t sessionLen,
                                                double nativeRate = 44100.0)
    {
        ImportedSourceDescriptor d;
        d.contentHash          = hash;
        d.sourceFileId         = ImportedSourceDescriptor::idForHash (hash);
        d.lastKnownPath        = "/music/" + hash + ".wav";
        d.displayName          = hash + ".wav";
        d.nativeSampleRate     = nativeRate;
        d.nativeChannelCount   = 2;
        d.nativeLengthSamples  = sessionLen;
        d.sessionLengthSamples = sessionLen;
        return d;
    }

    static juce::ValueTree lane0 (juce::ValueTree daw)
    {
        auto t = DawState::ensureTrackForDeck (daw, 0);
        return ChannelGroup::findLane (t.getChildWithName (DawIDs::lanes),
                                       ChannelGroup::LaneKind::Original);
    }

    //==========================================================================
    void testSampleRateReconciliation()
    {
        beginTest ("a 96 kHz file on a 44.1 kHz session yields the correct length");

        // 1 second at 96 kHz -> 44100 samples at the session rate.
        expectEquals ((int) reconcileLengthToSessionRate (96000, 96000.0, 44100.0), 44100);
        // Same rate is identity.
        expectEquals ((int) reconcileLengthToSessionRate (44100, 44100.0, 44100.0), 44100);
        // 22.05 kHz upsampled to 44.1 kHz doubles the sample count.
        expectEquals ((int) reconcileLengthToSessionRate (22050, 22050.0, 44100.0), 44100);
        // Guard: zero/invalid native rate returns the input unchanged.
        expectEquals ((int) reconcileLengthToSessionRate (1234, 0.0, 44100.0), 1234);
    }

    //==========================================================================
    void testRegistryDedupAndRefCount()
    {
        beginTest ("identical files collapse to one source; ref counts track clips");

        ImportSourceRegistry reg;
        auto id1 = reg.registerSource (makeSource ("HASH_A", 44100));
        auto id2 = reg.registerSource (makeSource ("HASH_A", 44100));   // identical
        expectEquals (id1, id2, "same content hash -> same source id");
        expectEquals (reg.size(), 1, "de-duplicated to one entry");

        // Two clips reference it; one undo releases one reference.
        reg.acquire (id1);
        reg.acquire (id1);
        expectEquals (reg.refCount (id1), 2);

        reg.release (id1);   // undo of one import
        expectEquals (reg.refCount (id1), 1, "still referenced by the other clip");
        expect (reg.contains (id1), "source NOT evicted while referenced");

        // A redo reattaches to the existing descriptor without re-registering.
        auto found = reg.find (id1);
        expect (found.has_value(), "descriptor retained for redo");
        expectEquals (found->sessionLengthSamples, (std::int64_t) 44100);

        reg.release (id1);
        expectEquals (reg.refCount (id1), 0);
        expect (reg.contains (id1), "retained at zero refs until explicit eviction");
        expectEquals (reg.evictUnreferenced(), 1, "explicit eviction reclaims it");
        expect (! reg.contains (id1));
    }

    //==========================================================================
    void testPlaceClipCropAndSnap()
    {
        beginTest ("placed clip spans the whole file and snaps to the grid");

        auto daw = DawState::createDawBranch();
        auto lane = lane0 (daw);
        juce::UndoManager undo;
        ImportSourceRegistry reg;

        auto src = makeSource ("HASH_B", 44100);
        reg.registerSource (src);

        ImportClipPlacer placer (daw, undo, reg);

        // Snap rounds down to a 4000-sample grid: 10000 -> 8000.
        auto snap = [] (std::int64_t s) { return (s / 4000) * 4000; };
        auto node = placer.placeClip (lane, src, 10000, snap);

        expect (node.isValid(), "a clip was created");
        auto clip = DawClip::fromValueTree (node);
        expectEquals ((int) clip.sourceStartSample, 0, "crop starts at 0");
        expectEquals ((int) clip.sourceEndSample, 44100, "crop ends at full file");
        expectEquals ((int) clip.sourceLengthSamples, 44100, "source length = full file");
        expectEquals ((int) clip.timelineStartSample, 8000, "drop snapped to grid");
        expectEquals (clip.sourceFileId, src.sourceFileId, "references source by id");
        expectEquals (reg.refCount (src.sourceFileId), 1, "placement acquired the source");

        // A degenerate (zero-length) source creates no clip.
        auto empty = makeSource ("HASH_EMPTY", 0);
        expect (! placer.placeClip (lane, empty, 0, snap).isValid(), "no clip for empty source");
    }

    //==========================================================================
    void testSingleUndoableTransaction()
    {
        beginTest ("one import is a single undoable transaction");

        auto daw = DawState::createDawBranch();
        auto lane = lane0 (daw);
        juce::UndoManager undo;
        ImportSourceRegistry reg;
        auto src = makeSource ("HASH_C", 22050);
        reg.registerSource (src);

        ImportClipPlacer placer (daw, undo, reg);
        placer.placeClip (lane, src, 0, nullptr);

        auto clips = lane.getChildWithName (DawIDs::clips);
        expectEquals (clips.getNumChildren(), 1, "clip present");
        expect (undo.canUndo(), "an undo action is available");

        expect (undo.undo(), "undo");
        expectEquals (clips.getNumChildren(), 0, "single undo removes the import");
        expect (! undo.canUndo(), "back to the empty baseline in one step");

        expect (undo.redo(), "redo");
        expectEquals (clips.getNumChildren(), 1, "redo restores the clip");
    }

    //==========================================================================
    void testSequentialMultiFile()
    {
        beginTest ("multiple files place as sequential, non-overlapping clips");

        auto daw = DawState::createDawBranch();
        auto lane = lane0 (daw);
        juce::UndoManager undo;
        ImportSourceRegistry reg;

        std::vector<ImportedSourceDescriptor> sources {
            makeSource ("M1", 10000),
            makeSource ("M2", 20000),
            makeSource ("M3", 5000)
        };
        for (auto& s : sources) reg.registerSource (s);

        ImportClipPlacer placer (daw, undo, reg);
        auto placed = placer.placeSequential (lane, sources, 1000, nullptr);

        expectEquals ((int) placed.size(), 3, "three clips placed");

        auto c0 = DawClip::fromValueTree (placed[0]);
        auto c1 = DawClip::fromValueTree (placed[1]);
        auto c2 = DawClip::fromValueTree (placed[2]);
        expectEquals ((int) c0.timelineStartSample, 1000, "first at drop");
        expectEquals ((int) c1.timelineStartSample, 1000 + 10000, "second after first");
        expectEquals ((int) c2.timelineStartSample, 1000 + 10000 + 20000, "third after second");

        // Whole multi-file drop is one undo step.
        expect (undo.canUndo(), "batch produced an undo action");
        undo.undo();
        expectEquals (lane.getChildWithName (DawIDs::clips).getNumChildren(), 0,
                      "one undo removes all imported clips");
        expect (! undo.canUndo(), "the entire batch was a single transaction");
    }

    //==========================================================================
    void testMultiFileAppendsPastExisting()
    {
        beginTest ("a drop overlapping existing clips appends after the last clip");

        auto daw = DawState::createDawBranch();
        auto lane = lane0 (daw);
        juce::UndoManager undo;
        ImportSourceRegistry reg;

        // Seed an existing clip occupying [0, 50000).
        auto seed = makeSource ("SEED", 50000);
        reg.registerSource (seed);
        ImportClipPlacer placer (daw, undo, reg);
        placer.placeClip (lane, seed, 0, nullptr);
        expectEquals ((int) ImportClipPlacer::laneEndSample (lane), 50000, "lane end tracked");

        // Drop a new batch at 0 (would overlap) -> appended after 50000.
        std::vector<ImportedSourceDescriptor> more { makeSource ("N1", 8000) };
        reg.registerSource (more[0]);
        auto placed = placer.placeSequential (lane, more, 0, nullptr);

        expectEquals ((int) placed.size(), 1);
        expectEquals ((int) DawClip::fromValueTree (placed[0]).timelineStartSample, 50000,
                      "appended past existing material");
    }
};

static ImportClipTests importClipTests;
