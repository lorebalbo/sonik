#include "SessionMigrator.h"

#include "SessionSchema.h"

namespace Daw::Session
{
    SessionMigrator::SessionMigrator()
    {
        // -------------------------------------------------------------------
        // v1 is the first shipped schema: it has no predecessor, so the table
        // is intentionally empty. The first future schema change registers a
        // single step here, e.g.:
        //
        //   registerStep (1, "add foo node", [] (juce::ValueTree t) { ... });
        //
        // and bumps kCurrentSchemaVersion to 2 — with no change to `migrate`
        // or to SessionSerializer::load (§1.5.2).
        // -------------------------------------------------------------------
    }

    void SessionMigrator::registerStep (int fromVersion, juce::String description, Step fn)
    {
        steps[fromVersion] = Registered { std::move (description), std::move (fn) };
    }

    bool SessionMigrator::canMigrate (int fromVersion, int toVersion) const
    {
        if (fromVersion > toVersion)
            return false;

        for (int v = fromVersion; v < toVersion; ++v)
            if (steps.find (v) == steps.end())
                return false;

        return true;
    }

    juce::ValueTree SessionMigrator::migrate (juce::ValueTree sessionRoot,
                                              int fromVersion,
                                              int toVersion,
                                              bool& ok) const
    {
        ok = true;

        if (fromVersion == toVersion)
            return sessionRoot; // no-op pass-through (the v1 == v1 common case)

        if (fromVersion > toVersion)
        {
            ok = false; // downgrade is never attempted
            return sessionRoot;
        }

        juce::ValueTree current = sessionRoot;

        for (int v = fromVersion; v < toVersion; ++v)
        {
            const auto it = steps.find (v);

            if (it == steps.end())
            {
                ok = false; // a hop is missing: treat as unsupported
                return sessionRoot;
            }

            current = it->second.fn (current);
        }

        // Stamp the upgraded document to the target version so a subsequent save
        // records it as current.
        current.setProperty (IDs::schemaVersion, toVersion, nullptr);
        return current;
    }
}
