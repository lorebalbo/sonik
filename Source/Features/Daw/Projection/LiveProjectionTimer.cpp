//==============================================================================
// PRD-0069: Live Deck Projection Bridge implementation.
//==============================================================================

#include "LiveProjectionTimer.h"

#include "../Model/DawClip.h"
#include "../../Deck/DeckIdentifiers.h"
#include "../../Deck/AudioThreadState.h"

#include <cmath>

namespace Daw
{

//==============================================================================
std::int64_t LiveProjectionTimer::snapStartToGrid (std::int64_t  beatgridAnchor,
                                                   double        beatgridInterval,
                                                   std::int64_t  sourceStart,
                                                   std::int64_t  rawTimelineStart,
                                                   const MasterGridService::GridContext& gridCtx)
{
    const double gridInterval = gridCtx.samplesPerBeat;
    if (beatgridInterval <= 0.0 || gridInterval <= 0.0)
        return rawTimelineStart; // no usable grid: leave the raw placement

    // First source downbeat at or after the clip's source start.
    const double beatsAhead = static_cast<double> (sourceStart - beatgridAnchor) / beatgridInterval;
    const std::int64_t firstDownbeatSrc =
        beatgridAnchor
        + static_cast<std::int64_t> (std::llround (std::ceil (beatsAhead) * beatgridInterval));
    const std::int64_t downbeatOffset = firstDownbeatSrc - sourceStart; // >= 0 source samples

    // With the live 1:1 source->timeline mapping the downbeat would land here;
    // snap it to the nearest master-grid line and back the start out so every
    // captured beat aligns to the grid.
    const std::int64_t gridOrigin = gridCtx.phaseOriginSample;
    const double beats = static_cast<double> (rawTimelineStart + downbeatOffset - gridOrigin)
                       / gridInterval;
    const std::int64_t snappedDownbeat =
        gridOrigin + static_cast<std::int64_t> (std::llround (std::llround (beats) * gridInterval));
    return snappedDownbeat - downbeatOffset;
}

const std::int64_t LiveProjectionTimer::kSeekToleranceSamples =
    static_cast<std::int64_t> (0.08 * DawState::kProjectSampleRate);

LiveProjectionTimer::LiveProjectionTimer (DeckProjectionSource& decks,
                                          juce::ValueTree       dawBranch,
                                          MasterGridService&    grid)
    : decks_ (decks),
      dawBranch_ (std::move (dawBranch)),
      grid_ (grid)
{
    // Anchor the live timeline at the master grid phase origin so the projection
    // shares the master grid phase (PRD-0069 §1.5.6). The now-line then advances
    // monotonically with real playback progress.
    nowLineSample_ = grid_.snapshotGrid().phaseOriginSample;
}

LiveProjectionTimer::~LiveProjectionTimer()
{
    stop();
}

void LiveProjectionTimer::start()
{
    startTimerHz (kRefreshHz);
}

void LiveProjectionTimer::stop()
{
    stopTimer();
}

ChannelGroup::LaneKind LiveProjectionTimer::laneKindFor (Lane lane)
{
    switch (lane)
    {
        case kInstrumental: return ChannelGroup::LaneKind::Instrumental;
        case kVocal:        return ChannelGroup::LaneKind::Vocal;
        case kOriginal:
        default:            return ChannelGroup::LaneKind::Original;
    }
}

juce::String LiveProjectionTimer::readContentHash (const juce::ValueTree& deckTree)
{
    auto meta = deckTree.getChildWithName (IDs::TrackMetadata);
    return meta.getProperty (IDs::contentHash, juce::String()).toString();
}

std::int64_t LiveProjectionTimer::readSourceLength (const juce::ValueTree& deckTree)
{
    auto meta = deckTree.getChildWithName (IDs::TrackMetadata);
    return static_cast<std::int64_t> (meta.getProperty (IDs::totalSamples, 0));
}

void LiveProjectionTimer::startLane (DeckProjection&     dp,
                                     int                 deckIndex,
                                     Lane                lane,
                                     std::int64_t        srcPos,
                                     std::int64_t        timelineStart,
                                     const juce::String& sourceFileId,
                                     std::int64_t        sourceLength)
{
    // Lazily materialise the deck's channel group + its three lanes.
    auto track    = DawState::ensureTrackForDeck (dawBranch_, deckIndex);
    auto laneTree = ChannelGroup::findLane (track, laneKindFor (lane));
    if (! laneTree.isValid())
        return;

    auto clips = laneTree.getChildWithName (DawIDs::clips);
    if (! clips.isValid())
        return;

    DawClip clip;
    clip.clipId              = juce::Uuid();
    clip.laneId              = juce::Uuid (laneTree.getProperty (DawIDs::laneId).toString());
    clip.sourceFileId        = sourceFileId;
    clip.sourceStartSample   = srcPos;
    clip.sourceEndSample     = srcPos;                 // zero-length until it grows
    clip.timelineStartSample = timelineStart;           // now-line, or split seam
    clip.sourceLengthSamples = sourceLength;
    clip.gainDb              = 0.0f;

    auto node = DawClip::toValueTree (clip);
    clips.addChild (node, -1, nullptr);

    dp.lanes[lane].clipNode = node;
    dp.lanes[lane].active   = true;
}

std::int64_t LiveProjectionTimer::laneTimelineEnd (const LaneProjection& lp)
{
    if (! lp.clipNode.isValid())
        return 0;
    const auto tlStart  = static_cast<std::int64_t> (lp.clipNode.getProperty (DawClipIDs::timelineStartSample));
    const auto srcStart = static_cast<std::int64_t> (lp.clipNode.getProperty (DawClipIDs::sourceStartSample));
    const auto srcEnd   = static_cast<std::int64_t> (lp.clipNode.getProperty (DawClipIDs::sourceEndSample));
    return tlStart + (srcEnd - srcStart);
}

void LiveProjectionTimer::growLane (LaneProjection& lp, std::int64_t srcPos)
{
    if (! lp.clipNode.isValid())
        return;

    const auto srcStart =
        static_cast<std::int64_t> (lp.clipNode.getProperty (DawClipIDs::sourceStartSample));
    const auto end = juce::jmax (srcStart, srcPos);

    lp.clipNode.setProperty (DawClipIDs::sourceEndSample, end, nullptr);
}

void LiveProjectionTimer::finaliseLane (LaneProjection& lp)
{
    // Freeze the clip in place (leave sourceEndSample), drop our handle so the
    // next play span starts a fresh clip (PRD-0069 §1.5.7).
    lp.clipNode = juce::ValueTree();
    lp.active   = false;
}

void LiveProjectionTimer::processTick()
{
    const int numDecks = decks_.getNumDecks();
    std::int64_t maxAdvance = 0;

    // One coherent read of the master grid for this tick (origin + samples/beat),
    // used to re-anchor the now-line and grid-snap fresh clip placements.
    const auto gridCtx = grid_.snapshotGrid();

    for (int slot = 0; slot < numDecks; ++slot)
    {
        auto* audio = decks_.getAudioState (slot);
        if (audio == nullptr)
            continue;

        const int          deckIndex = decks_.getDeckIndex (slot);
        const juce::ValueTree deckTree = decks_.getDeckTree (slot);

        auto& dp = projection_[deckIndex];

        const int  status  = audio->playbackStatus.load (std::memory_order_acquire);
        const auto srcPos  = audio->playheadPosition.load (std::memory_order_acquire);
        const bool playing = (status == static_cast<int> (PlaybackStatusCode::playing));

        const auto audibility = resolveAudibility (deckTree, *audio);

        // Consume any EXACT source-position discontinuity the audio thread
        // published since the last tick (loop wrap / cue / jump). The seq is
        // always consumed so a discontinuity that happened before recording
        // armed never triggers a stale split.
        const std::uint64_t seekSeq = audio->seekDiscontinuitySeq.load (std::memory_order_acquire);
        bool         exactSeek = false;
        std::int64_t seekFrom  = 0;
        std::int64_t seekTo    = 0;
        if (seekSeq != dp.lastSeekSeq)
        {
            seekFrom = audio->seekDiscontinuityFrom.load (std::memory_order_acquire);
            seekTo   = audio->seekDiscontinuityTo.load   (std::memory_order_acquire);
            dp.lastSeekSeq = seekSeq;
            exactSeek = true;
        }

        // Which lanes should carry a live clip this tick.
        // Clip writing is gated behind the capturing provider: clips must
        // only grow when the user has armed or started recording. When the
        // provider is absent (e.g. in unit tests that don't wire a controller)
        // the legacy always-capturing behaviour is preserved.
        const bool capturing = capturingProvider_ ? capturingProvider_() : true;
        std::array<bool, kLaneCount> want { false, false, false };
        if (playing && capturing)
        {
            if (audibility.original)     want[kOriginal]     = true;
            if (audibility.instrumental) want[kInstrumental] = true;
            if (audibility.vocal)        want[kVocal]        = true;
        }

        const juce::String sourceFileId = readContentHash (deckTree);
        const std::int64_t sourceLength = readSourceLength (deckTree);

        // A genuine split only when we were already recording this deck's
        // playback (there is an open clip to close at the exact out-point and
        // reopen at the exact in-point).
        const bool splitNow = exactSeek && dp.wasPlaying && playing && capturing;

        // Heuristic fallback for any discontinuity NOT published exactly
        // (e.g. an un-instrumented seek path): coarse, but never worse than before.
        bool heuristicSeek = false;
        if (! exactSeek && dp.wasPlaying && playing)
        {
            if (srcPos < dp.lastSourcePos
                || srcPos > dp.lastSourcePos + kSeekToleranceSamples)
                heuristicSeek = true;
        }

        if (splitNow)
        {
            // Phase 1 — close every open lane at the EXACT pre-jump sample so
            // none of the played audio up to the loop-out / jump-out is lost.
            // The seam (the clip's exact timeline end) anchors the reopen so the
            // two clips butt-join with no gap.
            std::array<std::int64_t, kLaneCount> seam { 0, 0, 0 };
            const std::int64_t pre = juce::jmax<std::int64_t> (0, seekFrom - dp.lastSourcePos);
            for (int li = 0; li < kLaneCount; ++li)
            {
                auto& lp = dp.lanes[li];
                if (lp.active)
                {
                    growLane (lp, seekFrom);
                    seam[static_cast<size_t> (li)] = laneTimelineEnd (lp);
                    finaliseLane (lp);
                }
                else
                {
                    seam[static_cast<size_t> (li)] = nowLineSample_ + pre;
                }
            }

            // A loop wrap repeats continuous audio, so its reopen must butt-join
            // the seam gaplessly. A cue/beat/hot-cue jump is a genuine
            // discontinuity: re-snap the reopen to the grid so the post-jump beats
            // land on grid lines (an off-beat cue would otherwise inherit the
            // seam's arbitrary, off-grid phase). loopActive distinguishes the two.
            const bool looping = audio->loopActive.load (std::memory_order_acquire);
            const std::int64_t jumpAnchor    = audio->beatgridAnchor.load (std::memory_order_acquire);
            const double       jumpInterval  = audio->beatgridInterval.load (std::memory_order_acquire);

            // Phase 2 — reopen wanted lanes at the seam, starting at the EXACT
            // in-point, and grow to the live position (capturing the post-jump
            // head that polling would otherwise have skipped).
            for (int li = 0; li < kLaneCount; ++li)
            {
                if (want[li])
                {
                    const std::int64_t rawStart = seam[static_cast<size_t> (li)];
                    const std::int64_t start    = looping
                        ? rawStart
                        : snapStartToGrid (jumpAnchor, jumpInterval, seekTo, rawStart, gridCtx);

                    startLane (dp, deckIndex, static_cast<Lane> (li),
                               seekTo, start, sourceFileId, sourceLength);
                    growLane (dp.lanes[li], srcPos);
                }
            }

            // The deck's real progress this tick is the pre-jump + post-jump
            // spans; feed it to the shared now-line (max across decks).
            const std::int64_t post = juce::jmax<std::int64_t> (0, srcPos - seekTo);
            maxAdvance = juce::jmax (maxAdvance, pre + post);
        }
        else
        {
            for (int li = 0; li < kLaneCount; ++li)
            {
                auto& lp = dp.lanes[li];
                if (want[li])
                {
                    if (! lp.active || heuristicSeek)
                    {
                        if (lp.active)
                            finaliseLane (lp);     // close the pre-seek span

                        // First clip of the take: re-anchor the now-line to the
                        // live grid origin (a beat line). Recording may have been
                        // armed while the decks were dormant, where the grid origin
                        // is published as 0 — anchoring now keeps the record cursor
                        // on a beat and the first clip aligned to the grid.
                        if (! sessionAnchored_)
                        {
                            nowLineSample_   = gridCtx.phaseOriginSample;
                            sessionAnchored_ = true;
                        }

                        const std::int64_t start = snapStartToGrid (
                            audio->beatgridAnchor.load (std::memory_order_acquire),
                            audio->beatgridInterval.load (std::memory_order_acquire),
                            srcPos, nowLineSample_, gridCtx);

                        startLane (dp, deckIndex, static_cast<Lane> (li),
                                   srcPos, start, sourceFileId, sourceLength);
                    }
                    else
                    {
                        growLane (lp, srcPos);
                    }
                }
                else if (lp.active)
                {
                    finaliseLane (lp);
                }
            }

            // Steady-state forward progress advances the now-line (PRD-0070).
            if (dp.wasPlaying && playing && ! heuristicSeek && srcPos > dp.lastSourcePos)
                maxAdvance = juce::jmax (maxAdvance, srcPos - dp.lastSourcePos);
        }

        dp.wasPlaying    = playing;
        dp.lastSourcePos = srcPos;
    }

    // The now-line advances ONLY by real deck progress: the record cursor parks
    // at the grid origin until a deck starts playing and freezes whenever every
    // deck is silent, so it never drifts to a between-beats position over silence.
    nowLineSample_ += maxAdvance;
}

} // namespace Daw
