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
//     to kReanchorFraction of the viewport. Trigger and re-anchor share the same
//     4/5 fraction: once the playhead reaches 4/5 of the viewport it is pinned
//     there, so every refresh tick nudges it back by the small amount playback
//     advanced — a continuous, smooth follow rather than a jumpy re-centre.
//   * Any manual scroll / zoom / pan disengages follow for the moment
//     (notifyManualScroll()); pressing Play or Stop re-engages it, exactly like
//     Logic's "catch when starting playback".
//   * Follow ALSO re-engages on its own: after a manual scroll, once the now-line
//     drops at/below the trigger fraction and then crosses back above it (a
//     rising edge), update() resumes follow. This is the "catch up" case — the DJ
//     scrolls ahead, playback advances, and the moment the now-line reaches 4/5
//     of the viewport again the view starts tracking it once more. A backward
//     scroll that leaves the now-line already past the trigger does NOT snap the
//     view forward: the now-line must first dip below the trigger (re-arm) before
//     a crossing counts, so reviewing earlier material is never interrupted.
//==============================================================================

namespace Daw
{

class FollowController
{
public:
    static constexpr double kTriggerFraction  = 0.80; // now-line crosses this -> scroll
    static constexpr double kReanchorFraction = 0.80; // pin the now-line here (4/5)

    bool isEnabled() const noexcept { return enabled_; }

    void setEnabled (bool shouldEnable) noexcept
    {
        enabled_ = shouldEnable;
        armed_   = false; // explicit state changes start the auto-re-engage from a clean slate
    }

    void toggle() noexcept { setEnabled (! enabled_); }

    // A manual interaction disengages follow. Clearing the armed flag means a
    // re-engage requires the now-line to first drop at/below the trigger again,
    // so scrolling backward (now-line already past the trigger) will NOT yank the
    // view forward — only a genuine "playback caught up" rising edge re-engages.
    void notifyManualScroll() noexcept
    {
        enabled_ = false;
        armed_   = false;
    }

    // Advance the auto-re-engage state machine; call once per refresh tick with
    // the live now-line position. Only meaningful while follow is disengaged: it
    // arms when the now-line sits at/below the trigger and re-engages follow the
    // moment the now-line crosses back above it (a rising edge).
    void update (double nowLineX, double viewportWidth) noexcept
    {
        if (enabled_ || viewportWidth <= 0.0)
            return;

        if (nowLineX > viewportWidth * kTriggerFraction)
        {
            // Above the trigger: only re-engage if we were armed by a prior dip
            // below it (otherwise a backward scroll would snap the view forward).
            if (armed_)
            {
                enabled_ = true;
                armed_   = false;
            }
        }
        else
        {
            // At/below the trigger: arm so the next upward crossing re-engages.
            armed_ = true;
        }
    }

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
    bool armed_   { false }; // re-engage pending: now-line dipped below the trigger
};

} // namespace Daw
