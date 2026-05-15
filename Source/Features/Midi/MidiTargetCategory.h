#pragma once
//==============================================================================
// PRD-0041: MIDI Target Category enum.
//
// Categories are the unit of routing classification. PRD-0042's binding
// resolver assigns one of these to every inbound MIDI event; PRD-0041's
// bridge consults a compile-time table to decide whether the event goes to
// the audio thread (lock-free FIFO) or the Message thread (callAsync).
//
// Add new categories by:
//   1. Adding the enum value here (before Count).
//   2. Adding its routing class in MidiMessageBridge.h's `routingTable`.
//   3. The static_assert in the bridge will fail-fast if you forget step 2.
//==============================================================================

#include <cstddef>
#include <cstdint>

namespace sonik::midi
{
    enum class MidiTargetCategory : std::uint8_t
    {
        // ---- Audio-thread targets (low-latency, jog/scratch) -----------------
        JogScratch,
        JogBend,
        JogTouch,

        // ---- Message-thread targets (UI, transport, library) -----------------
        TransportPlay,
        TransportCue,
        TransportSync,
        PitchFader,
        PitchRangeCycle,
        Gain,
        EqHigh,
        EqMid,
        EqLow,
        Crossfader,
        MasterGain,
        HeadphonesGain,
        HeadphoneCueToggle,
        KeyLockToggle,
        MasterTempoToggle,
        KeyShiftPlus,
        KeyShiftMinus,
        LoopIn,
        LoopOut,
        LoopSizeHalve,
        LoopSizeDouble,
        LoopToggle,
        HotCueTrigger,
        HotCueDelete,
        BeatJumpMinus,
        BeatJumpPlus,
        BeatJumpSizeCycle,
        QuantizeToggle,
        SlipToggle,
        PositionSeek,
        LibraryScrollUp,
        LibraryScrollDown,
        LibraryLoadDeck,
        LibraryFocusSearch,

        // ---- Resolver-internal (consumed by PRD-0044's mask manager) -------
        ModifierSet,
        ModifierClear,
        ModifierToggle,

        Count // Must remain last.
    };

    inline constexpr std::size_t MidiTargetCategoryCount =
        static_cast<std::size_t> (MidiTargetCategory::Count);

    enum class RoutingClass : std::uint8_t
    {
        AudioThread,
        MessageThread,
    };
} // namespace sonik::midi
