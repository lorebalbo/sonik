//==============================================================================
// PRD-0069: Live Deck Projection Bridge tests.
//
// Drives the LiveProjectionTimer::processTick() body directly with a synthetic
// DeckProjectionSource (atomics set from the test thread) and a real
// MasterGridService, then asserts the resulting `daw` ValueTree mutations:
//   * clip creation on the correct lane(s)
//   * monotonic sourceEndSample growth
//   * finalisation on stop and on pause
//   * lane switching on source-mode change
//   * no clip when both stems are muted
//   * a new clip on a simulated seek discontinuity
//   * timelineStartSample anchored at the master grid phase + now-line advance
//
// No juce::Timer and no rendering are involved, so these tests are deterministic
// and graphics-context free.
//==============================================================================

#include <vector>

#include <juce_data_structures/juce_data_structures.h>

#include "Features/Daw/Projection/LiveProjectionTimer.h"
#include "Features/Daw/State/DawState.h"
#include "Features/Daw/Model/ChannelGroup.h"
#include "Features/Daw/Model/DawClip.h"
#include "Features/Daw/Model/MasterGridService.h"
#include "Features/Sync/MasterClockPublisher.h"
#include "Features/Deck/AudioThreadState.h"
#include "Features/Deck/DeckIdentifiers.h"

namespace
{

//------------------------------------------------------------------------------
// One synthetic deck: a DeckAudioState the test mutates plus a minimal deck
// ValueTree carrying the source mode + a TrackMetadata content hash.
//------------------------------------------------------------------------------
struct StubDeck
{
    DeckAudioState  audio;
    juce::ValueTree tree { IDs::Deck };

    StubDeck (const juce::String& id, const juce::String& hash)
    {
        tree.setProperty (IDs::id, id, nullptr);
        tree.setProperty (IDs::sourceMode, "original", nullptr);

        juce::ValueTree meta (IDs::TrackMetadata);
        meta.setProperty (IDs::contentHash, hash, nullptr);
        meta.setProperty (IDs::totalSamples, (juce::int64) 10'000'000, nullptr);
        tree.addChild (meta, -1, nullptr);
    }

    void setPlaying  (juce::int64 pos)
    {
        audio.playbackStatus.store ((int) PlaybackStatusCode::playing, std::memory_order_release);
        audio.playheadPosition.store (pos, std::memory_order_release);
    }
    void setStopped() { audio.playbackStatus.store ((int) PlaybackStatusCode::stopped, std::memory_order_release); }
    void setPaused()  { audio.playbackStatus.store ((int) PlaybackStatusCode::paused,  std::memory_order_release); }
    void setPos (juce::int64 pos) { audio.playheadPosition.store (pos, std::memory_order_release); }

    // Simulate the audio thread stamping an exact source-position discontinuity
    // (loop wrap / cue / jump) the instant before the next poll observes it.
    void publishDiscontinuity (juce::int64 from, juce::int64 to)
    {
        publishSeekDiscontinuity (audio, from, to);
    }

    void setStems (bool stems) { tree.setProperty (IDs::sourceMode, stems ? "stems" : "original", nullptr); }
    void muteVocals (bool m) { audio.stemVocalsMuted.store (m, std::memory_order_release); }
    void muteInstrumental (bool m)
    {
        audio.stemDrumsMuted.store (m, std::memory_order_release);
        audio.stemBassMuted .store (m, std::memory_order_release);
        audio.stemOtherMuted.store (m, std::memory_order_release);
    }
};

class StubSource final : public Daw::DeckProjectionSource
{
public:
    void add (StubDeck* d) { decks_.push_back (d); }

    int getNumDecks() const override { return (int) decks_.size(); }
    int getDeckIndex (int slot) const override { return slot; }
    DeckAudioState* getAudioState (int slot) override { return &decks_[(size_t) slot]->audio; }
    juce::ValueTree getDeckTree (int slot) const override { return decks_[(size_t) slot]->tree; }

private:
    std::vector<StubDeck*> decks_;
};

juce::ValueTree laneClips (juce::ValueTree dawBranch, int deckIndex, ChannelGroup::LaneKind kind)
{
    auto track = DawState::findTrackForDeck (dawBranch, deckIndex);
    auto lane  = ChannelGroup::findLane (track, kind);
    return lane.getChildWithName (DawIDs::clips);
}

int clipCount (juce::ValueTree dawBranch, int deckIndex, ChannelGroup::LaneKind kind)
{
    auto clips = laneClips (dawBranch, deckIndex, kind);
    return clips.isValid() ? clips.getNumChildren() : 0;
}

juce::ValueTree lastClip (juce::ValueTree dawBranch, int deckIndex, ChannelGroup::LaneKind kind)
{
    auto clips = laneClips (dawBranch, deckIndex, kind);
    return clips.getChild (clips.getNumChildren() - 1);
}

} // namespace

class LiveProjectionBridgeTests : public juce::UnitTest
{
public:
    LiveProjectionBridgeTests()
        : juce::UnitTest ("Live Projection Bridge (PRD-0069)", "Sonik") {}

    void runTest() override
    {
        using LaneKind = ChannelGroup::LaneKind;

        //----------------------------------------------------------------------
        beginTest ("Creates a clip on the Original lane while playing");
        {
            MasterClockPublisher publisher;
            publisher.publish ({ 120.0, 120.0, /*phase*/ 0, /*playing*/ true });
            Daw::MasterGridService grid (publisher, [] { return 44100.0; });

            juce::ValueTree root (IDs::SonikState);
            auto dawBranch = DawState::getOrCreateDawBranch (root);

            StubDeck deck ("A", "hash-A");
            StubSource source; source.add (&deck);
            Daw::LiveProjectionTimer bridge (source, dawBranch, grid);

            deck.setPlaying (1000);
            bridge.processTick();

            expectEquals (clipCount (dawBranch, 0, LaneKind::Original), 1);
            expectEquals (clipCount (dawBranch, 0, LaneKind::Instrumental), 0);
            expectEquals (clipCount (dawBranch, 0, LaneKind::Vocal), 0);

            auto clip = DawClip::fromValueTree (lastClip (dawBranch, 0, LaneKind::Original));
            expectEquals ((int) clip.sourceStartSample, 1000);
            expectEquals ((int) clip.sourceEndSample, 1000);
            expect (clip.sourceFileId == "hash-A");
        }

        //----------------------------------------------------------------------
        beginTest ("sourceEndSample grows monotonically to track the playhead");
        {
            MasterClockPublisher publisher;
            publisher.publish ({ 120.0, 120.0, 0, true });
            Daw::MasterGridService grid (publisher, [] { return 44100.0; });

            juce::ValueTree root (IDs::SonikState);
            auto dawBranch = DawState::getOrCreateDawBranch (root);

            StubDeck deck ("A", "hash-A");
            StubSource source; source.add (&deck);
            Daw::LiveProjectionTimer bridge (source, dawBranch, grid);

            deck.setPlaying (1000); bridge.processTick();
            deck.setPos (2000);     bridge.processTick();
            deck.setPos (3000);     bridge.processTick();

            expectEquals (clipCount (dawBranch, 0, LaneKind::Original), 1);
            auto clip = DawClip::fromValueTree (lastClip (dawBranch, 0, LaneKind::Original));
            expectEquals ((int) clip.sourceStartSample, 1000);
            expectEquals ((int) clip.sourceEndSample, 3000);
        }

        //----------------------------------------------------------------------
        beginTest ("Stop finalises the clip; next play starts a fresh clip");
        {
            MasterClockPublisher publisher;
            publisher.publish ({ 120.0, 120.0, 0, true });
            Daw::MasterGridService grid (publisher, [] { return 44100.0; });

            juce::ValueTree root (IDs::SonikState);
            auto dawBranch = DawState::getOrCreateDawBranch (root);

            StubDeck deck ("A", "hash-A");
            StubSource source; source.add (&deck);
            Daw::LiveProjectionTimer bridge (source, dawBranch, grid);

            deck.setPlaying (1000); bridge.processTick();
            deck.setPos (2000);     bridge.processTick();
            deck.setStopped();      bridge.processTick();

            // Still one (finalised) clip, frozen at 2000.
            expectEquals (clipCount (dawBranch, 0, LaneKind::Original), 1);
            auto frozen = DawClip::fromValueTree (lastClip (dawBranch, 0, LaneKind::Original));
            expectEquals ((int) frozen.sourceEndSample, 2000);

            deck.setPlaying (20000); bridge.processTick();
            expectEquals (clipCount (dawBranch, 0, LaneKind::Original), 2);
            auto fresh = DawClip::fromValueTree (lastClip (dawBranch, 0, LaneKind::Original));
            expectEquals ((int) fresh.sourceStartSample, 20000);
        }

        //----------------------------------------------------------------------
        beginTest ("Pause finalises the clip; resuming starts a fresh clip");
        {
            MasterClockPublisher publisher;
            publisher.publish ({ 120.0, 120.0, 0, true });
            Daw::MasterGridService grid (publisher, [] { return 44100.0; });

            juce::ValueTree root (IDs::SonikState);
            auto dawBranch = DawState::getOrCreateDawBranch (root);

            StubDeck deck ("A", "hash-A");
            StubSource source; source.add (&deck);
            Daw::LiveProjectionTimer bridge (source, dawBranch, grid);

            deck.setPlaying (1000); bridge.processTick();
            deck.setPos (4000);     bridge.processTick();
            deck.setPaused();       bridge.processTick();
            deck.setPlaying (4000); bridge.processTick();

            expectEquals (clipCount (dawBranch, 0, LaneKind::Original), 2);
        }

        //----------------------------------------------------------------------
        beginTest ("Source-mode change switches lanes (Original -> stems)");
        {
            MasterClockPublisher publisher;
            publisher.publish ({ 120.0, 120.0, 0, true });
            Daw::MasterGridService grid (publisher, [] { return 44100.0; });

            juce::ValueTree root (IDs::SonikState);
            auto dawBranch = DawState::getOrCreateDawBranch (root);

            StubDeck deck ("A", "hash-A");
            StubSource source; source.add (&deck);
            Daw::LiveProjectionTimer bridge (source, dawBranch, grid);

            deck.setPlaying (1000); bridge.processTick();
            expectEquals (clipCount (dawBranch, 0, LaneKind::Original), 1);

            deck.setStems (true);   // both stems audible
            deck.setPos (2000);     bridge.processTick();

            // Original finalised (still 1), both stem lanes now growing.
            expectEquals (clipCount (dawBranch, 0, LaneKind::Original), 1);
            expectEquals (clipCount (dawBranch, 0, LaneKind::Instrumental), 1);
            expectEquals (clipCount (dawBranch, 0, LaneKind::Vocal), 1);
        }

        //----------------------------------------------------------------------
        beginTest ("Both stems muted draws no clip; unmuting starts one");
        {
            MasterClockPublisher publisher;
            publisher.publish ({ 120.0, 120.0, 0, true });
            Daw::MasterGridService grid (publisher, [] { return 44100.0; });

            juce::ValueTree root (IDs::SonikState);
            auto dawBranch = DawState::getOrCreateDawBranch (root);

            StubDeck deck ("A", "hash-A");
            StubSource source; source.add (&deck);
            Daw::LiveProjectionTimer bridge (source, dawBranch, grid);

            deck.setStems (true);
            deck.muteVocals (true);
            deck.muteInstrumental (true);
            deck.setPlaying (1000); bridge.processTick();

            expectEquals (clipCount (dawBranch, 0, LaneKind::Instrumental), 0);
            expectEquals (clipCount (dawBranch, 0, LaneKind::Vocal), 0);

            deck.muteVocals (false);     // vocal becomes audible
            deck.setPos (2000); bridge.processTick();
            expectEquals (clipCount (dawBranch, 0, LaneKind::Vocal), 1);
            expectEquals (clipCount (dawBranch, 0, LaneKind::Instrumental), 0);
        }

        //----------------------------------------------------------------------
        beginTest ("Forward seek discontinuity finalises and starts a new clip");
        {
            MasterClockPublisher publisher;
            publisher.publish ({ 120.0, 120.0, 0, true });
            Daw::MasterGridService grid (publisher, [] { return 44100.0; });

            juce::ValueTree root (IDs::SonikState);
            auto dawBranch = DawState::getOrCreateDawBranch (root);

            StubDeck deck ("A", "hash-A");
            StubSource source; source.add (&deck);
            Daw::LiveProjectionTimer bridge (source, dawBranch, grid);

            deck.setPlaying (1000); bridge.processTick();
            deck.setPos (2000);     bridge.processTick();
            // Jump far forward (beyond tolerance) -> a seek.
            const auto jumpPos = (juce::int64) 2000
                               + Daw::LiveProjectionTimer::kSeekToleranceSamples + 50000;
            deck.setPos (jumpPos);
            bridge.processTick();

            expectEquals (clipCount (dawBranch, 0, LaneKind::Original), 2);
            auto fresh = DawClip::fromValueTree (lastClip (dawBranch, 0, LaneKind::Original));
            expectEquals ((int) fresh.sourceStartSample, (int) jumpPos);
        }

        //----------------------------------------------------------------------
        beginTest ("Backward seek finalises and starts a new clip");
        {
            MasterClockPublisher publisher;
            publisher.publish ({ 120.0, 120.0, 0, true });
            Daw::MasterGridService grid (publisher, [] { return 44100.0; });

            juce::ValueTree root (IDs::SonikState);
            auto dawBranch = DawState::getOrCreateDawBranch (root);

            StubDeck deck ("A", "hash-A");
            StubSource source; source.add (&deck);
            Daw::LiveProjectionTimer bridge (source, dawBranch, grid);

            deck.setPlaying (50000); bridge.processTick();
            deck.setPos (50500);     bridge.processTick();
            deck.setPos (1000);      bridge.processTick();   // jump backwards

            expectEquals (clipCount (dawBranch, 0, LaneKind::Original), 2);
            auto fresh = DawClip::fromValueTree (lastClip (dawBranch, 0, LaneKind::Original));
            expectEquals ((int) fresh.sourceStartSample, 1000);
        }

        //----------------------------------------------------------------------
        // EPIC-0009: an EXACT loop wrap published by the audio thread must
        // capture the FULL audio up to the loop-out and resume at the loop-in,
        // with a gapless (contiguous) timeline seam — unlike the polled
        // approximation that drops the tail/head and leaves a gap.
        beginTest ("Loop wrap captures full audio with a contiguous, gapless seam");
        {
            MasterClockPublisher publisher;
            publisher.publish ({ 120.0, 120.0, /*phase*/ 0, true });
            Daw::MasterGridService grid (publisher, [] { return 44100.0; });

            juce::ValueTree root (IDs::SonikState);
            auto dawBranch = DawState::getOrCreateDawBranch (root);

            StubDeck deck ("A", "hash-A");
            StubSource source; source.add (&deck);
            Daw::LiveProjectionTimer bridge (source, dawBranch, grid);

            // Loop region [1000, 2000). Open the clip, poll once mid-loop at 1500.
            deck.setPlaying (1000); bridge.processTick();
            deck.setPos (1500);     bridge.processTick();

            // Audio thread wraps: leaves lpOut=2000, resumes lpIn=1000; the next
            // poll observes 1200 (well past the wrap the poller never saw).
            deck.publishDiscontinuity (2000, 1000);
            deck.setPos (1200);     bridge.processTick();

            auto clips = laneClips (dawBranch, 0, LaneKind::Original);
            expectEquals (clips.getNumChildren(), 2);

            auto c0 = DawClip::fromValueTree (clips.getChild (0));
            auto c1 = DawClip::fromValueTree (clips.getChild (1));

            // clip0 captured the FULL loop up to the exact loop-out (not 1500).
            expectEquals ((int) c0.sourceStartSample, 1000);
            expectEquals ((int) c0.sourceEndSample,   2000,
                          "outgoing clip must reach the exact loop-out (no lost tail)");

            // clip1 resumes at the exact loop-in (not the 1200 poll).
            expectEquals ((int) c1.sourceStartSample, 1000,
                          "incoming clip must start at the exact loop-in (no lost head)");

            // The seam is gapless: clip1 begins exactly where clip0 ends.
            const int c0End = (int) (c0.timelineStartSample
                                     + (c0.sourceEndSample - c0.sourceStartSample));
            expectEquals ((int) c1.timelineStartSample, c0End,
                          "loop seam must be contiguous (no silent gap)");
        }

        //----------------------------------------------------------------------
        beginTest ("timelineStartSample anchors at grid phase; now-line advances");
        {
            MasterClockPublisher publisher;
            publisher.publish ({ 120.0, 120.0, /*phase*/ 8000, true });
            Daw::MasterGridService grid (publisher, [] { return 44100.0; });

            juce::ValueTree root (IDs::SonikState);
            auto dawBranch = DawState::getOrCreateDawBranch (root);

            StubDeck deck ("A", "hash-A");
            StubSource source; source.add (&deck);
            Daw::LiveProjectionTimer bridge (source, dawBranch, grid);

            expectEquals ((int) bridge.getNowLineSample(), 8000);

            deck.setPlaying (1000); bridge.processTick();
            auto clip = DawClip::fromValueTree (lastClip (dawBranch, 0, LaneKind::Original));
            expectEquals ((int) clip.timelineStartSample, 8000);

            deck.setPos (3000); bridge.processTick();   // +2000 progress
            expectEquals ((int) bridge.getNowLineSample(), 10000);
        }
    }
};

static LiveProjectionBridgeTests liveProjectionBridgeTests;
