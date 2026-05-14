#pragma once
//==============================================================================
// PRD-0044: CompositeMidiCommandHandler.
//
// Owned by SonikApplication. Dispatches each MidiMessageEvent to the per-domain
// handler (Deck → Mixer → Library). If no domain handler consumes the event,
// logs a one-shot DBG warning for that category and otherwise no-ops.
//==============================================================================

#include "../Features/Midi/MidiCommandHandler.h"

#include "DeckMidiHandler.h"
#include "LibraryMidiHandler.h"
#include "MixerMidiHandler.h"

#include <array>
#include <atomic>

class CompositeMidiCommandHandler final : public sonik::midi::MidiCommandHandler
{
public:
    CompositeMidiCommandHandler (DeckMidiHandler&    deck,
                                 MixerMidiHandler&   mixer,
                                 LibraryMidiHandler& library);

    void handle (const sonik::midi::MidiMessageEvent& event) override;

    /** Diagnostic accessor: true if a one-shot warning has been emitted for
        this category in the current session. Test-only. */
    bool hasWarnedForCategory (sonik::midi::MidiTargetCategory category) const noexcept
    {
        const auto idx = static_cast<std::size_t> (category);
        if (idx >= warnedForCategory.size())
            return false;
        return warnedForCategory[idx].load (std::memory_order_acquire);
    }

private:
    DeckMidiHandler&    deck;
    MixerMidiHandler&   mixer;
    LibraryMidiHandler& library;

    // One-shot warning bitmap, indexed by category. Atomic so it can be
    // observed/reset by tests.
    std::array<std::atomic<bool>,
               static_cast<std::size_t> (sonik::midi::MidiTargetCategory::Count)>
        warnedForCategory {};
};
