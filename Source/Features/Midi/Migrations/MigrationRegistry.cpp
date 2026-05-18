#include "MigrationRegistry.h"

#include <juce_events/juce_events.h>

#include <exception>
#include <utility>

namespace sonik::midi
{
    namespace
    {
        juce::String missingMigrationMessage (int fromVersion)
        {
            return "no migration registered from v" + juce::String (fromVersion)
                 + " to v" + juce::String (fromVersion + 1);
        }
    }

    void MigrationRegistry::registerMigration (int fromVersion,
                                               juce::String description,
                                               MigrationFunction fn)
    {
        jassert (fromVersion >= 0);
        jassert (static_cast<bool> (fn));
        jassert (migrations.find (fromVersion) == migrations.end());

        migrations[fromVersion] = RegisteredMigration { std::move (description), std::move (fn) };
    }

    MigrationResult MigrationRegistry::apply (juce::var input,
                                             int fromVersion,
                                             int toVersion) const
    {
        return apply (std::move (input), fromVersion, toVersion, juce::StringRef());
    }

    MigrationResult MigrationRegistry::apply (juce::var input,
                                             int fromVersion,
                                             int toVersion,
                                             juce::StringRef sourcePath) const
    {
        JUCE_ASSERT_MESSAGE_THREAD;

        MigrationResult result;
        result.migratedJson = std::move (input);

        if (fromVersion == toVersion)
            return result;

        if (fromVersion > toVersion)
        {
            result.error = MigrationError { fromVersion,
                                            "schema version newer than supported",
                                            juce::String (sourcePath) };
            return result;
        }

        for (int version = fromVersion; version < toVersion; ++version)
        {
            const auto migration = migrations.find (version);
            if (migration == migrations.end() || ! migration->second.fn)
            {
                result.error = MigrationError { version,
                                                missingMigrationMessage (version),
                                                juce::String (sourcePath) };
                return result;
            }

            try
            {
                result.migratedJson = migration->second.fn (result.migratedJson);
            }
            catch (const std::exception& e)
            {
                result.error = MigrationError { version,
                                                "migration threw exception: " + juce::String (e.what()),
                                                juce::String (sourcePath) };
                return result;
            }
            catch (...)
            {
                result.error = MigrationError { version,
                                                "migration threw unknown exception",
                                                juce::String (sourcePath) };
                return result;
            }

            result.stepsApplied.push_back (MigrationStep { version,
                                                           version + 1,
                                                           migration->second.description });
        }

        return result;
    }
} // namespace sonik::midi