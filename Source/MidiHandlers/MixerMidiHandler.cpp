#include "MixerMidiHandler.h"
#include "../Features/Deck/DeckIdentifiers.h"

using sonik::midi::MidiMessageEvent;
using sonik::midi::MidiTargetCategory;

bool MixerMidiHandler::tryHandle (const MidiMessageEvent& event)
{
    switch (event.category)
    {
        case MidiTargetCategory::Crossfader:
        {
            if (stateTree.isValid())
                stateTree.setProperty (IDs::crossfader, event.normalisedValue, nullptr);
            return true;
        }
        case MidiTargetCategory::MasterGain:
        {
            if (stateTree.isValid())
                stateTree.setProperty (IDs::masterGain, event.normalisedValue, nullptr);
            return true;
        }
        case MidiTargetCategory::HeadphonesGain:
        {
            if (stateTree.isValid())
                stateTree.setProperty (IDs::headphonesGain, event.normalisedValue, nullptr);
            return true;
        }
        case MidiTargetCategory::HeadphoneCueToggle:
        {
            if (event.normalisedValue < 0.5f) return true;
            // deckIndex is 0-3 for deck A-D
            if (stateTree.isValid() && event.deckIndex < 4)
            {
                auto decks = stateTree.getChildWithName (IDs::Decks);
                if (event.deckIndex < static_cast<std::uint8_t> (decks.getNumChildren()))
                {
                    auto deckTree = decks.getChild (event.deckIndex);
                    const bool cur = static_cast<bool> (deckTree.getProperty (IDs::headphoneCueEnabled, false));
                    deckTree.setProperty (IDs::headphoneCueEnabled, ! cur, nullptr);
                }
            }
            return true;
        }
        default:
            return false;
    }
}
