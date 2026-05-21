//==============================================================================
// PRD-0052: Meter slot resolver — dotted path → MixerMeterSnapshot atomic.
//
// Implements MixerIDs::resolveMeterSlot.  Lives in a .cpp so that
// MixerIdentifiers.h does not have to include MixerMeterSnapshot.h
// (it only forward-declares MixerMeterSnapshot).
//==============================================================================

#include "MixerIdentifiers.h"
#include "MixerMeterSnapshot.h"

namespace MixerIDs
{
    MeterSlotRef resolveMeterSlot (MixerMeterSnapshot& meters,
                                    juce::StringRef dottedPath) noexcept
    {
        juce::String toks[5];
        const int n = detail::tokenise (dottedPath, toks);
        if (n < 3 || toks[0] != "mixer")
            return {};

        auto resolveSlot = [] (ChannelMeterSlots& slots, const juce::String& key) -> MeterSlotRef
        {
            if (key == "levelPeakL")     return { &slots.levelPeakL,     nullptr };
            if (key == "levelPeakR")     return { &slots.levelPeakR,     nullptr };
            if (key == "levelPeakHoldL") return { &slots.levelPeakHoldL, nullptr };
            if (key == "levelPeakHoldR") return { &slots.levelPeakHoldR, nullptr };
            if (key == "levelRmsL")      return { &slots.levelRmsL,      nullptr };
            if (key == "levelRmsR")      return { &slots.levelRmsR,      nullptr };
            if (key == "clip")           return { nullptr, &slots.clip };
            return {};
        };

        // "mixer.master.<slot>"
        if (n == 3 && toks[1] == "master")
            return resolveSlot (meters.master, toks[2]);

        // "mixer.channel.X.<slot>"
        if (n == 4 && toks[1] == "channel")
        {
            const int idx = letterToChannelIndex (juce::StringRef (toks[2].toRawUTF8()));
            if (idx < 0) return {};
            return resolveSlot (meters.getChannel (idx), toks[3]);
        }

        return {};
    }
}
