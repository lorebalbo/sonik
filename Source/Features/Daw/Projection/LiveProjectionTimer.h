#pragma once
//==============================================================================
// PRD-0069: Live Deck Projection Bridge.
//
// A MESSAGE-THREAD-ONLY juce::Timer that, at UI refresh rate, reads (never
// writes) each deck's audio-thread-published atomics and grows a "live clip" on
// the matching lane(s) of the deck's `daw` sub-tree, sample-for-sample, so the
// arrangement builds itself as the deck plays.
//
// THREADING / CLAUDE.md compliance:
//   * It only LOADS deck atomics (DeckAudioState) with acquire ordering.
//   * It only MUTATES the `daw` ValueTree, on the message thread.
//   * No audio-thread code, no allocation/lock/IO on the audio thread.
//   * No singletons; all dependencies injected by reference.
//
// The deck side is reached through the DeckProjectionSource abstraction so the
// bridge is fully testable with a synthetic deck source (no DeckStateManager /
// TrackDatabase required in tests). The per-deck-group projection state is plain
// message-thread state, NOT stored in the ValueTree (PRD-0069 §1.3.3).
//==============================================================================

#include <array>
#include <map>

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include "SourceModeReader.h"
#include "../Model/MasterGridService.h"
#include "../Model/ChannelGroup.h"
#include "../State/DawState.h"

namespace Daw
{

//------------------------------------------------------------------------------
// Read-only view onto the set of decks the bridge projects. Implemented by an
// adapter over DeckStateManager in production; stubbed in tests. `slot` is the
// 0-based position of the deck; the returned deck index keys the `daw` track.
//------------------------------------------------------------------------------
struct DeckProjectionSource
{
    virtual ~DeckProjectionSource() = default;

    virtual int            getNumDecks()             const = 0;
    virtual int            getDeckIndex (int slot)   const = 0;
    virtual DeckAudioState* getAudioState (int slot)       = 0; // bridge only reads it
    virtual juce::ValueTree getDeckTree  (int slot)  const = 0; // source mode + hash
};

class LiveProjectionTimer final : public juce::Timer
{
public:
    //--------------------------------------------------------------------------
    // Refresh cadence (PRD-0069 §1.5.1): 60 Hz default, hard floor 30 Hz. The
    // bridge's correctness does NOT depend on the rate — sourceEndSample is
    // always set to the actual observed playheadPosition, never accumulated.
    //--------------------------------------------------------------------------
    static constexpr int kRefreshHz = 60;

    //--------------------------------------------------------------------------
    // Forward source-position jump (samples) beyond which a continuing play is
    // treated as a seek/cue (PRD-0069 §1.5.3). ~80 ms at the project rate, well
    // above one tick of normal advance at any sane deck speed.
    //--------------------------------------------------------------------------
    static const std::int64_t kSeekToleranceSamples;

    LiveProjectionTimer (DeckProjectionSource& decks,
                         juce::ValueTree       dawBranch,
                         MasterGridService&    grid);
    ~LiveProjectionTimer() override;

    LiveProjectionTimer (const LiveProjectionTimer&)            = delete;
    LiveProjectionTimer& operator= (const LiveProjectionTimer&) = delete;

    // RAII start/stop of the message-thread timer.
    void start();
    void stop();

    void timerCallback() override { processTick(); }

    // Factored tick body (no juce::Timer dependency) so tests can drive it.
    void processTick();

    // The DAW's live timeline position (PRD-0070 now-line). Message thread only.
    std::int64_t getNowLineSample() const noexcept { return nowLineSample_; }

    // Reset the DAW timeline to the master-grid phase origin (beat 1 of bar 1).
    // Call before each new recording session so the first captured clip always
    // lands at beat 1 of the DAW timeline, regardless of how long decks played
    // before Record was pressed.
    void resetTimeline()
    {
        nowLineSample_   = grid_.snapshotGrid().phaseOriginSample;
        sessionAnchored_ = false; // re-anchor to the grid on the take's first clip
    }

    // Gate clip writing behind an external predicate. When the provider returns
    // false (recording not active) no clips are started or grown; any open lanes
    // are finalised. nowLineSample_ always advances regardless.
    // Call from the message thread before start().
    void setCapturingProvider (std::function<bool()> provider)
    {
        capturingProvider_ = std::move (provider);
    }

private:
    enum Lane { kOriginal = 0, kInstrumental = 1, kVocal = 2, kLaneCount = 3 };

    struct LaneProjection
    {
        juce::ValueTree clipNode;        // current live clip; invalid when none
        bool            active = false;  // is a live clip currently growing here
    };

    struct DeckProjection
    {
        bool                          wasPlaying    = false;
        std::int64_t                  lastSourcePos = 0;
        std::uint64_t                 lastSeekSeq   = 0; // last consumed discontinuity seq
        bool                          lastKeyLock   = false; // for key-lock-toggle splits
        std::array<LaneProjection, kLaneCount> lanes;
    };

    static ChannelGroup::LaneKind laneKindFor (Lane lane);

    // Opens a fresh live clip on `lane` anchored at `timelineStart` (the now-line
    // for an ordinary open, or the just-closed clip's exact timeline end for a
    // discontinuity split, so the seam is gapless).
    void startLane   (DeckProjection&, int deckIndex, Lane,
                      std::int64_t srcPos, std::int64_t timelineStart,
                      const juce::String& sourceFileId,
                      std::int64_t sourceLength,
                      double sourceBpm,
                      bool keyLock);

    // Timeline end (exclusive) a lane's current clip has grown to, ==
    // timelineStart + STRETCHED source span (span * stretchRatio). Used to
    // butt-join a split at the musical (stretched) end. ratio 1.0 => 1:1.
    static std::int64_t laneTimelineEnd (const LaneProjection&, double stretchRatio);
    void growLane    (LaneProjection&, std::int64_t srcPos);
    void finaliseLane (LaneProjection&);

    static juce::String readContentHash (const juce::ValueTree& deckTree);
    static std::int64_t readSourceLength (const juce::ValueTree& deckTree);

    // Snap a fresh clip's timeline start so the deck's first captured downbeat
    // lands exactly on a master-grid beat line. Pure: the deck's source beatgrid
    // (anchor + interval) positions the downbeat, then it is snapped against the
    // master grid (origin + samples-per-beat) and the start is backed out so the
    // whole clip aligns. `stretchRatio` (timeline/source = sourceBpm/masterBpm)
    // maps the source-domain downbeat offset into the stretched timeline so a
    // SYNC'd deck snaps correctly too (1.0 => 1:1). No usable beatgrid =>
    // rawTimelineStart.
    static std::int64_t snapStartToGrid (std::int64_t  beatgridAnchor,
                                         double        beatgridInterval,
                                         std::int64_t  sourceStart,
                                         std::int64_t  rawTimelineStart,
                                         double        stretchRatio,
                                         const MasterGridService::GridContext& gridCtx);

    DeckProjectionSource& decks_;
    juce::ValueTree       dawBranch_;
    MasterGridService&    grid_;

    std::map<int, DeckProjection> projection_;   // keyed by deck index
    std::int64_t                  nowLineSample_ = 0;
    std::function<bool()>         capturingProvider_;
    // False until the first clip of a take is opened; on that edge the now-line
    // is re-anchored to the live grid origin so the record cursor sits on a beat
    // (the origin published while dormant is 0 and would place it off-grid).
    bool                          sessionAnchored_ = false;
};

} // namespace Daw
