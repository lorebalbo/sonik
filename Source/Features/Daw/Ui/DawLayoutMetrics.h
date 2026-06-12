#pragma once
//==============================================================================
// PRD-0067: shared vertical-layout metrics for the DAW channel-group stack.
//
// These constants are owned by the channel-group / lane layer (this PRD owns
// "lane heights, header widths" per PRD-0067 §1.2). The horizontal time axis is
// owned by the PRD-0065 TimelineTransform / PRD-0066 ruler; the only horizontal
// value here is kTrackHeaderWidth — the fixed left gutter shared by the ruler
// and every lane so that a sample S maps to the same x in the ruler and in the
// lane content area.
//
// All values are DESIGN.md-grid-aligned (multiples of the 2-px base unit) and
// produce the strict-monochrome, zero-radius, 2-px-border arrangement layout.
//==============================================================================

namespace Daw
{

struct DawLayout
{
    // Left gutter occupied by group/lane header labels. Both the ruler and the
    // lane content surfaces start at this x, keeping their horizontal origins
    // identical (PRD-0067 §1.4 alignment criterion). Sized like a Logic track
    // header so the name, volume fader and automation selector breathe.
    static constexpr int kTrackHeaderWidth = 168;

    // Fixed lane row height (PRD-0067 §1.5.3 — never auto-stretched). Tall
    // enough for the clip header band + waveform body.
    static constexpr int kLaneHeight = 44;

    // Group header row height. Two stacked rows, Logic-style: the deck label +
    // collapse toggle on top, the volume fader + automation-parameter dropdown
    // beneath.
    static constexpr int kGroupHeaderHeight = 52;

    // Number of lanes per channel group (Original / Instrumental / Vocal).
    static constexpr int kLanesPerGroup = 3;

    // Grouped-tracks: child rows (source lanes, deck automation) are indented
    // by this much inside the header gutter, and the group draws a vertical
    // ink bracket in the freed strip — the Logic "track stack" embrace that
    // makes the deck's children read as one group. 2-px grid aligned.
    static constexpr int kGroupChildIndent = 12;

    // Fully-expanded height of one channel group.
    static constexpr int kExpandedGroupHeight =
        kGroupHeaderHeight + kLanesPerGroup * kLaneHeight;

    // Collapsed height of one channel group (header row only).
    static constexpr int kCollapsedGroupHeight = kGroupHeaderHeight;
};

} // namespace Daw
