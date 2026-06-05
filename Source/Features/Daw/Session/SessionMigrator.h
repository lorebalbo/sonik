#pragma once
//==============================================================================
// PRD-0095: SessionMigrator — a free-standing, table-driven forward-only
// upgrader for `.soniksession` documents.
//
// Each registered step maps a document at `version N` to `version N+1` via a
// pure function juce::ValueTree(juce::ValueTree). `migrate` applies the ordered
// chain from the document's persisted version up to the running build's
// kCurrentSchemaVersion. Version 1 (the schema this PRD ships) has no
// predecessor, so the built-in table is empty: the *framework* is delivered now
// so the first real schema change is a one-entry diff, not a refactor (mirrors
// the mapping-migration framework, PRD-0049, in spirit).
//
// Migration is forward-only: a document is always upgraded *up* to the running
// build, never down. A document NEWER than kCurrentSchemaVersion is rejected by
// SessionSerializer before the migrator is ever consulted.
//
// THREADING: pure ValueTree transforms on the message/background thread. No
// audio-thread access, no file I/O.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <unordered_map>

namespace Daw::Session
{
    class SessionMigrator final
    {
    public:
        //----------------------------------------------------------------------
        // A pure upgrade step: takes a SONIK_SESSION root at version `fromVersion`
        // and returns the equivalent tree at `fromVersion + 1`. Steps must be
        // ADDITIVE and non-destructive: they add or rename known nodes and never
        // blanket-strip unknown ones, so forward-compatible data survives an
        // upgrade pass (§1.5.6).
        //----------------------------------------------------------------------
        using Step = std::function<juce::ValueTree (juce::ValueTree)>;

        // Registers the built-in migration table (empty in v1).
        SessionMigrator();

        // Registers a step upgrading `fromVersion -> fromVersion + 1`.
        void registerStep (int fromVersion, juce::String description, Step fn);

        //----------------------------------------------------------------------
        // Applies the ordered chain fromVersion -> toVersion. Returns the
        // migrated tree with its schemaVersion property stamped to `toVersion`.
        // A no-op pass-through when fromVersion == toVersion. If a required step
        // is missing the original tree is returned unchanged and `ok` is set
        // false (the caller treats a missing step as a corrupt/unsupported file).
        //----------------------------------------------------------------------
        juce::ValueTree migrate (juce::ValueTree sessionRoot,
                                 int fromVersion,
                                 int toVersion,
                                 bool& ok) const;

        // True if a step exists for every hop in [fromVersion, toVersion).
        bool canMigrate (int fromVersion, int toVersion) const;

    private:
        struct Registered
        {
            juce::String description;
            Step         fn;
        };

        std::unordered_map<int, Registered> steps; // keyed by fromVersion
    };
}
