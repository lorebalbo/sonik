#pragma once

#include <juce_core/juce_core.h>
#include <optional>
#include <cstdint>

struct sqlite3;

struct SessionData
{
    int          deckCount      = 2;
    juce::String activeDeckId   = "A";
    juce::String loadedTracksJson;
};

struct TrackPersistentData
{
    juce::String cuePointsJson;
    juce::String beatgridJson;
    int          keyIndex           = -1;
    float        keyConfidence      = 0.0f;
    bool         keyManuallyAdjusted = false;
};

class TrackDatabase final
{
public:
    explicit TrackDatabase (const juce::File& dbFile);
    ~TrackDatabase();

    TrackDatabase (const TrackDatabase&) = delete;
    TrackDatabase& operator= (const TrackDatabase&) = delete;

    // Session state
    void        saveSessionState (int deckCount, const juce::String& activeDeckId,
                                  const juce::String& loadedTracksJson);
    SessionData loadSessionState();

    // Track data
    void saveTrackData (const juce::String& filePath, const juce::String& contentHash,
                        const juce::String& cuePointsJson, const juce::String& beatgridJson,
                        int keyIndex, float keyConfidence, bool keyManuallyAdjusted);
    std::optional<TrackPersistentData> loadTrackData (const juce::String& filePath,
                                                      const juce::String& contentHash);

private:
    void createTables();
    void exec (const juce::String& sql);

    sqlite3* dbHandle = nullptr;
};
