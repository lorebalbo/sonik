#include "WatchFolderScanner.h"
#include <sqlite3.h>

// =============================================================================
// Internal helpers
// =============================================================================

namespace
{

constexpr const char* kSupportedExtensions[] = {
    ".mp3", ".flac", ".wav", ".aiff", ".aif", ".ogg", ".m4a", nullptr
};

bool isSupportedExtension (const juce::File& f)
{
    const auto ext = f.getFileExtension().toLowerCase();
    for (int i = 0; kSupportedExtensions[i] != nullptr; ++i)
        if (ext == kSupportedExtensions[i])
            return true;
    return false;
}

int countAudioFilesInDir (const juce::File& folder)
{
    int n = 0;
    for (const auto& entry :
         juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
        if (isSupportedExtension (entry.getFile()))
            ++n;
    return n;
}

/// Try a list of metadata keys (case-sensitive) and return the first non-empty value.
juce::String tryMetaKeys (const juce::StringPairArray& meta,
                           std::initializer_list<const char*> keys)
{
    for (const auto* k : keys)
    {
        const auto v = meta.getValue (k, {});
        if (v.isNotEmpty())
            return v;
    }
    return {};
}

} // namespace

// =============================================================================
// Construction / destruction
// =============================================================================

WatchFolderScanner::WatchFolderScanner (TrackDatabase& dbRef)
    : juce::Thread ("WatchFolderScanner")
    , db     (dbRef)
    , dbFile (dbRef.getDbFile())
{
}

WatchFolderScanner::~WatchFolderScanner()
{
    // stopThread() MUST be called before any member is destroyed.
    // It joins the background thread (no more callAsync queued after this).
    // Already-queued callAsync lambdas hold a WeakReference that becomes null
    // once masterReference is destroyed (below), so they are safe no-ops.
    stopThread (5000);
}

// =============================================================================
// Listener registration
// =============================================================================

void WatchFolderScanner::addListener    (Listener* l) { listeners.add (l);    }
void WatchFolderScanner::removeListener (Listener* l) { listeners.remove (l); }

// =============================================================================
// Scan control
// =============================================================================

void WatchFolderScanner::startScan()
{
    if (isThreadRunning())
        stopThread (2000);

    filesScannedAtomic.store (0);
    startThread (juce::Thread::Priority::low);
}

void WatchFolderScanner::cancelScan()
{
    stopThread (2000);
}

// =============================================================================
// Watched-folder management  (Message Thread — uses db.getDbHandle())
// =============================================================================

void WatchFolderScanner::addWatchedFolder (const juce::File& folder)
{
    auto* handle = db.getDbHandle();
    if (handle == nullptr) return;

    sqlite3_stmt* stmt = nullptr;
    const char*   sql  =
        "INSERT OR IGNORE INTO watched_folders (folder_path, last_scanned_at)"
        " VALUES (?, NULL);";

    if (sqlite3_prepare_v2 (handle, sql, -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text (stmt, 1,
                           folder.getFullPathName().toRawUTF8(), -1, SQLITE_TRANSIENT);
        sqlite3_step (stmt);
        sqlite3_finalize (stmt);
    }

    startScan();
}

void WatchFolderScanner::removeWatchedFolder (const juce::File& folder)
{
    auto* handle = db.getDbHandle();
    if (handle == nullptr) return;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2 (handle,
                             "DELETE FROM watched_folders WHERE folder_path = ?;",
                             -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text (stmt, 1,
                           folder.getFullPathName().toRawUTF8(), -1, SQLITE_TRANSIENT);
        sqlite3_step (stmt);
        sqlite3_finalize (stmt);
    }
}

juce::StringArray WatchFolderScanner::getWatchedFolders() const
{
    juce::StringArray result;
    auto* handle = db.getDbHandle();
    if (handle == nullptr) return result;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2 (handle,
                             "SELECT folder_path FROM watched_folders ORDER BY folder_path;",
                             -1, &stmt, nullptr) == SQLITE_OK)
    {
        while (sqlite3_step (stmt) == SQLITE_ROW)
            result.add (juce::String::fromUTF8 (
                reinterpret_cast<const char*> (sqlite3_column_text (stmt, 0))));
        sqlite3_finalize (stmt);
    }

    return result;
}

void WatchFolderScanner::rescanFolder (const juce::File& folder)
{
    auto* handle = db.getDbHandle();
    if (handle == nullptr) return;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2 (handle,
                             "UPDATE watched_folders SET last_scanned_at = NULL"
                             " WHERE folder_path = ?;",
                             -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text (stmt, 1,
                           folder.getFullPathName().toRawUTF8(), -1, SQLITE_TRANSIENT);
        sqlite3_step (stmt);
        sqlite3_finalize (stmt);
    }

    startScan();
}

void WatchFolderScanner::setReconciliationProgressCallback (std::function<void()> callback)
{
    std::lock_guard<std::mutex> lock (reconciliationCallbackMutex);
    reconciliationProgressCallback = std::move (callback);
}

// =============================================================================
// SHA-256 helper
// =============================================================================

juce::String WatchFolderScanner::computeContentHash (const juce::File& file)
{
    // CRITICAL: this MUST stay byte-for-byte identical to the engine-canonical
    // hash in AudioFileLoader::computeContentHash (and AudioFileImporter::
    // computeContentHash): MD5 of the first 64 KB + the 8-byte file size.
    //
    // The deck stamps every loaded track (and therefore every clip the recorder
    // captures) with that hash, and EPIC-0010 playback resolves a clip back to
    // its file via getFilePathForContentHash(), which looks the hash up in
    // library_tracks.content_hash. If the scanner wrote a DIFFERENT hash (it
    // previously used full-file SHA-256) the reverse lookup never matched and
    // recorded clips played silence. Do NOT "upgrade" this back to SHA-256.
    juce::FileInputStream stream (file);
    if (stream.failedToOpen())
        return {};

    constexpr int hashBytes = 65536;
    juce::MemoryBlock block;
    auto bytesToRead = juce::jmin (stream.getTotalLength(), static_cast<int64_t> (hashBytes));
    block.setSize (static_cast<size_t> (bytesToRead));
    stream.read (block.getData(), static_cast<int> (bytesToRead));

    int64_t fileSize = file.getSize();
    block.append (&fileSize, sizeof (fileSize));

    return juce::MD5 (block.getData(), block.getSize()).toHexString();
}

// =============================================================================
// Directory-walk helpers
// =============================================================================

bool WatchFolderScanner::isSupportedAudio (const juce::File& file)
{
    return isSupportedExtension (file);
}

int WatchFolderScanner::countAudioFiles (const juce::File& folder)
{
    return countAudioFilesInDir (folder);
}

// =============================================================================
// Async notifications (called from background thread)
// =============================================================================

void WatchFolderScanner::notifyProgress (int scanned, int total,
                                          const juce::String& currentFile)
{
    juce::WeakReference<WatchFolderScanner> weakThis (this);
    juce::MessageManager::callAsync ([weakThis, scanned, total, currentFile]()
    {
        if (auto* self = weakThis.get())
            self->listeners.call ([&] (Listener& l)
            {
                l.scanProgressUpdate (scanned, total, currentFile);
            });
    });
}

void WatchFolderScanner::notifyCompleted()
{
    juce::WeakReference<WatchFolderScanner> weakThis (this);
    juce::MessageManager::callAsync ([weakThis]()
    {
        if (auto* self = weakThis.get())
            self->listeners.call ([] (Listener& l) { l.scanCompleted(); });
    });
}

// =============================================================================
// juce::Thread entry point
// =============================================================================

void WatchFolderScanner::run()
{
    DBG ("WatchFolderScanner: background thread started");

    // Open a dedicated sqlite3 connection for this background thread.
    // NEVER reuse db.getDbHandle() here — that connection belongs to the
    // Message Thread and must not be accessed from any other thread.
    sqlite3* bgDb = nullptr;
    if (sqlite3_open (dbFile.getFullPathName().toRawUTF8(), &bgDb) != SQLITE_OK)
    {
        DBG ("WatchFolderScanner: failed to open background DB connection");
        return;
    }

    // Mirror the main connection's pragmas for consistency.
    sqlite3_exec (bgDb, "PRAGMA journal_mode=WAL;",     nullptr, nullptr, nullptr);
    sqlite3_exec (bgDb, "PRAGMA foreign_keys=ON;",      nullptr, nullptr, nullptr);
    sqlite3_exec (bgDb, "PRAGMA synchronous=NORMAL;",   nullptr, nullptr, nullptr);

    // -------------------------------------------------------------------------
    // Read watched folders (inside this thread's own connection)
    // -------------------------------------------------------------------------
    struct FolderEntry
    {
        juce::String path;
        int64_t      lastScannedAt { 0 }; ///< 0 means NULL (full scan required)
    };

    juce::Array<FolderEntry> folders;
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2 (bgDb,
                                "SELECT folder_path, last_scanned_at"
                                " FROM watched_folders;",
                                -1, &stmt, nullptr) == SQLITE_OK)
        {
            while (sqlite3_step (stmt) == SQLITE_ROW)
            {
                FolderEntry fe;
                fe.path = juce::String::fromUTF8 (
                    reinterpret_cast<const char*> (sqlite3_column_text (stmt, 0)));
                if (sqlite3_column_type (stmt, 1) != SQLITE_NULL)
                    fe.lastScannedAt = sqlite3_column_int64 (stmt, 1);
                folders.add (fe);
            }
            sqlite3_finalize (stmt);
        }
    }

    DBG ("WatchFolderScanner: found " + juce::String (folders.size()) + " watched folder(s)");

    // Pre-count total audio files across all valid folders so the UI can show
    // a meaningful total (skip folders that don't exist).
    int grandTotal = 0;
    for (const auto& fe : folders)
    {
        if (threadShouldExit()) break;
        const juce::File dir (fe.path);
        if (dir.isDirectory())
            grandTotal += countAudioFilesInDir (dir);
    }

    DBG ("WatchFolderScanner: " + juce::String (grandTotal) + " audio files to scan");

    // Create a single AudioFormatManager for the lifetime of this scan.
    // Owned exclusively by this thread.
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    filesScannedAtomic.store (0);

    // -------------------------------------------------------------------------
    // Scan each watched folder
    // -------------------------------------------------------------------------
    for (const auto& fe : folders)
    {
        if (threadShouldExit()) break;

        const juce::File dir (fe.path);
        if (!dir.isDirectory())
        {
            DBG ("WatchFolderScanner: watched folder does not exist, skipping: "
                 + fe.path);
            continue;
        }

        DBG ("WatchFolderScanner: scanning folder: " + fe.path);
        scanFolder (bgDb, formatManager, fe.path, fe.lastScannedAt, grandTotal);

        if (threadShouldExit()) break;

        // Update last_scanned_at only after a complete (non-cancelled) scan.
        const int64_t now = juce::Time::currentTimeMillis() / 1000;
        sqlite3_stmt* updStmt = nullptr;
        if (sqlite3_prepare_v2 (bgDb,
                                "UPDATE watched_folders SET last_scanned_at = ?"
                                " WHERE folder_path = ?;",
                                -1, &updStmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64 (updStmt, 1, now);
            sqlite3_bind_text  (updStmt, 2, fe.path.toRawUTF8(), -1, SQLITE_TRANSIENT);
            sqlite3_step (updStmt);
            sqlite3_finalize (updStmt);
        }
    }

    // -------------------------------------------------------------------------
    // Missing-file reconciliation pass (only when not cancelled)
    // -------------------------------------------------------------------------
    if (!threadShouldExit())
        runReconciliationPass (bgDb);

    sqlite3_close (bgDb);

    if (!threadShouldExit())
    {
        DBG ("WatchFolderScanner: scan complete, " + juce::String (filesScannedAtomic.load()) + " file(s) processed");
        notifyCompleted();
    }
    else
    {
        DBG ("WatchFolderScanner: scan cancelled");
    }
}

// =============================================================================
// scanFolder — walks one directory and ingests each audio file
// =============================================================================

void WatchFolderScanner::scanFolder (sqlite3* bgDb,
                                      juce::AudioFormatManager& fmt,
                                      const juce::String& folderPath,
                                      int64_t             lastScannedAt,
                                      int                 grandTotal)
{
    const juce::File dir (folderPath);

    // Wrap the entire folder in a single transaction for atomicity and
    // dramatically better performance on large directories.
    sqlite3_exec (bgDb, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);

    bool committed = false;

    for (const auto& entry :
         juce::RangedDirectoryIterator (dir, true, "*", juce::File::findFiles))
    {
        if (threadShouldExit()) break;

        const juce::File f = entry.getFile();
        if (!isSupportedExtension (f)) continue;

        ingestFile (bgDb, fmt, f, lastScannedAt);

        const int total = filesScannedAtomic.fetch_add (1) + 1;

        // Fire progress notification on the first file, every 10 files
        // thereafter, and on the very last file in the grand total.
        if (total == 1 || total % 10 == 0 || total == grandTotal)
            notifyProgress (total, grandTotal, f.getFileName());
    }

    if (threadShouldExit())
    {
        sqlite3_exec (bgDb, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
    else
    {
        sqlite3_exec (bgDb, "COMMIT;", nullptr, nullptr, nullptr);
        committed = true;
    }

    (void) committed; // suppress unused-variable warning in release builds
}

// =============================================================================
// ingestFile — hash, tag-extract, and upsert one audio file
// =============================================================================

void WatchFolderScanner::ingestFile (sqlite3* bgDb,
                                      juce::AudioFormatManager& fmt,
                                      const juce::File& file,
                                      int64_t           lastScannedAt)
{
    const auto filePath = file.getFullPathName();
    const int64_t now   = juce::Time::currentTimeMillis() / 1000;

    // ------------------------------------------------------------------
    // Incremental scan: skip files whose mtime predates last_scanned_at
    // ------------------------------------------------------------------
    if (lastScannedAt > 0)
    {
        const int64_t fileModSecs =
            file.getLastModificationTime().toMilliseconds() / 1000;
        if (fileModSecs <= lastScannedAt)
            return;
    }

    if (threadShouldExit()) return;

    // ------------------------------------------------------------------
    // Compute SHA-256 content hash
    // ------------------------------------------------------------------
    const juce::String hash = computeContentHash (file);
    if (hash.isEmpty())
    {
        DBG ("WatchFolderScanner: cannot read/hash file (skipping): " + filePath);
        return;
    }

    if (threadShouldExit()) return;

    // ------------------------------------------------------------------
    // Determine the required DB action
    // ------------------------------------------------------------------
    enum class Action { NoOp, Update, Insert };
    Action  action          = Action::Insert;
    int64_t existingRowId   = -1;

    // 1. Does a record with this file_path already exist?
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2 (bgDb,
                                "SELECT id, content_hash"
                                " FROM library_tracks WHERE file_path = ?;",
                                -1, &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text (stmt, 1, filePath.toRawUTF8(), -1, SQLITE_TRANSIENT);

            if (sqlite3_step (stmt) == SQLITE_ROW)
            {
                existingRowId        = sqlite3_column_int64 (stmt, 0);
                const auto storedHash = juce::String::fromUTF8 (
                    reinterpret_cast<const char*> (sqlite3_column_text (stmt, 1)));
                sqlite3_finalize (stmt);

                if (storedHash == hash)
                    return; // path and hash both match — no-op

                action = Action::Update; // path matches, hash changed
            }
            else
            {
                sqlite3_finalize (stmt);

                // 2. Does any record share this content hash?
                //    (Moved-file detection / true-duplicate guard)
                sqlite3_stmt* hashStmt = nullptr;
                if (sqlite3_prepare_v2 (bgDb,
                                        "SELECT id, file_path FROM library_tracks"
                                        " WHERE content_hash = ? LIMIT 1;",
                                        -1, &hashStmt, nullptr) == SQLITE_OK)
                {
                    sqlite3_bind_text (hashStmt, 1, hash.toRawUTF8(), -1, SQLITE_TRANSIENT);

                    if (sqlite3_step (hashStmt) == SQLITE_ROW)
                    {
                        const int64_t  otherId   = sqlite3_column_int64 (hashStmt, 0);
                        const auto     otherPath = juce::String::fromUTF8 (
                            reinterpret_cast<const char*> (
                                sqlite3_column_text (hashStmt, 1)));
                        sqlite3_finalize (hashStmt);

                        if (juce::File (otherPath).existsAsFile())
                        {
                            // True duplicate: the other physical file still exists.
                            // Keep the existing record; log the duplicate path.
                            DBG ("WatchFolderScanner: duplicate content (skipping):"
                                 " existing=" + otherPath
                                 + "  duplicate=" + filePath);
                            return;
                        }
                        else
                        {
                            // File was moved: update path + clear is_missing flag
                            // on the existing record. No full re-ingest needed
                            // because the content is identical.
                            sqlite3_stmt* mvStmt = nullptr;
                            if (sqlite3_prepare_v2 (bgDb,
                                                    "UPDATE library_tracks"
                                                    " SET file_path = ?, last_seen = ?,"
                                                    "     is_missing = 0"
                                                    " WHERE id = ?;",
                                                    -1, &mvStmt, nullptr) == SQLITE_OK)
                            {
                                sqlite3_bind_text  (mvStmt, 1,
                                    filePath.toRawUTF8(), -1, SQLITE_TRANSIENT);
                                sqlite3_bind_int64 (mvStmt, 2, now);
                                sqlite3_bind_int64 (mvStmt, 3, otherId);
                                sqlite3_step (mvStmt);
                                sqlite3_finalize (mvStmt);
                            }
                            return;
                        }
                    }
                    else
                    {
                        sqlite3_finalize (hashStmt);
                        // Genuinely new file — fall through to INSERT
                        action = Action::Insert;
                    }
                }
            }
        }
    }

    if (threadShouldExit()) return;

    // ------------------------------------------------------------------
    // Extract tag metadata via JUCE AudioFormatReader
    // ------------------------------------------------------------------
    juce::String title, artist, album;
    double   bpmValue   = 0.0;
    bool     hasBpm     = false;
    double   duration   = 0.0;
    bool     hasDuration = false;
    const int64_t fileSize = file.getSize();

    {
        std::unique_ptr<juce::AudioFormatReader> reader (fmt.createReaderFor (file));
        if (reader != nullptr)
        {
            const auto& meta = reader->metadataValues;

            title  = tryMetaKeys (meta, {"title",  "TITLE",  "TIT2", "INAM"});
            artist = tryMetaKeys (meta, {"artist", "ARTIST", "TPE1", "IART",
                                         "author", "AUTHOR"});
            album  = tryMetaKeys (meta, {"album",  "ALBUM",  "TALB", "IPRD"});

            const auto bpmStr = tryMetaKeys (meta, {"bpm",  "BPM",
                                                      "TBPM", "BeatsPerMinute"});
            if (bpmStr.isNotEmpty())
            {
                const double parsed = bpmStr.getDoubleValue();
                if (parsed > 0.0)
                {
                    bpmValue = parsed;
                    hasBpm   = true;
                }
            }

            if (reader->sampleRate > 0.0 && reader->lengthInSamples > 0)
            {
                duration    = static_cast<double> (reader->lengthInSamples)
                              / reader->sampleRate;
                hasDuration = true;
            }
        }
        // reader is destroyed here — file handle released
    }

    // Per-spec fallback: title = filename without extension when absent
    if (title.isEmpty())
        title = file.getFileNameWithoutExtension();

    // ------------------------------------------------------------------
    // Write to database
    // ------------------------------------------------------------------
    if (action == Action::Update)
    {
        // Preserve date_added, play_count, rating — only update content columns.
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "UPDATE library_tracks"
            " SET content_hash = ?, title = ?, artist = ?, album = ?,"
            "     bpm = ?, duration_seconds = ?, file_size_bytes = ?,"
            "     last_seen = ?, is_missing = 0"
            " WHERE id = ?;";

        if (sqlite3_prepare_v2 (bgDb, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text  (stmt, 1, hash.toRawUTF8(),  -1, SQLITE_TRANSIENT);
            sqlite3_bind_text  (stmt, 2, title.toRawUTF8(), -1, SQLITE_TRANSIENT);

            if (artist.isNotEmpty())
                sqlite3_bind_text (stmt, 3, artist.toRawUTF8(), -1, SQLITE_TRANSIENT);
            else
                sqlite3_bind_null (stmt, 3);

            if (album.isNotEmpty())
                sqlite3_bind_text (stmt, 4, album.toRawUTF8(), -1, SQLITE_TRANSIENT);
            else
                sqlite3_bind_null (stmt, 4);

            if (hasBpm)
                sqlite3_bind_double (stmt, 5, bpmValue);
            else
                sqlite3_bind_null   (stmt, 5);

            if (hasDuration)
                sqlite3_bind_double (stmt, 6, duration);
            else
                sqlite3_bind_null   (stmt, 6);

            sqlite3_bind_int64 (stmt, 7, fileSize);
            sqlite3_bind_int64 (stmt, 8, now);
            sqlite3_bind_int64 (stmt, 9, existingRowId);

            sqlite3_step (stmt);
            sqlite3_finalize (stmt);
        }
    }
    else // Action::Insert
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "INSERT INTO library_tracks"
            "  (file_path, content_hash, title, artist, album, bpm,"
            "   duration_seconds, file_size_bytes, date_added, last_seen, is_missing)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0);";

        if (sqlite3_prepare_v2 (bgDb, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text  (stmt, 1, filePath.toRawUTF8(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text  (stmt, 2, hash.toRawUTF8(),     -1, SQLITE_TRANSIENT);
            sqlite3_bind_text  (stmt, 3, title.toRawUTF8(),    -1, SQLITE_TRANSIENT);

            if (artist.isNotEmpty())
                sqlite3_bind_text (stmt, 4, artist.toRawUTF8(), -1, SQLITE_TRANSIENT);
            else
                sqlite3_bind_null (stmt, 4);

            if (album.isNotEmpty())
                sqlite3_bind_text (stmt, 5, album.toRawUTF8(), -1, SQLITE_TRANSIENT);
            else
                sqlite3_bind_null (stmt, 5);

            if (hasBpm)
                sqlite3_bind_double (stmt, 6, bpmValue);
            else
                sqlite3_bind_null   (stmt, 6);

            if (hasDuration)
                sqlite3_bind_double (stmt, 7, duration);
            else
                sqlite3_bind_null   (stmt, 7);

            sqlite3_bind_int64 (stmt, 8,  fileSize); // file_size_bytes
            sqlite3_bind_int64 (stmt, 9,  now);       // date_added
            sqlite3_bind_int64 (stmt, 10, now);       // last_seen

            sqlite3_step (stmt);
            sqlite3_finalize (stmt);
        }
    }
}

// =============================================================================
// runReconciliationPass — mark / restore missing files
// =============================================================================

void WatchFolderScanner::runReconciliationPass (sqlite3* bgDb)
{
    if (threadShouldExit()) return;

    constexpr int kBatchSize = 25;
    int           pendingFlips = 0;

    auto firePendingProgress = [this]
    {
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lock (reconciliationCallbackMutex);
            cb = reconciliationProgressCallback;
        }
        if (! cb)
            return;

        juce::WeakReference<WatchFolderScanner> weakThis (this);
        juce::MessageManager::callAsync ([weakThis, cb]()
        {
            if (weakThis.get() != nullptr)
                cb();
        });
    };

    // ---- Mark currently-present records as missing if the file has gone ----
    {
        struct Row { int64_t id; juce::String path; };
        juce::Array<Row> rows;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2 (bgDb,
                                "SELECT id, file_path FROM library_tracks"
                                " WHERE is_missing = 0;",
                                -1, &stmt, nullptr) == SQLITE_OK)
        {
            while (sqlite3_step (stmt) == SQLITE_ROW)
            {
                Row r;
                r.id   = sqlite3_column_int64 (stmt, 0);
                r.path = juce::String::fromUTF8 (
                    reinterpret_cast<const char*> (sqlite3_column_text (stmt, 1)));
                rows.add (r);
            }
            sqlite3_finalize (stmt);
        }

        for (const auto& row : rows)
        {
            if (threadShouldExit()) return;

            if (!juce::File (row.path).existsAsFile())
            {
                sqlite3_stmt* upd = nullptr;
                if (sqlite3_prepare_v2 (bgDb,
                                        "UPDATE library_tracks SET is_missing = 1"
                                        " WHERE id = ?;",
                                        -1, &upd, nullptr) == SQLITE_OK)
                {
                    sqlite3_bind_int64 (upd, 1, row.id);
                    sqlite3_step (upd);
                    sqlite3_finalize (upd);
                }

                if (++pendingFlips >= kBatchSize)
                {
                    firePendingProgress();
                    pendingFlips = 0;
                }
            }
        }
    }

    if (threadShouldExit()) return;

    // ---- Restore records that were missing but whose file has returned -----
    {
        struct Row { int64_t id; juce::String path; };
        juce::Array<Row> rows;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2 (bgDb,
                                "SELECT id, file_path FROM library_tracks"
                                " WHERE is_missing = 1;",
                                -1, &stmt, nullptr) == SQLITE_OK)
        {
            while (sqlite3_step (stmt) == SQLITE_ROW)
            {
                Row r;
                r.id   = sqlite3_column_int64 (stmt, 0);
                r.path = juce::String::fromUTF8 (
                    reinterpret_cast<const char*> (sqlite3_column_text (stmt, 1)));
                rows.add (r);
            }
            sqlite3_finalize (stmt);
        }

        for (const auto& row : rows)
        {
            if (threadShouldExit()) return;

            if (juce::File (row.path).existsAsFile())
            {
                sqlite3_stmt* upd = nullptr;
                if (sqlite3_prepare_v2 (bgDb,
                                        "UPDATE library_tracks SET is_missing = 0"
                                        " WHERE id = ?;",
                                        -1, &upd, nullptr) == SQLITE_OK)
                {
                    sqlite3_bind_int64 (upd, 1, row.id);
                    sqlite3_step (upd);
                    sqlite3_finalize (upd);
                }

                if (++pendingFlips >= kBatchSize)
                {
                    firePendingProgress();
                    pendingFlips = 0;
                }
            }
        }
    }

    // Always fire one final progress callback so listeners catch the tail.
    firePendingProgress();
}
