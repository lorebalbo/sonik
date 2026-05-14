#include "CompositeMidiCommandHandler.h"

#include <juce_core/juce_core.h>

using sonik::midi::MidiMessageEvent;
using sonik::midi::MidiTargetCategory;

CompositeMidiCommandHandler::CompositeMidiCommandHandler (DeckMidiHandler&    d,
                                                          MixerMidiHandler&   m,
                                                          LibraryMidiHandler& l)
    : deck (d), mixer (m), library (l) {}

void CompositeMidiCommandHandler::handle (const MidiMessageEvent& event)
{
    if (deck.tryHandle (event)) return;
    if (mixer.tryHandle (event)) return;
    if (library.tryHandle (event)) return;

    const auto idx = static_cast<std::size_t> (event.category);
    if (idx >= warnedForCategory.size())
        return;

    bool expected = false;
    if (warnedForCategory[idx].compare_exchange_strong (expected, true,
                                                        std::memory_order_acq_rel))
    {
        DBG ("[MIDI] Unhandled category index=" << static_cast<int> (idx)
             << " deck=" << static_cast<int> (event.deckIndex));
    }
}
