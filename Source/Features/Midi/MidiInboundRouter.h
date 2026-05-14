#pragma once
//==============================================================================
// PRD-0044: MidiInboundRouter.
//
// One-and-only consumer of `MidiInboundEvent` for routing. Subscribes to
// `MidiDeviceManager` for inbound MIDI, to `MappingStore` for per-device
// active-mapping changes, and to `MidiMessageBridge` as the
// `MessageThreadSink`.
//
// THREAD CONTRACT
//   * `onMidiInbound`         : MIDI callback thread. noexcept. No
//                               allocation. No locking. Atomic loads only.
//   * `onMidiMessageThreadEvent`: Message thread (via callAsync from bridge).
//   * `activeMappingChanged`  : Message thread. Updates the device-state cache.
//   * Listener add/remove and ctor/dtor: Message thread.
//==============================================================================

#include "DeviceListChangeListener.h"
#include "MappingStore.h"
#include "MappingTypes.h"
#include "MidiCommandHandler.h"
#include "MidiInboundEvent.h"
#include "MidiInputSubscriber.h"
#include "MidiMessageBridge.h"
#include "MidiMessageEvent.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace sonik::midi
{
    class MidiDeviceManager;

    class MidiInboundRouter final : public MidiInputSubscriber,
                                    public MessageThreadSink,
                                    public MappingStoreListener,
                                    public DeviceListChangeListener
    {
    public:
        static constexpr int MaxTrackedDevices = 8;

        MidiInboundRouter (MidiDeviceManager& deviceManager,
                           MidiMessageBridge& bridge,
                           MappingStore&      store,
                           MidiCommandHandler& handler);
        ~MidiInboundRouter() override;

        MidiInboundRouter (const MidiInboundRouter&)            = delete;
        MidiInboundRouter& operator= (const MidiInboundRouter&) = delete;

        // ---- MidiInputSubscriber (MIDI callback thread) --------------------
        void onMidiInbound (const MidiInboundEvent& event) noexcept override;

        // ---- MessageThreadSink (Message thread) ----------------------------
        void onMidiMessageThreadEvent (const MidiMessageEvent& event) override;

        // ---- MappingStoreListener (Message thread) -------------------------
        void userProfilesLoaded() override;
        void activeMappingChanged (std::uint64_t deviceId) override;
        void userMappingSaved (juce::String, SaveResult) override {}

        // ---- DeviceListChangeListener (Message thread) ---------------------
        void midiDeviceAdded   (std::uint64_t deviceId) override;
        void midiDeviceRemoved (std::uint64_t deviceId) override;
        void midiDeviceOpened  (std::uint64_t) override {}
        void midiDeviceClosed  (std::uint64_t) override {}

        // ---- Diagnostics (any thread) --------------------------------------
        std::uint64_t getUnmatchedEventCount() const noexcept
        {
            return unmatchedEventCount.load (std::memory_order_relaxed);
        }

    private:
        struct DeviceState
        {
            std::atomic<std::uint64_t> deviceId    { 0 }; // 0 = unused slot
            std::atomic<std::uint32_t> modifierMask{ 0 };
            // Cached active mapping. Writes guarded by stateMutex; reads on
            // the MIDI callback thread use a relaxed pointer-copy. Because
            // the underlying Mapping is immutable, holding a stale
            // shared_ptr remains correct even across a swap.
            std::shared_ptr<const Mapping> activeMapping;
            ResolverState                  resolverState;
        };

        // Acquire a slot for `deviceId`, populating it on first sight.
        // Returns nullptr if all slots are occupied.
        DeviceState* getOrCreateSlot (std::uint64_t deviceId);
        // Wait-free lookup for the MIDI callback thread. Returns nullptr if
        // no slot exists for this device.
        DeviceState* findSlot (std::uint64_t deviceId) noexcept;
        // Re-resolve the cached mapping for one device from the store.
        void refreshMappingFor (std::uint64_t deviceId);

        MidiDeviceManager&  deviceManager;
        MidiMessageBridge&  bridge;
        MappingStore&       store;
        MidiCommandHandler& commandHandler;

        std::array<DeviceState, MaxTrackedDevices> deviceStates;

        // Protects slot allocation and `activeMapping` writes. Never taken
        // on the MIDI callback thread.
        mutable std::mutex stateMutex;

        std::atomic<std::uint64_t> unmatchedEventCount { 0 };
    };
} // namespace sonik::midi
