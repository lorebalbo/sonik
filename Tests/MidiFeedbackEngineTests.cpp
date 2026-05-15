// PRD-0047: MidiFeedbackEngine and MidiOutputThread unit tests.
//
// Strategy: build a real MidiDeviceManager + MappingStore over a StubHost
// returning one synthetic input device. Drop a JSON mapping file (matching
// any device via ".*" regexes) into the user-mapping dir so MappingStore
// resolves it as the active mapping. Drive the engine via ValueTree
// property writes and inspect a side-channel test tap populated by
// `enqueueOutbound`. The tap is independent of the MidiOutputThread, so
// inspection is race-free.

#include "Features/Midi/MidiFeedbackEngine.h"
#include "Features/Midi/MidiOutputThread.h"
#include "Features/Midi/MappingStore.h"
#include "Features/Midi/MappingParser.h"
#include "Features/Midi/MidiDeviceManager.h"
#include "Features/Midi/MidiHostInterface.h"
#include "Features/Midi/SoftTakeoverManager.h"
#include "Features/Midi/ControlTargetRegistry.h"
#include "Features/Deck/DeckIdentifiers.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <chrono>
#include <thread>

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
                             .getChildFile ("sonik-feedback-" + suffix
                                            + "-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
        dir.createDirectory();
        return dir;
    }

    void pumpFor (int ms)
    {
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
        while (juce::Time::getMillisecondCounter() < end)
        {
            if (juce::MessageManager::getInstance() != nullptr)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
        }
    }

    juce::String allDevicesProfileJson()
    {
        return R"({
            "schemaVersion": 1,
            "device": { "manufacturer": ".*", "product": ".*", "match": { "midiName": ".*" } },
            "modifiers": [],
            "bindings": [
                { "target": "deck.A.transport.play",
                  "midi": { "channel": 1, "status": "note", "data1": 22 },
                  "transform": "momentary",
                  "feedback": { "style": "binary", "channel": 1, "status": "note", "data1": 22,
                                "onValue": 127, "offValue": 0 } },
                { "target": "deck.A.transport.cue",
                  "midi": { "channel": 1, "status": "note", "data1": 23 },
                  "transform": "momentary",
                  "feedback": { "style": "binary", "channel": 1, "status": "note", "data1": 23,
                                "onValue": 100, "offValue": 5 } },
                { "target": "deck.A.transport.sync",
                  "midi": { "channel": 1, "status": "note", "data1": 24 },
                  "transform": "momentary",
                  "feedback": { "style": "binary", "channel": 1, "status": "note", "data1": 24,
                                "onValue": 127, "offValue": 0 } },
                { "target": "deck.A.loop.toggle",
                  "midi": { "channel": 1, "status": "note", "data1": 34 },
                  "transform": "toggle",
                  "feedback": { "style": "binary", "channel": 1, "status": "note", "data1": 34,
                                "onValue": 127, "offValue": 0 } },
                { "target": "deck.A.hotcue.1.trigger",
                  "midi": { "channel": 1, "status": "note", "data1": 48 },
                  "transform": "momentary",
                  "feedback": { "style": "colour", "channel": 1, "status": "note", "data1": 48,
                                "offValue": 0,
                                "palette": { "0": 0, "1": 10, "2": 20, "3": 30, "4": 48, "5": 60,
                                             "6": 70, "7": 80, "8": 90, "9": 100, "10": 110,
                                             "11": 115, "12": 120, "13": 122, "14": 124, "15": 126 } } },
                { "target": "deck.A.pitchFader",
                  "midi": { "channel": 1, "status": "cc", "data1": 9 },
                  "transform": "linear",
                  "softTakeover": "pickup",
                  "feedback": { "style": "continuous", "channel": 1, "status": "cc", "data1": 100,
                                "curve": "linear" },
                  "disengagedFeedback": { "style": "binary", "channel": 1, "status": "note", "data1": 25,
                                          "onValue": 127, "offValue": 0, "blinkHz": 4.0 } }
            ]
        })";
    }

    struct Fixture
    {
        StubHost host;
        MidiDeviceManager deviceManager { host };
        juce::File userDir;
        std::unique_ptr<MappingStore> store;
        juce::ValueTree root  { IDs::SonikState };
        juce::ValueTree decks { IDs::Decks };
        juce::ValueTree deckA { IDs::Deck };
        juce::ValueTree loopA { IDs::Loop };
        juce::ValueTree cuesA { IDs::CuePoints };
        std::unique_ptr<SoftTakeoverManager> takeover;
        std::unique_ptr<MidiFeedbackEngine>  engine;

        juce::MidiDeviceInfo inputInfo  { "Acme Test Controller", "acme-input-1" };
        juce::MidiDeviceInfo outputInfo { "Acme Test Controller", "acme-output-1" };

        std::uint64_t deviceId = 0;

        explicit Fixture (const juce::String& suffix = "default",
                          const juce::String& profileJson = allDevicesProfileJson())
        {
            root.addChild (decks, -1, nullptr);
            decks.addChild (deckA, -1, nullptr);
            deckA.setProperty (IDs::id, "A", nullptr);
            deckA.setProperty (IDs::playbackStatus, "paused", nullptr);
            deckA.setProperty (IDs::pitch, 0.5f, nullptr);
            deckA.setProperty (IDs::isSynced, false, nullptr);
            deckA.addChild (loopA, -1, nullptr);
            loopA.setProperty (IDs::active, false, nullptr);
            deckA.addChild (cuesA, -1, nullptr);
            for (int i = 0; i < 8; ++i)
            {
                juce::ValueTree cp (IDs::CuePoint);
                cp.setProperty (IDs::isValid, false, nullptr);
                cp.setProperty (IDs::colorIndex, 0, nullptr);
                cuesA.addChild (cp, -1, nullptr);
            }

            userDir = makeTempDir (suffix);
            userDir.getChildFile ("test-profile.json").replaceWithText (profileJson);

            store = std::make_unique<MappingStore> (deviceManager, userDir, false);
            takeover = std::make_unique<SoftTakeoverManager> (root, *store);

            host.inputs.add (inputInfo);
            host.outputs.add (outputInfo);
            deviceManager.initialise();

            engine = std::make_unique<MidiFeedbackEngine> (root, deviceManager, *store, *takeover, true);

            for (const auto& d : deviceManager.getDevices())
                if (d.isInput)
                    deviceId = d.deviceId;
        }

        ~Fixture()
        {
            engine.reset();
            takeover.reset();
            store.reset();
            userDir.deleteRecursively();
        }

        std::vector<OutboundMidiEvent> drainAll()
        {
            std::vector<OutboundMidiEvent> out;
            OutboundMidiEvent ev {};
            while (engine->drainOneForTest (ev))
                out.push_back (ev);
            return out;
        }

        int lastValueFor (std::uint8_t channel, std::uint8_t status, std::uint8_t data1,
                          const std::vector<OutboundMidiEvent>& evs)
        {
            int last = -1;
            for (const auto& e : evs)
                if (e.channel == channel && e.status == status && e.data1 == data1)
                    last = static_cast<int> (e.value);
            return last;
        }

        TargetIndex pitchTargetA() const
        {
            return ControlTargetRegistry::findByCategoryAndDeck (
                       MidiTargetCategory::PitchFader, 0).value();
        }
    };

    //--------------------------------------------------------------------------
    class MidiFeedbackEngineTests final : public juce::UnitTest
    {
    public:
        MidiFeedbackEngineTests()
            : juce::UnitTest ("MIDI Feedback Engine (PRD-0047)", "Sonik") {}

        void runTest() override
        {
            beginTest ("Boot dump enqueues feedback for every binding on the active mapping");
            {
                Fixture f ("boot");
                auto evs = f.drainAll();
                // Six feedback-bearing bindings → at least six events.
                expect (evs.size() >= 6);
                expectEquals (f.lastValueFor (1, 0x90, 22, evs), 0);   // play LED off (paused)
                expectEquals (f.lastValueFor (1, 0x90, 23, evs), 100); // cue LED on (paused)
            }

            beginTest ("Binary play/cue LEDs follow playbackStatus");
            {
                Fixture f ("binary");
                f.drainAll();
                f.deckA.setProperty (IDs::playbackStatus, "playing", nullptr);
                auto evs = f.drainAll();
                expectEquals (f.lastValueFor (1, 0x90, 22, evs), 127);
                expectEquals (f.lastValueFor (1, 0x90, 23, evs), 5);

                f.deckA.setProperty (IDs::playbackStatus, "paused", nullptr);
                evs = f.drainAll();
                expectEquals (f.lastValueFor (1, 0x90, 22, evs), 0);
                expectEquals (f.lastValueFor (1, 0x90, 23, evs), 100);
            }

            beginTest ("Sync LED follows isSynced");
            {
                Fixture f ("sync");
                f.drainAll();
                f.deckA.setProperty (IDs::isSynced, true, nullptr);
                auto evs = f.drainAll();
                expectEquals (f.lastValueFor (1, 0x90, 24, evs), 127);
                f.deckA.setProperty (IDs::isSynced, false, nullptr);
                evs = f.drainAll();
                expectEquals (f.lastValueFor (1, 0x90, 24, evs), 0);
            }

            beginTest ("Loop LED follows Loop.active");
            {
                Fixture f ("loop");
                f.drainAll();
                f.loopA.setProperty (IDs::active, true, nullptr);
                auto evs = f.drainAll();
                expectEquals (f.lastValueFor (1, 0x90, 34, evs), 127);
                f.loopA.setProperty (IDs::active, false, nullptr);
                evs = f.drainAll();
                expectEquals (f.lastValueFor (1, 0x90, 34, evs), 0);
            }

            beginTest ("Colour style: hot-cue palette index → velocity table");
            {
                Fixture f ("colour");
                f.drainAll();
                auto cue1 = f.cuesA.getChild (0);
                cue1.setProperty (IDs::colorIndex, 4, nullptr);
                cue1.setProperty (IDs::isValid, true, nullptr);
                auto evs = f.drainAll();
                expectEquals (f.lastValueFor (1, 0x90, 48, evs), 48);

                cue1.setProperty (IDs::isValid, false, nullptr);
                evs = f.drainAll();
                expectEquals (f.lastValueFor (1, 0x90, 48, evs), 0);
            }

            beginTest ("Continuous style: linear curve produces 0..127 CC");
            {
                Fixture f ("cont");
                f.drainAll();
                f.deckA.setProperty (IDs::pitch, 1.0f, nullptr);
                auto evs = f.drainAll();
                expectEquals (f.lastValueFor (1, 0xB0, 100, evs), 127);
                f.deckA.setProperty (IDs::pitch, 0.0f, nullptr);
                evs = f.drainAll();
                expectEquals (f.lastValueFor (1, 0xB0, 100, evs), 0);
                f.deckA.setProperty (IDs::pitch, 0.5f, nullptr);
                evs = f.drainAll();
                expectEquals (f.lastValueFor (1, 0xB0, 100, evs), 64);
            }

            beginTest ("Device epoch bump tags subsequent enqueues with new epoch");
            {
                Fixture f ("epoch");
                f.drainAll();
                const auto epoch0 = f.engine->outputThreadForTest().currentDeviceEpoch (f.deviceId);
                f.engine->outputThreadForTest().bumpDeviceEpoch (f.deviceId);
                const auto epoch1 = f.engine->outputThreadForTest().currentDeviceEpoch (f.deviceId);
                expect (epoch1 == epoch0 + 1);
                f.deckA.setProperty (IDs::isSynced, true, nullptr);
                auto evs = f.drainAll();
                bool foundNew = false;
                for (const auto& e : evs)
                    if (e.deviceId == f.deviceId && e.deviceEpoch == epoch1)
                        foundNew = true;
                expect (foundNew);
            }

            beginTest ("Disengaged feedback blink is started on takeover transition to Disengaged");
            {
                Fixture f ("blink");
                const auto target = f.pitchTargetA();
                f.takeover->forceEngage (f.deviceId, target, 0.5f, 0.5f);
                expect (! f.engine->isBlinkingForTest (f.deviceId, target));

                // Non-MIDI write to pitch resets the binding → Disengaged.
                f.deckA.setProperty (IDs::pitch, 0.6f, nullptr);
                expect (f.engine->isBlinkingForTest (f.deviceId, target));
            }
        }
    };

    static MidiFeedbackEngineTests midiFeedbackEngineTests;
} // namespace
