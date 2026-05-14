#include "MidiFeedbackEngine.h"

#include "../Deck/DeckIdentifiers.h"

namespace sonik::midi
{
    namespace
    {
        std::uint8_t midiChannelFromKey (std::uint32_t midiKey)
        {
            return static_cast<std::uint8_t> ((midiKey >> 16) & 0xFFu);
        }

        std::uint8_t midiStatusFromKey (std::uint32_t midiKey)
        {
            return static_cast<std::uint8_t> ((midiKey >> 8) & 0xFFu);
        }

        std::uint8_t midiData1FromKey (std::uint32_t midiKey)
        {
            return static_cast<std::uint8_t> (midiKey & 0xFFu);
        }
    }

    MidiFeedbackEngine::MidiFeedbackEngine (juce::ValueTree rootState,
                                            MidiDeviceManager& midiDeviceManager,
                                            MappingStore& mappingStore)
        : root (rootState),
          devices (midiDeviceManager),
          mappings (mappingStore)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        root.addListener (this);
        mappings.addListener (this);
        devices.addDeviceListChangeListener (this);

        // Push an initial boot dump for currently enumerated input devices so
        // LED state is coherent after app startup.
        for (const auto& record : devices.getDevices())
        {
            if (record.isInput)
                sendBootDumpForDevice (record.deviceId);
        }
    }

    MidiFeedbackEngine::~MidiFeedbackEngine()
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        devices.removeDeviceListChangeListener (this);
        mappings.removeListener (this);
        root.removeListener (this);
    }

    void MidiFeedbackEngine::valueTreePropertyChanged (juce::ValueTree& changedTree,
                                                       const juce::Identifier& property)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        if (property != IDs::playbackStatus)
            return;
        if (changedTree.getType() != IDs::Deck)
            return;

        const auto deckIndex = deckIndexFromTree (changedTree);
        if (! deckIndex.has_value())
            return;

        DBG("[MidiFeedbackEngine] valueTreePropertyChanged: property=" << property.toString());

        const bool playing = isDeckPlaying (changedTree);

        DBG("[MidiFeedbackEngine] Deck playback status changed: deckIndex=" << static_cast<int>(*deckIndex)
            << " playing=" << (playing ? "YES" : "NO"));

        for (const auto& record : devices.getDevices())
        {
            if (! record.isInput)
                continue;
            DBG("[MidiFeedbackEngine] Sending feedback to device " << static_cast<int>(record.deviceId));
            sendPlayFeedbackForDeckOnDevice (record.deviceId, *deckIndex, playing);
        }
    }

    void MidiFeedbackEngine::activeMappingChanged (std::uint64_t deviceId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        sendBootDumpForDevice (deviceId);
    }

    void MidiFeedbackEngine::midiDeviceAdded (std::uint64_t deviceId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        sendBootDumpForDevice (deviceId);
    }

    void MidiFeedbackEngine::midiDeviceOpened (std::uint64_t deviceId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        sendBootDumpForDevice (deviceId);
    }

    bool MidiFeedbackEngine::isDeckPlaying (const juce::ValueTree& deckTree)
    {
        return deckTree.getProperty (IDs::playbackStatus, "empty").toString() == "playing";
    }

    std::optional<std::uint8_t> MidiFeedbackEngine::deckIndexFromTree (const juce::ValueTree& deckTree)
    {
        const auto deckId = deckTree.getProperty (IDs::id).toString().trim().toUpperCase();
        if (deckId == "A") return static_cast<std::uint8_t> (0);
        if (deckId == "B") return static_cast<std::uint8_t> (1);
        if (deckId == "C") return static_cast<std::uint8_t> (2);
        if (deckId == "D") return static_cast<std::uint8_t> (3);
        return std::nullopt;
    }

    void MidiFeedbackEngine::sendBootDumpForDevice (std::uint64_t deviceId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        auto decks = root.getChildWithName (IDs::Decks);
        for (int i = 0; i < decks.getNumChildren(); ++i)
        {
            auto deckTree = decks.getChild (i);
            const auto deckIndex = deckIndexFromTree (deckTree);
            if (! deckIndex.has_value())
                continue;
            sendPlayFeedbackForDeckOnDevice (deviceId, *deckIndex, isDeckPlaying (deckTree));
        }
    }

    void MidiFeedbackEngine::sendPlayFeedbackForDeckOnDevice (std::uint64_t deviceId,
                                                               std::uint8_t deckIndex,
                                                               bool isPlaying)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        DBG("[MidiFeedbackEngine] sendPlayFeedbackForDeckOnDevice: deckIndex=" << static_cast<int>(deckIndex)
            << " isPlaying=" << (isPlaying ? "YES" : "NO"));

        const auto mapping = mappings.getActiveMappingForDevice (deviceId);
        if (mapping == nullptr)
        {
            DBG("[MidiFeedbackEngine] No active mapping found for device");
            return;
        }

        DBG("[MidiFeedbackEngine] Found mapping with " << static_cast<int>(mapping->bindings.size()) << " bindings");

        for (const auto& binding : mapping->bindings)
        {
            const auto& target = ControlTargetRegistry::get (binding.target);
            if (target.category != MidiTargetCategory::TransportPlay)
                continue;
            if (target.deckIndex != deckIndex)
                continue;

            DBG("[MidiFeedbackEngine] Found matching TransportPlay binding for Deck " << static_cast<int>(deckIndex));

            const auto feedbackKey = (binding.feedback.midiKey != 0)
                                   ? binding.feedback.midiKey
                                   : binding.midiKey;
            const auto status = midiStatusFromKey (feedbackKey);
            const auto channel = midiChannelFromKey (feedbackKey);
            const auto data1 = midiData1FromKey (feedbackKey);

            const auto onValue = (binding.feedback.midiKey != 0) ? binding.feedback.onValue
                                                                  : static_cast<std::uint8_t> (127);
            const auto offValue = (binding.feedback.midiKey != 0) ? binding.feedback.offValue
                                                                   : static_cast<std::uint8_t> (0);
            const auto value = static_cast<int> (isPlaying ? onValue : offValue);

            DBG("[MidiFeedbackEngine] MIDI: status=" << static_cast<int>(status)
                << " channel=" << static_cast<int>(channel) << " data1=" << static_cast<int>(data1)
                << " value=" << value << " (on=" << static_cast<int>(onValue) << " off=" << static_cast<int>(offValue) << ")");

            juce::MidiMessage message;
            if (status == 0x90)
            {
                message = juce::MidiMessage::noteOn (static_cast<int> (channel),
                                                     static_cast<int> (data1),
                                                     static_cast<juce::uint8> (value));
                DBG("[MidiFeedbackEngine] Created Note On message");
            }
            else if (status == 0xB0)
            {
                message = juce::MidiMessage::controllerEvent (static_cast<int> (channel),
                                                              static_cast<int> (data1),
                                                              value);
                DBG("[MidiFeedbackEngine] Created CC message");
            }
            else
            {
                DBG("[MidiFeedbackEngine] Unsupported status: " << static_cast<int>(status));
                continue; // Unsupported outbound type in slice 1.
            }

            DBG("[MidiFeedbackEngine] Sending MIDI message...");
            devices.sendOutput (deviceId, message);
            DBG("[MidiFeedbackEngine] sendOutput() completed");
        }
    }
} // namespace sonik::midi
