#include "SoftTakeoverManager.h"

#include "../Deck/DeckIdentifiers.h"

#include <algorithm>

namespace sonik::midi
{
    //==========================================================================
    // MidiOriginatedWriteScope — thread-local re-entrancy marker.
    //==========================================================================
    namespace
    {
        thread_local int g_midiOriginatedDepth = 0;
    }

    MidiOriginatedWriteScope::MidiOriginatedWriteScope() noexcept
    {
        ++g_midiOriginatedDepth;
    }

    MidiOriginatedWriteScope::~MidiOriginatedWriteScope() noexcept
    {
        --g_midiOriginatedDepth;
    }

    bool MidiOriginatedWriteScope::isActive() noexcept
    {
        return g_midiOriginatedDepth > 0;
    }

    //==========================================================================
    // SoftTakeoverManager
    //==========================================================================
    SoftTakeoverManager::SoftTakeoverManager (juce::ValueTree rootStateTree,
                                              MappingStore&   store)
        : rootTree (std::move (rootStateTree)), mappingStoreRef (store)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        if (rootTree.isValid())
            rootTree.addListener (this);
        mappingStoreRef.addListener (this);
    }

    SoftTakeoverManager::~SoftTakeoverManager()
    {
        mappingStoreRef.removeListener (this);
        if (rootTree.isValid())
            rootTree.removeListener (this);
    }

    bool SoftTakeoverManager::shouldPassThrough (std::uint64_t      deviceId,
                                                 TargetIndex        target,
                                                 float              hardwareValue,
                                                 float              softwareValue,
                                                 SoftTakeoverPolicy policy)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        // Always / Never bypass the state machine but still drag the entry to
        // Engaged so a subsequent policy change to Pickup behaves sensibly.
        if (policy != SoftTakeoverPolicy::Pickup)
        {
            auto& entry = entries[Key { deviceId, target }];
            const bool wasEngaged = (entry.state == TakeoverState::Engaged);
            entry.state                    = TakeoverState::Engaged;
            entry.lastHardwareValue        = hardwareValue;
            entry.lastSoftwareValueAtCheck = softwareValue;
            if (! wasEngaged)
                notifyStateChanged (deviceId, target, TakeoverState::Engaged);
            return true;
        }

        auto& entry = entries[Key { deviceId, target }];

        if (entry.state == TakeoverState::Engaged)
        {
            entry.lastHardwareValue        = hardwareValue;
            entry.lastSoftwareValueAtCheck = softwareValue;
            return true;
        }

        // Disengaged path.
        if (entry.lastHardwareValue < 0.0f)
        {
            // First hardware sample after a reset: only record, suppress write.
            entry.lastHardwareValue        = hardwareValue;
            entry.lastSoftwareValueAtCheck = softwareValue;
            return false;
        }

        // Crossing detection: sign flip of (hw - sw) since the last sample, or
        // exact equality on this sample.
        const float prevDelta = entry.lastHardwareValue - softwareValue;
        const float currDelta = hardwareValue           - softwareValue;
        const bool  crossed   = (prevDelta == 0.0f)
                                || (currDelta == 0.0f)
                                || ((prevDelta < 0.0f) != (currDelta < 0.0f));

        entry.lastHardwareValue        = hardwareValue;
        entry.lastSoftwareValueAtCheck = softwareValue;

        if (crossed)
        {
            entry.state = TakeoverState::Engaged;
            notifyStateChanged (deviceId, target, TakeoverState::Engaged);
            return true;
        }
        return false;
    }

    void SoftTakeoverManager::resetForDevice (std::uint64_t deviceId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        for (auto it = entries.begin(); it != entries.end();)
        {
            if (it->first.deviceId == deviceId)
            {
                const bool wasEngaged = (it->second.state == TakeoverState::Engaged);
                const auto target = it->first.target;
                it = entries.erase (it);
                if (wasEngaged)
                    notifyStateChanged (deviceId, target, TakeoverState::Disengaged);
            }
            else
                ++it;
        }
    }

    void SoftTakeoverManager::resetForBinding (std::uint64_t deviceId, TargetIndex target)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        auto it = entries.find (Key { deviceId, target });
        if (it == entries.end())
            return;
        const bool wasEngaged = (it->second.state == TakeoverState::Engaged);
        entries.erase (it);
        if (wasEngaged)
            notifyStateChanged (deviceId, target, TakeoverState::Disengaged);
    }

    void SoftTakeoverManager::resetForTarget (TargetIndex target)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        for (auto it = entries.begin(); it != entries.end();)
        {
            if (it->first.target == target)
            {
                const bool wasEngaged = (it->second.state == TakeoverState::Engaged);
                const auto deviceId = it->first.deviceId;
                it = entries.erase (it);
                if (wasEngaged)
                    notifyStateChanged (deviceId, target, TakeoverState::Disengaged);
            }
            else
                ++it;
        }
    }

    void SoftTakeoverManager::forceEngage (std::uint64_t deviceId,
                                           TargetIndex   target,
                                           float         hardwareValue,
                                           float         softwareValue)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        auto& entry = entries[Key { deviceId, target }];
        const bool wasEngaged = (entry.state == TakeoverState::Engaged);
        entry.state                    = TakeoverState::Engaged;
        entry.lastHardwareValue        = hardwareValue;
        entry.lastSoftwareValueAtCheck = softwareValue;
        if (! wasEngaged)
            notifyStateChanged (deviceId, target, TakeoverState::Engaged);
    }

    TakeoverState SoftTakeoverManager::getState (std::uint64_t deviceId, TargetIndex target) const
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        const auto it = entries.find (Key { deviceId, target });
        return it == entries.end() ? TakeoverState::Disengaged : it->second.state;
    }

    bool SoftTakeoverManager::hasEntry (std::uint64_t deviceId, TargetIndex target) const
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        return entries.find (Key { deviceId, target }) != entries.end();
    }

    void SoftTakeoverManager::addListener (SoftTakeoverManagerListener* l)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        if (l != nullptr
            && std::find (listeners.begin(), listeners.end(), l) == listeners.end())
            listeners.push_back (l);
    }

    void SoftTakeoverManager::removeListener (SoftTakeoverManagerListener* l)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        listeners.erase (std::remove (listeners.begin(), listeners.end(), l), listeners.end());
    }

    void SoftTakeoverManager::notifyStateChanged (std::uint64_t deviceId,
                                                  TargetIndex   target,
                                                  TakeoverState s)
    {
        // Copy to permit listener self-removal during callback.
        auto snapshot = listeners;
        for (auto* l : snapshot)
            if (l != nullptr)
                l->takeoverStateChanged (deviceId, target, s);
    }

    //--------------------------------------------------------------------------
    void SoftTakeoverManager::activeMappingChanged (std::uint64_t deviceId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        resetForDevice (deviceId);
    }

    //--------------------------------------------------------------------------
    std::optional<MidiTargetCategory>
    SoftTakeoverManager::categoryForDeckProperty (const juce::Identifier& p)
    {
        if (p == IDs::pitch) return MidiTargetCategory::PitchFader;
        if (p == IDs::gain)  return MidiTargetCategory::Gain;
        return std::nullopt;
    }

    std::optional<std::uint8_t>
    SoftTakeoverManager::deckIndexFor (const juce::ValueTree& deckTree) const
    {
        if (! deckTree.isValid()) return std::nullopt;
        if (! deckTree.hasType (IDs::Deck)) return std::nullopt;
        auto decks = deckTree.getParent();
        if (! decks.isValid() || ! decks.hasType (IDs::Decks)) return std::nullopt;
        const int idx = decks.indexOf (deckTree);
        if (idx < 0 || idx > 3) return std::nullopt;
        return static_cast<std::uint8_t> (idx);
    }

    void SoftTakeoverManager::valueTreePropertyChanged (juce::ValueTree& tree,
                                                        const juce::Identifier& property)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        // Writes originating from a MIDI-handled inbound event are tagged via
        // the thread-local RAII scope; ignore those so the manager doesn't
        // reset bindings it just allowed through.
        if (MidiOriginatedWriteScope::isActive())
            return;

        const auto category = categoryForDeckProperty (property);
        if (! category.has_value())
            return;

        const auto deckIdx = deckIndexFor (tree);
        if (! deckIdx.has_value())
            return;

        const auto target = ControlTargetRegistry::findByCategoryAndDeck (*category, *deckIdx);
        if (! target.has_value())
            return;

        resetForTarget (*target);
    }
} // namespace sonik::midi
