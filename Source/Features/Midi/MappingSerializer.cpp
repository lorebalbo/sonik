#include "MappingSerializer.h"

#include "ControlTargetRegistry.h"

#include <bit>

namespace sonik::midi
{
    namespace
    {
        const char* statusNibbleToString (std::uint8_t nibble) noexcept
        {
            switch (nibble)
            {
                case 0x90: return "note";
                case 0xB0: return "cc";
                case 0xE0: return "pitchBend";
                default:   return "cc"; // best-effort fallback; never expected.
            }
        }

        const char* transformToString (Transform t) noexcept
        {
            switch (t)
            {
                case Transform::Momentary:           return "momentary";
                case Transform::Toggle:              return "toggle";
                case Transform::Linear:              return "linear";
                case Transform::Linear14:            return "linear14";
                case Transform::SignedBitDelta:      return "signedBitDelta";
                case Transform::TwosComplementDelta: return "twosComplementDelta";
            }
            return "momentary";
        }

        const char* softTakeoverToString (SoftTakeoverPolicy p) noexcept
        {
            switch (p)
            {
                case SoftTakeoverPolicy::Pickup: return "pickup";
                case SoftTakeoverPolicy::Always: return "always";
                case SoftTakeoverPolicy::Never:  return "never";
            }
            return "pickup";
        }

        const char* modifierStyleToString (ModifierStyle s) noexcept
        {
            return (s == ModifierStyle::Latching) ? "latching" : "momentary";
        }

        juce::DynamicObject* makeMidiBlock (std::uint32_t midiKey, std::uint8_t lsbData1 = 255)
        {
            const auto channel = static_cast<int> ((midiKey >> 16) & 0xFFu);
            const auto status  = static_cast<std::uint8_t> ((midiKey >> 8) & 0xFFu);
            const auto data1   = static_cast<int> (midiKey & 0xFFu);

            auto* obj = new juce::DynamicObject();
            obj->setProperty ("channel", channel);
            obj->setProperty ("status",  juce::String (statusNibbleToString (status)));
            obj->setProperty ("data1",   data1);
            if (lsbData1 < 128)
                obj->setProperty ("data1Lsb", (int) lsbData1);
            return obj;
        }

        juce::String modifierIdForBit (const Mapping& m, std::uint8_t bit)
        {
            if (bit < m.modifierNames.size())
            {
                const auto& name = m.modifierNames[bit];
                if (name.isNotEmpty())
                    return name;
            }
            return "mod" + juce::String ((int) bit);
        }
    }

    juce::var MappingSerializer::serialize (const Mapping& m)
    {
        auto* root = new juce::DynamicObject();
        root->setProperty ("schemaVersion", m.schemaVersion);

        if (m.displayName.isNotEmpty())
            root->setProperty ("displayName", m.displayName);

        // ---- device --------------------------------------------------
        {
            auto* device = new juce::DynamicObject();
            device->setProperty ("manufacturer", m.deviceMatch.manufacturerPattern);
            device->setProperty ("product",      m.deviceMatch.productPattern);
            auto* match = new juce::DynamicObject();
            match->setProperty ("midiName", m.deviceMatch.midiNamePattern);
            device->setProperty ("match", juce::var (match));
            root->setProperty ("device", juce::var (device));
        }

        // ---- modifiers -----------------------------------------------
        {
            juce::Array<juce::var> modsArr;
            for (const auto& mod : m.modifiers)
            {
                auto* obj = new juce::DynamicObject();
                obj->setProperty ("id", modifierIdForBit (m, mod.bit));
                auto* midi = makeMidiBlock (mod.midiKey);
                midi->setProperty ("style", juce::String (modifierStyleToString (mod.style)));
                obj->setProperty ("binding", juce::var (midi));
                modsArr.add (juce::var (obj));
            }
            root->setProperty ("modifiers", modsArr);
        }

        // ---- bindings -----------------------------------------------
        {
            juce::Array<juce::var> bArr;
            for (const auto& b : m.bindings)
            {
                // Skip in-flight rows that have not yet been bound to a
                // concrete target (PRD-0048 Add Binding flow). Writing them
                // would dereference an out-of-range registry slot.
                if (b.target == InvalidTargetIndex
                    || b.target >= ControlTargetRegistry::size())
                    continue;

                auto* obj = new juce::DynamicObject();

                const auto& target = ControlTargetRegistry::get (b.target);
                obj->setProperty ("target", juce::String (target.id));

                obj->setProperty ("midi", juce::var (makeMidiBlock (b.midiKey, b.lsbData1)));
                obj->setProperty ("transform", juce::String (transformToString (b.transform)));

                if (b.requiredModifierMask != 0u)
                {
                    const auto popcnt = std::popcount (b.requiredModifierMask);
                    if (popcnt == 1)
                    {
                        const auto bit = static_cast<std::uint8_t> (std::countr_zero (b.requiredModifierMask));
                        obj->setProperty ("modifier", modifierIdForBit (m, bit));
                    }
                    else
                    {
                        juce::Array<juce::var> idsArr;
                        for (std::uint8_t bit = 0; bit < 32; ++bit)
                        {
                            if ((b.requiredModifierMask & (1u << bit)) != 0u)
                                idsArr.add (modifierIdForBit (m, bit));
                        }
                        obj->setProperty ("modifier", idsArr);
                    }
                }

                if (b.softTakeover != SoftTakeoverPolicy::Pickup)
                    obj->setProperty ("softTakeover", juce::String (softTakeoverToString (b.softTakeover)));

                if (b.feedback.midiKey != 0u)
                {
                    auto* fb = makeMidiBlock (b.feedback.midiKey);
                    fb->setProperty ("onValue",  (int) b.feedback.onValue);
                    fb->setProperty ("offValue", (int) b.feedback.offValue);
                    obj->setProperty ("feedback", juce::var (fb));
                }

                bArr.add (juce::var (obj));
            }
            root->setProperty ("bindings", bArr);
        }

        return juce::var (root);
    }
}
