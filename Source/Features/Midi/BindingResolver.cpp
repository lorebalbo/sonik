#include "BindingResolver.h"

#include "ControlTargetRegistry.h"

#include <bit>

namespace sonik::midi
{
    namespace
    {
        // True if `required` is a subset of `current` (or required == 0).
        constexpr bool modifierMatches (ModifierMask required, ModifierMask current) noexcept
        {
            return (required & current) == required;
        }

        // Pick the most-specific binding from up to MaxOverloadsPerMidiKey
        // candidates. "Most specific" = highest popcount of requiredModifierMask
        // among those whose mask is a subset of currentMask.
        // Returns InvalidTargetIndex if no candidate matches.
        TargetIndex pickBestOverload (const Mapping&       mapping,
                                      const BindingBucket& bucket,
                                      ModifierMask         currentMask) noexcept
        {
            TargetIndex best        = InvalidTargetIndex;
            int         bestPopcnt  = -1;

            for (std::size_t i = 0; i < MaxOverloadsPerMidiKey; ++i)
            {
                const auto idx = bucket[i];
                if (idx == InvalidTargetIndex)
                    continue;
                if (idx >= static_cast<TargetIndex> (mapping.bindings.size()))
                    continue;

                const auto& binding = mapping.bindings[idx];
                if (! modifierMatches (binding.requiredModifierMask, currentMask))
                    continue;

                const int pop = std::popcount (binding.requiredModifierMask);
                if (pop > bestPopcnt)
                {
                    bestPopcnt = pop;
                    best       = idx;
                }
            }
            return best;
        }

        constexpr float clamp01 (float v) noexcept
        {
            return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        }
    } // namespace

    std::optional<ResolvedBinding> BindingResolver::resolve (const Mapping&          mapping,
                                                             ResolverState&          state,
                                                             const MidiInboundEvent& event,
                                                             ModifierMask            currentMask) noexcept
    {
        const auto statusNibble = static_cast<std::uint8_t> (event.statusByte & 0xF0u);
        const auto channel      = static_cast<std::uint8_t> ((event.statusByte & 0x0Fu) + 1u);

        // Mappings are indexed under NoteOn (0x90); a NoteOff event for the
        // same note must look up the same binding so it can drive the "release"
        // value (0.0f for Momentary, etc.).
        const auto lookupStatus = (statusNibble == 0x80u) ? std::uint8_t { 0x90u } : statusNibble;

        // For Pitch Bend, data1 in the wire message is the LSB of a 14-bit value;
        // the user maps a pitch-bend control by channel only, so we collapse to data1=0.
        const std::uint8_t keyData1 = (lookupStatus == 0xE0u) ? 0u : event.data1;

        const std::uint32_t key = packMidiKey (channel, lookupStatus, keyData1);

        // ---- 1. Modifier check (pre-empts regular binding lookup) ----------
        if (const auto modIt = mapping.modifierIndex.find (key); modIt != mapping.modifierIndex.end())
        {
            const auto modIdx = modIt->second;
            if (modIdx >= mapping.modifiers.size())
                return std::nullopt;

            const auto& mod = mapping.modifiers[modIdx];

            // Determine on/off semantics for the modifier press.
            // Note On vel>0 = on, Note Off / vel 0 = off; CC value > 0 = on.
            bool isOn = false;
            if (statusNibble == 0x90u)
                isOn = (event.data2 > 0u);
            else if (statusNibble == 0x80u)
                isOn = false;
            else if (statusNibble == 0xB0u)
                isOn = (event.data2 > 0u);
            else
                isOn = true; // pitch bend etc — treat any event as a press.

            ResolvedBinding rb {};
            rb.target          = InvalidTargetIndex;
            rb.deckIndex       = GlobalDeckIndex;
            rb.normalisedValue = isOn ? 1.0f : 0.0f;
            rb.intDelta        = static_cast<std::int16_t> (mod.bit);
            rb.softTakeover    = SoftTakeoverPolicy::Always;

            if (mod.style == ModifierStyle::Toggle)
            {
                // PRD-0044 will flip the bit. We only emit Set on the press edge.
                if (! isOn)
                    return std::nullopt;
                rb.category = MidiTargetCategory::ModifierSet;
            }
            else
            {
                rb.category = isOn ? MidiTargetCategory::ModifierSet
                                   : MidiTargetCategory::ModifierClear;
            }
            return rb;
        }

        // ---- 2. Linear14 LSB cache update (CC only, no binding fires) ------
        if (statusNibble == 0xB0u
            && event.data1 < mapping.isLsbDataByte.size()
            && mapping.isLsbDataByte[event.data1])
        {
            state.lsbCache[event.data1] = event.data2;
            return std::nullopt;
        }

        // ---- 3. Binding lookup --------------------------------------------
        const auto bindIt = mapping.bindingIndex.find (key);
        if (bindIt == mapping.bindingIndex.end())
            return std::nullopt;

        const BindingBucket& bucket = bindIt->second;
        const auto bindingIdx = pickBestOverload (mapping, bucket, currentMask);
        if (bindingIdx == InvalidTargetIndex)
            return std::nullopt;

        const auto& binding = mapping.bindings[bindingIdx];
        const auto& target  = ControlTargetRegistry::get (binding.target);

        ResolvedBinding rb {};
        rb.target       = binding.target;
        rb.category     = target.category;
        rb.deckIndex    = target.deckIndex;
        rb.softTakeover = binding.softTakeover;
        rb.intDelta     = 0;
        rb.normalisedValue = 0.0f;

        switch (binding.transform)
        {
            case Transform::Momentary:
            case Transform::Toggle:
            {
                bool on = false;
                if (statusNibble == 0x90u)
                    on = (event.data2 > 0u);
                else if (statusNibble == 0x80u)
                    on = false;
                else
                    on = (event.data2 > 0u);
                rb.normalisedValue = on ? 1.0f : 0.0f;
                break;
            }

            case Transform::Linear:
            {
                rb.normalisedValue = static_cast<float> (event.data2) / 127.0f;
                break;
            }

            case Transform::Linear14:
            {
                if (binding.lsbData1 >= state.lsbCache.size())
                    return std::nullopt;
                const auto lsb = state.lsbCache[binding.lsbData1];
                if (lsb == 0xFFu)
                    return std::nullopt; // no LSB seen yet — drop instead of emitting bogus value
                const std::uint16_t combined = static_cast<std::uint16_t> (
                    (static_cast<std::uint16_t> (event.data2) << 7)
                    | static_cast<std::uint16_t> (lsb));
                rb.normalisedValue = clamp01 (static_cast<float> (combined) / 16383.0f);
                break;
            }

            case Transform::SignedBitDelta:
            {
                int delta = static_cast<int> (event.data2) - 64;
                if (delta < -63) delta = -63;
                if (delta >  63) delta =  63;
                rb.intDelta = static_cast<std::int16_t> (delta);
                break;
            }

            case Transform::TwosComplementDelta:
            {
                const int v = static_cast<int> (event.data2);
                rb.intDelta = static_cast<std::int16_t> (v < 64 ? v : v - 128);
                break;
            }
        }

        return rb;
    }
} // namespace sonik::midi
