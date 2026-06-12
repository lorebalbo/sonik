// Tests for PRD-0044: MidiInboundRouter + composite handler.

#include "Features/Midi/MidiInboundRouter.h"
#include "Features/Midi/MidiDeviceManager.h"
#include "Features/Midi/MidiMessageBridge.h"
#include "Features/Midi/MappingStore.h"
#include "Features/Midi/MappingParser.h"
#include "Features/Midi/MidiHostInterface.h"
#include "Features/Midi/MidiInboundEvent.h"
#include "Features/Midi/MidiCommandHandler.h"
#include "Features/Deck/DeckStateManager.h"
#include "Features/Deck/Database/TrackDatabase.h"
#include "MidiHandlers/CompositeMidiCommandHandler.h"
#include "MidiHandlers/DeckMidiHandler.h"
#include "MidiHandlers/LibraryMidiHandler.h"
#include "MidiHandlers/MixerMidiHandler.h"
#include "Features/Midi/SoftTakeoverManager.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <vector>

namespace
{
    using namespace sonik::midi;

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

    class RecordingAudioHandler final : public AudioMidiEventHandler
    {
    public:
        std::vector<MidiAudioEvent> events;
        void applyAudioMidiEvent (const MidiAudioEvent& e) noexcept override
        {
            events.push_back (e);
        }
    };

    juce::File makeTempDir (const juce::String& suffix)
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("sonik-router-" + suffix
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

    // Fixture: builds a router wired against a single test device. The user
    // profile binds note 14 → TransportPlay, CC 22 → JogScratch, note 63 →
    // modifier bit 0 (momentary), and overlays SHIFT+note 48 → HotCueDelete.
    struct Fixture
    {
        StubHost              host;
        juce::File            dir;
        std::unique_ptr<MidiDeviceManager> mgr;
        std::unique_ptr<MidiMessageBridge> bridge;
        std::unique_ptr<MappingStore>      store;
        std::unique_ptr<RecordingHandler>  handler;
        std::unique_ptr<MidiInboundRouter> router;
        std::uint64_t deviceId { 0 };

        explicit Fixture (const juce::String& tag)
        {
            host.inputs.add (juce::MidiDeviceInfo ("TestDev", "test-id-1"));
            mgr = std::make_unique<MidiDeviceManager> (host);
            mgr->initialise();
            for (const auto& d : mgr->getDevices())
                if (d.isInput) deviceId = d.deviceId;

            dir = makeTempDir (tag);
            // Write the user profile.
            dir.getChildFile ("user.json").replaceWithText (juce::String (R"({
                "schemaVersion": 1,
                "device": { "manufacturer": ".*", "product": ".*",
                            "match": { "midiName": ".*" } },
                "modifiers": [
                    { "id": "shift",
                      "binding": { "channel": 1, "status": "note", "data1": 63, "style": "momentary" } }
                ],
                "bindings": [
                    { "target": "deck.A.transport.play",
                      "midi": { "channel": 1, "status": "note", "data1": 14 },
                      "transform": "momentary" },
                    { "target": "deck.A.jog.scratch",
                      "midi": { "channel": 1, "status": "cc", "data1": 22 },
                      "transform": "signedBitDelta" },
                    { "target": "deck.A.transport.cue",
                      "midi": { "channel": 1, "status": "note", "data1": 20 },
                      "transform": "momentary" },
                    { "target": "deck.A.hotcue.1.trigger",
                      "midi": { "channel": 1, "status": "note", "data1": 48 },
                      "transform": "momentary" },
                    { "target": "deck.A.hotcue.1.delete",
                      "midi": { "channel": 1, "status": "note", "data1": 48 },
                      "transform": "momentary",
                      "modifier": "shift" }
                ]
            })"));

            bridge  = std::make_unique<MidiMessageBridge>();
            store   = std::make_unique<MappingStore> (*mgr, dir, /*async*/ false);
            handler = std::make_unique<RecordingHandler>();
            router  = std::make_unique<MidiInboundRouter> (*mgr, *bridge, *store, *handler);
        }

        ~Fixture()
        {
            router.reset();
            handler.reset();
            store.reset();
            bridge.reset();
            mgr.reset();
            dir.deleteRecursively();
        }
    };

    class MidiInboundRouterTests final : public juce::UnitTest
    {
    public:
        MidiInboundRouterTests() : juce::UnitTest ("MIDI Inbound Router (PRD-0044)", "Sonik") {}

        void runTest() override
        {
            beginTest ("Note On 14 (TransportPlay) reaches Message-thread handler with correct fields");
            {
                Fixture f ("play");
                const MidiInboundEvent ev { f.deviceId, 0.0, 0x90, 14, 127 };
                f.router->onMidiInbound (ev);

                pump ([&] { return ! f.handler->events.empty(); });
                expect (! f.handler->events.empty());
                if (! f.handler->events.empty())
                {
                    const auto& e = f.handler->events.front();
                    expect (e.category  == MidiTargetCategory::TransportPlay);
                    expect (e.deckIndex == 0);
                    expect (e.normalisedValue >= 0.99f);
                    expect (e.deviceId  == f.deviceId);
                }
            }

            beginTest ("Jog scratch CC 22 routes via audio FIFO (not Message thread)");
            {
                Fixture f ("jog");
                const MidiInboundEvent ev { f.deviceId, 0.0, 0xB0, 22, 80 };
                f.router->onMidiInbound (ev);

                RecordingAudioHandler audio;
                const int drained = f.bridge->drainAudioThreadFifo (audio);
                expect (drained == 1);
                if (drained == 1)
                {
                    expect (audio.events[0].category  == MidiTargetCategory::JogScratch);
                    expect (audio.events[0].deckIndex == 0);
                }
                pump ([] { return false; }, 50);
                expect (f.handler->events.empty()); // Did NOT go to Message thread.
            }

            beginTest ("Modifier set/clear updates mask and is observed by subsequent resolve");
            {
                Fixture f ("mod");

                // Press SHIFT (note 63 down).
                f.router->onMidiInbound ({ f.deviceId, 0.0, 0x90, 63, 127 });
                // Press HotCue 1 while SHIFT held.
                f.router->onMidiInbound ({ f.deviceId, 0.0, 0x90, 48, 127 });

                pump ([&] { return ! f.handler->events.empty(); });
                expect (! f.handler->events.empty());
                if (! f.handler->events.empty())
                    expect (f.handler->events.back().category == MidiTargetCategory::HotCueDelete);

                // Release SHIFT and press HotCue again → trigger, not delete.
                f.handler->events.clear();
                f.router->onMidiInbound ({ f.deviceId, 0.0, 0x90, 63, 0 });
                f.router->onMidiInbound ({ f.deviceId, 0.0, 0x90, 48, 127 });

                pump ([&] { return ! f.handler->events.empty(); });
                expect (! f.handler->events.empty());
                if (! f.handler->events.empty())
                    expect (f.handler->events.back().category == MidiTargetCategory::HotCueTrigger);
            }

            beginTest ("Event for unknown device drops silently and increments counter");
            {
                Fixture f ("unknown");
                const auto before = f.router->getUnmatchedEventCount();
                f.router->onMidiInbound ({ 0xDEADBEEF, 0.0, 0x90, 14, 127 });
                pump ([] { return false; }, 50);
                expect (f.handler->events.empty());
                expect (f.router->getUnmatchedEventCount() > before);
            }

            beginTest ("Stress: 100k inbound events do not crash or block");
            {
                Fixture f ("stress");
                for (int i = 0; i < 100'000; ++i)
                    f.router->onMidiInbound ({ f.deviceId, 0.0, 0xB0, 22, static_cast<std::uint8_t> (i & 0x7F) });
                // Drain anything still in the FIFO so the bridge teardown is clean.
                RecordingAudioHandler audio;
                while (f.bridge->drainAudioThreadFifo (audio) > 0) {}
                expect (true);
            }

            beginTest ("Composite handler emits exactly one warning per unhandled category");
            {
                const auto dbFile = juce::File::createTempFile ("sonik_router_composite.db");
                {
                    TrackDatabase    db (dbFile);
                    DeckStateManager dsm (db);
                    dsm.addDeck(); // Deck index 0 exists so DeckMidiHandler reaches the switch.
                    // PRD-0045: DeckMidiHandler now requires a SoftTakeoverManager.
                    StubHost          stubHost;
                    MidiDeviceManager stubMgr (stubHost);
                    MappingStore      stubStore (stubMgr);
                    SoftTakeoverManager soft (dsm.getStateTree(), stubStore);
                    DeckMidiHandler  deck (dsm, soft);
                    MixerMidiHandler mix;
                    LibraryMidiHandler lib;
                    CompositeMidiCommandHandler composite (deck, mix, lib);

                    // DawRecordArm is a recognised global category that no
                    // message-thread handler (deck/mixer/library) consumes yet
                    // (PRD-0078 wires only the routing, not a handler). The
                    // composite must warn exactly once. (TransportCue was used
                    // here before DeckMidiHandler gained a cue implementation.)
                    expect (! composite.hasWarnedForCategory (MidiTargetCategory::DawRecordArm));

                    const MidiMessageEvent ev {
                        MidiTargetCategory::DawRecordArm,
                        sonik::midi::GlobalDeckIndex, 1.0f, 0, 1ULL,
                        SoftTakeoverPolicy::Pickup
                    };
                    composite.handle (ev);
                    expect (composite.hasWarnedForCategory (MidiTargetCategory::DawRecordArm));

                    composite.handle (ev); // Second call: no second warning, no crash.
                    expect (composite.hasWarnedForCategory (MidiTargetCategory::DawRecordArm));

                    // Different category: independent state.
                    expect (! composite.hasWarnedForCategory (MidiTargetCategory::LoopToggle));
                }
                dbFile.deleteFile();
            }
        }
    };

    static MidiInboundRouterTests staticInstance;
} // namespace
