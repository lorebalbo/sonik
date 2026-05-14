// Tests for PRD-0045: SoftTakeoverManager (pickup mode for continuous controls).

#include "Features/Midi/SoftTakeoverManager.h"
#include "Features/Midi/MappingStore.h"
#include "Features/Midi/MidiDeviceManager.h"
#include "Features/Midi/MidiHostInterface.h"
#include "Features/Midi/ControlTargetRegistry.h"
#include "Features/Deck/DeckIdentifiers.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

namespace
{
    using namespace sonik::midi;

    class StubHost : public MidiHostInterface
    {
    public:
        juce::Array<juce::MidiDeviceInfo> getAvailableInputs()  override { return {}; }
        juce::Array<juce::MidiDeviceInfo> getAvailableOutputs() override { return {}; }
        std::unique_ptr<juce::MidiInput>  openInputDevice  (const juce::String&,
                                                            juce::MidiInputCallback*) override { return nullptr; }
        std::unique_ptr<juce::MidiOutput> openOutputDevice (const juce::String&) override      { return nullptr; }
    };

    struct Fixture
    {
        StubHost host;
        MidiDeviceManager deviceManager { host };
        MappingStore store { deviceManager };

        // Minimal SonikState tree with one Deck child at index 0.
        juce::ValueTree root  { IDs::SonikState };
        juce::ValueTree decks { IDs::Decks };
        juce::ValueTree deckA { IDs::Deck };

        SoftTakeoverManager mgr;

        Fixture()
            : mgr ([this]() -> SoftTakeoverManager
              {
                  root.addChild (decks, -1, nullptr);
                  decks.addChild (deckA, -1, nullptr);
                  deckA.setProperty (IDs::id, "A", nullptr);
                  deckA.setProperty (IDs::pitchRange, 8.0, nullptr);
                  deckA.setProperty (IDs::pitch, 0.0, nullptr);
                  deckA.setProperty (IDs::gain, 0.5f, nullptr);
                  return SoftTakeoverManager { root, store };
              }())
        {}

        static constexpr std::uint64_t kDeviceId = 0xDEADBEEFULL;

        TargetIndex gainTargetA()
        {
            return ControlTargetRegistry::findByCategoryAndDeck (
                       MidiTargetCategory::Gain, 0).value();
        }

        TargetIndex pitchTargetA()
        {
            return ControlTargetRegistry::findByCategoryAndDeck (
                       MidiTargetCategory::PitchFader, 0).value();
        }
    };

    class SoftTakeoverManagerTests final : public juce::UnitTest
    {
    public:
        SoftTakeoverManagerTests()
            : juce::UnitTest ("SoftTakeoverManager (PRD-0045)", "Sonik") {}

        void runTest() override
        {
            beginTest ("First hardware sample under Pickup is suppressed and records state");
            {
                Fixture f;
                const auto target = f.gainTargetA();
                expect (! f.mgr.shouldPassThrough (Fixture::kDeviceId, target,
                                                   0.1f, 0.5f, SoftTakeoverPolicy::Pickup));
                expect (f.mgr.getState (Fixture::kDeviceId, target) == TakeoverState::Disengaged);
            }

            beginTest ("Pickup remains disengaged while hardware stays on the same side");
            {
                Fixture f;
                const auto target = f.gainTargetA();
                f.mgr.shouldPassThrough (Fixture::kDeviceId, target, 0.10f, 0.50f, SoftTakeoverPolicy::Pickup);
                expect (! f.mgr.shouldPassThrough (Fixture::kDeviceId, target,
                                                   0.20f, 0.50f, SoftTakeoverPolicy::Pickup));
                expect (! f.mgr.shouldPassThrough (Fixture::kDeviceId, target,
                                                   0.40f, 0.50f, SoftTakeoverPolicy::Pickup));
                expect (f.mgr.getState (Fixture::kDeviceId, target) == TakeoverState::Disengaged);
            }

            beginTest ("Pickup engages on crossing and passes through subsequent samples");
            {
                Fixture f;
                const auto target = f.gainTargetA();
                f.mgr.shouldPassThrough (Fixture::kDeviceId, target, 0.10f, 0.50f, SoftTakeoverPolicy::Pickup);
                expect (f.mgr.shouldPassThrough (Fixture::kDeviceId, target,
                                                 0.55f, 0.50f, SoftTakeoverPolicy::Pickup));
                expect (f.mgr.getState (Fixture::kDeviceId, target) == TakeoverState::Engaged);
                expect (f.mgr.shouldPassThrough (Fixture::kDeviceId, target,
                                                 0.70f, 0.55f, SoftTakeoverPolicy::Pickup));
                expect (f.mgr.shouldPassThrough (Fixture::kDeviceId, target,
                                                 0.30f, 0.70f, SoftTakeoverPolicy::Pickup));
            }

            beginTest ("Always / Never policy bypasses the state machine");
            {
                Fixture f;
                const auto target = f.gainTargetA();
                expect (f.mgr.shouldPassThrough (Fixture::kDeviceId, target,
                                                 0.10f, 0.50f, SoftTakeoverPolicy::Always));
                expect (f.mgr.getState (Fixture::kDeviceId, target) == TakeoverState::Engaged);
                expect (f.mgr.shouldPassThrough (Fixture::kDeviceId, target,
                                                 0.10f, 0.50f, SoftTakeoverPolicy::Never));
            }

            beginTest ("resetForBinding returns the entry to Disengaged");
            {
                Fixture f;
                const auto target = f.gainTargetA();
                f.mgr.forceEngage (Fixture::kDeviceId, target, 0.5f, 0.5f);
                expect (f.mgr.getState (Fixture::kDeviceId, target) == TakeoverState::Engaged);
                f.mgr.resetForBinding (Fixture::kDeviceId, target);
                expect (f.mgr.getState (Fixture::kDeviceId, target) == TakeoverState::Disengaged);
            }

            beginTest ("forceEngage transitions immediately and a subsequent sample passes through");
            {
                Fixture f;
                const auto target = f.gainTargetA();
                f.mgr.forceEngage (Fixture::kDeviceId, target, 0.5f, 0.5f);
                expect (f.mgr.shouldPassThrough (Fixture::kDeviceId, target,
                                                 0.1f, 0.5f, SoftTakeoverPolicy::Pickup));
            }

            beginTest ("Non-MIDI VT writes trigger resetForBinding; MIDI-tagged writes do not");
            {
                Fixture f;
                const auto target = f.gainTargetA();
                f.mgr.forceEngage (Fixture::kDeviceId, target, 0.5f, 0.5f);

                // MIDI-originated write: scope active → manager ignores it.
                {
                    MidiOriginatedWriteScope guard;
                    f.deckA.setProperty (IDs::gain, 0.6f, nullptr);
                }
                expect (f.mgr.getState (Fixture::kDeviceId, target) == TakeoverState::Engaged);

                // External (e.g. mouse / track load) write: must reset.
                f.deckA.setProperty (IDs::gain, 0.0f, nullptr);
                expect (f.mgr.getState (Fixture::kDeviceId, target) == TakeoverState::Disengaged);
            }

            beginTest ("activeMappingChanged resets every binding for the device");
            {
                Fixture f;
                const auto gain  = f.gainTargetA();
                const auto pitch = f.pitchTargetA();
                f.mgr.forceEngage (Fixture::kDeviceId, gain,  0.5f, 0.5f);
                f.mgr.forceEngage (Fixture::kDeviceId, pitch, 0.5f, 0.5f);
                f.mgr.activeMappingChanged (Fixture::kDeviceId);
                expect (f.mgr.getState (Fixture::kDeviceId, gain)  == TakeoverState::Disengaged);
                expect (f.mgr.getState (Fixture::kDeviceId, pitch) == TakeoverState::Disengaged);
            }
        }
    };

    static SoftTakeoverManagerTests softTakeoverManagerTests;
} // namespace
