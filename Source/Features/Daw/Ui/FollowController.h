#pragma once
//==============================================================================
// PRD-0070: FollowController — the "follow playhead" auto-scroll state machine.
//
// Pure logic (no JUCE component / timer) so it is fully unit-testable. The
// DawPanel owns one and consults it every refresh tick.
//
// Behaviour (PRD-0070 §1.5.2):
//   * Follow is OFF by default — the arrangement does not move on its own.
//   * The user explicitly toggles it on; setEnabled(true) (re-)engages it.
//   * While enabled, when the now-line crosses kTriggerFraction of the viewport
//     width, shouldFollow() requests an auto-scroll that re-anchors the now-line
//     to kReanchorFraction of the viewport.
//   * Any manual scroll / zoom / pan disengages follow until the user re-enables
//     it (notifyManualScroll()).
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
    bool enabled_ { false };
};

} // namespace Daw
