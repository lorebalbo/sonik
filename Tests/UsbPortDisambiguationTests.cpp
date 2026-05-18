// PRD-0051: USB-Port Disambiguation and Multi-Instance Device Binding.
//
// Covers acceptance criteria (a)-(j) from PRD-0051 §1.4 final bullet.

#include "Features/Midi/MidiDeviceManager.h"
#include "Features/Midi/MidiHostInterface.h"
#include "Features/Midi/MidiDeviceRecord.h"
#include "Features/Midi/MappingStore.h"
#include "Features/Midi/MappingParser.h"
#include "Features/Midi/MappingSerializer.h"
#include "Features/Midi/Sha1.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace
{
    using namespace sonik::midi;

    //==========================================================================
    class StubHost : public MidiHostInterface
    {
    public:
        juce::Array<juce::MidiDeviceInfo> inputs;
        juce::Array<juce::MidiDeviceInfo> getAvailableInputs()  override { return inputs;  }
        juce::Array<juce::MidiDeviceInfo> getAvailableOutputs() override { return {}; }
        std::unique_ptr<juce::MidiInput>  openInputDevice  (const juce::String&,
                                                            juce::MidiInputCallback*) override { return nullptr; }
        std::unique_ptr<juce::MidiOutput> openOutputDevice (const juce::String&) override      { return nullptr; }
    };

    juce::MidiDeviceInfo makeInfo (const juce::String& name, const juce::String& identifier)
    {
        return juce::MidiDeviceInfo (name, identifier);
    }

    juce::File makeTempDir (const juce::String& tag)
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("sonik-prd0051-" + tag
                                            + "-" + juce::String::toHexString (
                                                juce::Random::getSystemRandom().nextInt64()));
        dir.createDirectory();
        return dir;
    }

    // Build a minimal valid mapping JSON with optional identifierHint.
    juce::String makeMappingJson (const juce::String& productPattern,
                                  const juce::String& identifierHintLiteralOrEmpty,
                                  const juce::String& identifierHintRegexOrEmpty = {})
    {
        juce::String hintBlock;
        if (identifierHintLiteralOrEmpty.isNotEmpty())
        {
            hintBlock = ", \"identifierHint\": \"" + identifierHintLiteralOrEmpty + "\"";
        }
        else if (identifierHintRegexOrEmpty.isNotEmpty())
        {
            hintBlock = ", \"identifierHint\": { \"regex\": \""
                        + identifierHintRegexOrEmpty + "\" }";
        }

        juce::String json;
        json << "{\n"
             << "  \"schemaVersion\": 1,\n"
             << "  \"device\": {\n"
             << "    \"manufacturer\": \".*\", \"product\": \"" << productPattern << "\",\n"
             << "    \"match\": { \"midiName\": \".*\"" << hintBlock << " }\n"
             << "  },\n"
             << "  \"modifiers\": [],\n"
             << "  \"bindings\": [\n"
             << "    { \"target\": \"deck.A.transport.play\",\n"
             << "      \"midi\": { \"channel\": 1, \"status\": \"note\", \"data1\": 60 },\n"
             << "      \"transform\": \"momentary\" }\n"
             << "  ]\n"
             << "}\n";
        return json;
    }
}

//==============================================================================
class UsbPortDisambiguationTests final : public juce::UnitTest
{
public:
    UsbPortDisambiguationTests()
        : juce::UnitTest ("USB-Port Disambiguation (PRD-0051)", "Sonik") {}

    void runTest() override
    {
        testA_TwoDistinctIdentifiersProduceDistinctDeviceIds();
        testB_EmptyIdentifiersFallBackToOrdinalAndDiffer();
        testC_SameIdentifierReproducesSameDeviceId();
        testD_IdentifierHintBeatsNoHint();
        testE_RegexIdentifierHintMatchesPartial();
        testF_DeviceStateRoundTripsAcrossReload();
        testG_SwapIdentifierHintsBetweenUserProfiles();
        testH_SwapRejectedWhenOneSideIsBundled();
        testI_PlatformUnstableIdentifierDisablesPath();
        testJ_BackwardCompatProfilesWithoutHintMatchAnyPort();
    }

private:
    //--------------------------------------------------------------------------
    // (a) two synthetic siblings with distinct identifiers -> distinct ids
    void testA_TwoDistinctIdentifiersProduceDistinctDeviceIds()
    {
        beginTest ("(a) Distinct identifiers produce distinct deviceIds");
        StubHost host;
        host.inputs.add (makeInfo ("Behringer DDM4000", "id-port-A"));
        host.inputs.add (makeInfo ("Behringer DDM4000", "id-port-B"));

        sonik::midi::MidiDeviceManager mgr (host);
        mgr.initialise();

        const auto devices = mgr.getDevices();
        expectEquals ((int) devices.size(), 2);
        expect (devices[0].deviceId != devices[1].deviceId,
                "two ports of same product must produce different deviceIds");
        expect (mgr.isIdentifierBasedDisambiguationAvailable(),
                "identifier-based path active when all identifiers non-empty + unique");

        // Each id is SHA-1("|<name>|<identifier>").
        const auto expectedA = sha1::sha1Low64 ("|Behringer DDM4000|id-port-A");
        const auto expectedB = sha1::sha1Low64 ("|Behringer DDM4000|id-port-B");
        std::vector<std::uint64_t> got { devices[0].deviceId, devices[1].deviceId };
        std::sort (got.begin(), got.end());
        std::vector<std::uint64_t> expected { expectedA, expectedB };
        std::sort (expected.begin(), expected.end());
        expectEquals ((long long) got[0], (long long) expected[0]);
        expectEquals ((long long) got[1], (long long) expected[1]);
    }

    //--------------------------------------------------------------------------
    // (b) empty identifiers fall back to ordinal; still differ.
    void testB_EmptyIdentifiersFallBackToOrdinalAndDiffer()
    {
        beginTest ("(b) Empty identifiers fall back to ordinal; ids still differ");
        StubHost host;
        host.inputs.add (makeInfo ("Generic DDM", ""));
        host.inputs.add (makeInfo ("Generic DDM", ""));

        sonik::midi::MidiDeviceManager mgr (host);
        mgr.initialise();

        const auto devices = mgr.getDevices();
        expectEquals ((int) devices.size(), 2);
        expect (devices[0].deviceId != devices[1].deviceId,
                "ordinal-fallback ids must still differ");
        expect (! mgr.isIdentifierBasedDisambiguationAvailable(),
                "identifier path latched off after empty identifier observed");

        // Each id is SHA-1("|<name>|<ordinal>") for ordinal 0 and 1.
        const auto e0 = sha1::sha1Low64 ("|Generic DDM|0");
        const auto e1 = sha1::sha1Low64 ("|Generic DDM|1");
        std::vector<std::uint64_t> got { devices[0].deviceId, devices[1].deviceId };
        std::sort (got.begin(), got.end());
        std::vector<std::uint64_t> expected { e0, e1 };
        std::sort (expected.begin(), expected.end());
        expectEquals ((long long) got[0], (long long) expected[0]);
        expectEquals ((long long) got[1], (long long) expected[1]);
    }

    //--------------------------------------------------------------------------
    // (c) re-enumerating the same physical-port identifier reproduces same id.
    void testC_SameIdentifierReproducesSameDeviceId()
    {
        beginTest ("(c) Same identifier reproduces same deviceId byte-for-byte");

        // Build two independent managers fed the same single-device list.
        const auto info = makeInfo ("Behringer DDM4000", "core-midi-port-XYZ");

        std::uint64_t id1 = 0;
        {
            StubHost host; host.inputs.add (info);
            sonik::midi::MidiDeviceManager mgr (host);
            mgr.initialise();
            for (const auto& d : mgr.getDevices()) if (d.isInput) id1 = d.deviceId;
        }
        std::uint64_t id2 = 0;
        {
            StubHost host; host.inputs.add (info);
            sonik::midi::MidiDeviceManager mgr (host);
            mgr.initialise();
            for (const auto& d : mgr.getDevices()) if (d.isInput) id2 = d.deviceId;
        }
        expect (id1 != 0);
        expectEquals ((long long) id1, (long long) id2,
                      "deviceId must be byte-identical for the same identifier");
    }

    //--------------------------------------------------------------------------
    // (d) identifierHint exact-match scores higher than no-hint resolution.
    void testD_IdentifierHintBeatsNoHint()
    {
        beginTest ("(d) identifierHint exact-match scores higher than no-hint");

        StubHost host;
        host.inputs.add (makeInfo ("Behringer DDM4000", "port-LEFT"));
        host.inputs.add (makeInfo ("Behringer DDM4000", "port-RIGHT"));

        sonik::midi::MidiDeviceManager mgr (host);
        mgr.initialise();

        const auto dir = makeTempDir ("d-hint");

        // Two user profiles: one with identifierHint matching the LEFT port,
        // the other with no hint (matches any port).
        dir.getChildFile ("LEFT.json").replaceWithText (
            makeMappingJson ("Behringer DDM4000", "port-LEFT"));
        dir.getChildFile ("ANY.json").replaceWithText (
            makeMappingJson ("Behringer DDM4000", ""));

        MappingStore store (mgr, dir, /*async*/ false);

        std::uint64_t leftId = 0, rightId = 0;
        for (const auto& d : mgr.getDevices())
        {
            if (d.juceIdentifier == "port-LEFT")  leftId  = d.deviceId;
            if (d.juceIdentifier == "port-RIGHT") rightId = d.deviceId;
        }

        const auto activeLeft  = store.getActiveMappingIdForDevice (leftId);
        const auto activeRight = store.getActiveMappingIdForDevice (rightId);

        expectEquals (activeLeft, juce::String ("LEFT"),
                      "LEFT port must resolve to identifier-hint profile (score 4)");
        expectEquals (activeRight, juce::String ("ANY"),
                      "RIGHT port falls back to the no-hint user profile (score 3)");

        dir.deleteRecursively();
    }

    //--------------------------------------------------------------------------
    // (e) regex identifierHint matches partial patterns.
    void testE_RegexIdentifierHintMatchesPartial()
    {
        beginTest ("(e) Regex identifierHint matches partial pattern");

        StubHost host;
        host.inputs.add (makeInfo ("Behringer DDM4000", "bus42::usb::0xCAFEBABE"));

        sonik::midi::MidiDeviceManager mgr (host);
        mgr.initialise();

        const auto dir = makeTempDir ("e-regex");
        dir.getChildFile ("REGEX.json").replaceWithText (
            makeMappingJson ("Behringer DDM4000",
                             /*literal*/ juce::String(),
                             /*regex*/   "0xCAFEBABE$"));

        MappingStore store (mgr, dir, /*async*/ false);

        std::uint64_t devId = 0;
        for (const auto& d : mgr.getDevices()) if (d.isInput) devId = d.deviceId;
        expect (devId != 0);

        const auto active = store.getActiveMappingIdForDevice (devId);
        expectEquals (active, juce::String ("REGEX"),
                      "regex identifierHint should match the device's identifier suffix");

        dir.deleteRecursively();
    }

    //--------------------------------------------------------------------------
    // (f) device-state round-trip across reload.
    void testF_DeviceStateRoundTripsAcrossReload()
    {
        beginTest ("(f) _active-mappings.json round-trip preserves selections");

        StubHost host;
        host.inputs.add (makeInfo ("Behringer DDM4000", "port-K1"));

        const auto dir = makeTempDir ("f-state");
        dir.getChildFile ("CUSTOM.json").replaceWithText (
            makeMappingJson ("Behringer DDM4000", ""));

        std::uint64_t devId = 0;
        {
            sonik::midi::MidiDeviceManager mgr (host);
            mgr.initialise();
            for (const auto& d : mgr.getDevices()) if (d.isInput) devId = d.deviceId;
            MappingStore store (mgr, dir, /*async*/ false);
            const auto rc = store.setActiveMapping (devId, "CUSTOM");
            expect (rc == SetActiveMappingResult::Ok);
            expectEquals (store.getActiveMappingIdForDevice (devId), juce::String ("CUSTOM"));
        }

        // Confirm the state file exists on disk.
        expect (dir.getChildFile ("_active-mappings.json").existsAsFile(),
                "active-mappings json must be written");

        // Re-construct fresh manager + store: persisted override should be loaded.
        {
            sonik::midi::MidiDeviceManager mgr2 (host);
            mgr2.initialise();
            MappingStore store2 (mgr2, dir, /*async*/ false);
            expectEquals (store2.getActiveMappingIdForDevice (devId), juce::String ("CUSTOM"),
                          "persisted active-mapping must reload");
        }

        dir.deleteRecursively();
    }

    //--------------------------------------------------------------------------
    // (g) swap identifierHints between two user profiles.
    void testG_SwapIdentifierHintsBetweenUserProfiles()
    {
        beginTest ("(g) swapIdentifierHints swaps hints between two user profiles");

        StubHost host;
        host.inputs.add (makeInfo ("Behringer DDM4000", "port-A"));
        host.inputs.add (makeInfo ("Behringer DDM4000", "port-B"));

        sonik::midi::MidiDeviceManager mgr (host);
        mgr.initialise();

        const auto dir = makeTempDir ("g-swap");
        dir.getChildFile ("LEFT.json").replaceWithText (
            makeMappingJson ("Behringer DDM4000", "port-A"));
        dir.getChildFile ("RIGHT.json").replaceWithText (
            makeMappingJson ("Behringer DDM4000", "port-B"));

        MappingStore store (mgr, dir, /*async*/ false);

        std::uint64_t devA = 0, devB = 0;
        for (const auto& d : mgr.getDevices())
        {
            if (d.juceIdentifier == "port-A") devA = d.deviceId;
            if (d.juceIdentifier == "port-B") devB = d.deviceId;
        }
        expectEquals (store.getActiveMappingIdForDevice (devA), juce::String ("LEFT"));
        expectEquals (store.getActiveMappingIdForDevice (devB), juce::String ("RIGHT"));

        const auto rc = store.swapIdentifierHints (devA, devB);
        expect (rc == SwapIdentifierHintsResult::Ok,
                "swap of two user profiles should succeed");

        // After swap, port-A should resolve to RIGHT and port-B to LEFT.
        expectEquals (store.getActiveMappingIdForDevice (devA), juce::String ("RIGHT"));
        expectEquals (store.getActiveMappingIdForDevice (devB), juce::String ("LEFT"));

        // The on-disk JSON files reflect the swap.
        const auto leftText  = dir.getChildFile ("LEFT.json").loadFileAsString();
        const auto rightText = dir.getChildFile ("RIGHT.json").loadFileAsString();
        expect (leftText.contains ("port-B"),  "LEFT now has port-B hint");
        expect (rightText.contains ("port-A"), "RIGHT now has port-A hint");

        dir.deleteRecursively();
    }

    //--------------------------------------------------------------------------
    // (h) swap rejected when one side is a bundled profile.
    void testH_SwapRejectedWhenOneSideIsBundled()
    {
        beginTest ("(h) swap is rejected when one side is bundled");

        StubHost host;
        host.inputs.add (makeInfo ("Behringer DDM4000", "port-A"));
        host.inputs.add (makeInfo ("Behringer DDM4000", "port-B"));

        sonik::midi::MidiDeviceManager mgr (host);
        mgr.initialise();

        const auto dir = makeTempDir ("h-bundled");
        // No user profiles -> both devices resolve to the bundled DDM4000.
        MappingStore store (mgr, dir, /*async*/ false);

        std::uint64_t devA = 0, devB = 0;
        for (const auto& d : mgr.getDevices())
        {
            if (d.juceIdentifier == "port-A") devA = d.deviceId;
            if (d.juceIdentifier == "port-B") devB = d.deviceId;
        }
        expect (devA != 0 && devB != 0);

        const auto rc = store.swapIdentifierHints (devA, devB);
        expect (rc == SwapIdentifierHintsResult::OneSideIsBundled
                || rc == SwapIdentifierHintsResult::SameMapping,
                "must refuse when one or both sides resolve to a bundled profile");

        dir.deleteRecursively();
    }

    //--------------------------------------------------------------------------
    // (i) bundled / platform-unstable both disable the bind toggle path.
    // We exercise the underlying store / manager guarantees that the UI relies on.
    void testI_PlatformUnstableIdentifierDisablesPath()
    {
        beginTest ("(i) Platform-unstable identifier path disabled; bundled rejects hint write");

        // Sub-case 1: platform with empty identifier latches the path off.
        {
            StubHost host;
            host.inputs.add (makeInfo ("Some MIDI Box", ""));
            sonik::midi::MidiDeviceManager mgr (host);
            mgr.initialise();
            expect (! mgr.isIdentifierBasedDisambiguationAvailable(),
                    "empty identifier -> identifier path unavailable");
        }

        // Sub-case 2: setIdentifierHint on a bundled mapping is rejected.
        {
            StubHost host;
            host.inputs.add (makeInfo ("Behringer DDM4000", "port-A"));
            sonik::midi::MidiDeviceManager mgr (host);
            mgr.initialise();
            const auto dir = makeTempDir ("i-bundled-hint");
            MappingStore store (mgr, dir, /*async*/ false);

            const auto rc = store.setIdentifierHint ("behringer-ddm4000",
                                                     juce::String ("port-A"));
            expect (rc == SetIdentifierHintResult::BundledNotEditable,
                    "bundled mappings must reject setIdentifierHint");

            dir.deleteRecursively();
        }
    }

    //--------------------------------------------------------------------------
    // (j) v1 profiles without identifierHint match any port unmodified.
    void testJ_BackwardCompatProfilesWithoutHintMatchAnyPort()
    {
        beginTest ("(j) profiles without identifierHint match any port (backward compat)");

        // Two ports, single user profile without identifierHint: both ports
        // should resolve to that profile (it scores 3 on both).
        StubHost host;
        host.inputs.add (makeInfo ("Behringer DDM4000", "port-X"));
        host.inputs.add (makeInfo ("Behringer DDM4000", "port-Y"));

        sonik::midi::MidiDeviceManager mgr (host);
        mgr.initialise();

        const auto dir = makeTempDir ("j-compat");
        dir.getChildFile ("ALL.json").replaceWithText (
            makeMappingJson ("Behringer DDM4000", ""));

        MappingStore store (mgr, dir, /*async*/ false);

        std::uint64_t devX = 0, devY = 0;
        for (const auto& d : mgr.getDevices())
        {
            if (d.juceIdentifier == "port-X") devX = d.deviceId;
            if (d.juceIdentifier == "port-Y") devY = d.deviceId;
        }
        expectEquals (store.getActiveMappingIdForDevice (devX), juce::String ("ALL"));
        expectEquals (store.getActiveMappingIdForDevice (devY), juce::String ("ALL"));

        // Round-trip the mapping through serializer/parser: the absent
        // identifierHint must remain absent.
        const auto m = store.getMappingById ("ALL");
        expect (m != nullptr);
        expect (! m->deviceMatch.hasIdentifierHint(),
                "no identifierHint after parse of v1 profile");

        const auto root = MappingSerializer::serialize (*m);
        const auto text = juce::JSON::toString (root, false);
        expect (! text.contains ("identifierHint"),
                "serializer must not emit identifierHint when absent");

        dir.deleteRecursively();
    }
};

static UsbPortDisambiguationTests usbPortDisambiguationTests;
