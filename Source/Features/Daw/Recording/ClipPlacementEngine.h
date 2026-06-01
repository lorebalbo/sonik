#pragma once
//==============================================================================
// PRD-0073: Clip Placement Engine & Clip Lifecycle.
//
// Consumes drained PRD-0072 performance events on the message thread and turns
// them into persisted DawClip nodes in the `daw` ValueTree, implementing the
// open -> grow -> close lifecycle. Message-thread ONLY; reads deck source
// positions through an injected provider and never blocks the audio thread.
//
// SCOPE: lifecycle + persistence. This engine does NOT own the alignment math
// (PRD-0074 — invoked through a seam, pass-through default here) nor the
// semantics of any specific jump/loop/source-mode event (PRD-0075/0076/0077,
// which layer on top of the open/close primitives defined here).
//==============================================================================

#include <juce_data_structures/juce_data_structures.h>

#include "PerformanceEventFifo.h"
#include "AlignmentResolver.h"

#include "../State/DawState.h"
#include "../State/DawClipModel.h"
#include "../Model/DawClip.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Daw
{

//==============================================================================
// Provider of the live, message-thread-visible deck source state. The engine
// reads the current source position (for grow), and the source file id / total
// length (for clip descriptors). No audio-thread access.
struct DeckSourceProvider
{
    virtual ~DeckSourceProvider() = default;
    virtual std::int64_t getSourceSample (int deckIndex) const = 0;
    virtual juce::String getSourceFileId (int deckIndex) const = 0;
    virtual std::int64_t getSourceLength (int deckIndex) const = 0;
};

//==============================================================================
// Resolves the lane id(s) a deck currently writes to, given its active source
// mode (Original -> one lane; stems -> the active stem lanes). Treated as
// opaque by this engine; PRD-0077 owns source-mode transition semantics.
struct LaneResolver
{
    virtual ~LaneResolver() = default;
    virtual std::vector<juce::Uuid> resolveLanes (int deckIndex) const = 0;
};

//==============================================================================
// Alignment seam (PRD-0074). Returns the timeline start sample AND the
// AlignmentMode for a *first* (non-contiguous) open. PRD-0074 supplies the real
// grid-snapping rule via ResolverAlignmentSeam; the default below is a
// pass-through that returns the raw playhead so the engine is correct and
// testable before alignment is wired.
struct ClipAlignmentSeam
{
    virtual ~ClipAlignmentSeam() = default;
    virtual AlignmentResult resolveOpen (int          deckIndex,
                                         std::int64_t rawPlayhead,
                                         std::int64_t rawSourceStart) const = 0;
};

struct PassThroughAlignment final : public ClipAlignmentSeam
{
    AlignmentResult resolveOpen (int, std::int64_t rawPlayhead, std::int64_t) const override
    {
        return { rawPlayhead, AlignmentMode::FirstBeatAnchored };
    }
};

// Production seam: wraps the pure PRD-0074 AlignmentResolver and a supplier that
// assembles the full AlignmentInputs for a deck at open (wired in PRD-0078).
struct ResolverAlignmentSeam final : public ClipAlignmentSeam
{
    ResolverAlignmentSeam (AlignmentResolver resolver,
                           std::function<AlignmentInputs (int, std::int64_t, std::int64_t)> inputs)
        : resolver_ (resolver), inputs_ (std::move (inputs)) {}

    AlignmentResult resolveOpen (int deckIndex, std::int64_t rawPlayhead, std::int64_t rawSourceStart) const override
    {
        return resolver_.resolve (inputs_ (deckIndex, rawPlayhead, rawSourceStart));
    }

    AlignmentResolver                                                  resolver_;
    std::function<AlignmentInputs (int, std::int64_t, std::int64_t)>   inputs_;
};

//==============================================================================
class ClipPlacementEngine final : public PerformanceEventHandler
{
public:
    // Explicit dependency injection, no singletons. `alignment` defaults to an
    // internal pass-through resolver when null.
    ClipPlacementEngine (juce::ValueTree                dawBranch,
                         DeckSourceProvider&            sourceProvider,
                         LaneResolver&                  laneResolver,
                         std::function<std::int64_t()>  playheadProvider,
                         ClipAlignmentSeam*             alignment = nullptr);

    ~ClipPlacementEngine() override = default;

    ClipPlacementEngine (const ClipPlacementEngine&)            = delete;
    ClipPlacementEngine& operator= (const ClipPlacementEngine&) = delete;

    //--------------------------------------------------------------------------
    // PRD-0072 drain entry point. Switches on the event type and applies the
    // open/grow/close lifecycle. Message thread only.
    void onEvent (const PerformanceEvent& event);

    // PerformanceEventHandler — the FIFO drains directly into the engine.
    void onPerformanceEvent (const PerformanceEvent& event) override { onEvent (event); }

    //--------------------------------------------------------------------------
    // Advances every open clip's sourceEndSample to its deck's current source
    // position (monotonic; never moves backwards). Called from PRD-0071's
    // projection cadence. Message thread only.
    void grow();

    // PRD-0071 stop: closes every still-open clip at each deck's current source
    // position, discards zero/negative-length clips, and clears all slots so the
    // engine is reusable for the next recording.
    void finaliseAll (std::int64_t stopPlayhead);

    //--------------------------------------------------------------------------
    // Test/diagnostic accessors.
    int numOpenSlots() const noexcept { return static_cast<int> (slots_.size()); }

    // PRD-0075: a source discontinuity at or below this magnitude is treated as
    // a normal per-projection advance and does NOT split the clip. Sized to the
    // 60 Hz projection block (~735 samples @ 44.1 kHz) plus margin (§1.5.1).
    static constexpr std::int64_t kSourceSplitThresholdSamples = 1024;

    // PRD-0076: true while the deck is in loop-capture mode (its ordinary grow
    // path is suspended and each pass is finalised as its own clip).
    bool isLooping (int deckIndex) const;

private:
    using SlotKey = std::pair<int, std::string>; // (deckIndex, laneId string)

    struct OpenSlot
    {
        juce::ValueTree clipNode;
        std::int64_t    sourceStart   = 0;
        std::int64_t    timelineStart = 0;
    };

    // Lifecycle primitives.
    void openOnLanes  (int deckIndex, std::int64_t sourceStart);
    void openOne      (int deckIndex, const juce::Uuid& laneId, std::int64_t sourceStart);
    void closeDeck    (int deckIndex, std::int64_t sourceEnd, bool continuation);
    void closeSlot    (const SlotKey& key, OpenSlot& slot, std::int64_t sourceEnd, bool continuation);

    // PRD-0077: close the deck's outgoing lanes and open its incoming lane set
    // (post-change mapping) at the live playhead. Reacts only for decks that are
    // actively recording; an empty incoming set is a silence gap (§1.5.6).
    void applySourceModeChange (int deckIndex, std::int64_t sourcePosition);
    void clearContinuationForDeck (int deckIndex);

    // PRD-0075: split the open clip(s) on a deck at a source discontinuity.
    void applyJump    (int deckIndex, std::int64_t jumpOutSource, std::int64_t jumpInSource);
    bool anyOpenForDeck (int deckIndex) const;

    // PRD-0076: loop-capture lifecycle.
    void enterLoop    (int deckIndex, std::int64_t engageSource);
    void applyLoopPass (int deckIndex, std::int64_t loopIn, std::int64_t loopOut);
    void exitLoop     (int deckIndex, std::int64_t exitSource, std::int64_t loopIn);
    // Writes one already-finalised clip [sourceStart, sourceEnd) on a lane,
    // placed contiguously after the prior segment (or aligned if it is first).
    void emitFinalizedClip (int deckIndex, const juce::Uuid& laneId,
                            std::int64_t sourceStart, std::int64_t sourceEnd);

    juce::ValueTree laneNodeFor (int deckIndex, const juce::Uuid& laneId);

    static std::int64_t readInt64 (const juce::ValueTree& node, const juce::Identifier& id)
    {
        return static_cast<std::int64_t> (node.getProperty (id));
    }

    juce::ValueTree                dawBranch_;
    DeckSourceProvider&            sourceProvider_;
    LaneResolver&                  laneResolver_;
    std::function<std::int64_t()>  playheadProvider_;

    PassThroughAlignment           defaultAlignment_;
    ClipAlignmentSeam&             alignment_;

    std::map<SlotKey, OpenSlot>      slots_;
    std::map<SlotKey, std::int64_t>  pendingContinuation_; // contiguous next-open timeline start

    // PRD-0076: lanes captured at loop-engage, per deck currently looping.
    std::map<int, std::vector<juce::Uuid>>  loopLanesByDeck_;

    // PRD-0077: decks that are actively recording (playing + channel-unmuted).
    // A source-mode change only re-targets lanes for a deck in this set, even
    // when it currently has no open clip (both stems muted = silence gap).
    std::set<int>  recordingDecks_;
};

} // namespace Daw
