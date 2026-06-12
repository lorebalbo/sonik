#pragma once
//==============================================================================
// Grouped-tracks collapsed overview: GroupOverviewStrip molecule.
//
// When a channel group (DECK 1..4) is collapsed to its header row, this strip
// overlays the header's timeline area and draws a "ghost clip": up to three
// thin horizontal lines — one row per lane, top-to-bottom Original /
// Instrumental / Vocal — spanning exactly the timeline ranges where that lane
// has clips. So even with the group folded, the DJ sees at a glance WHICH
// stems exist at any moment of the arrangement.
//
// The strip is faithful to the audio: a lane silenced by the mute/solo flags
// (MuteSolo — the same rule the arrangement compiler applies) draws its
// segments dimmed, so the solid lines are precisely the sum you hear.
//
// Horizontal mapping is the shared PRD-0065 TimelineTransform (the strip's
// local x = 0 sits at the content-axis origin, like a lane's clip layer), and
// segment spans use the same elastic-stretch length as ClipBlock so the ghost
// lines align with the real clips when the group expands.
//
// Purely visual (never intercepts the mouse). Message/UI thread only; observes
// its track subtree via a juce::ValueTree::Listener; no audio-thread code.
//==============================================================================

#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../../Transform/TimelineTransform.h"

namespace Daw
{

class GroupOverviewStrip final : public juce::Component,
                                 private juce::ValueTree::Listener
{
public:
    GroupOverviewStrip (juce::ValueTree trackTree, const TimelineTransform& transform);
    ~GroupOverviewStrip() override;

    // One drawn ghost line: laneRow 0..2 (Original/Instrumental/Vocal), local
    // x span (already clipped to the strip), audible per the mute/solo rules.
    struct Segment
    {
        int   laneRow  { 0 };
        float x0       { 0.0f };
        float x1       { 0.0f };
        bool  audible  { true };
    };

    // The segments the strip would draw at its current width (test seam; the
    // paint routine renders exactly this list).
    std::vector<Segment> computeSegments() const;

    // Re-derive segment x positions after a zoom/scroll transform change.
    void refreshLayout() { repaint(); }

    void paint (juce::Graphics& g) override;

private:
    // Any clip edit, recording append, or mute/solo flip inside this track's
    // subtree changes the ghost; repaint only while actually shown (collapsed).
    void repaintIfShown()
    {
        if (isVisible())
            repaint();
    }

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override
    {
        repaintIfShown();
    }
    void valueTreeChildAdded   (juce::ValueTree&, juce::ValueTree&) override { repaintIfShown(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { repaintIfShown(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    static inline const juce::Colour kInk { 0xFF2D2D2D };

    juce::ValueTree          trackTree_;
    const TimelineTransform& transform_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GroupOverviewStrip)
};

} // namespace Daw
