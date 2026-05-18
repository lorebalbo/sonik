#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <regex>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include "DeviceListChangeListener.h"
#include "MidiDeviceRecord.h"
#include "MidiHostInterface.h"
#include "MidiInboundEvent.h"
#include "MidiInputSubscriber.h"
#include "MidiOpenResult.h"

namespace sonik::midi
{
    /** Per PRD-0040: enumeration + hot-plug + open/close lifecycle for MIDI
        I/O. No mapping, no routing, no UI. */
    class MidiDeviceManager final : public juce::MidiInputCallback,
                                    private juce::Timer
    {
    public:
        static constexpr int MaxDevices     = 32;
        static constexpr int MaxSubscribers = 16;

        explicit MidiDeviceManager (MidiHostInterface& host);
        ~MidiDeviceManager() override;

        MidiDeviceManager (const MidiDeviceManager&)            = delete;
        MidiDeviceManager& operator= (const MidiDeviceManager&) = delete;

        // ---- Lifecycle (Message thread) -------------------------------------

        void initialise();

        // ---- Enumeration (Message thread) -----------------------------------

        std::vector<MidiDeviceRecord> getDevices() const;

        // ---- Open/close (Message thread) ------------------------------------

        MidiOpenResult openInput   (std::uint64_t deviceId);
        MidiOpenResult openOutput  (std::uint64_t deviceId);
        void           closeInput  (std::uint64_t deviceId);
        void           closeOutput (std::uint64_t deviceId);

        // ---- Output (Message thread) ----------------------------------------

        void sendOutput (std::uint64_t deviceId, const juce::MidiMessage& message);

        // ---- Listeners (Message thread) -------------------------------------

        void addDeviceListChangeListener    (DeviceListChangeListener* listener);
        void removeDeviceListChangeListener (DeviceListChangeListener* listener);

        // ---- Subscribers (Message thread for add/remove, MIDI callback thread for read) ----

        void addInputSubscriber    (MidiInputSubscriber* subscriber);
        void removeInputSubscriber (MidiInputSubscriber* subscriber);

        // ---- Auto-open rules (Message thread) -------------------------------

        void registerAutoOpenRule (const juce::String& manufacturerRegex,
                                   const juce::String& productNameRegex);

        // ---- PRD-0051: per-physical-USB-port disambiguation ----------------

        /** Returns true when the OS-reported `juce::MidiDeviceInfo::identifier`
            values are usable for per-port disambiguation, i.e. they have been
            observed non-empty and unique for every connected device since
            this manager was constructed. When this returns false, the UI must
            disable port-binding controls — the underlying identifier-based
            path is unavailable and the v1 ordinal-fallback is in effect. */
        bool isIdentifierBasedDisambiguationAvailable() const noexcept;

        // ---- juce::MidiInputCallback (MIDI callback thread) -----------------

        void handleIncomingMidiMessage (juce::MidiInput* source,
                                        const juce::MidiMessage& message) override;

    private:
        // Internal owning record (move-only, lives in `devices`).
        struct OwnedDevice
        {
            MidiDeviceRecord                 info;
            std::unique_ptr<juce::MidiInput>  input;
            std::unique_ptr<juce::MidiOutput> output;
        };

        // Pre-allocated O(1) lookup table populated on openInput / cleared on
        // closeInput, read by handleIncomingMidiMessage on the MIDI thread.
        // Slot atomics let the MIDI thread read consistent values without locks.
        struct DeviceIdLookup
        {
            std::atomic<juce::MidiInput*> input { nullptr };
            std::atomic<std::uint64_t>    deviceId { 0 };
        };

        struct AutoOpenRule
        {
            std::regex manufacturerRe;
            std::regex productNameRe;
        };

        // ---- juce::Timer ----------------------------------------------------
        void timerCallback() override;

        // ---- Internals (all Message thread except where noted) --------------
        void                       performEnumeration (bool firstPass);
        OwnedDevice*               findById   (std::uint64_t id) noexcept;
        const OwnedDevice*         findById   (std::uint64_t id) const noexcept;
        OwnedDevice*               findByIdAndDirection (std::uint64_t id, bool isInput) noexcept;
        std::uint64_t              computeDeviceId (const juce::MidiDeviceInfo& info,
                                                    const std::vector<juce::MidiDeviceInfo>& siblings,
                                                    int siblingIndex) const noexcept;
        void                       populateLookupSlot (juce::MidiInput* ptr, std::uint64_t id);
        void                       clearLookupSlot    (juce::MidiInput* ptr);
        void                       maybeAutoOpen      (const MidiDeviceRecord& record);
        void                       fireAdded   (std::uint64_t deviceId);
        void                       fireRemoved (std::uint64_t deviceId);
        void                       fireOpened  (std::uint64_t deviceId);
        void                       fireClosed  (std::uint64_t deviceId);

        MidiHostInterface&         host;

        std::vector<OwnedDevice>   devices;  // grows, never shrinks (preserves IDs on replug)
        std::vector<DeviceListChangeListener*> deviceListListeners;
        std::vector<AutoOpenRule>  autoOpenRules;

        // Lock-free subscriber array — written on Message thread, read on MIDI
        // thread. Empty slots store nullptr (tombstones); the MIDI reader
        // simply skips them.
        std::array<std::atomic<MidiInputSubscriber*>, MaxSubscribers> subscribers {};
        std::atomic<int> subscriberCount { 0 };

        // O(1) device-id resolution on the MIDI callback thread.
        std::array<DeviceIdLookup, MaxDevices> lookupTable;

        // PRD-0051: latched state — set to true on construction and cleared
        // (and never re-set) the first time we observe a sibling with an
        // empty/duplicate identifier. Read by `isIdentifierBasedDisambiguationAvailable()`.
        // `mutable` because the latch update happens inside the `const`
        // `computeDeviceId` helper.
        mutable std::atomic<bool> identifierPathAvailable { true };

        // One-shot DBG warning on the fallback path. `atomic_flag::test_and_set`
        // is the canonical "fire-once" primitive across all threads.
        mutable std::atomic_flag platformWarningFired = ATOMIC_FLAG_INIT;

        bool initialised = false;
    };
}
