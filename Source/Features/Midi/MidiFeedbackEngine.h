#pragma once

#include "ControlTargetRegistry.h"
#include "DeviceListChangeListener.h"
#include "MappingStore.h"
#include "MidiDeviceManager.h"

#include <juce_data_structures/juce_data_structures.h>

namespace sonik::midi
{
    /**
        PRD-0047 (slice 1): message-thread LED feedback for transport-play state.

        This first slice intentionally focuses on play-state feedback only:
        when a deck transitions playing/stopped, every mapped transport-play
        control for that deck receives outbound Note/CC on the corresponding
        device output.
    */
    class MidiFeedbackEngine final : private juce::ValueTree::Listener,
                                     private MappingStoreListener,
                                     private DeviceListChangeListener
    {
    public:
        MidiFeedbackEngine (juce::ValueTree rootState,
                            MidiDeviceManager& midiDeviceManager,
                            MappingStore& mappingStore);
        ~MidiFeedbackEngine() override;

        MidiFeedbackEngine (const MidiFeedbackEngine&) = delete;
        MidiFeedbackEngine& operator= (const MidiFeedbackEngine&) = delete;

    private:
        // juce::ValueTree::Listener
        void valueTreePropertyChanged (juce::ValueTree& treeWhosePropertyHasChanged,
                                       const juce::Identifier& property) override;

        // MappingStoreListener
        void activeMappingChanged (std::uint64_t deviceId) override;

        // DeviceListChangeListener
        void midiDeviceAdded  (std::uint64_t deviceId) override;
        void midiDeviceOpened (std::uint64_t deviceId) override;

        static bool isDeckPlaying (const juce::ValueTree& deckTree);
        static std::optional<std::uint8_t> deckIndexFromTree (const juce::ValueTree& deckTree);

        void sendBootDumpForDevice (std::uint64_t deviceId);
        void sendPlayFeedbackForDeckOnDevice (std::uint64_t deviceId,
                                              std::uint8_t deckIndex,
                                              bool isPlaying);

        juce::ValueTree root;
        MidiDeviceManager& devices;
        MappingStore& mappings;
    };
} // namespace sonik::midi
