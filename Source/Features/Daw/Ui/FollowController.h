#pragma once
//==============================================================================
// PRD-0070: FollowController — the "follow playhead" auto-scroll state machine.
//
// Pure logic (no JUCE component / timer) so it is fully unit-testable. The
// DawPanel owns one and consults it every refresh tick.
//
// Behaviour (PRD-0070 §1.5.2, revised — Logic-style "catch" workflow):
//   * Follow is ON by default — the arrangement tracks the playhead without a
//     dedicated toggle button cluttering the top bar.
//   * While enabled, when the now-line crosses kTriggerFraction of the viewport
//     width, shouldFollow() requests an auto-scroll that re-anchors the now-line
//     to kReanchorFraction of the viewport.
//   * Any manual scroll / zoom / pan disengages follow for the moment
//     (notifyManualScroll()); pressing Play or Stop re-engages it, exactly like
//     Logic's "catch when starting playback".
//==============================================================================

namespace Daw
{

class FollowController
{
public:
    static constexpr double kTriggerFraction  = 0.85; // now-line crosses this -> scroll
    static constexpr double kReanchorFraction = 0.50; // re-centre the now-line here

    bool isEnabled() const noexcept { return enabled_; }

    void setEnabled (bool shouldEnable) noexcept { enabled_ = shouldEnable; }

    void toggle() noexcept { enabled_ = ! enabled_; }

    // A manual interaction disengages follow (no-op when already disabled).
    void notifyManualScroll() noexcept { enabled_ = false; }

    // True when an auto-scroll should be applied this tick.
    bool shouldFollow (double nowLineX, double viewportWidth) const noexcept
    {
        if (! enabled_ || viewportWidth <= 0.0)
            return false;
        return nowLineX > viewportWidth * kTriggerFraction;
    }

    // The desired now-line x after a follow scroll: re-anchored to the re-anchor
    // fraction of the viewport.
    static double reanchorTargetX (double viewportWidth) noexcept
    {
        return viewportWidth * kReanchorFraction;
    }

private:
    bool enabled_ { true };
};

} // namespace Daw
