// PRD-0061: Mixer ↔ MIDI Wiring Validation & DDM4000 Profile Activation.
//
// These tests exercise the full inbound + outbound mixer MIDI plumbing:
//   * Bundled Behringer DDM4000 profile parses cleanly (no UnknownTarget)
//     and registers a cue binding per channel with LED feedback.
//   * Continuous mixer controls (channel fader, crossfader) are gated by
//     SoftTakeoverManager pickup mode through MixerMidiHandler.
//   * Per-channel toggle controls (kill*, assign*, cue) flip the matching
//     ValueTree boolean under MidiOriginatedWriteScope.
//   * Outbound LED feedback flows through MidiFeedbackEngine for kill,
//     assign, and cue when the mixer ValueTree state changes.

#include "Features/Midi/ControlTargetRegistry.h"
#include "Features/Midi/MappingStore.h"
#include "Features/Midi/MappingParser.h"
#include "Features/Midi/MappingSerializer.h"
#include "Features/Midi/MidiDeviceManager.h"
#include "Features/Midi/MidiHostInterface.h"
#include "Features/Midi/MidiMessageEvent.h"
#include "Features/Midi/MidiFeedbackEngine.h"
#include "Features/Midi/MidiOutputThread.h"
#include "Features/Midi/SoftTakeoverManager.h"
#include "Features/Midi/UI/Molecules/DeviceHeader.h"
#include "Features/Mixer/State/MixerStateSchema.h"
#include "Features/Mixer/State/MixerIdentifiers.h"
#include "Features/Mixer/State/MixerParam.h"
#include "Features/Deck/DeckIdentifiers.h"
#include "MidiHandlers/MixerMidiHandler.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <cstring>
#include <vector>

namespace
{
    using namespace sonik::midi;

    //--------------------------------------------------------------------------
    class StubHost : public MidiHostInterface
    {
    public:
        juce::Array<juce::MidiDeviceInfo> inputs;
        juce::Array<juce::MidiDeviceInfo> outputs;

        juce::Array<juce::MidiDeviceInfo> getAvailableInputs()  override { return inputs;  }
        juce::Array<juce::MidiDeviceInfo> getAvailableOutputs() override { return outputs; }

        std::unique_ptr<juce::MidiInput>  openInputDevice  (const juce::String&,
                                                            juce::MidiInputCallback*) override { return nullptr; }
        std::unique_ptr<juce::MidiOutput> openOutputDevice (const juce::String&) override      { return nullptr; }
    };

    juce::File makeTempDir (const juce::String& suffix)
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("sonik-mixer-midi-" + suffix
                                            + "-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
        dir.createDirectory();
        return dir;
    }

    //--------------------------------------------------------------------------
    // Helper: build a MidiMessageEvent for `category` + `deckIndex`. The
    // targetIndex is resolved from the registry so any consumer that reads
    // it (e.g. SoftTakeoverManager) finds a valid slot.
    MidiMessageEvent makeEvent (std::uint64_t deviceId,
                                MidiTargetCategory category,
                                std::uint8_t deckIndex,
                                float normalised,
                                SoftTakeoverPolicy policy = SoftTakeoverPolicy::Always)
    {
        MidiMessageEvent e {};
        e.deviceId = deviceId;
        e.category = category;
        e.deckIndex = deckIndex;
        e.normalisedValue = normalised;
        e.intDelta = 0;
        e.softTakeover = policy;
        const auto target = ControlTargetRegistry::findByCategoryAndDeck (category, deckIndex);
        e.targetIndex = target.has_value() ? *target : InvalidTargetIndex;
        return e;
    }

    //--------------------------------------------------------------------------
    class MixerMidiWiringTests final : public juce::UnitTest
    {
    public:
        MixerMidiWiringTests()
            : juce::UnitTest ("Mixer MIDI Wiring (PRD-0061)", "Sonik") {}

        void runTest() override
        {
            beginTest ("Bundled DDM4000 profile is registered and has cue + LED feedback");
            {
                StubHost host;
                MidiDeviceManager mgr (host);
                const auto dir = makeTempDir ("parse");
                MappingStore store (mgr, dir, false);

                const auto ddm = store.getMappingById ("behringer-ddm4000");
                expect (ddm != nullptr,
                        "Bundled DDM4000 profile must be present in MappingStore");
                if (ddm == nullptr)
                {
                    dir.deleteRecursively();
                    return;
                }

                // Every binding's target must reference a registered slot.
                for (const auto& b : ddm->bindings)
                {
                    const auto& t = ControlTargetRegistry::get (b.target);
                    expect (t.id != nullptr && std::strlen (t.id) > 0,
                            "DDM4000 binding references an unregistered target");
                }

                // Exactly four cue bindings (one per channel).
                int cueCount = 0;
                for (const auto& b : ddm->bindings)
                    if (ControlTargetRegistry::get (b.target).category
                            == MidiTargetCategory::ChannelCue)
                        ++cueCount;
                expectEquals (cueCount, 4,
                              "DDM4000 must bind cue/PFL button for all four channels");

                // Every kill/assign/cue binding must declare LED feedback.
                for (const auto& b : ddm->bindings)
                {
                    const auto cat = ControlTargetRegistry::get (b.target).category;
                    const bool needsFb =
                        cat == MidiTargetCategory::ChannelKillHigh
                     || cat == MidiTargetCategory::ChannelKillMid
                     || cat == MidiTargetCategory::ChannelKillLow
                     || cat == MidiTargetCategory::ChannelAssignA
                     || cat == MidiTargetCategory::ChannelAssignB
                     || cat == MidiTargetCategory::ChannelCue;
                    if (needsFb)
                        expect (b.feedback.midiKey != 0,
                                "DDM4000 toggle binding must declare LED feedback");
                }

                dir.deleteRecursively();
            }

            beginTest ("Channel fader: soft-takeover blocks first event then engages after crossing");
            {
                juce::ValueTree root (IDs::SonikState);
                MixerStateSchema schema (root);

                StubHost host;
                MidiDeviceManager mgr (host);
                const auto dir = makeTempDir ("st-fader");
                MappingStore store (mgr, dir, false);
                SoftTakeoverManager takeover (root, store);

                MixerMidiHandler handler (&takeover);
                handler.setMixerStateSchema (&schema);
                handler.setStateTree (root);

                // Force software fader to 0.5 so we can test pickup.
                schema.getChannelTree (0).setProperty (MixerIDs::fader, 0.5f, nullptr);

                // Hardware sends 0.9 first — must NOT take effect under Pickup.
                handler.tryHandle (makeEvent (1, MidiTargetCategory::ChannelFader, 0, 0.9f,
                                              SoftTakeoverPolicy::Pickup));
                const float afterBlocked = static_cast<float> (
                    schema.getChannelTree (0).getProperty (MixerIDs::fader, -1.0f));
                expectWithinAbsoluteError (afterBlocked, 0.5f, 1.0e-4f);

                // Hardware moves down through 0.5 → pickup engages.
                handler.tryHandle (makeEvent (1, MidiTargetCategory::ChannelFader, 0, 0.6f,
                                              SoftTakeoverPolicy::Pickup));
                handler.tryHandle (makeEvent (1, MidiTargetCategory::ChannelFader, 0, 0.4f,
                                              SoftTakeoverPolicy::Pickup));
                handler.tryHandle (makeEvent (1, MidiTargetCategory::ChannelFader, 0, 0.3f,
                                              SoftTakeoverPolicy::Pickup));
                const float afterEngaged = static_cast<float> (
                    schema.getChannelTree (0).getProperty (MixerIDs::fader, -1.0f));
                expect (afterEngaged < 0.5f,
                        "Once hardware crosses the software value the fader must follow");
                dir.deleteRecursively();
            }

            beginTest ("Crossfader: soft-takeover pickup gates the first event");
            {
                juce::ValueTree root (IDs::SonikState);
                MixerStateSchema schema (root);

                StubHost host;
                MidiDeviceManager mgr (host);
                const auto dir = makeTempDir ("st-xf");
                MappingStore store (mgr, dir, false);
                SoftTakeoverManager takeover (root, store);

                MixerMidiHandler handler (&takeover);
                handler.setMixerStateSchema (&schema);
                handler.setStateTree (root);

                // Default crossfader is 0.5. Hardware at 1.0 → blocked.
                handler.tryHandle (makeEvent (1, MidiTargetCategory::Crossfader, GlobalDeckIndex, 1.0f,
                                              SoftTakeoverPolicy::Pickup));
                const float xfBlocked = static_cast<float> (
                    schema.getMixerTree().getProperty (MixerIDs::crossfader, -1.0f));
                expectWithinAbsoluteError (xfBlocked, 0.5f, 1.0e-4f);

                // Hardware moves down past 0.5 → pickup engages.
                handler.tryHandle (makeEvent (1, MidiTargetCategory::Crossfader, GlobalDeckIndex, 0.4f,
                                              SoftTakeoverPolicy::Pickup));
                handler.tryHandle (makeEvent (1, MidiTargetCategory::Crossfader, GlobalDeckIndex, 0.7f,
                                              SoftTakeoverPolicy::Pickup));
                const float xfEngaged = static_cast<float> (
                    schema.getMixerTree().getProperty (MixerIDs::crossfader, -1.0f));
                expect (xfEngaged > 0.5f,
                        "Crossfader must engage after hardware crosses software value");
                dir.deleteRecursively();
            }

            beginTest ("Per-channel toggles flip ValueTree booleans (kill/assign/cue)");
            {
                juce::ValueTree root (IDs::SonikState);
                MixerStateSchema schema (root);
                MixerMidiHandler handler;
                handler.setMixerStateSchema (&schema);
                handler.setStateTree (root);

                auto press = [&] (MidiTargetCategory cat, int ch)
                {
                    handler.tryHandle (makeEvent (1, cat, static_cast<std::uint8_t> (ch), 1.0f));
                };

                // killHigh on channel B: defaults false → true.
                press (MidiTargetCategory::ChannelKillHigh, 1);
                expect (static_cast<bool> (
                    schema.getChannelEqTree (1).getProperty (MixerIDs::killHigh, false)));

                // assignA on channel A: default is true → flips to false.
                const bool aBefore = static_cast<bool> (
                    schema.getChannelTree (0).getProperty (MixerIDs::assignA, false));
                press (MidiTargetCategory::ChannelAssignA, 0);
                const bool aAfter = static_cast<bool> (
                    schema.getChannelTree (0).getProperty (MixerIDs::assignA, false));
                expect (aAfter != aBefore,
                        "assignA toggle should flip the channel-A boolean");

                // cue on channel C: defaults false → true, then false again.
                press (MidiTargetCategory::ChannelCue, 2);
                expect (static_cast<bool> (
                    schema.getChannelTree (2).getProperty (MixerIDs::cue, false)));
                press (MidiTargetCategory::ChannelCue, 2);
                expect (! static_cast<bool> (
                    schema.getChannelTree (2).getProperty (MixerIDs::cue, false)));
            }

            beginTest ("MidiFeedbackEngine emits LED on cue / assign / kill ValueTree change");
            {
                // Mapping mirrors the DDM4000 shape: kill/assign/cue bindings
                // each declare binary feedback. Dropped into the user dir so
                // MappingStore resolves it for the single synthetic device.
                const juce::String json = R"({
                    "schemaVersion": 1,
                    "device": { "manufacturer": ".*", "product": ".*",
                                "match": { "midiName": ".*" } },
                    "modifiers": [],
                    "bindings": [
                        { "target": "mixer.channel.A.cue",
                          "midi": { "channel": 1, "status": "note", "data1": 3 },
                          "transform": "momentary",
                          "feedback": { "style": "binary", "channel": 1, "status": "note",
                                        "data1": 3, "onValue": 127, "offValue": 0 } },
                        { "target": "mixer.channel.B.assignA",
                          "midi": { "channel": 1, "status": "note", "data1": 34 },
                          "transform": "momentary",
                          "feedback": { "style": "binary", "channel": 1, "status": "note",
                                        "data1": 34, "onValue": 127, "offValue": 0 } },
                        { "target": "mixer.channel.C.eq.killHigh",
                          "midi": { "channel": 1, "status": "note", "data1": 8 },
                          "transform": "momentary",
                          "feedback": { "style": "binary", "channel": 1, "status": "note",
                                        "data1": 8, "onValue": 127, "offValue": 0 } }
                    ]
                })";

                juce::ValueTree root (IDs::SonikState);
                MixerStateSchema schema (root);

                StubHost host;
                host.inputs.add  (juce::MidiDeviceInfo ("MixerFb", "mixerfb-in"));
                host.outputs.add (juce::MidiDeviceInfo ("MixerFb", "mixerfb-out"));

                MidiDeviceManager mgr (host);
                const auto dir = makeTempDir ("fb");
                dir.getChildFile ("mixer-fb.json").replaceWithText (json);

                MappingStore store (mgr, dir, false);
                mgr.initialise();

                SoftTakeoverManager takeover (root, store);
                MidiFeedbackEngine engine (root, mgr, store, takeover, /*testTap*/ true);

                // Drain the boot dump.
                {
                    OutboundMidiEvent drop {};
                    while (engine.drainOneForTest (drop)) {}
                }

                // Drive the ValueTree: each write should emit one LED.
                schema.getChannelTree (0).setProperty (MixerIDs::cue, true, nullptr);
                schema.getChannelTree (1).setProperty (MixerIDs::assignA, true, nullptr);
                schema.getChannelEqTree (2).setProperty (MixerIDs::killHigh, true, nullptr);

                std::vector<OutboundMidiEvent> evs;
                OutboundMidiEvent ev {};
                while (engine.drainOneForTest (ev))
                    evs.push_back (ev);

                auto lastValue = [&] (std::uint8_t status, std::uint8_t data1) -> int
                {
                    int last = -1;
                    for (const auto& e : evs)
                        if (e.status == status && e.data1 == data1)
                            last = static_cast<int> (e.value);
                    return last;
                };

                expectEquals (lastValue (0x90, 3),  127); // cue.A on
                expectEquals (lastValue (0x90, 34), 127); // assignA.B on
                expectEquals (lastValue (0x90, 8),  127); // killHigh.C on

                dir.deleteRecursively();
            }

            beginTest ("Newly-learned binding without softTakeover field defaults to Always");
            {
                // A binding JSON object that omits the softTakeover field must
                // parse to SoftTakeoverPolicy::Always so a freshly-learned
                // hardware control writes through on the very first event.
                const juce::String json = R"({
                    "schemaVersion": 1,
                    "device": { "manufacturer": ".*", "product": ".*",
                                "match": { "midiName": ".*" } },
                    "modifiers": [],
                    "bindings": [
                        { "target": "mixer.channel.A.gain",
                          "midi": { "channel": 1, "status": "cc", "data1": 50 },
                          "transform": "linear" }
                    ]
                })";

                const auto var = juce::JSON::parse (json);
                const auto parsed = MappingParser::parse (var, "default-takeover.json");
                expectEquals ((int) parsed.errors.size(), 0,
                              "JSON without softTakeover must parse without errors");
                expectEquals ((int) parsed.mapping.bindings.size(), 1);
                expect (parsed.mapping.bindings[0].softTakeover
                            == SoftTakeoverPolicy::Always,
                        "Default soft-takeover for new bindings must be Always");
            }

            beginTest ("Round-trip of an explicit Always binding stays stable");
            {
                const juce::String json = R"({
                    "schemaVersion": 1,
                    "device": { "manufacturer": ".*", "product": ".*",
                                "match": { "midiName": ".*" } },
                    "modifiers": [],
                    "bindings": [
                        { "target": "mixer.channel.A.gain",
                          "midi": { "channel": 1, "status": "cc", "data1": 50 },
                          "transform": "linear",
                          "softTakeover": "always" }
                    ]
                })";

                const auto var    = juce::JSON::parse (json);
                const auto first  = MappingParser::parse (var, "always-rt.json");
                expect (first.mapping.bindings.size() == 1);
                expect (first.mapping.bindings[0].softTakeover
                            == SoftTakeoverPolicy::Always);

                const auto serialised = MappingSerializer::serialize (first.mapping);
                const auto second = MappingParser::parse (serialised, "always-rt-2.json");
                expect (second.mapping.bindings.size() == 1);
                expect (second.mapping.bindings[0].softTakeover
                            == SoftTakeoverPolicy::Always,
                        "Round-trip of an Always binding must remain Always");
            }

            beginTest ("Always-policy event writes the ValueTree on the first sample");
            {
                // Replicates the "MIDI-mapped knob produces no UI feedback"
                // bug: with the new Always default, even the first hardware
                // event must commit to the ValueTree regardless of how far
                // the hardware value sits from the current software value.
                juce::ValueTree root (IDs::SonikState);
                MixerStateSchema schema (root);

                StubHost host;
                MidiDeviceManager mgr (host);
                const auto dir = makeTempDir ("st-always");
                MappingStore store (mgr, dir, false);
                SoftTakeoverManager takeover (root, store);

                MixerMidiHandler handler (&takeover);
                handler.setMixerStateSchema (&schema);
                handler.setStateTree (root);

                // Software gain knob defaults to 0 dB (norm 0.5). Hardware
                // emits a value far away from that; with Always policy the
                // very first event must be written through.
                handler.tryHandle (makeEvent (1, MidiTargetCategory::ChannelGain, 0,
                                              0.9f, SoftTakeoverPolicy::Always));
                const float gainDb = static_cast<float> (
                    schema.getChannelTree (0).getProperty (MixerIDs::gain, -999.0f));
                expect (gainDb > 0.0f,
                        "Always policy must commit the first event to the ValueTree, "
                        "got gain dB = " + juce::String (gainDb, 3));

                dir.deleteRecursively();
            }

            beginTest ("Learn-default transform: CC promotes Momentary → Linear, "
                       "Pitch-Bend → Linear14, Note untouched");
            {
                using sonik::midi::ui::DeviceHeader;

                // Build a packed midi key: (status<<8) | data1, with channel
                // baked into the low nibble of status as the parser does.
                auto pack = [] (std::uint8_t status, std::uint8_t data1) noexcept
                {
                    return ((std::uint32_t) status << 8) | (std::uint32_t) data1;
                };

                // CC #23 on channel 1 → status 0xB0.
                expect (DeviceHeader::inferDefaultTransform (pack (0xB0u, 23u),
                                                              Transform::Momentary)
                            == Transform::Linear,
                        "CC status byte must promote Momentary to Linear");

                // Pitch-Bend on channel 1 → status 0xE0.
                expect (DeviceHeader::inferDefaultTransform (pack (0xE0u, 0u),
                                                              Transform::Momentary)
                            == Transform::Linear14,
                        "Pitch-Bend status byte must promote Momentary to Linear14");

                // Note On on channel 1 → status 0x90: stays Momentary.
                expect (DeviceHeader::inferDefaultTransform (pack (0x90u, 60u),
                                                              Transform::Momentary)
                            == Transform::Momentary,
                        "Note On must keep Momentary (button semantics)");

                // Already-explicit choice must never be clobbered.
                expect (DeviceHeader::inferDefaultTransform (pack (0xB0u, 23u),
                                                              Transform::Toggle)
                            == Transform::Toggle,
                        "Explicit Transform::Toggle must be preserved on CC");
                expect (DeviceHeader::inferDefaultTransform (pack (0x90u, 60u),
                                                              Transform::Linear)
                            == Transform::Linear,
                        "Explicit Transform::Linear must be preserved on Note");
            }

            beginTest ("Learned CC drives full knob range when transform=Linear "
                       "(regression: knob would pin to max when Momentary)");
            {
                juce::ValueTree root (IDs::SonikState);
                MixerStateSchema schema (root);

                StubHost host;
                MidiDeviceManager mgr (host);
                const auto dir = makeTempDir ("learn-linear");
                MappingStore store (mgr, dir, false);
                SoftTakeoverManager takeover (root, store);

                MixerMidiHandler handler (&takeover);
                handler.setMixerStateSchema (&schema);
                handler.setStateTree (root);

                // Always policy so the very first event commits and we can
                // probe min/mid/max in isolation.
                auto sendCc7Norm = [&] (float norm)
                {
                    handler.tryHandle (makeEvent (1, MidiTargetCategory::ChannelGain, 0,
                                                  juce::jlimit (0.0f, 1.0f, norm),
                                                  SoftTakeoverPolicy::Always));
                };

                // data2 = 0  → norm 0.0   → expect ≈ -60 dB (gain floor).
                sendCc7Norm (0.0f);
                const float gainMin = static_cast<float> (
                    schema.getChannelTree (0).getProperty (MixerIDs::gain, 999.0f));
                expect (gainMin < -40.0f,
                        "CC data2=0 with Linear must drive gain to its floor, got "
                        + juce::String (gainMin, 3) + " dB");

                // data2 = 64 → norm ~0.504 → expect ≈ 0 dB (unity).
                sendCc7Norm (64.0f / 127.0f);
                const float gainMid = static_cast<float> (
                    schema.getChannelTree (0).getProperty (MixerIDs::gain, 999.0f));
                expectWithinAbsoluteError (gainMid, 0.0f, 1.5f);

                // data2 = 127 → norm 1.0  → expect ≈ +12 dB (ceiling).
                sendCc7Norm (1.0f);
                const float gainMax = static_cast<float> (
                    schema.getChannelTree (0).getProperty (MixerIDs::gain, -999.0f));
                expect (gainMax > 6.0f,
                        "CC data2=127 with Linear must drive gain near the ceiling, got "
                        + juce::String (gainMax, 3) + " dB");

                // Bonus: the three observed values must form a strictly
                // increasing sequence — proves the knob "moves" rather than
                // snapping to a single binary value, which is the user-visible
                // regression we are guarding against.
                expect (gainMin < gainMid && gainMid < gainMax,
                        "Channel gain must be monotonically increasing across "
                        "the CC sweep; instead got "
                        + juce::String (gainMin, 2) + ", "
                        + juce::String (gainMid, 2) + ", "
                        + juce::String (gainMax, 2));

                dir.deleteRecursively();
            }
        }
    };

    static MixerMidiWiringTests mixerMidiWiringTests;
} // namespace
