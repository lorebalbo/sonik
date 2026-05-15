// Tests for PRD-0042: BindingResolver.

#include "Features/Midi/BindingResolver.h"
#include "Features/Midi/MappingParser.h"
#include "Features/Midi/ControlTargetRegistry.h"

#include <juce_core/juce_core.h>

#include <cmath>

namespace
{
    using namespace sonik::midi;

    Mapping parseOk (const char* json)
    {
        const auto v = juce::JSON::parse (juce::String::fromUTF8 (json));
        auto r = MappingParser::parse (v, "test");
        jassert (r.ok());
        return std::move (r.mapping);
    }

    constexpr MidiInboundEvent makeEvent (std::uint8_t status, std::uint8_t d1, std::uint8_t d2)
    {
        return MidiInboundEvent { 0u, 0.0, status, d1, d2 };
    }

    class BindingResolverTests final : public juce::UnitTest
    {
    public:
        BindingResolverTests() : juce::UnitTest ("Binding Resolver (PRD-0042)", "Sonik") {}

        void runTest() override
        {
            beginTest ("Momentary transform: NoteOn vel>0 -> 1.0, NoteOff -> 0.0");
            {
                auto m = parseOk (R"({
                    "schemaVersion": 1,
                    "bindings": [{
                        "target": "deck.A.transport.play",
                        "midi": { "channel": 1, "status": "note", "data1": 14 },
                        "transform": "momentary"
                    }]
                })");
                ResolverState state;
                const auto on = BindingResolver::resolve (m, state, makeEvent (0x90, 14, 127), 0);
                expect (on.has_value());
                if (on.has_value())
                {
                    expectEquals (on->normalisedValue, 1.0f);
                    expectEquals ((int) on->category, (int) MidiTargetCategory::TransportPlay);
                    expectEquals ((int) on->deckIndex, 0);
                }
                const auto off = BindingResolver::resolve (m, state, makeEvent (0x80, 14, 0), 0);
                expect (off.has_value());
                if (off.has_value())
                    expectEquals (off->normalisedValue, 0.0f);

                // Unmapped key
                const auto miss = BindingResolver::resolve (m, state, makeEvent (0x90, 99, 127), 0);
                expect (! miss.has_value());
            }

            beginTest ("Linear transform: value/127");
            {
                auto m = parseOk (R"({
                    "schemaVersion": 1,
                    "bindings": [{
                        "target": "deck.A.gain",
                        "midi": { "channel": 1, "status": "cc", "data1": 7 },
                        "transform": "linear"
                    }]
                })");
                ResolverState state;
                const auto r = BindingResolver::resolve (m, state, makeEvent (0xB0, 7, 64), 0);
                expect (r.has_value());
                if (r.has_value())
                    expect (std::abs (r->normalisedValue - (64.0f / 127.0f)) < 1e-6f);
            }

            beginTest ("Linear14 transform: combines MSB + cached LSB");
            {
                auto m = parseOk (R"({
                    "schemaVersion": 1,
                    "bindings": [{
                        "target": "deck.A.pitchFader",
                        "midi": { "channel": 1, "status": "cc", "data1": 9, "data1Lsb": 41 },
                        "transform": "linear14"
                    }]
                })");
                ResolverState state;

                // Without LSB seen, MSB-only should drop (no value).
                const auto noLsb = BindingResolver::resolve (m, state, makeEvent (0xB0, 9, 64), 0);
                expect (! noLsb.has_value());

                // Push LSB first; resolver returns nullopt (cache update).
                const auto lsbUpdate = BindingResolver::resolve (m, state, makeEvent (0xB0, 41, 100), 0);
                expect (! lsbUpdate.has_value());

                // Now the MSB triggers a real value.
                const auto val = BindingResolver::resolve (m, state, makeEvent (0xB0, 9, 64), 0);
                expect (val.has_value());
                if (val.has_value())
                {
                    const float expected = ((64 << 7) | 100) / 16383.0f;
                    expect (std::abs (val->normalisedValue - expected) < 1e-6f);
                }
            }

            beginTest ("SignedBitDelta: value-64 clamped to [-63,63]");
            {
                auto m = parseOk (R"({
                    "schemaVersion": 1,
                    "bindings": [{
                        "target": "deck.A.jog.scratch",
                        "midi": { "channel": 1, "status": "cc", "data1": 30 },
                        "transform": "signedBitDelta"
                    }]
                })");
                ResolverState state;
                const auto fwd = BindingResolver::resolve (m, state, makeEvent (0xB0, 30, 65), 0);
                expect (fwd.has_value());
                if (fwd.has_value()) expectEquals ((int) fwd->intDelta, 1);

                const auto rev = BindingResolver::resolve (m, state, makeEvent (0xB0, 30, 60), 0);
                expect (rev.has_value());
                if (rev.has_value()) expectEquals ((int) rev->intDelta, -4);

                const auto big = BindingResolver::resolve (m, state, makeEvent (0xB0, 30, 127), 0);
                expect (big.has_value());
                if (big.has_value()) expectEquals ((int) big->intDelta, 63);
            }

            beginTest ("TwosComplementDelta: v<64 -> v, else v-128");
            {
                auto m = parseOk (R"({
                    "schemaVersion": 1,
                    "bindings": [{
                        "target": "deck.A.jog.bend",
                        "midi": { "channel": 1, "status": "cc", "data1": 31 },
                        "transform": "twosComplementDelta"
                    }]
                })");
                ResolverState state;
                const auto fwd = BindingResolver::resolve (m, state, makeEvent (0xB0, 31, 1), 0);
                expect (fwd.has_value());
                if (fwd.has_value()) expectEquals ((int) fwd->intDelta, 1);

                const auto rev = BindingResolver::resolve (m, state, makeEvent (0xB0, 31, 127), 0);
                expect (rev.has_value());
                if (rev.has_value()) expectEquals ((int) rev->intDelta, -1);
            }

            beginTest ("Modifier overload: SHIFT picks the modifier-required binding");
            {
                auto m = parseOk (R"({
                    "schemaVersion": 1,
                    "modifiers": [
                        { "id": "shift", "binding": { "channel": 1, "status": "note", "data1": 24 } }
                    ],
                    "bindings": [
                        { "target": "deck.A.transport.play",
                          "midi": { "channel": 1, "status": "note", "data1": 14 },
                          "transform": "momentary" },
                        { "target": "deck.A.transport.cue",
                          "midi": { "channel": 1, "status": "note", "data1": 14 },
                          "modifier": "shift",
                          "transform": "momentary" }
                    ]
                })");
                ResolverState state;

                // No modifier: play.
                const auto plain = BindingResolver::resolve (m, state, makeEvent (0x90, 14, 127), 0);
                expect (plain.has_value());
                if (plain.has_value())
                    expectEquals ((int) plain->category, (int) MidiTargetCategory::TransportPlay);

                // SHIFT held: cue.
                const auto shifted = BindingResolver::resolve (m, state, makeEvent (0x90, 14, 127), 1u);
                expect (shifted.has_value());
                if (shifted.has_value())
                    expectEquals ((int) shifted->category, (int) MidiTargetCategory::TransportCue);
            }

            beginTest ("Modifier press emits ModifierSet, release emits ModifierClear");
            {
                auto m = parseOk (R"({
                    "schemaVersion": 1,
                    "modifiers": [
                        { "id": "shift", "binding": { "channel": 1, "status": "note", "data1": 24, "style": "momentary" } }
                    ],
                    "bindings": []
                })");
                ResolverState state;
                const auto press = BindingResolver::resolve (m, state, makeEvent (0x90, 24, 127), 0);
                expect (press.has_value());
                if (press.has_value())
                {
                    expectEquals ((int) press->category, (int) MidiTargetCategory::ModifierSet);
                    expectEquals ((int) press->intDelta, 0);
                }
                const auto release = BindingResolver::resolve (m, state, makeEvent (0x80, 24, 0), 0);
                expect (release.has_value());
                if (release.has_value())
                    expectEquals ((int) release->category, (int) MidiTargetCategory::ModifierClear);
            }

            beginTest ("Latching modifier emits ModifierToggle only on press edge");
            {
                auto m = parseOk (R"({
                    "schemaVersion": 1,
                    "modifiers": [
                        { "id": "lock", "binding": { "channel": 1, "status": "note", "data1": 24, "style": "latching" } }
                    ],
                    "bindings": []
                })");
                ResolverState state;
                const auto press = BindingResolver::resolve (m, state, makeEvent (0x90, 24, 127), 0);
                expect (press.has_value());
                if (press.has_value())
                    expectEquals ((int) press->category, (int) MidiTargetCategory::ModifierToggle);
                const auto release = BindingResolver::resolve (m, state, makeEvent (0x80, 24, 0), 0);
                expect (! release.has_value());
            }

            beginTest ("PODs are trivially copyable");
            {
                expect (std::is_trivially_copyable_v<ResolvedBinding>);
                expect (std::is_trivially_copyable_v<Binding>);
                expect (std::is_trivially_copyable_v<Modifier>);
                expect (std::is_trivially_copyable_v<MidiInboundEvent>);
            }
        }
    };

    static BindingResolverTests bindingResolverTests;
} // namespace
