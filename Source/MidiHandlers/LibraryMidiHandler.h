#pragma once
//==============================================================================
// PRD-0044: LibraryMidiHandler — routes library MIDI events to the UI via
// injected std::function callbacks. Wiring is done in SonikApplication after
// MainWindow (and therefore LibraryComponent) exists.
//==============================================================================

#include "../Features/Midi/MidiMessageEvent.h"

#include <functional>

class LibraryMidiHandler final
{
public:
    bool tryHandle (const sonik::midi::MidiMessageEvent& event);

    // ---- Callbacks wired by SonikApplication --------------------------------
    std::function<void()>     onScrollUp;
    std::function<void()>     onScrollDown;
    std::function<void()>     onFocusSearch;
    /** Called with the 0-based deck index (0=A, 1=B, 2=C, 3=D). */
    std::function<void(int)>  onLoadDeck;
    /** Called with a signed step count (+N = scroll down N rows, -N = up N rows).
     *  Wire to a relative encoder using SignedBitDelta or TwosComplementDelta transform. */
    std::function<void(int)>  onBrowse;
};
