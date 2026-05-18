// Tests for PRD-0049: Mapping schema migrations.

#include "Features/Midi/Migrations/MigrationRegistry.h"
#include "Features/Midi/MappingStore.h"
#include "Features/Midi/MidiDeviceManager.h"
#include "Features/Midi/MidiHostInterface.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <stdexcept>

namespace
{
    using namespace sonik::midi;

    class StubHost : public MidiHostInterface
    {
    public:
        juce::Array<juce::MidiDeviceInfo> inputs;

        juce::Array<juce::MidiDeviceInfo> getAvailableInputs() override { return inputs; }
        juce::Array<juce::MidiDeviceInfo> getAvailableOutputs() override { return {}; }

        std::unique_ptr<juce::MidiInput> openInputDevice (const juce::String&,
                                                          juce::MidiInputCallback*) override { return nullptr; }
        std::unique_ptr<juce::MidiOutput> openOutputDevice (const juce::String&) override { return nullptr; }
    };

    juce::var parseJson (const char* json)
    {
        return juce::JSON::parse (juce::String::fromUTF8 (json));
    }

    juce::String canonicalJson (const juce::var& json)
    {
        return juce::JSON::toString (json, true);
    }

    juce::File makeTempDir (const juce::String& suffix)
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("sonik-migrations-" + suffix
                                            + "-" + juce::String::toHexString (
                                                juce::Random::getSystemRandom().nextInt64()));
        dir.createDirectory();
        return dir;
    }

    juce::String makeCurrentSchemaMappingJson (juce::String displayName)
    {
        auto* root = new juce::DynamicObject();
        root->setProperty ("schemaVersion", 1);
        root->setProperty ("displayName", std::move (displayName));

        auto* device = new juce::DynamicObject();
        device->setProperty ("manufacturer", ".*");
        device->setProperty ("product", ".*");
        auto* match = new juce::DynamicObject();
        match->setProperty ("midiName", ".*");
        device->setProperty ("match", juce::var (match));
        root->setProperty ("device", juce::var (device));

        root->setProperty ("bindings", juce::Array<juce::var>());
        return juce::JSON::toString (juce::var (root), false);
    }

    MigrationRegistry makeLegacyToCurrentRegistry (juce::String displayName,
                                                   juce::String productPattern = ".*")
    {
        MigrationRegistry registry;
        registry.registerMigration (0, "upgrade test mapping to schema v1",
                                    [displayName = std::move (displayName),
                                     productPattern = std::move (productPattern)] (const juce::var&) -> juce::var
        {
            auto* root = new juce::DynamicObject();
            root->setProperty ("schemaVersion", 1);
            root->setProperty ("displayName", displayName);

            auto* device = new juce::DynamicObject();
            device->setProperty ("manufacturer", ".*");
            device->setProperty ("product", productPattern);
            auto* match = new juce::DynamicObject();
            match->setProperty ("midiName", productPattern);
            device->setProperty ("match", juce::var (match));
            root->setProperty ("device", juce::var (device));

            root->setProperty ("bindings", juce::Array<juce::var>());
            return juce::var (root);
        });
        return registry;
    }

    class MappingMigrationTests final : public juce::UnitTest
    {
    public:
        MappingMigrationTests() : juce::UnitTest ("Mapping Migration (PRD-0049)", "Sonik") {}

        void runTest() override
        {
            beginTest ("Registry returns unchanged input when versions match");
            {
                MigrationRegistry registry;
                const auto input = parseJson (R"({"schemaVersion":1,"name":"same"})");
                const auto result = registry.apply (input, 1, 1, "same.json");

                expect (! result.error.has_value());
                expect (result.stepsApplied.empty());
                expectEquals (canonicalJson (result.migratedJson), canonicalJson (input));
            }

            beginTest ("Registry rejects schemas newer than supported");
            {
                MigrationRegistry registry;
                const auto result = registry.apply (parseJson (R"({"schemaVersion":3})"), 3, 2, "future.json");

                expect (result.error.has_value());
                if (result.error.has_value())
                {
                    expectEquals (result.error->atVersion, 3);
                    expectEquals (result.error->reason, juce::String ("schema version newer than supported"));
                    expectEquals (result.error->sourcePath, juce::String ("future.json"));
                }
            }

            beginTest ("Registry reports a missing migration gap");
            {
                MigrationRegistry registry;
                const auto result = registry.apply (parseJson (R"({"schemaVersion":1})"), 1, 2, "gap.json");

                expect (result.error.has_value());
                if (result.error.has_value())
                {
                    expectEquals (result.error->atVersion, 1);
                    expectEquals (result.error->reason,
                                  juce::String ("no migration registered from v1 to v2"));
                    expectEquals (result.error->sourcePath, juce::String ("gap.json"));
                }
                expect (result.stepsApplied.empty());
            }

            beginTest ("Registry reports thrown migration exceptions as structured errors");
            {
                MigrationRegistry registry;
                registry.registerMigration (1, "throwing test migration", [] (const juce::var&) -> juce::var
                {
                    throw std::runtime_error ("synthetic migration failure");
                });

                const auto input = parseJson (R"({"schemaVersion":1,"name":"Throwing"})");
                const auto result = registry.apply (input, 1, 2, "throws.json");

                expect (result.error.has_value());
                if (result.error.has_value())
                {
                    expectEquals (result.error->atVersion, 1);
                    expect (result.error->reason.contains ("synthetic migration failure"));
                    expectEquals (result.error->sourcePath, juce::String ("throws.json"));
                }
                expect (result.stepsApplied.empty());
                expectEquals (canonicalJson (result.migratedJson), canonicalJson (input));
            }

            beginTest ("Registry chains synthetic migrations to a golden output");
            {
                MigrationRegistry registry;
                registry.registerMigration (1, "add title field", [] (const juce::var& input) -> juce::var
                {
                    auto* root = new juce::DynamicObject();
                    root->setProperty ("schemaVersion", 2);
                    root->setProperty ("title", input.getProperty ("name", "").toString());
                    return juce::var (root);
                });
                registry.registerMigration (2, "add enabled flag", [] (const juce::var& input) -> juce::var
                {
                    auto* root = new juce::DynamicObject();
                    root->setProperty ("schemaVersion", 3);
                    root->setProperty ("title", input.getProperty ("title", "").toString());
                    root->setProperty ("enabled", true);
                    return juce::var (root);
                });

                const auto result = registry.apply (parseJson (R"({"schemaVersion":1,"name":"Golden"})"),
                                                    1, 3, "golden.json");

                expect (! result.error.has_value());
                expectEquals ((int) result.stepsApplied.size(), 2);
                if (result.stepsApplied.size() == 2)
                {
                    expectEquals (result.stepsApplied[0].fromVersion, 1);
                    expectEquals (result.stepsApplied[0].toVersion, 2);
                    expectEquals (result.stepsApplied[0].description, juce::String ("add title field"));
                    expectEquals (result.stepsApplied[1].fromVersion, 2);
                    expectEquals (result.stepsApplied[1].toVersion, 3);
                    expectEquals (result.stepsApplied[1].description, juce::String ("add enabled flag"));
                }

                const juce::String golden = R"({"schemaVersion":3,"title":"Golden","enabled":true})";
                expectEquals (canonicalJson (result.migratedJson), canonicalJson (parseJson (golden.toRawUTF8())));
            }

            beginTest ("MappingStore loads current-schema mappings without migration records");
            {
                StubHost host;
                MidiDeviceManager manager (host);
                const auto dir = makeTempDir ("current");
                const auto file = dir.getChildFile ("current.json");
                file.replaceWithText (makeCurrentSchemaMappingJson ("Current Profile"));

                MappingStore store (manager, dir, false);

                expect (store.getLoadErrors().empty());
                expect (store.getMigratedMappings().empty());
                expect (! store.saveMigratedCopy ("current"));
                const auto saved = juce::JSON::parse (file.loadFileAsString());
                expectEquals ((int) saved.getProperty ("schemaVersion", 0), 1);

                dir.deleteRecursively();
            }

            beginTest ("MappingStore keeps current-schema parser failures as validation errors");
            {
                StubHost host;
                MidiDeviceManager manager (host);
                const auto dir = makeTempDir ("validation");
                dir.getChildFile ("invalid-current.json").replaceWithText (R"({
                    "schemaVersion": 1,
                    "bindings": [
                        { "target": "deck.A.not.a.real.target",
                          "midi": { "channel": 1, "status": "note", "data1": 14 },
                          "transform": "momentary" }
                    ]
                })");

                MappingStore store (manager, dir, false);
                const auto errors = store.getLoadErrors();

                bool sawValidationError = false;
                bool sawMigrationProducedInvalidOutput = false;
                for (const auto& error : errors)
                {
                    if (error.kind == MappingLoadError::Kind::ValidationError)
                    {
                        sawValidationError = true;
                        expect (error.parserError.has_value());
                    }
                    if (error.kind == MappingLoadError::Kind::MigrationProducedInvalidOutput)
                        sawMigrationProducedInvalidOutput = true;
                }
                expect (sawValidationError);
                expect (! sawMigrationProducedInvalidOutput);
                expect (store.getMigratedMappings().empty());

                dir.deleteRecursively();
            }

            beginTest ("MappingStore rejects a user mapping from a newer schema");
            {
                StubHost host;
                MidiDeviceManager manager (host);
                const auto dir = makeTempDir ("newer");
                dir.getChildFile ("future.json").replaceWithText (R"({"schemaVersion":2,"bindings":[]})");

                MappingStore store (manager, dir, false);
                const auto errors = store.getLoadErrors();

                bool sawUnsupported = false;
                for (const auto& error : errors)
                {
                    if (error.kind == MappingLoadError::Kind::UnsupportedSchemaVersion)
                    {
                        sawUnsupported = true;
                        expectEquals (error.fromVersion, 2);
                        expectEquals (error.maxSupportedVersion, kCurrentSchemaVersion);
                        expect (error.migrationError.has_value());
                    }
                }
                expect (sawUnsupported);
                expect (store.getMigratedMappings().empty());

                dir.deleteRecursively();
            }

            beginTest ("MappingStore surfaces missing migrations as MigrationFailed");
            {
                StubHost host;
                MidiDeviceManager manager (host);
                const auto dir = makeTempDir ("missing");
                dir.getChildFile ("legacy.json").replaceWithText (R"({"schemaVersion":0,"bindings":[]})");

                MappingStore store (manager, dir, false);
                const auto errors = store.getLoadErrors();

                bool sawMigrationFailure = false;
                for (const auto& error : errors)
                {
                    if (error.kind == MappingLoadError::Kind::MigrationFailed)
                    {
                        sawMigrationFailure = true;
                        expect (error.message.contains ("no migration registered from v0 to v1"));
                        expect (error.migrationError.has_value());
                        if (error.migrationError.has_value())
                            expect (error.migrationError->sourcePath.contains ("legacy.json"));
                    }
                }
                expect (sawMigrationFailure);
                expect (store.getMigratedMappings().empty());

                dir.deleteRecursively();
            }

            beginTest ("MappingStore surfaces faulty migrated JSON as MigrationProducedInvalidOutput");
            {
                MigrationRegistry registry;
                registry.registerMigration (0, "faulty v0 to v1 test migration", [] (const juce::var&) -> juce::var
                {
                    auto* root = new juce::DynamicObject();
                    root->setProperty ("schemaVersion", 1);

                    juce::Array<juce::var> bindings;
                    auto* binding = new juce::DynamicObject();
                    binding->setProperty ("target", "deck.A.not.a.real.target");

                    auto* midi = new juce::DynamicObject();
                    midi->setProperty ("channel", 1);
                    midi->setProperty ("status", "note");
                    midi->setProperty ("data1", 14);
                    binding->setProperty ("midi", juce::var (midi));
                    binding->setProperty ("transform", "momentary");
                    bindings.add (juce::var (binding));
                    root->setProperty ("bindings", bindings);

                    return juce::var (root);
                });

                StubHost host;
                MidiDeviceManager manager (host);
                const auto dir = makeTempDir ("faulty");
                dir.getChildFile ("faulty.json").replaceWithText (R"({"schemaVersion":0,"bindings":[]})");

                MappingStore store (manager, dir, false, std::move (registry));
                const auto errors = store.getLoadErrors();

                bool sawInvalidOutput = false;
                for (const auto& error : errors)
                {
                    if (error.kind == MappingLoadError::Kind::MigrationProducedInvalidOutput)
                    {
                        sawInvalidOutput = true;
                        expect (error.lastMigrationStep.has_value());
                        expect (error.parserError.has_value());
                        if (error.lastMigrationStep.has_value())
                        {
                            expectEquals (error.lastMigrationStep->fromVersion, 0);
                            expectEquals (error.lastMigrationStep->toVersion, 1);
                            expectEquals (error.lastMigrationStep->description,
                                          juce::String ("faulty v0 to v1 test migration"));
                        }
                    }
                }
                expect (sawInvalidOutput);
                expect (store.getMigratedMappings().empty());

                dir.deleteRecursively();
            }

            beginTest ("saveMigratedCopy writes the migrated JSON and clears the record");
            {
                StubHost host;
                host.inputs.add (juce::MidiDeviceInfo ("Migrated Controller", "migrated-controller-in"));
                MidiDeviceManager manager (host);
                manager.initialise();
                std::uint64_t deviceId = 0;
                for (const auto& record : manager.getDevices())
                    if (record.isInput)
                    {
                        deviceId = record.deviceId;
                        break;
                    }
                expect (deviceId != 0);

                const auto dir = makeTempDir ("savecopy");
                const auto file = dir.getChildFile ("legacy.json");
                file.replaceWithText (R"({"schemaVersion":0,"legacy":true})");

                {
                    MappingStore store (manager,
                                        dir,
                                        false,
                                        makeLegacyToCurrentRegistry ("Migrated Profile",
                                                                     "Migrated Controller"));

                    auto migrated = store.getMigratedMappings();
                    expectEquals ((int) migrated.size(), 1);
                    if (! migrated.empty())
                    {
                        expectEquals (migrated.front().deviceId, deviceId);
                        expectEquals (migrated.front().mappingId, juce::String ("legacy"));
                        expectEquals (migrated.front().sourcePath, file.getFullPathName());
                        expectEquals ((int) migrated.front().stepsApplied.size(), 1);
                        if (! migrated.front().stepsApplied.empty())
                            expectEquals (migrated.front().stepsApplied.front().description,
                                          juce::String ("upgrade test mapping to schema v1"));
                    }
                    expect (file.loadFileAsString().contains ("\"schemaVersion\":0"));

                    expect (! store.saveMigratedCopy ("missing"));
                    expect (store.saveMigratedCopy ("legacy"));
                    expect (store.getMigratedMappings().empty());
                    expect (! dir.getChildFile ("legacy.json.tmp").existsAsFile());

                    const auto saved = juce::JSON::parse (file.loadFileAsString());
                    expectEquals ((int) saved.getProperty ("schemaVersion", 0), 1);
                    expectEquals (saved.getProperty ("displayName", "").toString(),
                                  juce::String ("Migrated Profile"));
                }

                MappingStore reloadedStore (manager, dir, false);
                expect (reloadedStore.getLoadErrors().empty());
                expect (reloadedStore.getMigratedMappings().empty());

                dir.deleteRecursively();
            }
        }
    };

    static MappingMigrationTests mappingMigrationTests;
} // namespace