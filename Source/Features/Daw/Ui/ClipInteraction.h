#pragma once
//==============================================================================
// PRD-0102: Shared clip-interaction context — grid snap settings and the
// single-clip selection model.
//
// These two small value/coordination objects are owned by the DawPanel and
// shared (by pointer) with every ClipBlock through the same group->lane->clip
// plumbing the EditCommandDispatcher already uses. Keeping them here, header-
// only, lets the panel flip snap/granularity or move the selection in one place
// and have every clip observe it without re-propagation.
//
//   * SnapSettings  — the global snap toggle + granularity. Pure: snaps a
//                     timeline sample to the nearest grid line of the chosen
//                     subdivision via the shared TimelineTransform grid.
//   * ClipSelection — a ChangeBroadcaster holding the one selected clip id, so
//                     every ClipBlock repaints its selected state when the
//                     selection moves.
//
// Message/UI thread only. No audio-thread contact.
//==============================================================================

#include <cmath>
#include <cstdint>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>    // juce::ChangeBroadcaster

#include "../State/DawState.h"          // kBeatsPerBar
#include "../Transform/TimelineTransform.h"

namespace Daw
{

//==============================================================================
// Snap granularity — the grid subdivision a snapped edit lands on (PRD-0102
// §1.5.3). Ordered coarse -> fine so the toolbar selector can cycle through it.
//==============================================================================
enum class SnapGranularity : int
{
    Bar = 0,    // one bar (kBeatsPerBar beats)
    Beat,       // one beat
    Half,       // 1/2 beat
    Quarter     // 1/4 beat
};

inline double beatsForGranularity (SnapGranularity g) noexcept
{
    switch (g)
    {
        case SnapGranularity::Bar:     return DawState::kBeatsPerBar;
        case SnapGranularity::Beat:    return 1.0;
        case SnapGranularity::Half:    return 0.5;
        case SnapGranularity::Quarter: return 0.25;
    }
    return 1.0;
}

inline juce::String labelForGranularity (SnapGranularity g)
{
    switch (g)
    {
        case SnapGranularity::Bar:     return "BAR";
        case SnapGranularity::Beat:    return "BEAT";
        case SnapGranularity::Half:    return "1/2";
        case SnapGranularity::Quarter: return "1/4";
    }
    return "BEAT";
}

// Cycle to the next finer granularity, wrapping Quarter -> Bar.
inline SnapGranularity nextGranularity (SnapGranularity g) noexcept
{
    return static_cast<SnapGranularity> ((static_cast<int> (g) + 1) % 4);
}

//==============================================================================
// SnapSettings — the global snap toggle + granularity (PRD-0102 §1.5.2/§1.5.3).
//==============================================================================
struct SnapSettings
{
    bool            enabled     { true };
    SnapGranularity granularity { SnapGranularity::Beat };

    // Snap a timeline sample to the nearest grid line of the current
    // granularity, using the shared transform's grid (origin + samples/beat).
    // Returns the sample unchanged when snapping is off or the grid is
    // degenerate. The caller passes `bypass = true` to honour the momentary
    // modifier override (PRD-0102 §1.5.2) without flipping the toggle.
    std::int64_t snap (std::int64_t sample,
                       const TimelineTransform& transform,
                       bool bypass = false) const
    {
        if (! enabled || bypass)
            return sample;

        const auto&  g    = transform.grid();
        const double step = g.samplesPerBeat * beatsForGranularity (granularity);
        if (step <= 0.0)
            return sample;

        const double origin = static_cast<double> (g.phaseOriginSample);
        const double n      = std::round ((static_cast<double> (sample) - origin) / step);
        return static_cast<std::int64_t> (std::llround (origin + n * step));
    }
};

//==============================================================================
// ClipSelection — the single-clip selection model (PRD-0102 §1.5.5).
//
// Holds the one selected clip id and broadcasts a change so every ClipBlock can
// repaint its selected state. Selecting a new clip implicitly deselects the
// previous one (single selection for this Epic, per PRD-0086 §1.5.4).
//==============================================================================
class ClipSelection final : public juce::ChangeBroadcaster
{
public:
    void select (const juce::String& clipId)
    {
        if (selectedId_ == clipId)
            return;
        selectedId_ = clipId;
        sendChangeMessage();
    }

    void clear() { select ({}); }

    const juce::String& selectedId() const noexcept { return selectedId_; }

    bool isSelected (const juce::String& clipId) const noexcept
    {
        return clipId.isNotEmpty() && clipId == selectedId_;
    }

private:
    juce::String selectedId_;
};

} // namespace Daw
