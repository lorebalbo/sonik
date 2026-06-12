#include "MidiFeedbackEngine.h"

#include "../Deck/DeckIdentifiers.h"
#include "../Mixer/State/MixerIdentifiers.h"

#include <algorithm>
#include <cstring>

namespace sonik::midi
{
    namespace
    {
        constexpr int kBlinkTimerHz = 40;   // 25 ms tick.
        constexpr int kBootDumpStaggerMs = 5;

        std::uint8_t midiChannelFromKey (std::uint32_t k) noexcept
        {
            return static_cast<std::uint8_t> ((k >> 16) & 0xFFu);
        }

        std::uint8_t midiStatusFromKey (std::uint32_t k) noexcept
        {
            return static_cast<std::uint8_t> ((k >> 8) & 0xFFu);
        }

        std::uint8_t midiData1FromKey (std::uint32_t k) noexcept
        {
            return static_cast<std::uint8_t> (k & 0xFFu);
        }
    }

    //==========================================================================
    MidiFeedbackEngine::MidiFeedbackEngine (juce::ValueTree       rootState,
                                            MidiDeviceManager&    midiDeviceManager,
                                            MappingStore&         mappingStore,
                                            SoftTakeoverManager&  softTakeoverManager,
                                            bool                  testTapEnabledIn)
        : root (rootState),
          devices (midiDeviceManager),
          mappings (mappingStore),
          takeover (softTakeoverManager),
          testTapEnabled (testTapEnabledIn)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        // Output worker — sends are issued on its thread, never on Message
        // thread, never on the audio thread. In test-tap mode the worker's
        // send callback is a no-op: the test reads the side-channel tap to
        // verify behaviour without touching the (unsynchronised) device list.
        if (testTapEnabled)
        {
            outputThread = std::make_unique<MidiOutputThread> (
                [] (std::uint64_t, const juce::MidiMessage&) { return true; });
        }
        else
        {
            outputThread = std::make_unique<MidiOutputThread> (
                [this] (std::uint64_t deviceId, const juce::MidiMessage& msg)
                {
                    devices.sendOutput (deviceId, msg);
                    return true;
                });
        }

        root.addListener (this);
        mappings.addListener (this);
        devices.addDeviceListChangeListener (this);
        takeover.addListener (this);

        // Initial boot dump for any devices already enumerated.
        for (const auto& record : devices.getDevices())
            if (record.isInput)
                sendBootDumpForDevice (record.deviceId);
    }

    MidiFeedbackEngine::~MidiFeedbackEngine()
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        stopTimer();
        takeover.removeListener (this);
        devices.removeDeviceListChangeListener (this);
        mappings.removeListener (this);
        root.removeListener (this);
        outputThread.reset();
    }

    //==========================================================================
    // Source resolution
    //==========================================================================
    // This switch intentionally maps only the categories that HAVE a feedback
    // source; every other category falls through to SourceKind::None. A new
    // category without feedback must NOT force an edit here, so the
    // exhaustiveness warning is silenced for this one function.
    JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wswitch-enum")
    auto MidiFeedbackEngine::sourceKeyForBinding (const Binding& binding) -> SourceKey
    {
        const auto& tgt = ControlTargetRegistry::get (binding.target);
        SourceKey out {};
        out.deckIndex = tgt.deckIndex;

        switch (tgt.category)
        {
            case MidiTargetCategory::TransportPlay:
                out.kind = SourceKind::DeckPlaying;
                break;
            case MidiTargetCategory::TransportCue:
                out.kind = SourceKind::DeckPaused;
                break;
            case MidiTargetCategory::TransportSync:
                out.kind = SourceKind::DeckSynced;
                break;
            case MidiTargetCategory::LoopToggle:
                out.kind = SourceKind::LoopActive;
                break;
            case MidiTargetCategory::HotCueTrigger:
            {
                if (const auto cueIdx = parseHotCueIndexFromTargetId (tgt.id))
                {
                    out.kind     = (binding.feedback.style == FeedbackStyle::Colour)
                                       ? SourceKind::HotCueColour
                                       : SourceKind::HotCueValid;
                    out.auxIndex = *cueIdx;
                }
                break;
            }
            case MidiTargetCategory::PitchFader:
            case MidiTargetCategory::Gain:
            case MidiTargetCategory::EqHigh:
            case MidiTargetCategory::EqMid:
            case MidiTargetCategory::EqLow:
            case MidiTargetCategory::Crossfader:
            case MidiTargetCategory::MasterGain:
            case MidiTargetCategory::HeadphonesGain:
                out.kind = SourceKind::Continuous;
                break;

            // PRD-0061: per-channel mixer boolean LEDs.
            case MidiTargetCategory::ChannelKillHigh:
                out.kind = SourceKind::MixerChannelBool;
                out.auxIndex = static_cast<std::uint8_t> (MixerBoolProp::KillHigh);
                break;
            case MidiTargetCategory::ChannelKillMid:
                out.kind = SourceKind::MixerChannelBool;
                out.auxIndex = static_cast<std::uint8_t> (MixerBoolProp::KillMid);
                break;
            case MidiTargetCategory::ChannelKillLow:
                out.kind = SourceKind::MixerChannelBool;
                out.auxIndex = static_cast<std::uint8_t> (MixerBoolProp::KillLow);
                break;
            case MidiTargetCategory::ChannelAssignA:
                out.kind = SourceKind::MixerChannelBool;
                out.auxIndex = static_cast<std::uint8_t> (MixerBoolProp::AssignA);
                break;
            case MidiTargetCategory::ChannelAssignB:
                out.kind = SourceKind::MixerChannelBool;
                out.auxIndex = static_cast<std::uint8_t> (MixerBoolProp::AssignB);
                break;
            case MidiTargetCategory::ChannelCue:
                out.kind = SourceKind::MixerChannelBool;
                out.auxIndex = static_cast<std::uint8_t> (MixerBoolProp::Cue);
                break;
            default:
                break;
        }
        return out;
    }
    JUCE_END_IGNORE_WARNINGS_GCC_LIKE

    std::optional<std::uint8_t>
    MidiFeedbackEngine::parseHotCueIndexFromTargetId (const char* id)
    {
        // "deck.A.hotcue.<N>.trigger" — extract N (1-based).
        const auto* p = std::strstr (id, "hotcue.");
        if (p == nullptr)
            return std::nullopt;
        p += std::strlen ("hotcue.");
        int n = 0;
        bool any = false;
        while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); ++p; any = true; }
        if (! any || n < 1 || n > 8)
            return std::nullopt;
        return static_cast<std::uint8_t> (n);
    }

    juce::ValueTree MidiFeedbackEngine::deckTreeFor (std::uint8_t deckIndex) const
    {
        if (deckIndex > 3) return {};
        auto decks = root.getChildWithName (IDs::Decks);
        if (! decks.isValid()) return {};
        for (int i = 0; i < decks.getNumChildren(); ++i)
        {
            auto deck = decks.getChild (i);
            if (deckIndexFromTree (deck) == deckIndex)
                return deck;
        }
        return {};
    }

    std::optional<std::uint8_t> MidiFeedbackEngine::deckIndexFromTree (const juce::ValueTree& deckTree)
    {
        const auto id = deckTree.getProperty (IDs::id).toString().trim().toUpperCase();
        if (id == "A") return static_cast<std::uint8_t> (0);
        if (id == "B") return static_cast<std::uint8_t> (1);
        if (id == "C") return static_cast<std::uint8_t> (2);
        if (id == "D") return static_cast<std::uint8_t> (3);
        return std::nullopt;
    }

    auto MidiFeedbackEngine::readSourceValue (const SourceKey& key) const -> SourceValue
    {
        SourceValue out {};
        if (key.kind == SourceKind::None || key.deckIndex > 3)
            return out;

        // PRD-0061: mixer per-channel boolean sources read from MixerIDs::Mixer tree.
        if (key.kind == SourceKind::MixerChannelBool)
        {
            auto mixer = root.getChildWithName (MixerIDs::Mixer);
            if (! mixer.isValid()) return out;
            auto chContainer = mixer.getChildWithName (MixerIDs::channel);
            if (! chContainer.isValid()) return out;
            const juce::Identifier letters[] = { MixerIDs::A, MixerIDs::B, MixerIDs::C, MixerIDs::D };
            auto ch = chContainer.getChildWithName (letters[key.deckIndex]);
            if (! ch.isValid()) return out;

            const auto prop = static_cast<MixerBoolProp> (key.auxIndex);
            switch (prop)
            {
                case MixerBoolProp::AssignA:
                    out.boolValue = static_cast<bool> (ch.getProperty (MixerIDs::assignA, false));
                    out.valid = true;
                    return out;
                case MixerBoolProp::AssignB:
                    out.boolValue = static_cast<bool> (ch.getProperty (MixerIDs::assignB, false));
                    out.valid = true;
                    return out;
                case MixerBoolProp::Cue:
                    out.boolValue = static_cast<bool> (ch.getProperty (MixerIDs::cue, false));
                    out.valid = true;
                    return out;
                case MixerBoolProp::KillHigh:
                case MixerBoolProp::KillMid:
                case MixerBoolProp::KillLow:
                {
                    auto eq = ch.getChildWithName (MixerIDs::eq);
                    if (! eq.isValid()) return out;
                    const auto id = (prop == MixerBoolProp::KillHigh) ? MixerIDs::killHigh
                                  : (prop == MixerBoolProp::KillMid)  ? MixerIDs::killMid
                                                                      : MixerIDs::killLow;
                    out.boolValue = static_cast<bool> (eq.getProperty (id, false));
                    out.valid = true;
                    return out;
                }
            }
            return out;
        }

        auto deck = deckTreeFor (key.deckIndex);
        if (! deck.isValid())
            return out;

        switch (key.kind)
        {
            case SourceKind::DeckPlaying:
            {
                const auto s = deck.getProperty (IDs::playbackStatus, "").toString();
                out.boolValue = (s == "playing");
                out.valid = true;
                break;
            }
            case SourceKind::DeckPaused:
            {
                const auto s = deck.getProperty (IDs::playbackStatus, "").toString();
                out.boolValue = (s == "paused");
                out.valid = true;
                break;
            }
            case SourceKind::DeckSynced:
            {
                out.boolValue = static_cast<bool> (deck.getProperty (IDs::isSynced, false));
                out.valid = true;
                break;
            }
            case SourceKind::LoopActive:
            {
                auto loop = deck.getChildWithName (IDs::Loop);
                if (loop.isValid())
                {
                    out.boolValue = static_cast<bool> (loop.getProperty (IDs::active, false));
                    out.valid = true;
                }
                break;
            }
            case SourceKind::HotCueValid:
            case SourceKind::HotCueColour:
            {
                auto cues = deck.getChildWithName (IDs::CuePoints);
                if (! cues.isValid()) break;
                const int idx0 = static_cast<int> (key.auxIndex) - 1; // auxIndex is 1-based.
                if (idx0 < 0 || idx0 >= cues.getNumChildren()) break;
                auto cue = cues.getChild (idx0);
                out.boolValue = static_cast<bool> (cue.getProperty (IDs::isValid, false));
                out.intValue  = static_cast<int>  (cue.getProperty (IDs::colorIndex, 0));
                out.valid     = true;
                break;
            }
            case SourceKind::Continuous:
            {
                // Coarse: read the deck's `pitch` or `gain` property as float.
                // PRD-0047 only requires the Continuous CC computation; the
                // exact source ValueTree path per-category is intentionally
                // pluggable. For the DDM4000 bundled profile, no continuous
                // feedback is declared today, so this branch is exercised by
                // tests only.
                const auto v = deck.getProperty (IDs::pitch, 0.0);
                out.floatValue = juce::jlimit (0.0f, 1.0f, static_cast<float> (v));
                out.valid = true;
                break;
            }
            case SourceKind::None:             // early-returned above
            case SourceKind::MixerChannelBool: // handled by the branch above
            default: break;
        }
        return out;
    }

    std::optional<std::uint8_t>
    MidiFeedbackEngine::computeFeedbackVelocity (const BindingFeedback& fb,
                                                 const SourceValue&     source)
    {
        if (fb.style == FeedbackStyle::None || fb.midiKey == 0)
            return std::nullopt;
        if (! source.valid)
            return std::nullopt;

        switch (fb.style)
        {
            case FeedbackStyle::Binary:
                return source.boolValue ? fb.onValue : fb.offValue;

            case FeedbackStyle::Colour:
            {
                const int idx = std::clamp (source.intValue, 0, 15);
                // Lit only when source.boolValue (i.e., hot-cue isValid).
                return source.boolValue ? fb.paletteVelocities[idx] : fb.offValue;
            }
            case FeedbackStyle::Continuous:
            {
                const float v = std::clamp (source.floatValue, 0.0f, 1.0f);
                const float out = (fb.curve == FeedbackCurve::LinearInverse)
                                      ? (1.0f - v) : v;
                return static_cast<std::uint8_t> (std::lround (out * 127.0f));
            }
            case FeedbackStyle::None: // early-returned above
            default:
                return std::nullopt;
        }
    }

    //==========================================================================
    // Enqueueing
    //==========================================================================
    void MidiFeedbackEngine::enqueueOutbound (std::uint64_t deviceId,
                                              const BindingFeedback& fb,
                                              std::uint8_t value,
                                              std::chrono::steady_clock::time_point earliestSendTime)
    {
        if (fb.midiKey == 0)
            return;

        OutboundMidiEvent ev {};
        ev.deviceId        = deviceId;
        ev.deviceEpoch     = outputThread->currentDeviceEpoch (deviceId);
        ev.status          = midiStatusFromKey (fb.midiKey);
        ev.channel         = midiChannelFromKey (fb.midiKey);
        ev.data1           = midiData1FromKey (fb.midiKey);
        ev.value           = value;
        ev.earliestSendTime = earliestSendTime;

        if (ev.status != 0x90 && ev.status != 0xB0)
            return; // PRD-0047 supports Note/CC only.

        if (testTapEnabled)
        {
            const juce::ScopedLock sl (testTapLock);
            testTap.push_back (ev);
        }

        if (outputThread->fifo().push (ev))
            outputThread->notify();
    }

    void MidiFeedbackEngine::emitFeedbackForBinding (std::uint64_t deviceId,
                                                     const Binding& binding,
                                                     std::chrono::steady_clock::time_point earliestSendTime)
    {
        // Skip if a blink is currently active for this (deviceId, target):
        // the blink owns the LED.
        for (const auto& b : blinks)
            if (b.deviceId == deviceId && b.target == binding.target)
                return;

        // Determine which feedback POD to use. PRD-0047 stores the regular
        // feedback in `binding.feedback`; `disengagedFeedback` is owned by
        // the blink path.
        if (binding.feedback.style == FeedbackStyle::None && binding.feedback.midiKey == 0)
            return;

        const auto src = readSourceValue (sourceKeyForBinding (binding));
        if (! src.valid)
        {
            // Source not available: emit off-value so LEDs settle.
            if (binding.feedback.midiKey != 0)
                enqueueOutbound (deviceId, binding.feedback, binding.feedback.offValue, earliestSendTime);
            return;
        }

        // For bindings without an explicit `style` (legacy/back-compat), the
        // parser sets style = Binary. Treat midiKey-set + style==None as binary.
        BindingFeedback effective = binding.feedback;
        if (effective.style == FeedbackStyle::None && effective.midiKey != 0)
            effective.style = FeedbackStyle::Binary;

        if (const auto v = computeFeedbackVelocity (effective, src))
            enqueueOutbound (deviceId, effective, *v, earliestSendTime);
    }

    //==========================================================================
    // Dispatch on VT change
    //==========================================================================
    void MidiFeedbackEngine::valueTreePropertyChanged (juce::ValueTree& tree,
                                                       const juce::Identifier& property)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        // Map (tree.type, property) → SourceKey(kind, deckIndex, auxIndex?)

        SourceKey key {};

        const auto type = tree.getType();
        if (type == IDs::Deck)
        {
            const auto deckIdx = deckIndexFromTree (tree);
            if (! deckIdx) return;

            if (property == IDs::playbackStatus)
            {
                // Drives both Play LEDs and Cue LEDs.
                key = { SourceKind::DeckPlaying, *deckIdx, 0 };
                dispatchSourceChanged (key);
                key = { SourceKind::DeckPaused, *deckIdx, 0 };
                dispatchSourceChanged (key);
                return;
            }
            if (property == IDs::isSynced)
            {
                key = { SourceKind::DeckSynced, *deckIdx, 0 };
                dispatchSourceChanged (key);
                return;
            }
            if (property == IDs::pitch || property == IDs::gain)
            {
                key = { SourceKind::Continuous, *deckIdx, 0 };
                dispatchSourceChanged (key);
                return;
            }
        }
        else if (type == IDs::Loop)
        {
            // Loop child of Deck.
            const auto deck = tree.getParent();
            const auto deckIdx = deckIndexFromTree (deck);
            if (! deckIdx) return;

            if (property == IDs::active)
            {
                key = { SourceKind::LoopActive, *deckIdx, 0 };
                dispatchSourceChanged (key);
            }
        }
        else if (type == IDs::CuePoint)
        {
            // CuePoint → CuePoints → Deck.
            const auto cues = tree.getParent();
            const auto deck = cues.getParent();
            const auto deckIdx = deckIndexFromTree (deck);
            if (! deckIdx) return;
            const int idx0 = cues.indexOf (tree);
            if (idx0 < 0 || idx0 > 7) return;
            const auto cueIndex1Based = static_cast<std::uint8_t> (idx0 + 1);

            if (property == IDs::isValid || property == IDs::colorIndex)
            {
                key = { SourceKind::HotCueValid,  *deckIdx, cueIndex1Based };
                dispatchSourceChanged (key);
                key = { SourceKind::HotCueColour, *deckIdx, cueIndex1Based };
                dispatchSourceChanged (key);
            }
        }
        else
        {
            // PRD-0061: mixer channel tree (MixerIDs::A/B/C/D) — top-level
            // properties (assignA/B, cue) live here; kill* lives on the
            // nested MixerIDs::eq tree.
            auto mixerChannelIndex = [] (const juce::Identifier& id) -> int
            {
                if (id == MixerIDs::A) return 0;
                if (id == MixerIDs::B) return 1;
                if (id == MixerIDs::C) return 2;
                if (id == MixerIDs::D) return 3;
                return -1;
            };

            const int idxFromSelf = mixerChannelIndex (type);
            if (idxFromSelf >= 0)
            {
                std::uint8_t aux = 0;
                if (property == MixerIDs::assignA) aux = static_cast<std::uint8_t> (MixerBoolProp::AssignA);
                else if (property == MixerIDs::assignB) aux = static_cast<std::uint8_t> (MixerBoolProp::AssignB);
                else if (property == MixerIDs::cue)     aux = static_cast<std::uint8_t> (MixerBoolProp::Cue);
                if (aux != 0)
                {
                    key = { SourceKind::MixerChannelBool,
                            static_cast<std::uint8_t> (idxFromSelf), aux };
                    dispatchSourceChanged (key);
                }
            }
            else if (type == MixerIDs::eq)
            {
                const int idxFromParent = mixerChannelIndex (tree.getParent().getType());
                if (idxFromParent >= 0)
                {
                    std::uint8_t aux = 0;
                    if (property == MixerIDs::killHigh) aux = static_cast<std::uint8_t> (MixerBoolProp::KillHigh);
                    else if (property == MixerIDs::killMid)  aux = static_cast<std::uint8_t> (MixerBoolProp::KillMid);
                    else if (property == MixerIDs::killLow)  aux = static_cast<std::uint8_t> (MixerBoolProp::KillLow);
                    if (aux != 0)
                    {
                        key = { SourceKind::MixerChannelBool,
                                static_cast<std::uint8_t> (idxFromParent), aux };
                        dispatchSourceChanged (key);
                    }
                }
            }
        }
    }

    void MidiFeedbackEngine::dispatchSourceChanged (const SourceKey& key)
    {
        if (key.kind == SourceKind::None)
            return;
        const auto src = readSourceValue (key);

        // Walk every connected device's mapping.
        for (const auto& record : devices.getDevices())
        {
            if (! record.isInput)
                continue;
            const auto mapping = mappings.getActiveMappingForDevice (record.deviceId);
            if (mapping == nullptr) continue;

            for (const auto& binding : mapping->bindings)
            {
                const auto bk = sourceKeyForBinding (binding);
                if (bk.kind != key.kind) continue;
                if (bk.deckIndex != key.deckIndex) continue;
                if ((bk.kind == SourceKind::HotCueValid || bk.kind == SourceKind::HotCueColour)
                    && bk.auxIndex != key.auxIndex)
                    continue;
                if (bk.kind == SourceKind::MixerChannelBool && bk.auxIndex != key.auxIndex)
                    continue;

                // Reject styles that don't match this kind (Colour binding
                // ignores DeckPlaying, etc.). The lookup above already aligns
                // them, so just emit.

                if (binding.feedback.style == FeedbackStyle::None
                    && binding.feedback.midiKey == 0)
                    continue;

                // Skip while blinking.
                bool blinking = false;
                for (const auto& b : blinks)
                    if (b.deviceId == record.deviceId && b.target == binding.target)
                    { blinking = true; break; }
                if (blinking) continue;

                BindingFeedback effective = binding.feedback;
                if (effective.style == FeedbackStyle::None && effective.midiKey != 0)
                    effective.style = FeedbackStyle::Binary;

                if (const auto v = computeFeedbackVelocity (effective, src))
                    enqueueOutbound (record.deviceId, effective, *v,
                                     std::chrono::steady_clock::now());
            }
        }
    }

    //==========================================================================
    // Listener entries
    //==========================================================================
    void MidiFeedbackEngine::activeMappingChanged (std::uint64_t deviceId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        const auto newMapping = mappings.getActiveMappingForDevice (deviceId);

        // Blackout: anything the OLD mapping addressed that the NEW one doesn't.
        if (auto it = lastDumpedByDevice.find (deviceId); it != lastDumpedByDevice.end())
        {
            if (auto old = it->second; old != nullptr)
                sendBlackoutDumpForOldMapping (deviceId, *old, newMapping.get());
        }

        sendBootDumpForDevice (deviceId);
        lastDumpedByDevice[deviceId] = newMapping;
    }

    void MidiFeedbackEngine::midiDeviceAdded (std::uint64_t deviceId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        sendBootDumpForDevice (deviceId);
        if (auto m = mappings.getActiveMappingForDevice (deviceId))
            lastDumpedByDevice[deviceId] = m;
    }

    void MidiFeedbackEngine::midiDeviceOpened (std::uint64_t deviceId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        sendBootDumpForDevice (deviceId);
        if (auto m = mappings.getActiveMappingForDevice (deviceId))
            lastDumpedByDevice[deviceId] = m;
    }

    void MidiFeedbackEngine::midiDeviceRemoved (std::uint64_t deviceId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        // Drop every queued message for this device.
        outputThread->bumpDeviceEpoch (deviceId);
        // Drop any blink entries for this device.
        blinks.erase (std::remove_if (blinks.begin(), blinks.end(),
                                       [deviceId] (const BlinkEntry& b) { return b.deviceId == deviceId; }),
                      blinks.end());
        lastDumpedByDevice.erase (deviceId);
    }

    void MidiFeedbackEngine::midiDeviceClosed (std::uint64_t deviceId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        outputThread->bumpDeviceEpoch (deviceId);
        blinks.erase (std::remove_if (blinks.begin(), blinks.end(),
                                       [deviceId] (const BlinkEntry& b) { return b.deviceId == deviceId; }),
                      blinks.end());
    }

    //==========================================================================
    // Boot dump & blackout
    //==========================================================================
    void MidiFeedbackEngine::sendBootDumpForDevice (std::uint64_t deviceId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        const auto mapping = mappings.getActiveMappingForDevice (deviceId);
        if (mapping == nullptr) return;

        auto sendTime = std::chrono::steady_clock::now();
        for (const auto& binding : mapping->bindings)
        {
            if (binding.feedback.style == FeedbackStyle::None && binding.feedback.midiKey == 0)
                continue;
            emitFeedbackForBinding (deviceId, binding, sendTime);
            sendTime += std::chrono::milliseconds (kBootDumpStaggerMs);
        }
    }

    void MidiFeedbackEngine::sendBlackoutDumpForOldMapping (std::uint64_t deviceId,
                                                            const Mapping& oldMapping,
                                                            const Mapping* newMapping)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        auto sendTime = std::chrono::steady_clock::now();

        // Build a set of (status, channel, data1) triples addressed by newMapping.
        std::vector<std::uint32_t> newAddressedKeys;
        if (newMapping != nullptr)
        {
            newAddressedKeys.reserve (newMapping->bindings.size());
            for (const auto& nb : newMapping->bindings)
                if (nb.feedback.midiKey != 0)
                    newAddressedKeys.push_back (nb.feedback.midiKey);
        }

        for (const auto& binding : oldMapping.bindings)
        {
            const auto fb = binding.feedback;
            if (fb.midiKey == 0)
                continue;
            // Only Binary/Colour are turned off via off-velocity. Continuous
            // feedbacks ride along but their "off" interpretation is moot.
            if (fb.style == FeedbackStyle::Continuous)
                continue;

            if (std::find (newAddressedKeys.begin(), newAddressedKeys.end(), fb.midiKey)
                != newAddressedKeys.end())
                continue; // New mapping owns this key; let its boot dump handle it.

            enqueueOutbound (deviceId, fb, fb.offValue, sendTime);
            sendTime += std::chrono::milliseconds (kBootDumpStaggerMs);
        }
    }

    //==========================================================================
    // Soft-takeover integration & blink
    //==========================================================================
    void MidiFeedbackEngine::takeoverStateChanged (std::uint64_t deviceId,
                                                   TargetIndex   target,
                                                   TakeoverState newState)
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        const auto mapping = mappings.getActiveMappingForDevice (deviceId);
        if (mapping == nullptr) return;

        // Find the binding for (device, target) — assumes one feedback per
        // target per device.
        const Binding* bindingPtr = nullptr;
        for (const auto& b : mapping->bindings)
        {
            if (b.target == target) { bindingPtr = &b; break; }
        }
        if (bindingPtr == nullptr) return;

        if (newState == TakeoverState::Disengaged)
        {
            const auto& dfb = bindingPtr->disengagedFeedback;
            if (dfb.midiKey == 0 || dfb.style == FeedbackStyle::None)
                return;
            startBlink (deviceId, target, dfb);
        }
        else
        {
            cancelBlink (deviceId, target);
            // Re-emit the regular feedback so the LED reflects current state.
            emitFeedbackForBinding (deviceId, *bindingPtr,
                                    std::chrono::steady_clock::now());
        }
    }

    std::vector<MidiFeedbackEngine::BlinkEntry>::iterator
    MidiFeedbackEngine::findBlink (std::uint64_t deviceId, TargetIndex target)
    {
        return std::find_if (blinks.begin(), blinks.end(),
                             [deviceId, target] (const BlinkEntry& b)
                             { return b.deviceId == deviceId && b.target == target; });
    }

    void MidiFeedbackEngine::startBlink (std::uint64_t deviceId,
                                         TargetIndex   target,
                                         const BindingFeedback& fb)
    {
        if (auto it = findBlink (deviceId, target); it != blinks.end())
            return; // already blinking

        BlinkEntry e {};
        e.deviceId = deviceId;
        e.target   = target;
        e.feedback = fb;
        const float hz = (fb.blinkHz > 0.0f) ? fb.blinkHz : 2.0f;
        // Two MIDI events per blink cycle (on, off) → period = 1/(2*hz).
        e.periodMs = static_cast<int> (std::round (1000.0f / (2.0f * hz)));
        if (e.periodMs < 25) e.periodMs = 25;
        e.nextToggleAt = std::chrono::steady_clock::now();
        e.currentlyOn  = false;
        blinks.push_back (e);

        // Send the first toggle immediately to start visual confirmation.
        timerCallback();
        if (! isTimerRunning())
            startTimerHz (kBlinkTimerHz);
    }

    void MidiFeedbackEngine::cancelBlink (std::uint64_t deviceId, TargetIndex target)
    {
        auto it = findBlink (deviceId, target);
        if (it == blinks.end()) return;

        // Emit an explicit "off" so the LED settles to dark before the regular
        // feedback re-asserts.
        enqueueOutbound (it->deviceId, it->feedback, it->feedback.offValue,
                         std::chrono::steady_clock::now());
        blinks.erase (it);
        if (blinks.empty())
            stopTimer();
    }

    void MidiFeedbackEngine::timerCallback()
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        const auto now = std::chrono::steady_clock::now();

        for (auto& b : blinks)
        {
            if (now < b.nextToggleAt) continue;

            b.currentlyOn = ! b.currentlyOn;
            const std::uint8_t val = b.currentlyOn ? b.feedback.onValue : b.feedback.offValue;
            enqueueOutbound (b.deviceId, b.feedback, val, now);
            b.nextToggleAt = now + std::chrono::milliseconds (b.periodMs);
        }
    }

    //==========================================================================
    // Test helpers
    //==========================================================================
    bool MidiFeedbackEngine::drainOneForTest (OutboundMidiEvent& out) noexcept
    {
        const juce::ScopedLock sl (testTapLock);
        if (testTap.empty())
            return false;
        out = testTap.front();
        testTap.pop_front();
        return true;
    }

    int MidiFeedbackEngine::pendingForTest() const noexcept
    {
        const juce::ScopedLock sl (testTapLock);
        return static_cast<int> (testTap.size());
    }

    bool MidiFeedbackEngine::isBlinkingForTest (std::uint64_t deviceId,
                                                TargetIndex target) const noexcept
    {
        for (const auto& b : blinks)
            if (b.deviceId == deviceId && b.target == target)
                return true;
        return false;
    }
} // namespace sonik::midi
