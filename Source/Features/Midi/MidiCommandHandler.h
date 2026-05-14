#pragma once
//==============================================================================
// PRD-0044: MidiCommandHandler — the single seam between MidiInboundRouter
// (MIDI feature module) and the application's per-domain handlers (Deck,
// Mixer, Library). The router has no knowledge of Feature modules; it
// invokes `handle(...)` on the Message thread for every resolved event
// classified as MessageThread.
//==============================================================================

#include "MidiMessageEvent.h"

namespace sonik::midi
{
    class MidiCommandHandler
    {
    public:
        virtual ~MidiCommandHandler() = default;

        /** Invoked on the JUCE Message thread for every resolved binding whose
            routing class is MessageThread. Implementations may write to
            ValueTrees, call Feature-module managers, or log warnings. */
        virtual void handle (const MidiMessageEvent& event) = 0;
    };
} // namespace sonik::midi
