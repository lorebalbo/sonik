#include "LibraryMidiHandler.h"

using sonik::midi::MidiMessageEvent;
using sonik::midi::MidiTargetCategory;

// Chain-of-responsibility dispatch: this handler consumes only the library
// categories and returns false for everything else. The switch is
// intentionally non-exhaustive, so the exhaustiveness warning is silenced
// for this one function.
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wswitch-enum")
bool LibraryMidiHandler::tryHandle (const MidiMessageEvent& event)
{
    switch (event.category)
    {
        case MidiTargetCategory::LibraryScrollUp:
        {
            if (event.normalisedValue < 0.5f) return true; // ignore note-off
            if (onScrollUp) onScrollUp();
            return true;
        }
        case MidiTargetCategory::LibraryScrollDown:
        {
            if (event.normalisedValue < 0.5f) return true;
            if (onScrollDown) onScrollDown();
            return true;
        }
        case MidiTargetCategory::LibraryFocusSearch:
        {
            if (event.normalisedValue < 0.5f) return true;
            if (onFocusSearch) onFocusSearch();
            return true;
        }
        case MidiTargetCategory::LibraryLoadDeck:
        {
            if (event.normalisedValue < 0.5f) return true;
            if (onLoadDeck && event.deckIndex < 4)
                onLoadDeck (static_cast<int> (event.deckIndex));
            return true;
        }
        case MidiTargetCategory::LibraryBrowse:
        {
            // intDelta is populated by SignedBitDelta or TwosComplementDelta
            // transforms. Positive = encoder turned CW (scroll down the list),
            // negative = CCW (scroll up).
            if (event.intDelta == 0) return true;
            if (onBrowse) onBrowse (static_cast<int> (event.intDelta));
            return true;
        }
        default:
            return false;
    }
}
JUCE_END_IGNORE_WARNINGS_GCC_LIKE
