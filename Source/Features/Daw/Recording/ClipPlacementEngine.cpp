#include "ClipPlacementEngine.h"

#include <cstdlib>

namespace Daw
{

ClipPlacementEngine::ClipPlacementEngine (juce::ValueTree                dawBranch,
                                          DeckSourceProvider&            sourceProvider,
                                          LaneResolver&                  laneResolver,
                                          std::function<std::int64_t()>  playheadProvider,
                                          ClipAlignmentSeam*             alignment)
    : dawBranch_ (std::move (dawBranch)),
      sourceProvider_ (sourceProvider),
      laneResolver_ (laneResolver),
      playheadProvider_ (std::move (playheadProvider)),
      alignment_ (alignment != nullptr ? *alignment
                                       : static_cast<ClipAlignmentSeam&> (defaultAlignment_))
{
}

//==============================================================================
void ClipPlacementEngine::onEvent (const PerformanceEvent& event)
{
    const int          deck   = static_cast<int> (event.deckIndex);
    const std::int64_t srcPos = event.sourceSamplePosition;

    switch (event.type)
    {
        // ---- Open-class -----------------------------------------------------
        case PerformanceEventType::DeckPlay:
        case PerformanceEventType::ChannelUnmute:
            openOnLanes (deck, srcPos);
            recordingDecks_.insert (deck);   // deck is now producing audio (PRD-0077)
            break;

        case PerformanceEventType::CueJumpIn:
            openOnLanes (deck, srcPos);
            break;

        // ---- Close-class, terminal -----------------------------------------
        case PerformanceEventType::DeckStop:
        case PerformanceEventType::ChannelMute:
            closeDeck (deck, srcPos, /*continuation*/ false);
            recordingDecks_.erase (deck);    // deck no longer producing audio
            break;

        // ---- Close-class, contiguous continuation --------------------------
        case PerformanceEventType::CueJumpOut:
            closeDeck (deck, srcPos, /*continuation*/ true);
            break;

        // ---- Loop capture (PRD-0076) ---------------------------------------
        // LoopIn  : out=engageSource. Close the lead-in clip, enter loop mode.
        // LoopPass: out=loopOut (srcPos), in=loopIn (payload). One pass clip.
        // LoopOut : out=exitSource (srcPos), in=loopIn (payload). Partial +
        //           resume an ordinary growing clip.
        case PerformanceEventType::LoopIn:
            enterLoop (deck, srcPos);
            break;
        case PerformanceEventType::LoopPass:
            applyLoopPass (deck, /*loopIn*/ event.payload, /*loopOut*/ srcPos);
            break;
        case PerformanceEventType::LoopOut:
            exitLoop (deck, /*exitSource*/ srcPos, /*loopIn*/ event.payload);
            break;

        // ---- Single-event jumps: split on source discontinuity (PRD-0075) --
        // out = sourceSamplePosition, in = payload.
        case PerformanceEventType::BeatJump:
        case PerformanceEventType::HotCueJump:
            applyJump (deck, srcPos, event.payload);
            break;

        // ---- Source-mode split (PRD-0077): re-target lanes at the switch ----
        case PerformanceEventType::SourceModeChange:
            applySourceModeChange (deck, srcPos);
            break;
    }
}

//==============================================================================
void ClipPlacementEngine::openOnLanes (int deckIndex, std::int64_t sourceStart)
{
    for (const auto& laneId : laneResolver_.resolveLanes (deckIndex))
        openOne (deckIndex, laneId, sourceStart);
}

void ClipPlacementEngine::openOne (int deckIndex, const juce::Uuid& laneId, std::int64_t sourceStart)
{
    const SlotKey key { deckIndex, laneId.toString().toStdString() };

    // Idempotent: an open-class event for an already-open (deck,lane) is ignored.
    if (slots_.find (key) != slots_.end())
        return;

    // Contiguous continuation (a split just closed this lane) reuses the closed
    // clip's timeline end so there is no gap/overlap; otherwise the alignment
    // seam fixes the timeline start AND classifies the placement.
    std::int64_t  timelineStart;
    AlignmentMode mode = AlignmentMode::FirstBeatAnchored;
    auto contIt = pendingContinuation_.find (key);
    if (contIt != pendingContinuation_.end())
    {
        // A continuation is never re-aligned; it inherits free placement.
        timelineStart = contIt->second;
        pendingContinuation_.erase (contIt);
    }
    else
    {
        const std::int64_t playhead = playheadProvider_ ? playheadProvider_() : 0;
        const auto result = alignment_.resolveOpen (deckIndex, playhead, sourceStart);
        timelineStart = result.timelineStartSample;
        mode          = result.mode;
    }

    auto laneNode = laneNodeFor (deckIndex, laneId);
    if (! laneNode.isValid())
        return;

    auto clips = laneNode.getOrCreateChildWithName (DawIDs::clips, nullptr);

    DawClip clip;
    clip.clipId              = juce::Uuid();
    clip.laneId              = laneId;
    clip.sourceFileId        = sourceProvider_.getSourceFileId (deckIndex);
    clip.sourceStartSample   = sourceStart;
    clip.sourceEndSample     = sourceStart; // grows from here
    clip.timelineStartSample = timelineStart;
    clip.sourceLengthSamples = sourceProvider_.getSourceLength (deckIndex);
    clip.gainDb              = 0.0f;

    auto node = DawClip::toValueTree (clip);
    node.setProperty (DawClipIDs::alignmentMode,
                      mode == AlignmentMode::GridAligned ? "GridAligned" : "FirstBeatAnchored",
                      nullptr);
    clips.appendChild (node, nullptr);

    slots_[key] = OpenSlot { node, sourceStart, timelineStart };
}

//==============================================================================
void ClipPlacementEngine::grow()
{
    for (auto& [key, slot] : slots_)
    {
        const int          deckIndex = key.first;
        const std::int64_t srcPos    = sourceProvider_.getSourceSample (deckIndex);
        const std::int64_t current   = readInt64 (slot.clipNode, DawClipIDs::sourceEndSample);

        // Monotonic: never move the crop end backwards for a playing deck.
        if (srcPos > current)
            slot.clipNode.setProperty (DawClipIDs::sourceEndSample, srcPos, nullptr);
    }
}

//==============================================================================
void ClipPlacementEngine::closeDeck (int deckIndex, std::int64_t sourceEnd, bool continuation)
{
    // Collect matching keys first (closeSlot mutates the map).
    std::vector<SlotKey> toClose;
    for (auto& [key, slot] : slots_)
        if (key.first == deckIndex)
            toClose.push_back (key);

    for (const auto& key : toClose)
    {
        auto it = slots_.find (key);
        if (it != slots_.end())
            closeSlot (key, it->second, sourceEnd, continuation);
    }
}

void ClipPlacementEngine::closeSlot (const SlotKey& key, OpenSlot& slot,
                                     std::int64_t sourceEnd, bool continuation)
{
    // Finalise the crop end at the event position (never below the start).
    const std::int64_t finalEnd = juce::jmax (sourceEnd, slot.sourceStart);
    slot.clipNode.setProperty (DawClipIDs::sourceEndSample, finalEnd, nullptr);

    const std::int64_t length = finalEnd - slot.sourceStart;

    // Record the contiguity point for the next open BEFORE any discard, so a
    // degenerate split clip does not introduce a timeline gap (§1.5.2).
    if (continuation)
        pendingContinuation_[key] = slot.timelineStart + juce::jmax<std::int64_t> (length, 0);

    // Discard zero/negative-length clips; keep everything positive (§1.5.2).
    if (length <= 0)
    {
        auto parent = slot.clipNode.getParent();
        if (parent.isValid())
            parent.removeChild (slot.clipNode, nullptr);
    }

    slots_.erase (key);
}

//==============================================================================
void ClipPlacementEngine::applySourceModeChange (int deckIndex, std::int64_t sourcePosition)
{
    // §1.2: only a deck that is actively recording (playing + channel-unmuted)
    // reacts. A change on a stopped/channel-muted deck mutates no clips; it only
    // affects which lane(s) the next play event opens on. A both-stems-muted
    // deck remains in recordingDecks_, so un-muting later re-opens clips.
    if (recordingDecks_.find (deckIndex) == recordingDecks_.end())
        return;

    // Close every outgoing-lane clip terminally at the switch source position.
    // Terminal (no continuation): the incoming lanes open at the live playhead
    // via the alignment seam, re-resolved per open (§1.5.4 / §1.5.7).
    closeDeck (deckIndex, sourcePosition, /*continuation*/ false);

    // Drop any stale continuation anchors so a lane that was previously closed
    // with continuation (and not reopened) is not mis-placed on a later reopen.
    clearContinuationForDeck (deckIndex);

    // Open the incoming lane set (post-change mapping from the resolver). An
    // empty set is a silence gap (both stems muted, §1.5.6): close, open nothing.
    openOnLanes (deckIndex, sourcePosition);
}

void ClipPlacementEngine::clearContinuationForDeck (int deckIndex)
{
    for (auto it = pendingContinuation_.begin(); it != pendingContinuation_.end(); )
    {
        if (it->first.first == deckIndex)
            it = pendingContinuation_.erase (it);
        else
            ++it;
    }
}

//==============================================================================
bool ClipPlacementEngine::anyOpenForDeck (int deckIndex) const
{
    for (const auto& [key, slot] : slots_)
        if (key.first == deckIndex)
            return true;
    return false;
}

void ClipPlacementEngine::applyJump (int deckIndex, std::int64_t jumpOutSource, std::int64_t jumpInSource)
{
    // §1.3.5 / §1.5.6: a jump with no open clip writes nothing — the deck is
    // paused/muted and nothing was heard. The next play/unmute opens at the
    // post-jump source position via the normal path.
    if (! anyOpenForDeck (deckIndex))
        return;

    // §1.5.1: only a genuine discontinuity splits; a normal per-block advance
    // (<= threshold) leaves the active clip growing uninterrupted.
    if (std::llabs (jumpInSource - jumpOutSource) <= kSourceSplitThresholdSamples)
        return;

    // §1.5.3 contiguous: close at the out-point, reopen at the in-point. The
    // re-open re-runs the alignment seam (§1.5.5). Degenerate (zero-length)
    // clips are discarded by closeSlot while preserving contiguity (§1.5.4).
    closeDeck   (deckIndex, jumpOutSource, /*continuation*/ true);
    openOnLanes (deckIndex, jumpInSource);
}

//==============================================================================
bool ClipPlacementEngine::isLooping (int deckIndex) const
{
    return loopLanesByDeck_.find (deckIndex) != loopLanesByDeck_.end();
}

void ClipPlacementEngine::enterLoop (int deckIndex, std::int64_t engageSource)
{
    // §1.5.4: the lead-in (the clip that was growing when the loop engaged) is
    // an ordinary clip finalised early at the engage position, contiguously, so
    // the first pass tiles straight after it. If nothing was open (deck paused),
    // there is simply no lead-in and the first pass aligns via the seam.
    closeDeck (deckIndex, engageSource, /*continuation*/ true);
    loopLanesByDeck_[deckIndex] = laneResolver_.resolveLanes (deckIndex);
}

void ClipPlacementEngine::applyLoopPass (int deckIndex, std::int64_t loopIn, std::int64_t loopOut)
{
    auto it = loopLanesByDeck_.find (deckIndex);
    if (it == loopLanesByDeck_.end())
        return; // a pass with no engaged loop is ignored (§1.5.6 robustness).

    // §1.2 / §1.5.1: one finalised clip per pass, crop exactly [loopIn,loopOut),
    // written to every lane the deck was writing to at engage.
    for (const auto& laneId : it->second)
        emitFinalizedClip (deckIndex, laneId, loopIn, loopOut);
}

void ClipPlacementEngine::exitLoop (int deckIndex, std::int64_t exitSource, std::int64_t loopIn)
{
    auto it = loopLanesByDeck_.find (deckIndex);
    if (it == loopLanesByDeck_.end())
    {
        // Defensive: a loop-exit with no engaged loop is a plain continuation.
        closeDeck (deckIndex, exitSource, /*continuation*/ true);
        return;
    }

    const auto lanes = it->second;
    loopLanesByDeck_.erase (it);

    // §1.5.4: a mid-cycle exit emits a partial final pass [loopIn, exitSource);
    // an exit exactly at the boundary (exitSource<=loopIn) emits nothing
    // (emitFinalizedClip drops the zero/negative-length clip).
    for (const auto& laneId : lanes)
        emitFinalizedClip (deckIndex, laneId, loopIn, exitSource);

    // Hand control back to the ordinary open/grow/close lifecycle, contiguous.
    openOnLanes (deckIndex, exitSource);
}

void ClipPlacementEngine::emitFinalizedClip (int deckIndex, const juce::Uuid& laneId,
                                             std::int64_t sourceStart, std::int64_t sourceEnd)
{
    const std::int64_t length = sourceEnd - sourceStart;
    if (length <= 0)
        return; // never write a zero/negative-length pass clip (§1.5.4).

    const SlotKey key { deckIndex, laneId.toString().toStdString() };

    // First segment of a loop run aligns via the seam (§1.5.7); every later pass
    // tiles contiguously from the prior segment's timeline end.
    std::int64_t  timelineStart;
    AlignmentMode mode = AlignmentMode::FirstBeatAnchored;
    auto contIt = pendingContinuation_.find (key);
    if (contIt != pendingContinuation_.end())
    {
        timelineStart = contIt->second;
    }
    else
    {
        const std::int64_t playhead = playheadProvider_ ? playheadProvider_() : 0;
        const auto result = alignment_.resolveOpen (deckIndex, playhead, sourceStart);
        timelineStart = result.timelineStartSample;
        mode          = result.mode;
    }

    auto laneNode = laneNodeFor (deckIndex, laneId);
    if (! laneNode.isValid())
        return;

    auto clips = laneNode.getOrCreateChildWithName (DawIDs::clips, nullptr);

    DawClip clip;
    clip.clipId              = juce::Uuid();
    clip.laneId              = laneId;
    clip.sourceFileId        = sourceProvider_.getSourceFileId (deckIndex);
    clip.sourceStartSample   = sourceStart;
    clip.sourceEndSample     = sourceEnd;
    clip.timelineStartSample = timelineStart;
    clip.sourceLengthSamples = sourceProvider_.getSourceLength (deckIndex);
    clip.gainDb              = 0.0f;

    auto node = DawClip::toValueTree (clip);
    node.setProperty (DawClipIDs::alignmentMode,
                      mode == AlignmentMode::GridAligned ? "GridAligned" : "FirstBeatAnchored",
                      nullptr);
    clips.appendChild (node, nullptr);

    // Advance the contiguity anchor for the next pass / the post-loop resume.
    pendingContinuation_[key] = timelineStart + length;
}

//==============================================================================
void ClipPlacementEngine::finaliseAll (std::int64_t /*stopPlayhead*/)
{
    std::vector<SlotKey> keys;
    keys.reserve (slots_.size());
    for (auto& [key, slot] : slots_)
        keys.push_back (key);

    for (const auto& key : keys)
    {
        auto it = slots_.find (key);
        if (it == slots_.end())
            continue;

        const int          deckIndex = key.first;
        const std::int64_t srcPos    = sourceProvider_.getSourceSample (deckIndex);
        closeSlot (key, it->second, srcPos, /*continuation*/ false);
    }

    slots_.clear();
    pendingContinuation_.clear();
    loopLanesByDeck_.clear();
    recordingDecks_.clear();
}

//==============================================================================
juce::ValueTree ClipPlacementEngine::laneNodeFor (int deckIndex, const juce::Uuid& laneId)
{
    auto track = DawState::ensureTrackForDeck (dawBranch_, deckIndex);
    if (! track.isValid())
        return {};

    auto lanes = track.getChildWithName (DawIDs::lanes);
    if (! lanes.isValid())
        return {};

    const auto wanted = laneId.toString();
    for (int i = 0; i < lanes.getNumChildren(); ++i)
    {
        auto lane = lanes.getChild (i);
        if (lane.hasType (DawIDs::lane)
            && lane.getProperty (DawIDs::laneId).toString() == wanted)
            return lane;
    }
    return {};
}

} // namespace Daw
