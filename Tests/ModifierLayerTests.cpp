// Tests for PRD-0046: Modifier Layer (multi-bit, strict equality, latching).

#include "Features/Midi/BindingResolver.h"
#include "Features/Midi/MappingParser.h"
#include "Features/Midi/MidiInboundRouter.h"
#include "Features/Midi/MidiDeviceManager.h"
#include "Features/Midi/MidiMessageBridge.h"
#include "Features/Midi/MappingStore.h"
#include "Features/Midi/MidiHostInterface.h"
#include "Features/Midi/MidiCommandHandler.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <vector>

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

    constexpr MidiInboundEvent makeEvent (std::uint64_t deviceId,
                                          std::uint8_t  status,
                                          std::uint8_t  d1,
                                          std::uint8_t  d2)
    {
        return MidiInboundEvent { deviceId, 0.0, status, d1, d2 };
    }

    class StubHost : public MidiHostInterface
    {
    public:
        juce::Array<juce::MidiDeviceInfo> inputs;
        juce::Array<juce::MidiDeviceInfo> getAvailableInputs()  override { return inputs; }
        juce::Array<juce::MidiDeviceInfo> getAvailableOutputs() override { return {}; }
        std::unique_ptr<juce::MidiInput>  openInputDevice  (const juce::String&,
                                                            juce::MidiInputCallback*) override { return nullptr; }
        std::unique_ptr<juce::MidiOutput> openOutputDevice (const juce::String&) override      { return nullptr; }
    };

    class RecordingHandler final : public MidiCommandHandler
    {
    public:
        std::vector<MidiMessageEvent> events;
        void handle (const MidiMessageEvent& e) override { events.push_back (e); }
    };

    juce::File makeTempDir (const juce::String& suffix)
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("sonik-modlayer-" + suffix
                                            + "-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
        dir.createDirectory();
        return dir;
    }

    bool pump (std::function<bool()> cond, int timeoutMs = 1000)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (! cond())
        {
            if (juce::MessageManager::getInstance() != nullptr)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
            if (juce::Time::getMillisecondCounter() > deadline)
                return cond();
        }
        return true;
    }

    class ModifierLayerTests final : public juce::UnitTest
    {
    public:
        ModifierLayerTests() : juce::UnitTest ("Modifier Layer (PRD-0046)", "Sonik") {}

        void runTest() override
        {
            beginTest ("Momentary modifier: press sets bit, release clears bit");
            {
                auto m = parseOk (R"({
                    "schemaVersion": 1,
                    "modifiers": [
                        { "id": "shift",
                          "binding": { "channel": 1, "status": "note", "data1": 24, "style": "momentary" } }
                    ],
                    "bindings": []
                })");
                ResolverState state;
                const auto press = BindingResolver::resolve (m, state, makeEvent (0, 0x90, 24, 127), 0);
                expect (press.has_value());
                if (press.has_value())
                {
                    expectEquals ((int) press->category, (int) MidiTargetCategory::ModifierSet);
                    expectEquals ((int) press->intDelta, 0);
                }
                const auto release = BindingResolver::resolve (m, state, makeEvent (0, 0x80, 24, 0), 0);
                expect (release.has_value());
                if (release.has_value())
                    expectEquals ((int) release->category, (int) MidiTargetCategory::ModifierClear);
            }

            beginTest ("Latching modifier: press emits Toggle, release ignored");
            {
                auto m = parseOk (R"({
                    "schemaVersion": 1,
                    "modifiers": [
                        { "id": "lock",
                          "binding": { "channel": 1, "status": "note", "data1": 25, "style": "latching" } }
                    ],
                    "bindings": []
                })");
                ResolverState state;
                const auto p1 = BindingResolver::resolve (m, state, makeEvent (0, 0x90, 25, 127), 0);
                expect (p1.has_value());
                if (p1.has_value())
                    expectEquals ((int) p1->category, (int) MidiTargetCategory::ModifierToggle);
                const auto r1 = BindingResolver::resolve (m, state, makeEvent (0, 0x80, 25, 0), 0);
                expect (! r1.has_value());
                const auto p2 = BindingResolver::resolve (m, state, makeEvent (0, 0x90, 25, 127), 0);
                expect (p2.has_value());
                if (p2.has_value())
                    expectEquals ((int) p2->category, (int) MidiTargetCategory::ModifierToggle);
            }

            beginTest ("Strict equality: holding extra modifier does NOT match a less-specific overload");
            {
                auto m = parseOk (R"({
                    "schemaVersion": 1,
                    "modifiers": [
                        { "id": "shift", "binding": { "channel": 1, "status": "note", "data1": 24 } },
                        { "id": "alt",   "binding": { "channel": 1, "status": "note", "data1": 25 } }
                    ],
                    "bindings": [
                        { "target": "deck.A.transport.cue",
                          "midi": { "channel": 1, "status": "note", "data1": 14 },
                          "transform": "momentary",
                          "modifier": "shift" }
                    ]
                })");
                ResolverState state;
                // shift held alone (mask=1) → fires.
                const auto hit = BindingResolver::resolve (m, state, makeEvent (0, 0x90, 14, 127), 0b01u);
                expect (hit.has_value());
                if (hit.has_value())
                    expectEquals ((int) hit->category, (int) MidiTargetCategory::TransportCue);
                // shift+alt held (mask=3) → strict equality fails.
                const auto miss = BindingResolver::resolve (m, state, makeEvent (0, 0x90, 14, 127), 0b11u);
                expect (! miss.has_value());
            }

            beginTest ("Multi-modifier: both must be held");
            {
                auto m = parseOk (R"({
                    "schemaVersion": 1,
                    "modifiers": [
                        { "id": "shift", "binding": { "channel": 1, "status": "note", "data1": 24 } },
                        { "id": "alt",   "binding": { "channel": 1, "status": "note", "data1": 25 } }
                    ],
                    "bindings": [
                        { "target": "deck.A.transport.cue",
                          "midi": { "channel": 1, "status": "note", "data1": 14 },
                          "transform": "momentary",
                          "modifier": ["shift", "alt"] }
                    ]
                })");
                expect (! m.bindings.empty());
                if (! m.bindings.empty())
                    expectEquals ((int) m.bindings.front().requiredModifierMask, 0b11);

                ResolverState state;
                expect (! BindingResolver::resolve (m, state, makeEvent (0, 0x90, 14, 127), 0b01u).has_value());
                expect (! BindingResolver::resolve (m, state, makeEvent (0, 0x90, 14, 127), 0b10u).has_value());
                expect (  BindingResolver::resolve (m, state, makeEvent (0, 0x90, 14, 127), 0b11u).has_value());
            }

            beginTest ("ModifierTargetConflict: same MIDI key as modifier and binding drops both");
            {
                const auto v = juce::JSON::parse (juce::String::fromUTF8 (R"({
                    "schemaVersion": 1,
                    "modifiers": [
                        { "id": "shift",
                          "binding": { "channel": 1, "status": "note", "data1": 24, "style": "momentary" } }
                    ],
                    "bindings": [
                        { "target": "deck.A.transport.play",
                          "midi": { "channel": 1, "status": "note", "data1": 24 },
                          "transform": "momentary" }
                    ]
                })"));
                auto r = MappingParser::parse (v, "test");
                bool sawConflict = false;
                for (const auto& e : r.errors)
                    if (e.kind == ValidationError::Kind::ModifierTargetConflict)
                        sawConflict = true;
                expect (sawConflict);
                expect (r.mapping.modifiers.empty());
                expect (r.mapping.bindings.empty());
            }

            beginTest ("UnknownModifierReference: binding referring to unknown id is dropped");
            {
                const auto v = juce::JSON::parse (juce::String::fromUTF8 (R"({
                    "schemaVersion": 1,
                    "modifiers": [
                        { "id": "shift",
                          "binding": { "channel": 1, "status": "note", "data1": 24 } }
                    ],
                    "bindings": [
                        { "target": "deck.A.transport.cue",
                          "midi": { "channel": 1, "status": "note", "data1": 14 },
                          "transform": "momentary",
                          "modifier": "nope" }
                    ]
                })"));
                auto r = MappingParser::parse (v, "test");
                bool sawUnknown = false;
                for (const auto& e : r.errors)
                    if (e.kind == ValidationError::Kind::UnknownModifierReference)
                        sawUnknown = true;
                expect (sawUnknown);
                expect (r.mapping.bindings.empty());
            }

            beginTest ("Mapping retains original modifier id strings (for serializer round-trip)");
            {
                auto m = parseOk (R"({
                    "schemaVersion": 1,
                    "modifiers": [
                        { "id": "shift", "binding": { "channel": 1, "status": "note", "data1": 24 } },
                        { "id": "alt",   "binding": { "channel": 1, "status": "note", "data1": 25 } }
                    ],
                    "bindings": []
                })");
                expect (m.modifierNames.size() >= 2);
                if (m.modifierNames.size() >= 2)
                {
                    expectEquals (m.modifierNames[0], juce::String ("shift"));
                    expectEquals (m.modifierNames[1], juce::String ("alt"));
                }
            }

            beginTest ("Router: disconnect clears modifier mask");
            {
                StubHost host;
                host.inputs.add (juce::MidiDeviceInfo ("TestDev", "modlayer-dev-1"));
                MidiDeviceManager mgr (host);
                mgr.initialise();
                std::uint64_t deviceId = 0;
                for (const auto& d : mgr.getDevices())
                    if (d.isInput) deviceId = d.deviceId;

                const auto dir = makeTempDir ("disconnect");
                dir.getChildFile ("user.json").replaceWithText (juce::String (R"({
                    "schemaVersion": 1,
                    "device": { "manufacturer": ".*", "product": ".*",
                                "match": { "midiName": ".*" } },
                    "modifiers": [
                        { "id": "shift",
                          "binding": { "channel": 1, "status": "note", "data1": 24, "style": "momentary" } }
                    ],
                    "bindings": []
                })"));

                MidiMessageBridge bridge;
                MappingStore      store (mgr, dir, /*async*/ false);
                RecordingHandler  handler;
                MidiInboundRouter router (mgr, bridge, store, handler);

                router.onMidiInbound (makeEvent (deviceId, 0x90, 24, 127));
                pump ([&] { return router.getModifierMask (deviceId) != 0u; });
                expectEquals ((int) router.getModifierMask (deviceId), 1);

                router.midiDeviceRemoved (deviceId);
                expectEquals ((int) router.getModifierMask (deviceId), 0);

                dir.deleteRecursively();
            }

            beginTest ("Router: active-mapping switch clears modifier mask");
            {
                StubHost host;
                host.inputs.add (juce::MidiDeviceInfo ("TestDev", "modlayer-dev-2"));
                MidiDeviceManager mgr (host);
                mgr.initialise();
                std::uint64_t deviceId = 0;
                for (const auto& d : mgr.getDevices())
                    if (d.isInput) deviceId = d.deviceId;

                const auto dir = makeTempDir ("switch");
                dir.getChildFile ("user.json").replaceWithText (juce::String (R"({
                    "schemaVersion": 1,
                    "device": { "manufacturer": ".*", "product": ".*",
                                "match": { "midiName": ".*" } },
                    "modifiers": [
                        { "id": "shift",
                          "binding": { "channel": 1, "status": "note", "data1": 24, "style": "momentary" } }
                    ],
                    "bindings": []
                })"));

                MidiMessageBridge bridge;
                MappingStore      store (mgr, dir, /*async*/ false);
                RecordingHandler  handler;
                MidiInboundRouter router (mgr, bridge, store, handler);

                router.onMidiInbound (makeEvent (deviceId, 0x90, 24, 127));
                pump ([&] { return router.getModifierMask (deviceId) != 0u; });
                expectEquals ((int) router.getModifierMask (deviceId), 1);

                router.activeMappingChanged (deviceId);
                expectEquals ((int) router.getModifierMask (deviceId), 0);

                dir.deleteRecursively();
            }
        }
    };

    static ModifierLayerTests modifierLayerTests;
} // namespace
