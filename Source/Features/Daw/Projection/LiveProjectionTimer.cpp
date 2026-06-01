//==============================================================================
// PRD-0069: Live Deck Projection Bridge implementation.
//==============================================================================

#include "LiveProjectionTimer.h"

#include "../Model/DawClip.h"
#include "../../Deck/DeckIdentifiers.h"

namespace Daw
{

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
    clip.timelineStartSample = nowLineSample_;          // raw now-line anchor (§1.5.6)
    clip.sourceLengthSamples = sourceLength;
    clip.gainDb              = 0.0f;

    auto node = DawClip::toValueTree (clip);
    clips.addChild (node, -1, nullptr);

    dp.lanes[lane].clipNode = node;
    dp.lanes[lane].active   = true;
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

        // Detect a seek/cue: a backward move, or a forward jump beyond tolerance.
        bool seeked = false;
        if (dp.wasPlaying && playing)
        {
            if (srcPos < dp.lastSourcePos
                || srcPos > dp.lastSourcePos + kSeekToleranceSamples)
                seeked = true;
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

        for (int li = 0; li < kLaneCount; ++li)
        {
            auto& lp = dp.lanes[li];
            if (want[li])
            {
                if (! lp.active || seeked)
                {
                    if (lp.active)
                        finaliseLane (lp);     // close the pre-seek span
                    startLane (dp, deckIndex, static_cast<Lane> (li),
                               srcPos, sourceFileId, sourceLength);
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
        if (dp.wasPlaying && playing && ! seeked && srcPos > dp.lastSourcePos)
            maxAdvance = juce::jmax (maxAdvance, srcPos - dp.lastSourcePos);

        dp.wasPlaying    = playing;
        dp.lastSourcePos = srcPos;
    }

    // When recording is active but no deck advanced this tick, keep the
    // timeline moving in real time so the record cursor advances over silence.
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    if (maxAdvance == 0 && capturingProvider_ && capturingProvider_())
    {
        if (lastTickMs_ > 0.0)
        {
            const double dt   = (nowMs - lastTickMs_) * 0.001;
            const double sr   = grid_.snapshotGrid().sampleRate;
            const double rate = sr > 0.0 ? sr : DawState::kProjectSampleRate;
            if (dt > 0.0)
                maxAdvance = static_cast<std::int64_t> (std::llround (dt * rate));
        }
    }
    lastTickMs_ = nowMs;

    nowLineSample_ += maxAdvance;
}

} // namespace Daw
