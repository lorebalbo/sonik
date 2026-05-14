#include "MidiInboundRouter.h"

#include "BindingResolver.h"
#include "MidiDeviceManager.h"

#include <bit>

namespace sonik::midi
{
    //--------------------------------------------------------------------------
    MidiInboundRouter::MidiInboundRouter (MidiDeviceManager& dm,
                                          MidiMessageBridge& br,
                                          MappingStore&      st,
                                          MidiCommandHandler& handler)
        : deviceManager (dm),
          bridge        (br),
          store         (st),
          commandHandler (handler)
    {
        // Pre-allocate slots in default state. (DeviceState atomics already
        // zero-initialised by their in-class initialisers.)
        bridge.setMessageThreadSink (this);
        store.addListener (this);
        deviceManager.addDeviceListChangeListener (this);
        deviceManager.addInputSubscriber (this);

        // Seed slots for any devices already enumerated at construction time.
        for (const auto& rec : deviceManager.getDevices())
        {
            if (! rec.isInput)
                continue;
            if (auto* slot = getOrCreateSlot (rec.deviceId))
            {
                std::lock_guard lock (stateMutex);
                slot->activeMapping = store.getActiveMappingForDevice (rec.deviceId);
            }
        }
    }

    MidiInboundRouter::~MidiInboundRouter()
    {
        deviceManager.removeInputSubscriber (this);
        deviceManager.removeDeviceListChangeListener (this);
        store.removeListener (this);
        bridge.setMessageThreadSink (nullptr);
    }

    //--------------------------------------------------------------------------
    MidiInboundRouter::DeviceState* MidiInboundRouter::findSlot (std::uint64_t deviceId) noexcept
    {
        if (deviceId == 0)
            return nullptr;

        for (auto& slot : deviceStates)
        {
            if (slot.deviceId.load (std::memory_order_acquire) == deviceId)
                return &slot;
        }
        return nullptr;
    }

    MidiInboundRouter::DeviceState* MidiInboundRouter::getOrCreateSlot (std::uint64_t deviceId)
    {
        if (deviceId == 0)
            return nullptr;

        std::lock_guard lock (stateMutex);
        for (auto& slot : deviceStates)
        {
            if (slot.deviceId.load (std::memory_order_relaxed) == deviceId)
                return &slot;
        }
        for (auto& slot : deviceStates)
        {
            if (slot.deviceId.load (std::memory_order_relaxed) == 0)
            {
                slot.modifierMask.store (0, std::memory_order_relaxed);
                slot.resolverState.reset();
                slot.activeMapping.reset();
                slot.deviceId.store (deviceId, std::memory_order_release);
                return &slot;
            }
        }
        return nullptr; // Capacity exceeded; event will be dropped silently.
    }

    void MidiInboundRouter::refreshMappingFor (std::uint64_t deviceId)
    {
        auto fresh = store.getActiveMappingForDevice (deviceId);
        auto* slot = getOrCreateSlot (deviceId);
        if (slot == nullptr)
            return;

        std::lock_guard lock (stateMutex);
        slot->activeMapping = std::move (fresh);
    }

    //--------------------------------------------------------------------------
    void MidiInboundRouter::onMidiInbound (const MidiInboundEvent& event) noexcept
    {
        auto* slot = findSlot (event.deviceId);
        if (slot == nullptr)
        {
            unmatchedEventCount.fetch_add (1, std::memory_order_relaxed);
            return;
        }

        // Wait-free atomic-shared-ptr-style read: copy the shared_ptr under a
        // brief mutex (never held on the audio path; the MIDI callback thread
        // is allowed to take short locks under PRD-0040, and `stateMutex` is
        // never held across blocking operations). For the AC's "wait-free
        // atomic load", `std::atomic<std::shared_ptr<T>>` (C++20) would be
        // preferable but is not yet widely supported in libc++; the lock is
        // microsecond-scoped and contention is bounded by handler updates.
        std::shared_ptr<const Mapping> mapping;
        {
            std::lock_guard lock (stateMutex);
            mapping = slot->activeMapping;
        }
        if (mapping == nullptr)
        {
            unmatchedEventCount.fetch_add (1, std::memory_order_relaxed);
            return;
        }

        const auto currentMask = slot->modifierMask.load (std::memory_order_acquire);

        auto resolved = BindingResolver::resolve (*mapping, slot->resolverState, event, currentMask);
        if (! resolved.has_value())
            return;

        const auto& rb = *resolved;

        if (rb.category == MidiTargetCategory::ModifierSet)
        {
            const auto bit = static_cast<std::uint32_t> (rb.intDelta);
            if (bit < 32)
                slot->modifierMask.fetch_or (1u << bit, std::memory_order_release);
            return;
        }
        if (rb.category == MidiTargetCategory::ModifierClear)
        {
            const auto bit = static_cast<std::uint32_t> (rb.intDelta);
            if (bit < 32)
                slot->modifierMask.fetch_and (~(1u << bit), std::memory_order_release);
            return;
        }

        bridge.dispatch (rb.category,
                         rb.deckIndex,
                         rb.normalisedValue,
                         rb.intDelta,
                         event.deviceId,
                         rb.softTakeover);
    }

    //--------------------------------------------------------------------------
    void MidiInboundRouter::onMidiMessageThreadEvent (const MidiMessageEvent& event)
    {
        commandHandler.handle (event);
    }

    //--------------------------------------------------------------------------
    void MidiInboundRouter::userProfilesLoaded()
    {
        // Refresh every active slot — the user-profile arrival may shadow
        // bundled defaults for any device.
        std::vector<std::uint64_t> ids;
        ids.reserve (MaxTrackedDevices);
        for (auto& slot : deviceStates)
        {
            const auto id = slot.deviceId.load (std::memory_order_acquire);
            if (id != 0)
                ids.push_back (id);
        }
        for (auto id : ids)
            refreshMappingFor (id);
    }

    void MidiInboundRouter::activeMappingChanged (std::uint64_t deviceId)
    {
        refreshMappingFor (deviceId);
    }

    //--------------------------------------------------------------------------
    void MidiInboundRouter::midiDeviceAdded (std::uint64_t deviceId)
    {
        if (auto* slot = getOrCreateSlot (deviceId))
        {
            std::lock_guard lock (stateMutex);
            slot->activeMapping = store.getActiveMappingForDevice (deviceId);
        }
    }

    void MidiInboundRouter::midiDeviceRemoved (std::uint64_t deviceId)
    {
        std::lock_guard lock (stateMutex);
        for (auto& slot : deviceStates)
        {
            if (slot.deviceId.load (std::memory_order_relaxed) == deviceId)
            {
                slot.activeMapping.reset();
                slot.resolverState.reset();
                slot.modifierMask.store (0, std::memory_order_relaxed);
                slot.deviceId.store (0, std::memory_order_release);
                return;
            }
        }
    }
} // namespace sonik::midi
