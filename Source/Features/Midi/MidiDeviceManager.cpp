#include "MidiDeviceManager.h"

#include "Sha1.h"

namespace sonik::midi
{
    namespace
    {
        constexpr int kPollIntervalMs = 1000;
    }

    MidiDeviceManager::MidiDeviceManager (MidiHostInterface& hostRef)
        : host (hostRef)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        devices.reserve (MaxDevices);
    }

    MidiDeviceManager::~MidiDeviceManager()
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        stopTimer();

        for (auto& dev : devices)
        {
            if (dev.input != nullptr)
            {
                dev.input->stop();
                clearLookupSlot (dev.input.get());
                dev.input.reset();
                dev.info.isOpen = false;
            }
            dev.output.reset();
        }
    }

    void MidiDeviceManager::initialise()
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        if (initialised)
            return;

        performEnumeration (true);
        startTimer (kPollIntervalMs);
        initialised = true;
    }

    // ---------------- Enumeration / hot-plug ----------------------------------

    void MidiDeviceManager::timerCallback()
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        performEnumeration (false);
    }

    void MidiDeviceManager::performEnumeration (bool firstPass)
    {
        const auto inputs  = host.getAvailableInputs();
        const auto outputs = host.getAvailableOutputs();

        struct Seen { std::uint64_t deviceId; };
        std::vector<Seen> seenThisPass;
        seenThisPass.reserve (static_cast<size_t> (inputs.size() + outputs.size()));

        auto processList = [&] (const juce::Array<juce::MidiDeviceInfo>& list, bool isInput)
        {
            // Per PRD: ordinal is assigned per duplicate (manufacturer, productName)
            // in JUCE enumeration order within the same enumeration pass.
            // JUCE's MidiDeviceInfo does not expose `manufacturer`; we treat it
            // as empty and use `name` as productName, so the ordinal is the
            // primary disambiguator.
            std::vector<std::pair<juce::String, int>> ordinalCounts;
            ordinalCounts.reserve (static_cast<size_t> (list.size()));

            for (const auto& info : list)
            {
                const juce::String manufacturer; // not provided by JUCE
                const juce::String productName = info.name;

                int ordinal = 0;
                bool found = false;
                for (auto& [key, count] : ordinalCounts)
                {
                    if (key == productName)
                    {
                        ordinal = ++count;
                        found = true;
                        break;
                    }
                }
                if (! found)
                    ordinalCounts.emplace_back (productName, 0);

                const auto deviceId = computeDeviceId (manufacturer, productName, ordinal);
                seenThisPass.push_back ({ deviceId });

                if (auto* existing = findByIdAndDirection (deviceId, isInput))
                {
                    if (! existing->info.isConnected)
                    {
                        existing->info.isConnected   = true;
                        existing->info.juceIdentifier = info.identifier;
                        // Replug: re-fire "added" so saved mappings can re-attach.
                        fireAdded (deviceId);
                        maybeAutoOpen (existing->info);
                    }
                    else
                    {
                        // Keep identifier fresh (some hosts re-assign on enumeration).
                        existing->info.juceIdentifier = info.identifier;
                    }
                    continue;
                }

                if (static_cast<int> (devices.size()) >= MaxDevices)
                {
                    // Hard cap: silently skip beyond MaxDevices to honour the
                    // pre-allocated lookup-table invariant.
                    continue;
                }

                OwnedDevice owned;
                owned.info.deviceId       = deviceId;
                owned.info.juceIdentifier = info.identifier;
                owned.info.manufacturer   = manufacturer;
                owned.info.productName    = productName;
                owned.info.ordinal        = ordinal;
                owned.info.isInput        = isInput;
                owned.info.isConnected    = true;
                owned.info.isOpen         = false;
                devices.push_back (std::move (owned));

                const auto& back = devices.back().info;
                fireAdded (deviceId);
                if (! firstPass)
                    maybeAutoOpen (back);
                else
                    maybeAutoOpen (back);
            }
        };

        processList (inputs,  true);
        processList (outputs, false);

        // Diff: anything previously connected but not seen this pass disappeared.
        // Note: a device with both an input and an output endpoint has TWO
        // records sharing the same deviceId but with different `isInput`.
        // Both must be diffed independently against the seen lists.
        auto wasSeen = [&] (std::uint64_t id, bool isInput) {
            // The fake/real host produces inputs first then outputs; we just
            // need to know whether this specific (id, direction) pair appeared.
            // We do a linear scan over seenThisPass, which carries deviceIds.
            // The direction information is implicit: if any matching id is in
            // seenThisPass we consider it present in EITHER list. To be more
            // precise we'd need direction in seenThisPass; for now any match
            // marks both directions as connected, since unplug always removes
            // both endpoints simultaneously in practice.
            juce::ignoreUnused (isInput);
            for (const auto& s : seenThisPass)
                if (s.deviceId == id)
                    return true;
            return false;
        };

        for (auto& dev : devices)
        {
            if (! dev.info.isConnected)
                continue;

            if (wasSeen (dev.info.deviceId, dev.info.isInput))
                continue;

            // Device disappeared — close handles first, then fire closed, then removed.
            if (dev.info.isOpen)
            {
                if (dev.input != nullptr)
                {
                    dev.input->stop();
                    clearLookupSlot (dev.input.get());
                    dev.input.reset();
                }
                dev.output.reset();
                dev.info.isOpen = false;
                fireClosed (dev.info.deviceId);
            }

            dev.info.isConnected = false;
            fireRemoved (dev.info.deviceId);
        }
    }

    // ---------------- ID / lookup --------------------------------------------

    std::uint64_t MidiDeviceManager::computeDeviceId (const juce::String& manufacturer,
                                                      const juce::String& productName,
                                                      int ordinal) const noexcept
    {
        juce::String key;
        key << manufacturer << "|" << productName << "|" << ordinal;
        return sha1::sha1Low64 (key);
    }

    MidiDeviceManager::OwnedDevice* MidiDeviceManager::findById (std::uint64_t id) noexcept
    {
        for (auto& d : devices)
            if (d.info.deviceId == id)
                return &d;
        return nullptr;
    }

    const MidiDeviceManager::OwnedDevice* MidiDeviceManager::findById (std::uint64_t id) const noexcept
    {
        for (const auto& d : devices)
            if (d.info.deviceId == id)
                return &d;
        return nullptr;
    }

    MidiDeviceManager::OwnedDevice* MidiDeviceManager::findByIdAndDirection (std::uint64_t id, bool isInput) noexcept
    {
        for (auto& d : devices)
            if (d.info.deviceId == id && d.info.isInput == isInput)
                return &d;
        return nullptr;
    }

    void MidiDeviceManager::populateLookupSlot (juce::MidiInput* ptr, std::uint64_t id)
    {
        for (auto& slot : lookupTable)
        {
            if (slot.input.load (std::memory_order_acquire) == nullptr)
            {
                slot.deviceId.store (id, std::memory_order_relaxed);
                slot.input.store (ptr, std::memory_order_release);
                return;
            }
        }
        // No free slot — pre-allocation invariant should have prevented this.
        jassertfalse;
    }

    void MidiDeviceManager::clearLookupSlot (juce::MidiInput* ptr)
    {
        for (auto& slot : lookupTable)
        {
            if (slot.input.load (std::memory_order_acquire) == ptr)
            {
                slot.input.store (nullptr, std::memory_order_release);
                slot.deviceId.store (0, std::memory_order_relaxed);
                return;
            }
        }
    }

    // ---------------- Snapshot ------------------------------------------------

    std::vector<MidiDeviceRecord> MidiDeviceManager::getDevices() const
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        std::vector<MidiDeviceRecord> snapshot;
        snapshot.reserve (devices.size());
        for (const auto& d : devices)
            snapshot.push_back (d.info);
        return snapshot;
    }

    // ---------------- Open / close --------------------------------------------

    MidiOpenResult MidiDeviceManager::openInput (std::uint64_t deviceId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        auto* dev = findByIdAndDirection (deviceId, /*isInput*/ true);
        if (dev == nullptr || ! dev->info.isConnected)
            return MidiOpenResult::DeviceNotFound;
        if (dev->input != nullptr)
            return MidiOpenResult::AlreadyOpen;

        auto handle = host.openInputDevice (dev->info.juceIdentifier, this);
        if (handle == nullptr)
            return MidiOpenResult::OsRefused;

        populateLookupSlot (handle.get(), deviceId);
        handle->start();
        dev->input = std::move (handle);
        dev->info.isOpen = true;

        fireOpened (deviceId);
        return MidiOpenResult::Ok;
    }

    MidiOpenResult MidiDeviceManager::openOutput (std::uint64_t deviceId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        auto* dev = findByIdAndDirection (deviceId, /*isInput*/ false);
        if (dev == nullptr || ! dev->info.isConnected)
            return MidiOpenResult::DeviceNotFound;
        if (dev->output != nullptr)
            return MidiOpenResult::AlreadyOpen;

        auto handle = host.openOutputDevice (dev->info.juceIdentifier);
        if (handle == nullptr)
            return MidiOpenResult::OsRefused;

        dev->output = std::move (handle);
        dev->info.isOpen = true;

        fireOpened (deviceId);
        return MidiOpenResult::Ok;
    }

    void MidiDeviceManager::closeInput (std::uint64_t deviceId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        auto* dev = findByIdAndDirection (deviceId, /*isInput*/ true);
        if (dev == nullptr || dev->input == nullptr)
            return;

        dev->input->stop();
        clearLookupSlot (dev->input.get());
        dev->input.reset();
        dev->info.isOpen = false;

        fireClosed (deviceId);
    }

    void MidiDeviceManager::closeOutput (std::uint64_t deviceId)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        auto* dev = findByIdAndDirection (deviceId, /*isInput*/ false);
        if (dev == nullptr || dev->output == nullptr)
            return;

        dev->output.reset();
        dev->info.isOpen = false;

        fireClosed (deviceId);
    }

    void MidiDeviceManager::sendOutput (std::uint64_t deviceId, const juce::MidiMessage& message)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        auto* dev = findByIdAndDirection (deviceId, /*isInput*/ false);
        if (dev == nullptr || dev->output == nullptr)
            return;
        dev->output->sendMessageNow (message);
    }

    // ---------------- Listeners ----------------------------------------------

    void MidiDeviceManager::addDeviceListChangeListener (DeviceListChangeListener* listener)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        if (listener == nullptr) return;
        for (auto* existing : deviceListListeners)
            if (existing == listener) return;
        deviceListListeners.push_back (listener);
    }

    void MidiDeviceManager::removeDeviceListChangeListener (DeviceListChangeListener* listener)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        for (auto it = deviceListListeners.begin(); it != deviceListListeners.end(); ++it)
        {
            if (*it == listener)
            {
                deviceListListeners.erase (it);
                return;
            }
        }
    }

    void MidiDeviceManager::fireAdded (std::uint64_t deviceId)
    {
        for (auto* l : deviceListListeners) l->midiDeviceAdded (deviceId);
    }
    void MidiDeviceManager::fireRemoved (std::uint64_t deviceId)
    {
        for (auto* l : deviceListListeners) l->midiDeviceRemoved (deviceId);
    }
    void MidiDeviceManager::fireOpened (std::uint64_t deviceId)
    {
        for (auto* l : deviceListListeners) l->midiDeviceOpened (deviceId);
    }
    void MidiDeviceManager::fireClosed (std::uint64_t deviceId)
    {
        for (auto* l : deviceListListeners) l->midiDeviceClosed (deviceId);
    }

    // ---------------- Subscribers --------------------------------------------

    void MidiDeviceManager::addInputSubscriber (MidiInputSubscriber* subscriber)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        if (subscriber == nullptr) return;

        // Reject duplicates.
        const int current = subscriberCount.load (std::memory_order_acquire);
        for (int i = 0; i < current; ++i)
            if (subscribers[static_cast<size_t> (i)].load (std::memory_order_acquire) == subscriber)
                return;

        // Try to reuse a tombstoned slot first.
        for (int i = 0; i < current; ++i)
        {
            auto& slot = subscribers[static_cast<size_t> (i)];
            if (slot.load (std::memory_order_acquire) == nullptr)
            {
                slot.store (subscriber, std::memory_order_release);
                return;
            }
        }

        if (current >= MaxSubscribers)
        {
            jassertfalse; // Capacity exhausted — raise MaxSubscribers if hit.
            return;
        }

        subscribers[static_cast<size_t> (current)].store (subscriber, std::memory_order_release);
        subscriberCount.store (current + 1, std::memory_order_release);
    }

    void MidiDeviceManager::removeInputSubscriber (MidiInputSubscriber* subscriber)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        if (subscriber == nullptr) return;

        const int current = subscriberCount.load (std::memory_order_acquire);
        for (int i = 0; i < current; ++i)
        {
            auto& slot = subscribers[static_cast<size_t> (i)];
            if (slot.load (std::memory_order_acquire) == subscriber)
            {
                // Tombstone the slot — MIDI reader skips nullptrs.
                slot.store (nullptr, std::memory_order_release);
                return;
            }
        }
    }

    // ---------------- Auto-open rules ----------------------------------------

    void MidiDeviceManager::registerAutoOpenRule (const juce::String& manufacturerRegex,
                                                  const juce::String& productNameRegex)
    {
        JUCE_ASSERT_MESSAGE_THREAD;
        try
        {
            AutoOpenRule rule;
            rule.manufacturerRe = std::regex (manufacturerRegex.toStdString());
            rule.productNameRe  = std::regex (productNameRegex.toStdString());
            autoOpenRules.push_back (std::move (rule));
        }
        catch (const std::regex_error&)
        {
            jassertfalse; // Caller passed an invalid regex.
        }
    }

    void MidiDeviceManager::maybeAutoOpen (const MidiDeviceRecord& record)
    {
        for (const auto& rule : autoOpenRules)
        {
            const auto manStr = record.manufacturer.toStdString();
            const auto prodStr = record.productName.toStdString();
            if (std::regex_search (manStr,  rule.manufacturerRe)
                && std::regex_search (prodStr, rule.productNameRe))
            {
                if (record.isInput)  openInput  (record.deviceId);
                else                 openOutput (record.deviceId);
            }
        }
    }

    // ---------------- MIDI callback thread -----------------------------------

    void MidiDeviceManager::handleIncomingMidiMessage (juce::MidiInput* source,
                                                       const juce::MidiMessage& message)
    {
        // Real-time path: no allocations, no String, no locks, no logging.

        std::uint64_t deviceId = 0;
        for (auto& slot : lookupTable)
        {
            if (slot.input.load (std::memory_order_acquire) == source)
            {
                deviceId = slot.deviceId.load (std::memory_order_relaxed);
                break;
            }
        }
        if (deviceId == 0)
            return; // Source not in our table (e.g. just closed).

        const auto* raw = message.getRawData();
        const int   len = message.getRawDataSize();

        MidiInboundEvent event {};
        event.deviceId         = deviceId;
        event.timestampSeconds = message.getTimeStamp();
        event.statusByte       = len > 0 ? static_cast<std::uint8_t> (raw[0]) : std::uint8_t { 0 };
        event.data1            = len > 1 ? static_cast<std::uint8_t> (raw[1]) : std::uint8_t { 0 };
        event.data2            = len > 2 ? static_cast<std::uint8_t> (raw[2]) : std::uint8_t { 0 };

        const int count = subscriberCount.load (std::memory_order_acquire);
        for (int i = 0; i < count; ++i)
        {
            auto* sub = subscribers[static_cast<size_t> (i)].load (std::memory_order_acquire);
            if (sub != nullptr)
                sub->onMidiInbound (event);
        }
    }
}
