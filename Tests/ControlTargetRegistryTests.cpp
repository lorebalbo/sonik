// Tests for PRD-0042: Control Target Registry.

#include "Features/Midi/ControlTargetRegistry.h"

#include <juce_core/juce_core.h>

#include <set>
#include <string>

namespace
{
    using namespace sonik::midi;

    class ControlTargetRegistryTests final : public juce::UnitTest
    {
    public:
        ControlTargetRegistryTests()
            : juce::UnitTest ("Control Target Registry (PRD-0042)", "Sonik")
        {}

        void runTest() override
        {
            beginTest ("Registry contains the documented minimum vocabulary");
            expectGreaterOrEqual ((int) ControlTargetRegistry::size(), 200);

            beginTest ("All ids are unique");
            std::set<std::string> seen;
            for (TargetIndex i = 0; i < (TargetIndex) ControlTargetRegistry::size(); ++i)
            {
                const auto& t = ControlTargetRegistry::get (i);
                expect (seen.insert (t.id).second, juce::String ("Duplicate id: ") + t.id);
            }

            beginTest ("lookup returns the correct index for every registered id");
            for (TargetIndex i = 0; i < (TargetIndex) ControlTargetRegistry::size(); ++i)
            {
                const auto& t = ControlTargetRegistry::get (i);
                const auto idx = ControlTargetRegistry::lookup (juce::StringRef (t.id));
                expect (idx.has_value(), juce::String ("missing lookup for ") + t.id);
                if (idx.has_value())
                    expectEquals ((int) ControlTargetRegistry::get (*idx).category,
                                  (int) t.category);
            }

            beginTest ("lookup returns nullopt for unknown ids");
            expect (! ControlTargetRegistry::lookup ("totally.bogus.id").has_value());
            expect (! ControlTargetRegistry::lookup ("").has_value());
            expect (! ControlTargetRegistry::lookup ("deck.E.transport.play").has_value());

            beginTest ("Per-deck targets carry the correct deckIndex");
            const std::pair<const char*, int> expected[] = {
                { "deck.A.transport.play", 0 },
                { "deck.B.transport.play", 1 },
                { "deck.C.transport.play", 2 },
                { "deck.D.transport.play", 3 },
                { "deck.A.jog.scratch",    0 },
                { "deck.D.hotcue.8.trigger", 3 },
            };
            for (auto [id, deck] : expected)
            {
                const auto idx = ControlTargetRegistry::lookup (juce::StringRef (id));
                expect (idx.has_value(), juce::String ("missing ") + id);
                if (idx.has_value())
                {
                    const auto& t = ControlTargetRegistry::get (*idx);
                    expectEquals ((int) t.deckIndex, deck);
                    expectEquals ((int) t.deckScope, (int) DeckScope::PerDeck);
                }
            }

            beginTest ("Global mixer targets carry GlobalDeckIndex");
            const auto cf = ControlTargetRegistry::lookup ("mixer.crossfader");
            expect (cf.has_value());
            if (cf.has_value())
            {
                const auto& t = ControlTargetRegistry::get (*cf);
                expectEquals ((int) t.deckScope, (int) DeckScope::Global);
                expectEquals ((int) t.deckIndex, (int) GlobalDeckIndex);
                expectEquals ((int) t.valueKind, (int) TargetValueKind::Continuous);
            }

            beginTest ("library.load.deck.* targets carry the correct deck index");
            const auto load = ControlTargetRegistry::lookup ("library.load.deck.C");
            expect (load.has_value());
            if (load.has_value())
            {
                const auto& t = ControlTargetRegistry::get (*load);
                expectEquals ((int) t.deckIndex, 2);
                expectEquals ((int) t.category, (int) MidiTargetCategory::LibraryLoadDeck);
            }

            beginTest ("Jog targets are RelativeDelta");
            const auto jog = ControlTargetRegistry::lookup ("deck.A.jog.scratch");
            expect (jog.has_value());
            if (jog.has_value())
                expectEquals ((int) ControlTargetRegistry::get (*jog).valueKind,
                              (int) TargetValueKind::RelativeDelta);

            beginTest ("Beat jump targets registered for size and direction");
            expect (ControlTargetRegistry::lookup ("deck.A.beatjump.size.cycle").has_value());
            expect (ControlTargetRegistry::lookup ("deck.A.beatjump.minus.16").has_value());
            expect (ControlTargetRegistry::lookup ("deck.D.beatjump.plus.32").has_value());

            beginTest ("Headphone cue and library navigation registered");
            expect (ControlTargetRegistry::lookup ("mixer.deck.A.headphoneCue.toggle").has_value());
            expect (ControlTargetRegistry::lookup ("library.scroll.up").has_value());
            expect (ControlTargetRegistry::lookup ("library.focus.search").has_value());
        }
    };

    static ControlTargetRegistryTests controlTargetRegistryTests;
} // namespace
