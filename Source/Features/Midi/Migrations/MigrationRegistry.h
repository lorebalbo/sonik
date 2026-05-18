#pragma once
//==============================================================================
// PRD-0049: MIDI mapping schema migration registry.
//
// Migrations are pure JSON-to-JSON transforms used only on the Message thread
// while loading mapping files. They are never called from the audio thread or
// the MIDI callback thread.
//==============================================================================

#include <juce_core/juce_core.h>

#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace sonik::midi
{
    inline constexpr int kCurrentSchemaVersion = 1;

    using MigrationFunction = std::function<juce::var (const juce::var&)>;

    struct MigrationStep
    {
        int          fromVersion { 0 };
        int          toVersion   { 0 };
        juce::String description;
    };

    struct MigrationError
    {
        int          atVersion { 0 };
        juce::String reason;
        juce::String sourcePath;
    };

    struct MigrationResult
    {
        juce::var                     migratedJson;
        std::vector<MigrationStep>    stepsApplied;
        std::optional<MigrationError> error;
    };

    class MigrationRegistry final
    {
    public:
        MigrationRegistry() = default;

        void registerMigration (int fromVersion,
                                juce::String description,
                                MigrationFunction fn);

        MigrationResult apply (juce::var input,
                               int fromVersion,
                               int toVersion) const;

        MigrationResult apply (juce::var input,
                               int fromVersion,
                               int toVersion,
                               juce::StringRef sourcePath) const;

    private:
        struct RegisteredMigration
        {
            juce::String      description;
            MigrationFunction fn;
        };

        std::unordered_map<int, RegisteredMigration> migrations;
    };
} // namespace sonik::midi