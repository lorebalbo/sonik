#include "DeckMidiHandler.h"

#include "../Features/Deck/DeckIdentifiers.h"
#include "../Features/Deck/AudioThreadState.h"
#include "../Features/AudioEngine/AudioEngine.h"
#include "../Features/BeatJump/BeatJumpEngine.h"
#include "../Features/Loop/LoopEngine.h"
#include "../Features/Cue/HotCueManager.h"
#include "../Features/Midi/ControlTargetRegistry.h"
#include "../Features/Midi/SoftTakeoverManager.h"

#include <cmath>

using sonik::midi::ControlTargetRegistry;
using sonik::midi::MidiMessageEvent;
using sonik::midi::MidiOriginatedWriteScope;
using sonik::midi::MidiTargetCategory;

DeckMidiHandler::DeckMidiHandler (DeckStateManager& d,
                                  sonik::midi::SoftTakeoverManager& soft)
    : deckState (d), softTakeover (soft) {}

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

void DeckMidiHandler::registerDeckEngines (std::uint8_t deckIndex,
                                            BeatJumpEngine* beatJump,
                                            LoopEngine*     loop,
                                            HotCueManager*  hotCue) noexcept
{
    if (deckIndex >= 4)
        return;
    beatJumpEngines[deckIndex] = beatJump;
    loopEngines[deckIndex]     = loop;
    hotCueManagers[deckIndex]  = hotCue;
}

void DeckMidiHandler::deregisterDeckEngines (std::uint8_t deckIndex) noexcept
{
    if (deckIndex >= 4)
        return;
    beatJumpEngines[deckIndex] = nullptr;
    loopEngines[deckIndex]     = nullptr;
    hotCueManagers[deckIndex]  = nullptr;
}

// Chain-of-responsibility dispatch: this handler consumes only the deck
// categories and returns false for everything else (library/mixer categories
// flow to the next handler). The switch is intentionally non-exhaustive, so
// the exhaustiveness warning is silenced for this one function.
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wswitch-enum")
bool DeckMidiHandler::tryHandle (const MidiMessageEvent& event)
{
    // GlobalDeckIndex (255) signals a library or mixer category.
    // Those have deckIndex=255 in the registry. Pass them downstream
    // so LibraryMidiHandler / MixerMidiHandler can handle them.
    if (event.deckIndex == sonik::midi::GlobalDeckIndex)
        return false;

    auto deckTree = deckTreeFor (event.deckIndex);
    if (! deckTree.isValid())
        return true; // Per-deck category but deck slot not active — consume silently.

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
        case MidiTargetCategory::TransportCue:
        {
            if (! isPress (event.normalisedValue)) return true;
            if (audioEngine == nullptr) return true;
            const auto deckId = deckTree.getProperty (IDs::id).toString();
            auto* audioState = deckState.getAudioState (deckId);
            if (audioState != nullptr)
            {
                int64_t cuePos = audioState->tempCuePosition.load (std::memory_order_relaxed);
                audioEngine->seekDeck (deckId, cuePos >= 0 ? cuePos : 0);
            }
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

            // PRD-0045: soft-takeover. Compare hardware-normalised value
            // against the on-screen equivalent (mapped back into 0..1 via
            // the current pitch range) so the user can place the fader at
            // any pitchRange setting.
            const double curPitch    = static_cast<double> (deckTree.getProperty (IDs::pitch, 0.0));
            const float  softwareNorm = (range > 0.0)
                                          ? static_cast<float> ((curPitch / range + 1.0) * 0.5)
                                          : 0.5f;
            const auto target = ControlTargetRegistry::findByCategoryAndDeck (
                MidiTargetCategory::PitchFader, event.deckIndex);
            if (target.has_value()
                && ! softTakeover.shouldPassThrough (event.deviceId, *target,
                                                    event.normalisedValue, softwareNorm,
                                                    event.softTakeover))
                return true;

            MidiOriginatedWriteScope guard;
            deckTree.setProperty (IDs::pitch, pitchPercent, nullptr);
            return true;
        }
        case MidiTargetCategory::PitchRangeCycle:
        {
            if (! isPress (event.normalisedValue)) return true;
            // Cycle through standard DJ pitch ranges in ascending order.
            static constexpr double kRanges[] = { 4.0, 6.0, 8.0, 10.0, 16.0, 50.0, 100.0 };
            static constexpr int    kNumRanges = static_cast<int> (sizeof (kRanges) / sizeof (kRanges[0]));
            const double current = static_cast<double> (deckTree.getProperty (IDs::pitchRange, 8.0));
            int nextIdx = 0;
            for (int i = 0; i < kNumRanges; ++i)
            {
                if (std::abs (kRanges[i] - current) < 0.01)
                {
                    nextIdx = (i + 1) % kNumRanges;
                    break;
                }
            }
            deckTree.setProperty (IDs::pitchRange, kRanges[nextIdx], nullptr);
            return true;
        }
        case MidiTargetCategory::Gain:
        {
            const float curGain = static_cast<float> (deckTree.getProperty (IDs::gain, 0.0));
            const auto target = ControlTargetRegistry::findByCategoryAndDeck (
                MidiTargetCategory::Gain, event.deckIndex);
            if (target.has_value()
                && ! softTakeover.shouldPassThrough (event.deviceId, *target,
                                                    event.normalisedValue, curGain,
                                                    event.softTakeover))
                return true;

            MidiOriginatedWriteScope guard;
            deckTree.setProperty (IDs::gain, event.normalisedValue, nullptr);
            return true;
        }
        case MidiTargetCategory::EqHigh:
        {
            MidiOriginatedWriteScope guard;
            deckTree.setProperty (IDs::eqHigh, event.normalisedValue, nullptr);
            return true;
        }
        case MidiTargetCategory::EqMid:
        {
            MidiOriginatedWriteScope guard;
            deckTree.setProperty (IDs::eqMid, event.normalisedValue, nullptr);
            return true;
        }
        case MidiTargetCategory::EqLow:
        {
            MidiOriginatedWriteScope guard;
            deckTree.setProperty (IDs::eqLow, event.normalisedValue, nullptr);
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
        case MidiTargetCategory::LoopIn:
        {
            if (! isPress (event.normalisedValue)) return true;
            if (event.deckIndex < 4 && loopEngines[event.deckIndex] != nullptr)
                loopEngines[event.deckIndex]->setLoopIn();
            return true;
        }
        case MidiTargetCategory::LoopOut:
        {
            if (! isPress (event.normalisedValue)) return true;
            if (event.deckIndex < 4 && loopEngines[event.deckIndex] != nullptr)
                loopEngines[event.deckIndex]->setLoopOut();
            return true;
        }
        case MidiTargetCategory::LoopToggle:
        {
            if (! isPress (event.normalisedValue)) return true;
            if (event.deckIndex < 4 && loopEngines[event.deckIndex] != nullptr)
                loopEngines[event.deckIndex]->toggleLoop();
            return true;
        }
        case MidiTargetCategory::LoopSizeHalve:
        {
            if (! isPress (event.normalisedValue)) return true;
            if (event.deckIndex < 4 && loopEngines[event.deckIndex] != nullptr)
                loopEngines[event.deckIndex]->loopHalve();
            return true;
        }
        case MidiTargetCategory::LoopSizeDouble:
        {
            if (! isPress (event.normalisedValue)) return true;
            if (event.deckIndex < 4 && loopEngines[event.deckIndex] != nullptr)
                loopEngines[event.deckIndex]->loopDouble();
            return true;
        }
        case MidiTargetCategory::HotCueTrigger:
        case MidiTargetCategory::HotCueDelete:
        {
            if (! isPress (event.normalisedValue)) return true;
            if (event.deckIndex >= 4 || hotCueManagers[event.deckIndex] == nullptr)
                return true;

            // Extract 1-based pad number from target id "deck.X.hotcue.N.trigger"
            int padNumber = 1;
            if (event.targetIndex < static_cast<sonik::midi::TargetIndex> (ControlTargetRegistry::size()))
            {
                const juce::String id { ControlTargetRegistry::get (event.targetIndex).id };
                const int hotcuePos = id.indexOf (".hotcue.");
                if (hotcuePos >= 0)
                    padNumber = id.substring (hotcuePos + 8).getIntValue(); // stops at first '.'
            }

            const int padIndex = padNumber - 1; // 0-based
            if (event.category == MidiTargetCategory::HotCueTrigger)
                hotCueManagers[event.deckIndex]->triggerCue (padIndex);
            else
                hotCueManagers[event.deckIndex]->deleteCue (padIndex);
            return true;
        }
        case MidiTargetCategory::BeatJumpMinus:
        case MidiTargetCategory::BeatJumpPlus:
        {
            if (! isPress (event.normalisedValue)) return true;
            if (event.deckIndex >= 4 || beatJumpEngines[event.deckIndex] == nullptr)
                return true;

            // Extract beat size from target id "deck.X.beatjump.minus.N" → N
            if (event.targetIndex < static_cast<sonik::midi::TargetIndex> (ControlTargetRegistry::size()))
            {
                const juce::String id { ControlTargetRegistry::get (event.targetIndex).id };
                const int lastDot = id.lastIndexOf (".");
                if (lastDot >= 0)
                {
                    const double size = id.substring (lastDot + 1).getDoubleValue();
                    if (size > 0.0)
                        beatJumpEngines[event.deckIndex]->setJumpSize (size);
                }
            }

            if (event.category == MidiTargetCategory::BeatJumpMinus)
                beatJumpEngines[event.deckIndex]->jumpBackward();
            else
                beatJumpEngines[event.deckIndex]->jumpForward();
            return true;
        }
        case MidiTargetCategory::BeatJumpSizeCycle:
        {
            if (! isPress (event.normalisedValue)) return true;
            if (event.deckIndex < 4 && beatJumpEngines[event.deckIndex] != nullptr)
                beatJumpEngines[event.deckIndex]->cycleJumpSize (true);
            return true;
        }
        case MidiTargetCategory::PositionSeek:
        {
            if (audioEngine == nullptr) return true;
            const auto deckId = deckTree.getProperty (IDs::id).toString();
            const auto metaNode = deckTree.getChildWithName (IDs::TrackMetadata);
            const int64_t totalSamples = static_cast<int64_t> (
                static_cast<double> (metaNode.getProperty (IDs::totalSamples, 0)));
            if (totalSamples <= 0) return true;
            const int64_t targetSample = static_cast<int64_t> (event.normalisedValue * static_cast<float> (totalSamples));
            audioEngine->seekDeck (deckId, targetSample);
            return true;
        }
        default:
            return false;
    }
}
JUCE_END_IGNORE_WARNINGS_GCC_LIKE
