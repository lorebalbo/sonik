#pragma once
//==============================================================================
// PRD-0041: MidiMessageEvent — POD delivered to MessageThreadSink via
// juce::MessageManager::callAsync. Captured by value in the lambda, so it
// must remain trivially copyable.
//==============================================================================

#include "MidiTargetCategory.h"
#include "MappingTypes.h"
#include <cstdint>
#include <type_traits>

namespace sonik::midi
{
    struct MidiMessageEvent
    {
        MidiTargetCategory category;
        std::uint8_t       deckIndex;
        float              normalisedValue;
        std::int16_t       intDelta;
        std::uint64_t      deviceId;
        SoftTakeoverPolicy softTakeover; // PRD-0044: passed through for PRD-0045 to consult.
    };

    static_assert (std::is_trivially_copyable_v<MidiMessageEvent>,
                   "MidiMessageEvent must be trivially copyable for lambda capture-by-value");
} // namespace sonik::midi
