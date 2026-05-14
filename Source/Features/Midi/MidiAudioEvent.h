#pragma once
//==============================================================================
// PRD-0041: MidiAudioEvent — POD pushed onto the lock-free audio FIFO.
//==============================================================================

#include "MidiTargetCategory.h"
#include <cstdint>
#include <type_traits>

namespace sonik::midi
{
    struct MidiAudioEvent
    {
        MidiTargetCategory category;
        std::uint8_t       deckIndex;
        float              normalisedValue;   // 0..1 or signed jog delta normalised
        std::int16_t       intDelta;          // raw signed delta where meaningful
        std::int64_t       sampleTimestamp;   // 0 if unknown (set on enqueue)
    };

    static_assert (std::is_trivially_copyable_v<MidiAudioEvent>,
                   "MidiAudioEvent must be trivially copyable for lock-free FIFO use");
} // namespace sonik::midi
