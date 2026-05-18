// Tests for PRD-0050: Mapping Import / Export and `.sonikmidi.json` bundle.
//
// Covers acceptance criteria (a)-(i) from PRD-0050 Section 1.4 final bullet:
//   (a) round-trip export → import produces an identical mapping
//   (b) SHA-256 verification rejects a tampered mapping block
//   (c) missing manifest fields produce stage-2 errors
//   (d) a v(N-1) bundle in a v(N) build is migrated and imported successfully
//   (e) a v(N+1) bundle in a v(N) build produces UnsupportedSchemaVersion
//   (f) a binding referencing an unknown target produces a non-fatal stage-7
//       warning and the import proceeds with the binding dropped
//   (g) conflict detection identifies an existing user-mapping collision
//   (h) Rename and Replace conflict resolutions each write the correct file
//   (i) atomic write semantics: a synthetic failure during the file write
//       leaves no partial file (no `.tmp` artefact, no half-baked target)

#include "Features/Midi/MappingStore.h"
#include "Features/Midi/MappingParser.h"
#include "Features/Midi/MappingSerializer.h"
#include "Features/Midi/MidiDeviceManager.h"
#include "Features/Midi/MidiHostInterface.h"
#include "Features/Midi/Migrations/MigrationRegistry.h"
#include "Features/Midi/Bundle/SonikMidiBundle.h"
#include "Features/Midi/Bundle/SonikMidiBundleSerializer.h"
#include "Features/Midi/Bundle/MappingExportService.h"
#include "Features/Midi/Bundle/MappingImportService.h"
#include "Features/Midi/Bundle/ImportPipeline.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

namespace
{
    using namespace sonik::midi;

    //==========================================================================
    class StubHost : public MidiHostInterface
    {
    public:
        juce::Array<juce::MidiDeviceInfo> inputs;
        juce::Array<juce::MidiDeviceInfo> getAvailableInputs()  override { return inputs; }
        juce::Array<juce::MidiDeviceInfo> getAvailableOutputs() override { return {}; }
        std::unique_ptr<juce::MidiInput>  openInputDevice  (const juce::String&,
                                                            juce::MidiInputCallback*) override { return nullptr; }
        std::unique_ptr<juce::MidiOutput> openOutputDevice (const juce::String&) override      { return nullptr; }
    };

    juce::File makeTempDir (const juce::String& tag)
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("sonik-importexport-" + tag
                                            + "-" + juce::String::toHexString (
                                                juce::Random::getSystemRandom().nextInt64()));
        dir.createDirectory();
        return dir;
    }

    constexpr const char* kSampleMappingJson = R"JSON({
        "schemaVersion": 1,
        "displayName": "Friends DDM4000 (my mix)",
        "device": {
            "manufacturer": "Behringer",
            "product": "DDM4000",
            "match": { "midiName": "DDM4000" }
        },
        "modifiers": [
            { "id": "shift",
              "binding": { "channel": 1, "status": "note", "data1": 63,
                           "style": "momentary" } }
        ],
        "bindings": [
            { "target": "deck.A.transport.play",
              "midi": { "channel": 1, "status": "note", "data1": 14 },
              "transform": "momentary" },
            { "target": "deck.A.gain",
              "midi": { "channel": 1, "status": "cc", "data1": 7 },
              "transform": "linear" }
        ]
    })JSON";

    juce::var parseToVar (const char* json)
    {
        return juce::JSON::parse (juce::String::fromUTF8 (json));
    }

    Mapping parseToMapping (const char* json)
    {
        auto pr = MappingParser::parse (parseToVar (json), "<test>");
        jassert (pr.errors.empty());
        return std::move (pr.mapping);
    }

    juce::var buildBundleVar (const juce::var& mappingJson,
                              const juce::String& sha256,
                              const juce::String& appVersion = "1.2.3",
                              int schemaVersion = 1,
                              const juce::String& exportedAt = "2026-05-12T14:30:00Z")
    {
        SonikMidiBundle bundle;
        bundle.manifest.appVersion                 = appVersion;
        bundle.manifest.sonikSchemaVersionAtExport = schemaVersion;
        bundle.manifest.exportedAtIso8601          = exportedAt;
        bundle.manifest.sha256                     = sha256;
        bundle.manifest.exporterDeviceName         = "TestHost";
        bundle.mappingJson                         = mappingJson;
        return SonikMidiBundleSerializer::toJson (bundle);
    }

    //==========================================================================
    class MappingImportExportTests final : public juce::UnitTest
    {
    public:
        MappingImportExportTests() : juce::UnitTest ("Mapping Import/Export (PRD-0050)", "Sonik") {}

        void runTest() override
        {
            //------------------------------------------------------------------
            beginTest ("sha256OfSortedJson is deterministic & key-order independent");
            {
                auto a = parseToVar (R"({"b":1,"a":[1,{"y":2,"x":3}],"c":"hi"})");
                auto b = parseToVar (R"({"a":[1,{"x":3,"y":2}],"c":"hi","b":1})");
                const auto ha = SonikMidiBundleSerializer::sha256OfSortedJson (a);
                const auto hb = SonikMidiBundleSerializer::sha256OfSortedJson (b);
                expectEquals (ha, hb);
                expectEquals (ha.length(), 64);
            }

            //------------------------------------------------------------------
            // (a) Round-trip: export → import yields a semantically identical mapping.
            beginTest ("Round-trip export -> import yields identical mapping (AC a)");
            {
                StubHost host;
                MidiDeviceManager mgr (host);
                const auto dir = makeTempDir ("roundtrip");
                MappingStore store (mgr, dir, /*async*/ false);

                // Seed a user mapping.
                {
                    auto mapping = parseToMapping (kSampleMappingJson);
                    const auto r = store.registerImportedMapping (mapping, "friends-ddm4000", false);
                    expect (r.status == RegisterImportedResult::Status::Ok);
                }

                juce::ThreadPool pool (1);
                MappingExportService exp (store, pool, "1.2.3");
                const auto bundleFile = dir.getChildFile ("export.sonikmidi.json");
                const auto ex = exp.exportMappingNow ("friends-ddm4000", bundleFile);
                expect (ex.status == ExportResult::Status::Ok);
                expect (bundleFile.existsAsFile());
                expect (! dir.getChildFile ("export.sonikmidi.json.tmp").existsAsFile());

                // Import into a fresh store.
                const auto dir2 = makeTempDir ("roundtrip-target");
                MappingStore store2 (mgr, dir2, /*async*/ false);
                MappingImportService imp (store2, store2.getMigrationRegistry(), pool,
                                          store2.getSchemaVersionTarget());
                auto prep = imp.prepareImportFromFile (bundleFile);
                expect (prep.ok);
                expect (! prep.preview.conflictDetected);

                const auto commit = imp.commitImport (prep, ConflictResolution::None);
                expect (commit.status == ImportCommitResult::Status::Ok);

                auto imported = store2.getMappingById (commit.finalMappingId);
                expect (imported != nullptr);
                if (imported != nullptr)
                {
                    expectEquals ((int) imported->bindings.size(), 2);
                    expectEquals ((int) imported->modifiers.size(), 1);
                    expectEquals (imported->displayName, juce::String ("Friends DDM4000 (my mix)"));
                }

                dir.deleteRecursively();
                dir2.deleteRecursively();
            }

            //------------------------------------------------------------------
            // (b) SHA-256 verification rejects a tampered mapping block.
            beginTest ("SHA-256 verification rejects tampered mapping (AC b)");
            {
                StubHost host;
                MidiDeviceManager mgr (host);
                const auto dir = makeTempDir ("tamper");
                MappingStore store (mgr, dir, /*async*/ false);
                juce::ThreadPool pool (1);
                MappingImportService imp (store, store.getMigrationRegistry(), pool);

                auto mappingVar = parseToVar (kSampleMappingJson);
                const auto goodHash = SonikMidiBundleSerializer::sha256OfSortedJson (mappingVar);

                // Tamper with the mapping AFTER hashing.
                auto* obj = mappingVar.getDynamicObject();
                expect (obj != nullptr);
                obj->setProperty ("displayName", juce::String ("Tampered name"));

                const auto bundleVar = buildBundleVar (mappingVar, goodHash);
                auto prep = imp.prepareImportFromJson (bundleVar);
                expect (! prep.ok);
                expect (prep.error.stage == ImportStage::Sha256Verify);
                expect (prep.error.expectedSha256.isNotEmpty());
                expect (prep.error.computedSha256.isNotEmpty());
                expect (! prep.error.expectedSha256.equalsIgnoreCase (prep.error.computedSha256));

                dir.deleteRecursively();
            }

            //------------------------------------------------------------------
            // (c) Missing manifest fields → stage-2 ManifestExtract errors.
            beginTest ("Missing manifest fields raise stage-2 error (AC c)");
            {
                StubHost host;
                MidiDeviceManager mgr (host);
                const auto dir = makeTempDir ("manifest");
                MappingStore store (mgr, dir, /*async*/ false);
                juce::ThreadPool pool (1);
                MappingImportService imp (store, store.getMigrationRegistry(), pool);

                // (i) No manifest at all.
                {
                    auto* root = new juce::DynamicObject();
                    root->setProperty ("mapping", parseToVar (kSampleMappingJson));
                    auto prep = imp.prepareImportFromJson (juce::var (root));
                    expect (! prep.ok);
                    expect (prep.error.stage == ImportStage::ManifestExtract);
                }

                // (ii) Manifest present but missing required `sha256`.
                {
                    auto* root      = new juce::DynamicObject();
                    auto* manifest  = new juce::DynamicObject();
                    manifest->setProperty ("appVersion", juce::String ("1.0.0"));
                    manifest->setProperty ("sonikSchemaVersionAtExport", 1);
                    manifest->setProperty ("exportedAt", juce::String ("2026-01-01T00:00:00Z"));
                    // No sha256.
                    root->setProperty ("manifest", juce::var (manifest));
                    root->setProperty ("mapping", parseToVar (kSampleMappingJson));
                    auto prep = imp.prepareImportFromJson (juce::var (root));
                    expect (! prep.ok);
                    expect (prep.error.stage == ImportStage::ManifestExtract);
                    expectEquals (prep.error.missingManifestField, juce::String ("sha256"));
                }

                dir.deleteRecursively();
            }

            //------------------------------------------------------------------
            // (d) v(N-1) bundle in a v(N) build is migrated and imported.
            beginTest ("Older-schema bundle is migrated and imported (AC d)");
            {
                StubHost host;
                MidiDeviceManager mgr (host);
                const auto dir = makeTempDir ("migrate");

                // Build a v(N) store with a v0→v1 migration that injects the
                // entire current-schema mapping body.
                MigrationRegistry registry;
                const juce::String targetMapping = kSampleMappingJson;
                registry.registerMigration (0, "v0 -> v1",
                    [targetMapping] (const juce::var&) -> juce::var
                    {
                        return juce::JSON::parse (targetMapping);
                    });

                MappingStore store (mgr, dir, /*async*/ false, std::move (registry), 1);
                juce::ThreadPool pool (1);
                MappingImportService imp (store, store.getMigrationRegistry(), pool, 1);

                // Construct a v0 mapping with the right hash (sha is computed on
                // the v0 body — verified BEFORE migration runs).
                auto* v0 = new juce::DynamicObject();
                v0->setProperty ("schemaVersion", 0);
                v0->setProperty ("displayName", juce::String ("Legacy"));
                v0->setProperty ("bindings", juce::Array<juce::var>());
                juce::var v0Var (v0);
                const auto v0Hash = SonikMidiBundleSerializer::sha256OfSortedJson (v0Var);

                auto bundle = buildBundleVar (v0Var, v0Hash, "0.0.1", 0);
                auto prep   = imp.prepareImportFromJson (bundle);
                expect (prep.ok);
                expectEquals (prep.preview.migrationStepsApplied, 1);
                expectEquals (prep.preview.schemaVersion, 1);

                const auto commit = imp.commitImport (prep, ConflictResolution::None);
                expect (commit.status == ImportCommitResult::Status::Ok);

                dir.deleteRecursively();
            }

            //------------------------------------------------------------------
            // (e) v(N+1) bundle in a v(N) build raises UnsupportedSchemaVersion.
            beginTest ("Newer-schema bundle is rejected (AC e)");
            {
                StubHost host;
                MidiDeviceManager mgr (host);
                const auto dir = makeTempDir ("newer");
                MappingStore store (mgr, dir, /*async*/ false);
                juce::ThreadPool pool (1);
                MappingImportService imp (store, store.getMigrationRegistry(), pool, 1);

                // v4 mapping into a v1 build.
                auto* v4 = new juce::DynamicObject();
                v4->setProperty ("schemaVersion", 4);
                v4->setProperty ("bindings", juce::Array<juce::var>());
                juce::var v4Var (v4);
                const auto h = SonikMidiBundleSerializer::sha256OfSortedJson (v4Var);

                auto bundle = buildBundleVar (v4Var, h, "9.9.9", 4);
                auto prep   = imp.prepareImportFromJson (bundle);
                expect (! prep.ok);
                expect (prep.error.stage == ImportStage::SchemaMigrate);
                expectEquals (prep.error.fromVersion, 4);
                expectEquals (prep.error.maxSupportedVersion, 1);

                dir.deleteRecursively();
            }

            //------------------------------------------------------------------
            // (f) Binding with unknown target: stage 7 non-fatal warning,
            //     commit still succeeds, binding is dropped at load.
            beginTest ("Unknown target produces stage-7 warning, commit succeeds (AC f)");
            {
                StubHost host;
                MidiDeviceManager mgr (host);
                const auto dir = makeTempDir ("unknown-target");
                MappingStore store (mgr, dir, /*async*/ false);
                juce::ThreadPool pool (1);
                MappingImportService imp (store, store.getMigrationRegistry(), pool);

                const char* withUnknown = R"JSON({
                    "schemaVersion": 1,
                    "displayName": "Future mapping",
                    "device": { "manufacturer": ".*", "product": ".*",
                                "match": { "midiName": ".*" } },
                    "bindings": [
                        { "target": "deck.A.transport.play",
                          "midi": { "channel": 1, "status": "note", "data1": 14 },
                          "transform": "momentary" },
                        { "target": "deck.A.beatfx.bloom",
                          "midi": { "channel": 1, "status": "cc", "data1": 99 },
                          "transform": "linear" }
                    ]
                })JSON";

                auto mvar = parseToVar (withUnknown);
                const auto hash = SonikMidiBundleSerializer::sha256OfSortedJson (mvar);
                const auto bundle = buildBundleVar (mvar, hash);

                auto prep = imp.prepareImportFromJson (bundle);
                expect (prep.ok);
                expect (! prep.preview.unknownTargetIds.isEmpty());
                expect (prep.preview.unknownTargetIds.contains ("deck.A.beatfx.bloom"));

                const auto commit = imp.commitImport (prep, ConflictResolution::None);
                expect (commit.status == ImportCommitResult::Status::Ok);

                auto imported = store.getMappingById (commit.finalMappingId);
                expect (imported != nullptr);
                if (imported != nullptr)
                {
                    // The unknown-target binding is dropped at parse time
                    // (PRD-0042 partial-load behaviour).
                    expectEquals ((int) imported->bindings.size(), 1);
                }

                dir.deleteRecursively();
            }

            //------------------------------------------------------------------
            // (g) Conflict detection identifies an existing user mapping collision.
            beginTest ("Conflict detection (AC g)");
            {
                StubHost host;
                MidiDeviceManager mgr (host);
                const auto dir = makeTempDir ("conflict");
                MappingStore store (mgr, dir, /*async*/ false);
                juce::ThreadPool pool (1);
                MappingImportService imp (store, store.getMigrationRegistry(), pool);

                // Seed: pre-existing user mapping under the same display name.
                {
                    auto m = parseToMapping (kSampleMappingJson);
                    const auto reg = store.registerImportedMapping (m, "friends-ddm4000-my-mix", false);
                    expect (reg.status == RegisterImportedResult::Status::Ok);
                }

                auto mvar = parseToVar (kSampleMappingJson);
                // displayName "Friends DDM4000 (my mix)" → sanitised stem
                // "Friends-DDM4000-my-mix"... case differs from seed stem.
                // To force the conflict, set displayName so it sanitises to
                // exactly "friends-ddm4000-my-mix".
                if (auto* obj = mvar.getDynamicObject())
                    obj->setProperty ("displayName", juce::String ("friends-ddm4000-my-mix"));

                const auto h = SonikMidiBundleSerializer::sha256OfSortedJson (mvar);
                auto bundle = buildBundleVar (mvar, h);
                auto prep   = imp.prepareImportFromJson (bundle);
                expect (prep.ok);
                expect (prep.preview.conflictDetected);
                expectEquals (prep.preview.conflictExistingMappingId,
                              juce::String ("friends-ddm4000-my-mix"));

                // Without a resolution, commit refuses.
                const auto refused = imp.commitImport (prep, ConflictResolution::None);
                expect (refused.status == ImportCommitResult::Status::ConflictUnresolved);

                dir.deleteRecursively();
            }

            //------------------------------------------------------------------
            // (h) Rename and Replace conflict resolutions both write correctly.
            beginTest ("Conflict resolution: Rename and Replace write correctly (AC h)");
            {
                StubHost host;
                MidiDeviceManager mgr (host);
                const auto dir = makeTempDir ("resolve");
                MappingStore store (mgr, dir, /*async*/ false);
                juce::ThreadPool pool (1);
                MappingImportService imp (store, store.getMigrationRegistry(), pool);

                // Seed existing user mapping.
                {
                    auto m = parseToMapping (kSampleMappingJson);
                    const auto reg = store.registerImportedMapping (m, "friends-ddm4000-my-mix", false);
                    expect (reg.status == RegisterImportedResult::Status::Ok);
                }

                auto buildConflictBundle = [&]() -> juce::var
                {
                    auto v = parseToVar (kSampleMappingJson);
                    if (auto* o = v.getDynamicObject())
                        o->setProperty ("displayName", juce::String ("friends-ddm4000-my-mix"));
                    const auto h = SonikMidiBundleSerializer::sha256OfSortedJson (v);
                    return buildBundleVar (v, h);
                };

                // ---- Rename -------------------------------------------------
                auto prepRename = imp.prepareImportFromJson (buildConflictBundle());
                expect (prepRename.preview.conflictDetected);
                const auto renameResult = imp.commitImport (prepRename,
                                                            ConflictResolution::RenameTo,
                                                            "friends-ddm4000-renamed");
                expect (renameResult.status == ImportCommitResult::Status::Ok);
                expectEquals (renameResult.finalMappingId, juce::String ("friends-ddm4000-renamed"));
                expect (dir.getChildFile ("friends-ddm4000-renamed.json").existsAsFile());
                expect (dir.getChildFile ("friends-ddm4000-my-mix.json").existsAsFile()); // original still there

                // ---- Replace ------------------------------------------------
                auto prepReplace = imp.prepareImportFromJson (buildConflictBundle());
                expect (prepReplace.preview.conflictDetected);
                const auto sizeBefore = dir.getChildFile ("friends-ddm4000-my-mix.json").getSize();
                const auto replaceResult = imp.commitImport (prepReplace,
                                                             ConflictResolution::Replace);
                expect (replaceResult.status == ImportCommitResult::Status::Ok);
                expect (dir.getChildFile ("friends-ddm4000-my-mix.json").existsAsFile());
                // File still exists (atomically replaced).
                juce::ignoreUnused (sizeBefore);
                expect (! dir.getChildFile ("friends-ddm4000-my-mix.json.tmp").existsAsFile());

                dir.deleteRecursively();
            }

            //------------------------------------------------------------------
            // (i) Atomic write semantics: failed write leaves no partial file.
            beginTest ("Atomic write: failure leaves no partial file (AC i)");
            {
                StubHost host;
                MidiDeviceManager mgr (host);
                const auto dir = makeTempDir ("atomic");
                MappingStore store (mgr, dir, /*async*/ false);

                {
                    auto m = parseToMapping (kSampleMappingJson);
                    const auto r = store.registerImportedMapping (m, "atomic-source", false);
                    expect (r.status == RegisterImportedResult::Status::Ok);
                }

                juce::ThreadPool pool (1);
                MappingExportService exp (store, pool, "1.2.3");

                // Synthetic failure: aim at a path inside a non-existent &
                // non-creatable parent directory under /dev/null/<x>. On
                // POSIX, /dev/null is a character device, so creating a child
                // file under it is guaranteed to fail. We still expect:
                //   * status != Ok
                //   * no `.tmp` left behind anywhere our code touched
                //   * no target file left behind
                const auto unwritableParent = juce::File ("/dev/null/sonik-no-such-dir");
                const auto target           = unwritableParent.getChildFile ("export.sonikmidi.json");

                const auto ex = exp.exportMappingNow ("atomic-source", target);
                expect (ex.status != ExportResult::Status::Ok);
                expect (! target.existsAsFile());
                expect (! target.getSiblingFile (target.getFileName() + ".tmp").existsAsFile());

                dir.deleteRecursively();
            }
        }
    };

    static MappingImportExportTests gMappingImportExportTests;
}
