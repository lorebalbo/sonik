#include "LibraryMidiHandler.h"

using sonik::midi::MidiMessageEvent;
using sonik::midi::MidiTargetCategory;

bool LibraryMidiHandler::tryHandle (const MidiMessageEvent& event)
{
    switch (event.category)
    {
        case MidiTargetCategory::LibraryScrollUp:
        case MidiTargetCategory::LibraryScrollDown:
        case MidiTargetCategory::LibraryLoadDeck:
        case MidiTargetCategory::LibraryFocusSearch:
            return false; // Library wiring lands in PRD-0048; defer to composite warning.
        default:
            return false;
    }
}
