#pragma once

#include <juce_core/juce_core.h>
#include <optional>
#include <cstdint>
#include <vector>

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

    // Waveform cache (PRD-0006)
    void storeWaveformData (const juce::String& contentHash, const juce::MemoryBlock& data);
    bool loadWaveformData (const juce::String& contentHash, juce::MemoryBlock& data);

    // Hot cue persistence (PRD-0012)
    void saveCuePointsJson (const juce::String& filePath, const juce::String& contentHash,
                            const juce::String& json);
    juce::String loadCuePointsJson (const juce::String& contentHash);

    // Loop persistence (PRD-0014)
    void saveLoopsJson (const juce::String& contentHash, const juce::String& json);
    juce::String loadLoopsJson (const juce::String& contentHash);

    // Stem cache persistence (PRD-0020)
    void insertStemRecord (const juce::String& contentHash,
                            const juce::String& modelVersion,
                            const juce::String& status);
    void updateStemRecord (const juce::String& contentHash,
                            const juce::String& status,
                            int64_t fileSizeBytes,
                            const juce::String& vocalPath,
                            const juce::String& drumsPath,
                            const juce::String& bassPath,
                            const juce::String& otherPath);
    bool hasStemRecord (const juce::String& contentHash) const;
    void deleteStemRecord (const juce::String& contentHash);
    juce::StringArray getPendingStemHashes();

    struct StemRecordInfo
    {
        juce::String contentHash;
        int64_t      fileSizeBytes = 0;
        int64_t      createdAt     = 0;
    };
    std::vector<StemRecordInfo> getAllStemRecords();

private:
    void createTables();
    void exec (const juce::String& sql);

    sqlite3* dbHandle = nullptr;
};
