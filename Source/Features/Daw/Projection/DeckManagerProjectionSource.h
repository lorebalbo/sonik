#pragma once
//==============================================================================
// PRD-0069: Production DeckProjectionSource adapter over DeckStateManager.
//
// Maps each deck (by its position among the IDs::Deck children of the central
// state tree, which is the same positional `deckIndex` the DAW track resolver
// and waveform resolver use) to its DeckAudioState atomics and deck ValueTree.
// Message-thread only; the bridge only reads through it.
//==============================================================================

#include "LiveProjectionTimer.h"

#include "../../Deck/DeckStateManager.h"
#include "../../Deck/DeckIdentifiers.h"

namespace Daw
{

class DeckManagerProjectionSource final : public DeckProjectionSource
{
public:
    explicit DeckManagerProjectionSource (DeckStateManager& manager)
        : manager_ (manager),
          decksTree_ (manager.getStateTree().getChildWithName (IDs::Decks))
    {
    }

    int getNumDecks() const override
    {
        int n = 0;
        for (int i = 0; i < decksTree_.getNumChildren(); ++i)
            if (decksTree_.getChild (i).hasType (IDs::Deck))
                ++n;
        return n;
    }

    int getDeckIndex (int slot) const override { return slot; }

    DeckAudioState* getAudioState (int slot) override
    {
        auto deck = deckAt (slot);
        if (! deck.isValid())
            return nullptr;
        return manager_.getAudioState (deck.getProperty (IDs::id).toString());
    }

    juce::ValueTree getDeckTree (int slot) const override { return deckAt (slot); }

private:
    juce::ValueTree deckAt (int slot) const
    {
        int count = 0;
        for (int i = 0; i < decksTree_.getNumChildren(); ++i)
        {
            auto deck = decksTree_.getChild (i);
            if (deck.hasType (IDs::Deck))
            {
                if (count == slot)
                    return deck;
                ++count;
            }
        }
        return {};
    }

    DeckStateManager& manager_;
    juce::ValueTree   decksTree_;
};

} // namespace Daw
