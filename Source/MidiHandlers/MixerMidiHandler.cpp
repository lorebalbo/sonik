#include "MixerMidiHandler.h"
#include "../Features/Deck/DeckIdentifiers.h"
#include "../Features/Midi/ControlTargetRegistry.h"

using sonik::midi::ControlTargetRegistry;
using sonik::midi::MidiMessageEvent;
using sonik::midi::MidiOriginatedWriteScope;
using sonik::midi::MidiTargetCategory;

bool MixerMidiHandler::passSoftTakeover (const MidiMessageEvent& event,
                                          MidiTargetCategory category,
                                          float currentSoftwareNorm) noexcept
{
    if (softTakeover == nullptr)
        return true; // No manager wired (legacy tests) → write-through.

    const auto target = ControlTargetRegistry::findByCategoryAndDeck (
        category, event.deckIndex);
    if (! target.has_value())
        return true; // No registered target for this (category, deck).

    return softTakeover->shouldPassThrough (event.deviceId, *target,
                                            event.normalisedValue,
                                            currentSoftwareNorm,
                                            event.softTakeover);
}

bool MixerMidiHandler::tryHandle (const MidiMessageEvent& event)
{
    switch (event.category)
    {
        //----------------------------------------------------------------------
        // Legacy global mixer controls (pre-PRD-0052, stored on root tree)
        //----------------------------------------------------------------------
        case MidiTargetCategory::Crossfader:
        {
            if (mixerSchema == nullptr) return true;

            const float curNorm = MixerParam::crossfaderToNormalised (
                static_cast<float> (mixerSchema->getMixerTree().getProperty (
                    MixerIDs::crossfader, 0.5f)));
            if (! passSoftTakeover (event, MidiTargetCategory::Crossfader, curNorm))
                return true;

            const float val = MixerParam::normalisedToCrossfader (event.normalisedValue);
            MidiOriginatedWriteScope guard;
            mixerSchema->getMixerTree().setProperty (MixerIDs::crossfader, val, nullptr);
            return true;
        }
        case MidiTargetCategory::MasterGain:
        {
            if (mixerSchema == nullptr) return true;

            const float curDb = static_cast<float> (
                mixerSchema->getMasterTree().getProperty (MixerIDs::gain, 0.0f));
            const float curNorm = MixerParam::gainDbToNormalised (curDb);
            if (! passSoftTakeover (event, MidiTargetCategory::MasterGain, curNorm))
                return true;

            const float db = MixerParam::normalisedToGainDb (event.normalisedValue);
            MidiOriginatedWriteScope guard;
            mixerSchema->getMasterTree().setProperty (MixerIDs::gain, db, nullptr);
            return true;
        }
        case MidiTargetCategory::HeadphonesGain:
        {
            if (! stateTree.isValid()) return true;

            const float curNorm = static_cast<float> (
                stateTree.getProperty (IDs::headphonesGain, 1.0f));
            if (! passSoftTakeover (event, MidiTargetCategory::HeadphonesGain, curNorm))
                return true;

            MidiOriginatedWriteScope guard;
            stateTree.setProperty (IDs::headphonesGain, event.normalisedValue, nullptr);
            return true;
        }
        case MidiTargetCategory::HeadphoneCueToggle:
        {
            if (event.normalisedValue < 0.5f) return true;
            if (stateTree.isValid() && event.deckIndex < 4)
            {
                auto decks = stateTree.getChildWithName (IDs::Decks);
                if (event.deckIndex < static_cast<std::uint8_t> (decks.getNumChildren()))
                {
                    auto deckTree = decks.getChild (event.deckIndex);
                    const bool cur = static_cast<bool> (deckTree.getProperty (IDs::headphoneCueEnabled, false));
                    MidiOriginatedWriteScope guard;
                    deckTree.setProperty (IDs::headphoneCueEnabled, ! cur, nullptr);
                }
            }
            return true;
        }

        //----------------------------------------------------------------------
        // Per-channel continuous controls (PRD-0052 + PRD-0061)
        //----------------------------------------------------------------------
        case MidiTargetCategory::ChannelGain:
        {
            if (mixerSchema == nullptr || event.deckIndex >= 4) return true;
            const float curDb = static_cast<float> (
                mixerSchema->getChannelTree (event.deckIndex).getProperty (
                    MixerIDs::gain, 0.0f));
            const float curNorm = MixerParam::gainDbToNormalised (curDb);
            if (! passSoftTakeover (event, MidiTargetCategory::ChannelGain, curNorm))
                return true;

            const float db = MixerParam::normalisedToGainDb (event.normalisedValue);
            applyChannelContinuous (event.deckIndex, MixerIDs::gain, db);
            return true;
        }
        case MidiTargetCategory::ChannelEqHigh:
        {
            if (mixerSchema == nullptr || event.deckIndex >= 4) return true;
            const float curDb = static_cast<float> (
                mixerSchema->getChannelEqTree (event.deckIndex).getProperty (
                    MixerIDs::high, 0.0f));
            const float curNorm = MixerParam::eqDbToNormalised (curDb);
            if (! passSoftTakeover (event, MidiTargetCategory::ChannelEqHigh, curNorm))
                return true;

            const float db = MixerParam::normalisedToEqDb (event.normalisedValue);
            applyChannelEqContinuous (event.deckIndex, MixerIDs::high, db);
            return true;
        }
        case MidiTargetCategory::ChannelEqMid:
        {
            if (mixerSchema == nullptr || event.deckIndex >= 4) return true;
            const float curDb = static_cast<float> (
                mixerSchema->getChannelEqTree (event.deckIndex).getProperty (
                    MixerIDs::mid, 0.0f));
            const float curNorm = MixerParam::eqDbToNormalised (curDb);
            if (! passSoftTakeover (event, MidiTargetCategory::ChannelEqMid, curNorm))
                return true;

            const float db = MixerParam::normalisedToEqDb (event.normalisedValue);
            applyChannelEqContinuous (event.deckIndex, MixerIDs::mid, db);
            return true;
        }
        case MidiTargetCategory::ChannelEqLow:
        {
            if (mixerSchema == nullptr || event.deckIndex >= 4) return true;
            const float curDb = static_cast<float> (
                mixerSchema->getChannelEqTree (event.deckIndex).getProperty (
                    MixerIDs::low, 0.0f));
            const float curNorm = MixerParam::eqDbToNormalised (curDb);
            if (! passSoftTakeover (event, MidiTargetCategory::ChannelEqLow, curNorm))
                return true;

            const float db = MixerParam::normalisedToEqDb (event.normalisedValue);
            applyChannelEqContinuous (event.deckIndex, MixerIDs::low, db);
            return true;
        }
        case MidiTargetCategory::ChannelFilter:
        {
            if (mixerSchema == nullptr || event.deckIndex >= 4) return true;
            const float curBipolar = static_cast<float> (
                mixerSchema->getChannelTree (event.deckIndex).getProperty (
                    MixerIDs::filter, 0.0f));
            const float curNorm = MixerParam::filterBipolarToNormalised (curBipolar);
            if (! passSoftTakeover (event, MidiTargetCategory::ChannelFilter, curNorm))
                return true;

            const float bipolar = MixerParam::normalisedToFilterBipolar (event.normalisedValue);
            applyChannelContinuous (event.deckIndex, MixerIDs::filter, bipolar);
            return true;
        }
        case MidiTargetCategory::ChannelFader:
        {
            if (mixerSchema == nullptr || event.deckIndex >= 4) return true;
            const float curNorm = MixerParam::faderToNormalised (static_cast<float> (
                mixerSchema->getChannelTree (event.deckIndex).getProperty (
                    MixerIDs::fader, 1.0f)));
            if (! passSoftTakeover (event, MidiTargetCategory::ChannelFader, curNorm))
                return true;

            const float val = MixerParam::normalisedToFader (event.normalisedValue);
            applyChannelContinuous (event.deckIndex, MixerIDs::fader, val);
            return true;
        }

        //----------------------------------------------------------------------
        // Per-channel toggle controls (PRD-0052 + PRD-0061)
        //----------------------------------------------------------------------
        case MidiTargetCategory::ChannelKillHigh:
            applyChannelEqToggle (event.deckIndex, MixerIDs::killHigh, event.normalisedValue);
            return true;

        case MidiTargetCategory::ChannelKillMid:
            applyChannelEqToggle (event.deckIndex, MixerIDs::killMid, event.normalisedValue);
            return true;

        case MidiTargetCategory::ChannelKillLow:
            applyChannelEqToggle (event.deckIndex, MixerIDs::killLow, event.normalisedValue);
            return true;

        case MidiTargetCategory::ChannelAssignA:
            applyChannelToggle (event.deckIndex, MixerIDs::assignA, event.normalisedValue);
            return true;

        case MidiTargetCategory::ChannelAssignB:
            applyChannelToggle (event.deckIndex, MixerIDs::assignB, event.normalisedValue);
            return true;

        case MidiTargetCategory::ChannelCue:
            applyChannelToggle (event.deckIndex, MixerIDs::cue, event.normalisedValue);
            return true;

        default:
            return false;
    }
}

//==============================================================================
// Private helpers
//==============================================================================

void MixerMidiHandler::applyChannelContinuous (int channelIdx,
                                                const juce::Identifier& prop,
                                                float nativeValue) noexcept
{
    if (mixerSchema == nullptr || channelIdx < 0 || channelIdx >= 4)
        return;
    MidiOriginatedWriteScope guard;
    mixerSchema->getChannelTree (channelIdx).setProperty (prop, nativeValue, nullptr);
}

void MixerMidiHandler::applyChannelToggle (int channelIdx,
                                             const juce::Identifier& prop,
                                             float normalisedValue) noexcept
{
    if (mixerSchema == nullptr || channelIdx < 0 || channelIdx >= 4)
        return;
    if (normalisedValue < 0.5f)
        return;
    auto ch = mixerSchema->getChannelTree (channelIdx);
    const bool cur = static_cast<bool> (ch.getProperty (prop, false));
    MidiOriginatedWriteScope guard;
    ch.setProperty (prop, ! cur, nullptr);
}

void MixerMidiHandler::applyChannelEqContinuous (int channelIdx,
                                                   const juce::Identifier& prop,
                                                   float nativeValue) noexcept
{
    if (mixerSchema == nullptr || channelIdx < 0 || channelIdx >= 4)
        return;
    MidiOriginatedWriteScope guard;
    mixerSchema->getChannelEqTree (channelIdx).setProperty (prop, nativeValue, nullptr);
}

void MixerMidiHandler::applyChannelEqToggle (int channelIdx,
                                               const juce::Identifier& prop,
                                               float normalisedValue) noexcept
{
    if (mixerSchema == nullptr || channelIdx < 0 || channelIdx >= 4)
        return;
    if (normalisedValue < 0.5f)
        return;
    auto eq = mixerSchema->getChannelEqTree (channelIdx);
    const bool cur = static_cast<bool> (eq.getProperty (prop, false));
    MidiOriginatedWriteScope guard;
    eq.setProperty (prop, ! cur, nullptr);
}
