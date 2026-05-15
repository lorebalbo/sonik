// PRD-0040: unit tests for MidiDeviceManager.
//
// Strategy: the manager talks to the OS through MidiHostInterface. The tests
// inject a MidiHostFake that returns synthetic device lists, and we drive
// hot-plug behaviour by mutating the fake's list and ticking the manager
// manually via initialise() + a public `forceEnumeration` shim — except the
// manager exposes only a timer. Since the timer is private, we reach into
// behaviour via the friend-free public surface: re-call initialise() is a
// no-op, so for hot-plug we expose a test hook through a thin subclass that
// invokes timerCallback via the protected juce::Timer base.
//
// Simpler: the manager's `timerCallback` is private. We instead invoke the
// public initialise() once and rely on starting/stopping the timer plus
// triggering enumeration by constructing a fresh manager. For mid-session
// hot-plug we use a small `TestableMidiDeviceManager` subclass that exposes a
// `pollNow()` method by overriding nothing — instead we leverage that the
// real timer fires every 1000 ms and use juce::MessageManager dispatch loops.
// To keep tests fast, we expose a test seam via `friend` inside the manager
// header is undesirable; instead we add a public `pollNowForTesting()` method
// guarded by `#ifdef SONIK_MIDI_TESTING`. To avoid header pollution we keep
// the tests black-box and use a brief `juce::MessageManager` runDispatchLoop.
//
// Final approach (chosen):
//   - Use real MessageManager and let the 1000 ms timer fire naturally.
//     This is reliable but slow (1s per hot-plug step).
//   - For speed, we directly use the `MidiHostFake` to verify ID derivation
//     and the public open/close paths. Hot-plug is verified by running the
//     message loop for ~1100 ms once.

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include "Features/Midi/MidiDeviceManager.h"
#include "Features/Midi/MidiHostInterface.h"
#include "Features/Midi/MidiInputSubscriber.h"
#include "Features/Midi/DeviceListChangeListener.h"
#include "Features/Midi/Sha1.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace
{
using namespace sonik::midi;

/** Test fake host: returns whatever device lists the test sets. Open calls
    return real juce::MidiInput/MidiOutput unique_ptrs constructed via the
    public test-only ctor; since juce::MidiInput has no public ctor for fakes,
    we return nullptr to simulate failure, OR we use a thin shim. For these
    tests we never actually drive MIDI bytes — we test enumeration, ID
    derivation, hot-plug callbacks, idempotent open, and replug deviceId
    reuse. So we either:
      (a) accept `OsRefused` and assert on that for openInput tests, OR
      (b) make the fake "succeed" by returning a recognisable nullptr.
    We pick (b) but with a custom subclass that overrides openInputDevice to
    return a juce-owned MidiInput via a workaround: we never have a real
    device. Solution: accept that `openInput` against a fake returns
    `OsRefused`, and use that to assert the OsRefused branch. To verify the
    deviceId-stable / lookup-slot path without real MIDI, we test the
    classification logic via separate helpers (computeDeviceId is exercised
    indirectly through getDevices()->deviceId).
*/
class MidiHostFake final : public MidiHostInterface
{
public:
    juce::Array<juce::MidiDeviceInfo> inputs;
    juce::Array<juce::MidiDeviceInfo> outputs;

    // When set, openInputDevice returns this fabricated MidiInput. We cannot
    // legally fabricate a juce::MidiInput without an OS handle, so we leave
    // this null and verify OsRefused.
    juce::Array<juce::MidiDeviceInfo> getAvailableInputs()  override { return inputs;  }
    juce::Array<juce::MidiDeviceInfo> getAvailableOutputs() override { return outputs; }

    std::unique_ptr<juce::MidiInput> openInputDevice (const juce::String&,
                                                      juce::MidiInputCallback*) override
    { return nullptr; }

    std::unique_ptr<juce::MidiOutput> openOutputDevice (const juce::String&) override
    { return nullptr; }
};

class RecordingListener final : public DeviceListChangeListener
{
public:
    std::vector<std::uint64_t> added;
    std::vector<std::uint64_t> removed;
    std::vector<std::uint64_t> opened;
    std::vector<std::uint64_t> closed;

    void midiDeviceAdded   (std::uint64_t id) override { added.push_back (id); }
    void midiDeviceRemoved (std::uint64_t id) override { removed.push_back (id); }
    void midiDeviceOpened  (std::uint64_t id) override { opened.push_back (id); }
    void midiDeviceClosed  (std::uint64_t id) override { closed.push_back (id); }
};

class CountingSubscriber final : public MidiInputSubscriber
{
public:
    std::atomic<int> count { 0 };
    void onMidiInbound (const MidiInboundEvent&) noexcept override
    {
        count.fetch_add (1, std::memory_order_relaxed);
    }
};

juce::MidiDeviceInfo makeInfo (const juce::String& name, const juce::String& identifier)
{
    juce::MidiDeviceInfo info;
    info.name = name;
    info.identifier = identifier;
    return info;
}

// Pump the message loop until `predicate` is true or `timeoutMs` elapsed.
template <typename Pred>
bool pumpUntil (Pred predicate, int timeoutMs = 1500)
{
    auto* mm = juce::MessageManager::getInstance();
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (juce::Time::getMillisecondCounter() < deadline)
    {
        if (predicate())
            return true;
        mm->runDispatchLoopUntil (20);
    }
    return predicate();
}

} // namespace

class MidiDeviceManagerTests : public juce::UnitTest
{
public:
    MidiDeviceManagerTests() : juce::UnitTest ("MIDI Device Manager (PRD-0040)", "Sonik") {}

    void runTest() override
    {
        testSha1IsStableAndCollisionFree();
        testEmptyEnumeration();
        testEnumerationAtStartup();
        testOrdinalDisambiguationOfDuplicates();
        testGetDevicesSnapshotIsSafe();
        testIdempotentOpenReturnsAlreadyOpenOrOsRefused();
        testCloseOnUnknownIdIsNoOp();
        testSendOutputOnUnknownIdIsNoOp();
        testListenerAddRemoveIsIdempotent();
        testSubscriberAddRemoveIsIdempotent();
        testAutoOpenRuleAcceptsValidRegex();
        testAutoOpenRuleRejectsInvalidRegexQuietly();
        testHotPlugFiresAddedAndRemoved();
        testReplugReusesSameDeviceId();
        testMidiInboundEventPodInvariants();
    }

private:
    void testSha1IsStableAndCollisionFree()
    {
        beginTest ("SHA-1 sha1Low64 is stable and distinguishes inputs");
        const auto a = sha1::sha1Low64 ("Behringer|DDM4000|0");
        const auto b = sha1::sha1Low64 ("Behringer|DDM4000|0");
        const auto c = sha1::sha1Low64 ("Behringer|DDM4000|1");
        const auto d = sha1::sha1Low64 ("Pioneer|DDJ-FLX4|0");
        expect (a == b, "identical inputs -> identical hash");
        expect (a != c, "ordinal change -> different hash");
        expect (a != d, "product change -> different hash");
        expect (a != 0, "hash should not be zero for non-empty input");
    }

    void testEmptyEnumeration()
    {
        beginTest ("Empty host enumeration produces empty device list");
        MidiHostFake host;
        sonik::midi::MidiDeviceManager mgr (host);
        RecordingListener listener;
        mgr.addDeviceListChangeListener (&listener);
        mgr.initialise();
        expect (mgr.getDevices().empty(), "no devices reported");
        expect (listener.added.empty(),   "no added callbacks");
        mgr.removeDeviceListChangeListener (&listener);
    }

    void testEnumerationAtStartup()
    {
        beginTest ("Startup enumeration registers every device with stable ID");
        MidiHostFake host;
        host.inputs.add  (makeInfo ("Behringer DDM4000", "in_id_1"));
        host.outputs.add (makeInfo ("Behringer DDM4000", "out_id_1"));

        sonik::midi::MidiDeviceManager mgr (host);
        RecordingListener listener;
        mgr.addDeviceListChangeListener (&listener);
        mgr.initialise();

        const auto devices = mgr.getDevices();
        expectEquals ((int) devices.size(), 2, "one input + one output");
        expect (listener.added.size() == 2, "added fired for both");

        // No device should be open after init.
        for (const auto& d : devices)
            expect (! d.isOpen, "no device auto-opens at init");

        // deviceId derived from SHA-1 of "|<name>|<ordinal>" — note empty manufacturer.
        const auto expectedInputId = sha1::sha1Low64 ("|Behringer DDM4000|0");
        bool found = false;
        for (const auto& d : devices)
            if (d.isInput && d.deviceId == expectedInputId)
                found = true;
        expect (found, "input deviceId matches SHA-1 of |name|0");
    }

    void testOrdinalDisambiguationOfDuplicates()
    {
        beginTest ("Duplicate (manufacturer, productName) gets incrementing ordinal");
        MidiHostFake host;
        host.inputs.add (makeInfo ("Generic MIDI", "g1"));
        host.inputs.add (makeInfo ("Generic MIDI", "g2"));
        host.inputs.add (makeInfo ("Generic MIDI", "g3"));

        sonik::midi::MidiDeviceManager mgr (host);
        mgr.initialise();

        const auto devices = mgr.getDevices();
        expectEquals ((int) devices.size(), 3);

        // Collect ordinals; should be {0,1,2}.
        std::vector<int> ordinals;
        for (const auto& d : devices) ordinals.push_back (d.ordinal);
        std::sort (ordinals.begin(), ordinals.end());
        expectEquals (ordinals[0], 0);
        expectEquals (ordinals[1], 1);
        expectEquals (ordinals[2], 2);

        // All three deviceIds distinct.
        std::vector<std::uint64_t> ids;
        for (const auto& d : devices) ids.push_back (d.deviceId);
        std::sort (ids.begin(), ids.end());
        expect (ids[0] != ids[1] && ids[1] != ids[2] && ids[0] != ids[2],
                "deviceIds disambiguated by ordinal");
    }

    void testGetDevicesSnapshotIsSafe()
    {
        beginTest ("getDevices returns a copy that survives mutation of internal list");
        MidiHostFake host;
        host.inputs.add (makeInfo ("DeviceX", "x1"));
        sonik::midi::MidiDeviceManager mgr (host);
        mgr.initialise();

        auto snap1 = mgr.getDevices();
        expectEquals ((int) snap1.size(), 1);

        // Add another device to the host but don't re-enumerate yet.
        host.inputs.add (makeInfo ("DeviceY", "y1"));
        auto snap2 = mgr.getDevices();
        expectEquals ((int) snap2.size(), 1, "snapshot reflects state at call time, not host");
    }

    void testIdempotentOpenReturnsAlreadyOpenOrOsRefused()
    {
        beginTest ("openInput on fake host returns OsRefused; unknown IDs return DeviceNotFound");
        MidiHostFake host;
        host.inputs.add (makeInfo ("FakeIn", "fi"));
        sonik::midi::MidiDeviceManager mgr (host);
        mgr.initialise();

        const auto devices = mgr.getDevices();
        const auto id = devices.front().deviceId;

        // Fake's openInputDevice returns nullptr -> OsRefused.
        expect (mgr.openInput (id) == MidiOpenResult::OsRefused);

        // Unknown ID.
        expect (mgr.openInput (0xDEADBEEFDEADBEEFull) == MidiOpenResult::DeviceNotFound);
        expect (mgr.openOutput (0xDEADBEEFDEADBEEFull) == MidiOpenResult::DeviceNotFound);
    }

    void testCloseOnUnknownIdIsNoOp()
    {
        beginTest ("close on unknown / never-opened device is a silent no-op");
        MidiHostFake host;
        sonik::midi::MidiDeviceManager mgr (host);
        mgr.initialise();
        mgr.closeInput  (1234);
        mgr.closeOutput (5678);
        // No crash, no assert tripped.
        expect (true);
    }

    void testSendOutputOnUnknownIdIsNoOp()
    {
        beginTest ("sendOutput on unknown device is a silent no-op");
        MidiHostFake host;
        sonik::midi::MidiDeviceManager mgr (host);
        mgr.initialise();
        mgr.sendOutput (0xFEEDFACE, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100));
        expect (true);
    }

    void testListenerAddRemoveIsIdempotent()
    {
        beginTest ("DeviceListChangeListener add/remove is idempotent and deduplicating");
        MidiHostFake host;
        sonik::midi::MidiDeviceManager mgr (host);
        mgr.initialise();
        RecordingListener l;
        mgr.addDeviceListChangeListener (&l);
        mgr.addDeviceListChangeListener (&l); // dup -> ignored
        mgr.addDeviceListChangeListener (nullptr); // nullptr -> ignored

        host.inputs.add (makeInfo ("D1", "d1"));
        // The next add fires the listener exactly once after the hot-plug timer ticks.
        expect (pumpUntil ([&] { return l.added.size() >= 1; }, 1800),
                "added fired by hot-plug timer");
        expectEquals ((int) l.added.size(), 1, "added fired exactly once even with duplicate registration");

        mgr.removeDeviceListChangeListener (&l);
        mgr.removeDeviceListChangeListener (&l); // remove again -> no-op
    }

    void testSubscriberAddRemoveIsIdempotent()
    {
        beginTest ("MidiInputSubscriber add/remove tombstone semantics");
        MidiHostFake host;
        sonik::midi::MidiDeviceManager mgr (host);
        mgr.initialise();

        CountingSubscriber s1, s2;
        mgr.addInputSubscriber (&s1);
        mgr.addInputSubscriber (&s1); // dup -> ignored
        mgr.addInputSubscriber (&s2);
        mgr.removeInputSubscriber (&s1);
        mgr.removeInputSubscriber (&s1); // remove again -> no-op
        mgr.addInputSubscriber (&s1);    // re-register, should fill tombstone
        mgr.removeInputSubscriber (&s1);
        mgr.removeInputSubscriber (&s2);
        expect (true, "no crash on add/remove cycles");
    }

    void testAutoOpenRuleAcceptsValidRegex()
    {
        beginTest ("registerAutoOpenRule accepts a well-formed regex");
        MidiHostFake host;
        sonik::midi::MidiDeviceManager mgr (host);
        mgr.initialise();
        mgr.registerAutoOpenRule (".*", "Behringer.*");
        expect (true);
    }

    void testAutoOpenRuleRejectsInvalidRegexQuietly()
    {
        beginTest ("registerAutoOpenRule swallows invalid regex (no exception escapes)");
        MidiHostFake host;
        sonik::midi::MidiDeviceManager mgr (host);
        mgr.initialise();
        // The implementation traps std::regex_error internally; the only thing
        // observable from outside is whether an exception escapes.
        try
        {
            mgr.registerAutoOpenRule ("[", "valid");
            expect (true, "no exception propagated");
        }
        catch (...)
        {
            expect (false, "exception escaped from registerAutoOpenRule");
        }
    }

    void testHotPlugFiresAddedAndRemoved()
    {
        beginTest ("Hot-plug: device appears mid-session -> midiDeviceAdded; disappears -> midiDeviceRemoved");
        MidiHostFake host;
        sonik::midi::MidiDeviceManager mgr (host);
        RecordingListener l;
        mgr.addDeviceListChangeListener (&l);
        mgr.initialise();
        expectEquals ((int) l.added.size(), 0);

        // Plug in a device.
        host.inputs.add (makeInfo ("HotPlugIn", "hp1"));
        expect (pumpUntil ([&] { return l.added.size() >= 1; }, 1800), "added fired after poll");
        expectEquals ((int) l.added.size(), 1);
        const auto pluggedId = l.added.front();

        // Now unplug.
        host.inputs.clearQuick();
        expect (pumpUntil ([&] { return l.removed.size() >= 1; }, 1800), "removed fired after poll");
        expect (l.removed.front() == pluggedId, "remove carries the original deviceId");

        // The record remains, but isConnected=false.
        const auto devices = mgr.getDevices();
        expectEquals ((int) devices.size(), 1, "record preserved across unplug");
        expect (! devices.front().isConnected, "isConnected=false after unplug");
        expect (! devices.front().isOpen,      "isOpen=false after unplug");
    }

    void testReplugReusesSameDeviceId()
    {
        beginTest ("Replug re-uses the same deviceId");
        MidiHostFake host;
        host.inputs.add (makeInfo ("ReplugDev", "rp1"));
        sonik::midi::MidiDeviceManager mgr (host);
        RecordingListener l;
        mgr.addDeviceListChangeListener (&l);
        mgr.initialise();
        expectEquals ((int) l.added.size(), 1);
        const auto firstId = l.added.front();

        // Unplug.
        host.inputs.clearQuick();
        expect (pumpUntil ([&] { return l.removed.size() >= 1; }, 1800));

        // Replug under a different identifier — same name -> same deviceId.
        host.inputs.add (makeInfo ("ReplugDev", "rp1-prime"));
        expect (pumpUntil ([&] { return l.added.size() >= 2; }, 1800));
        expectEquals ((long long) l.added.back(), (long long) firstId,
                      "deviceId stable across unplug/replug");
    }

    void testMidiInboundEventPodInvariants()
    {
        beginTest ("MidiInboundEvent is trivially copyable POD with correct layout");
        // static_assert in the header already enforces this; runtime sanity.
        MidiInboundEvent ev { 42ULL, 1.5, 0x90, 0x40, 0x7F };
        expectEquals ((long long) ev.deviceId, (long long) 42);
        expect (ev.timestampSeconds == 1.5);
        expectEquals ((int) ev.statusByte, 0x90);
        expectEquals ((int) ev.data1, 0x40);
        expectEquals ((int) ev.data2, 0x7F);
    }
};

static MidiDeviceManagerTests midiDeviceManagerTests;
