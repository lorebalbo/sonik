#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include "Features/Deck/Database/TrackDatabase.h"

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

// =============================================================================
// Data types
// =============================================================================

/// A single row result from library_tracks.
struct LibraryTrackRow
{
    int64_t      id              = 0;
    juce::String filePath;
    juce::String contentHash;
    juce::String title;
    juce::String artist;
    juce::String album;
    double       bpm             = 0.0;
    juce::String key;
    int          keyIndex        = -1;
    double       durationSeconds = 0.0;
    int64_t      fileSizeBytes   = 0;
    int64_t      dateAdded       = 0;
    int          isMissing       = 0;
    int          playCount       = 0;
    int          rating          = 0;
    int64_t      playlistEntryId = 0;
    int          playlistPosition = 0;
    juce::String playedAt;
};

struct PlaylistInfo
{
    int64_t      id         = 0;
    juce::String name;
    juce::String type;
    int          trackCount = 0;

    bool isNormal() const noexcept { return type == "normal"; }
    bool isHistory() const noexcept { return type == "history"; }
};

/// Deck-aware filter parameters — computed on the Message Thread.
struct DeckAwareFilterState
{
    juce::Array<int>                       compatibleKeyIndices; ///< Union across all loaded decks
    juce::Array<std::pair<double, double>> bpmWindows;           ///< [minBpm, maxBpm] per deck
    bool                                   keyMatchActive = false;
    bool                                   bpmMatchActive = false;
    double                                 bpmVision      = 6.0;
};

/// Parsed query parameters — produced by the scope operator parser on the Message Thread.
struct QueryParams
{
    juce::String         rawSearchString;
    juce::String         ftsTerms;          ///< Bare words joined for FTS MATCH
    juce::String         ftsTitleTerm;       ///< title:xxx
    juce::String         ftsArtistTerm;      ///< artist:xxx
    juce::String         ftsAlbumTerm;       ///< album:xxx
    bool                 hasBpmRange    = false;
    double               bpmMin         = 0.0;
    double               bpmMax         = 0.0;
    bool                 hasKeyFilter   = false;
    int                  keyIndex       = -1;
    bool                 hasRatingFilter = false;
    int                  ratingMin      = 0;
    juce::String         folderPath;
    int64_t              playlistId     = -1;
    juce::String         playlistType;
    std::vector<int64_t> preparationTrackIds;
    juce::String         sortColumn     = "date_added";
    bool                 sortAscending  = false;
    DeckAwareFilterState deckFilter;
};

// =============================================================================
// LibraryQueryThread
// =============================================================================

/// Persistent background thread that owns an exclusive sqlite3* connection and
/// executes all library read queries off the Message Thread.
///
/// Thread safety contract:
///   - The sqlite3* handle is opened inside run() and never touched from any
///     other thread. No mutex guards it — isolation by ownership is sufficient.
///   - pendingParams / hasPending are guarded by pendingMutex (std::mutex).
///   - cancelFlag is std::atomic<bool>.
///   - resultCallback is guarded by callbackLock (juce::CriticalSection).
///   - Results are posted back via juce::MessageManager::callAsync.
class LibraryQueryThread final : public juce::Thread
{
public:
    using ResultCallback = std::function<void(std::vector<LibraryTrackRow>)>;
    using PlaylistListCallback = std::function<void(std::vector<PlaylistInfo>)>;
    using PlaylistMutationCallback = std::function<void(bool ok, juce::String message, int64_t playlistId)>;

    explicit LibraryQueryThread (TrackDatabase& db);
    ~LibraryQueryThread() override;

    LibraryQueryThread (const LibraryQueryThread&)             = delete;
    LibraryQueryThread& operator= (const LibraryQueryThread&)  = delete;

    // -------------------------------------------------------------------------
    // Message-Thread API
    // -------------------------------------------------------------------------

    /// Dispatch a new query. Cancels any in-flight query and replaces the
    /// pending-query slot atomically. Thread-safe.
    void dispatchQuery (QueryParams params);

    /// Update the deck-aware filter and immediately re-dispatch the current
    /// query with the new filter (no debounce).
    void updateDeckFilter (DeckAwareFilterState state);

    /// Register the callback invoked on the Message Thread with result rows.
    void setResultCallback (ResultCallback cb);

    /// Change sort column and direction, then immediately re-dispatch.
    void setSortColumn (const juce::String& column, bool ascending);

    void requestPlaylists (PlaylistListCallback callback);
    void createPlaylist (juce::String name, PlaylistMutationCallback callback);
    void createPlaylistWithTracks (juce::String name, std::vector<int64_t> trackIds,
                                   PlaylistMutationCallback callback);
    void renamePlaylist (int64_t playlistId, juce::String newName,
                         PlaylistMutationCallback callback);
    void deletePlaylist (int64_t playlistId, PlaylistMutationCallback callback);
    void addTracksToPlaylist (int64_t playlistId, std::vector<int64_t> trackIds,
                              PlaylistMutationCallback callback);
    void addFilePathToPlaylist (int64_t playlistId, juce::String filePath,
                                PlaylistMutationCallback callback);
    void removePlaylistEntries (int64_t playlistId, std::vector<int64_t> entryIds,
                                PlaylistMutationCallback callback);
    void movePlaylistEntry (int64_t playlistId, int64_t entryId, int newZeroBasedIndex,
                            PlaylistMutationCallback callback);
    void appendHistoryEntryForFilePath (juce::String filePath,
                                        PlaylistMutationCallback callback);

    // -------------------------------------------------------------------------
    // Parsing utilities (static — run on Message Thread before dispatch)
    // -------------------------------------------------------------------------

    /// Parse a raw search string into a QueryParams struct.
    static QueryParams parseSearchString (const juce::String& raw);

    /// Map a Camelot key string (e.g. "8A") to key_index integer (0–23).
    /// Returns -1 if the string is not a valid Camelot notation.
    static int camelotKeyToIndex (const juce::String& key);

private:
    // -------------------------------------------------------------------------
    // juce::Thread entry point
    // -------------------------------------------------------------------------
    void run() override;

    // -------------------------------------------------------------------------
    // Background-thread helpers (called only from run())
    // -------------------------------------------------------------------------
    void executeQuery (sqlite3* bgDb, const QueryParams& params);

    /// Build the parameterised SQL for params.
    /// User-supplied values are emitted as ? and appended to the binding lists.
    struct SqlBind;
    juce::String buildSql (const QueryParams& params, std::vector<SqlBind>& binds);
    void enqueueOperation (std::function<void(sqlite3*)> operation);

    // -------------------------------------------------------------------------
    // Members
    // -------------------------------------------------------------------------
    TrackDatabase&        db;
    juce::File            dbFile;

    ResultCallback        resultCallback;

    std::mutex            pendingMutex;
    QueryParams           pendingParams;
    bool                  hasPending    = false;

    std::atomic<bool>     cancelFlag    { false };
    juce::WaitableEvent   wakeEvent;          ///< Auto-reset, wakes the thread
    juce::CriticalSection callbackLock;

    std::mutex operationMutex;
    std::deque<std::function<void(sqlite3*)>> operations;

    JUCE_DECLARE_WEAK_REFERENCEABLE (LibraryQueryThread)
};
