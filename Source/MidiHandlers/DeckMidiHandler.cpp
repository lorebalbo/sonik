#include "DeckMidiHandler.h"

#include "../Features/Deck/DeckIdentifiers.h"

#include <cmath>

using sonik::midi::MidiMessageEvent;
using sonik::midi::MidiTargetCategory;

DeckMidiHandler::DeckMidiHandler (DeckStateManager& d) : deckState (d) {}

juce::ValueTree DeckMidiHandler::deckTreeFor (std::uint8_t deckIndex) const
{
    auto decks = deckState.getStateTree().getChildWithName (IDs::Decks);
    if (deckIndex >= static_cast<std::uint8_t> (decks.getNumChildren()))
        return {};
    return decks.getChild (deckIndex);
}

void DeckMidiHandler::toggleBool (juce::ValueTree& deckTree, const juce::Identifier& id)
{
    const bool current = static_cast<bool> (deckTree.getProperty (id, false));
    deckTree.setProperty (id, ! current, nullptr);
}

bool DeckMidiHandler::tryHandle (const MidiMessageEvent& event)
{
    auto deckTree = deckTreeFor (event.deckIndex);
    if (! deckTree.isValid())
        return true; // Recognised category but no deck — consume silently.

    switch (event.category)
    {
        case MidiTargetCategory::TransportPlay:
        {
            if (! isPress (event.normalisedValue))
                return true;
            const auto deckId = deckTree.getProperty (IDs::id).toString();
            const auto currentStatus = deckTree.getProperty (IDs::playbackStatus, "empty").toString();
            if (currentStatus == "empty")
                return true; // No track; ignore.
            const juce::String next = (currentStatus == "playing") ? "stopped" : "playing";
            deckState.setPlaybackStatus (deckId, next);
            return true;
        }
        case MidiTargetCategory::TransportSync:
        {
            if (! isPress (event.normalisedValue))
                return true;
            toggleBool (deckTree, IDs::syncEnabled);
            return true;
        }
        case MidiTargetCategory::PitchFader:
        {
            // Normalised 0..1 → mapped through current pitchRange to a signed
            // percentage. Centre (0.5) is 0%, ends are ±pitchRange.
            const double range = static_cast<double> (deckTree.getProperty (IDs::pitchRange, 8.0));
            const double pitchPercent = range * ((2.0 * event.normalisedValue) - 1.0);
            deckTree.setProperty (IDs::pitch, pitchPercent, nullptr);
            return true;
        }
        case MidiTargetCategory::Gain:
        {
            deckTree.setProperty (IDs::gain, event.normalisedValue, nullptr);
            return true;
        }
        case MidiTargetCategory::KeyLockToggle:
        {
            if (! isPress (event.normalisedValue)) return true;
            toggleBool (deckTree, IDs::keyLockEnabled);
            return true;
        }
        case MidiTargetCategory::MasterTempoToggle:
        {
            if (! isPress (event.normalisedValue)) return true;
            toggleBool (deckTree, IDs::isMasterTempo);
            return true;
        }
        case MidiTargetCategory::QuantizeToggle:
        {
            if (! isPress (event.normalisedValue)) return true;
            toggleBool (deckTree, IDs::quantizeEnabled);
            return true;
        }
        case MidiTargetCategory::SlipToggle:
        {
            if (! isPress (event.normalisedValue)) return true;
            toggleBool (deckTree, IDs::slipEnabled);
            return true;
        }
        case MidiTargetCategory::KeyShiftPlus:
        {
            if (! isPress (event.normalisedValue)) return true;
            const int v = static_cast<int> (deckTree.getProperty (IDs::keyShift, 0));
            deckTree.setProperty (IDs::keyShift, v + 1, nullptr);
            return true;
        }
        case MidiTargetCategory::KeyShiftMinus:
        {
            if (! isPress (event.normalisedValue)) return true;
            const int v = static_cast<int> (deckTree.getProperty (IDs::keyShift, 0));
            deckTree.setProperty (IDs::keyShift, v - 1, nullptr);
            return true;
        }
        // ---- Recognised per-deck categories without a wired feature yet ----
        // The composite handler treats `tryHandle` returning true as "consumed";
        // we explicitly enumerate these so a future contributor sees the gap.
        case MidiTargetCategory::TransportCue:
        case MidiTargetCategory::PitchRangeCycle:
        case MidiTargetCategory::EqHigh:
        case MidiTargetCategory::EqMid:
        case MidiTargetCategory::EqLow:
        case MidiTargetCategory::LoopIn:
        case MidiTargetCategory::LoopOut:
        case MidiTargetCategory::LoopSizeHalve:
        case MidiTargetCategory::LoopSizeDouble:
        case MidiTargetCategory::LoopToggle:
        case MidiTargetCategory::HotCueTrigger:
        case MidiTargetCategory::HotCueDelete:
        case MidiTargetCategory::BeatJumpMinus:
        case MidiTargetCategory::BeatJumpPlus:
        case MidiTargetCategory::BeatJumpSizeCycle:
        case MidiTargetCategory::PositionSeek:
            // Fall through to the composite's unhandled-category warning.
            return false;
        default:
            return false;
    }
}
