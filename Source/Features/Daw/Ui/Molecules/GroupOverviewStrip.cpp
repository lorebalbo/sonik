//==============================================================================
// Grouped-tracks collapsed overview: GroupOverviewStrip implementation.
//==============================================================================

#include "GroupOverviewStrip.h"

#include "../../Model/ChannelGroup.h"
#include "../../Model/MuteSolo.h"
#include "../../State/DawClipModel.h"

namespace Daw
{

GroupOverviewStrip::GroupOverviewStrip (juce::ValueTree trackTree,
                                        const TimelineTransform& transform)
    : trackTree_ (std::move (trackTree)),
      transform_ (transform)
{
    setInterceptsMouseClicks (false, false); // purely visual overlay
    if (trackTree_.isValid())
        trackTree_.addListener (this);
}

GroupOverviewStrip::~GroupOverviewStrip()
{
    if (trackTree_.isValid())
        trackTree_.removeListener (this);
}

std::vector<GroupOverviewStrip::Segment> GroupOverviewStrip::computeSegments() const
{
    std::vector<Segment> out;
    if (! trackTree_.isValid())
        return out;

    // Global solo scope: scan the whole tracks container (a solo on another
    // deck silences this one too). The track may be parentless in tests, in
    // which case anySoloActive over the invalid tree reports false.
    const bool soloActive = MuteSolo::anySoloActive (trackTree_.getParent());
    const double masterBpm = transform_.grid().bpm;
    const float  width     = static_cast<float> (getWidth());

    for (int row = 0; row < ChannelGroup::kLaneCount; ++row)
    {
        const auto kind = static_cast<ChannelGroup::LaneKind> (row);
        auto lane = ChannelGroup::findLane (trackTree_, kind);
        if (! lane.isValid())
            continue;

        const bool laneAudible = MuteSolo::isLaneAudible (trackTree_, lane, soloActive);

        auto clips = lane.getChildWithName (DawIDs::clips);
        for (int c = 0; c < clips.getNumChildren(); ++c)
        {
            auto clip = clips.getChild (c);
            if (! clip.hasType (DawIDs::clip))
                continue;

            const auto start = static_cast<std::int64_t> (
                static_cast<double> (clip.getProperty (DawClipIDs::timelineStartSample)));
            const auto srcStart = static_cast<std::int64_t> (
                static_cast<double> (clip.getProperty (DawClipIDs::sourceStartSample)));
            const auto srcEnd = static_cast<std::int64_t> (
                static_cast<double> (clip.getProperty (DawClipIDs::sourceEndSample)));
            const auto sourceBpm = static_cast<double> (clip.getProperty (DawClipIDs::sourceBpm, 0.0));

            // Stretched on-timeline span — the same elastic length ClipBlock
            // draws (DawClip::timelineLengthSamples), so ghost and clip align.
            std::int64_t span = srcEnd - srcStart;
            if (sourceBpm > 0.0 && masterBpm > 0.0)
                span = static_cast<std::int64_t> (
                    std::llround (static_cast<double> (span) * (sourceBpm / masterBpm)));
            if (span <= 0)
                continue;

            auto x0 = static_cast<float> (transform_.sampleToX (start));
            auto x1 = static_cast<float> (transform_.sampleToX (start + span));
            if (x1 <= 0.0f || x0 >= width)
                continue; // scrolled out of view

            // A missing-source clip is silent (excluded from the snapshot), so
            // its ghost is drawn dimmed like a muted lane.
            const bool missing =
                static_cast<bool> (clip.getProperty (DawClipIDs::missingSource, false));

            Segment seg;
            seg.laneRow = row;
            seg.x0      = juce::jmax (0.0f, x0);
            seg.x1      = juce::jmin (width, x1);
            seg.audible = laneAudible && ! missing;
            out.push_back (seg);
        }
    }

    return out;
}

void GroupOverviewStrip::paint (juce::Graphics& g)
{
    const int rowH  = getHeight() / ChannelGroup::kLaneCount;
    const int lineH = 4;

    for (const auto& seg : computeSegments())
    {
        const int y = seg.laneRow * rowH + (rowH - lineH) / 2;
        g.setColour (seg.audible ? kInk : kInk.withAlpha (0.22f));
        g.fillRect (seg.x0, static_cast<float> (y),
                    juce::jmax (1.0f, seg.x1 - seg.x0),
                    static_cast<float> (lineH));
    }
}

} // namespace Daw
