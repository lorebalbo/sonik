#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>
#include <juce_events/juce_events.h>
#include "Features/Deck/Database/TrackDatabase.h"

#include <functional>
#include <mutex>

struct sqlite3;

/// Background watch-folder scanner and library track ingestor (PRD-0031).
///
/// Runs exclusively on a juce::Thread. The Message Thread is never blocked.
/// The scanner opens a SEPARATE sqlite3 connection for all background-thread
/// writes; db.getDbHandle() is reserved for Message-Thread callers only.
///
/// Progress events and completion are dispatched to Listeners via
/// juce::MessageManager::callAsync(), so Listener callbacks always execute
/// on the Message Thread.
class WatchFolderScanner final : public juce::Thread
{
public:
    // -------------------------------------------------------------------------
    // Listener interface
    // -------------------------------------------------------------------------
    struct Listener
    {
        virtual ~Listener() = default;

        /// Called every N files to report progress. Always on Message Thread.
        virtual void scanProgressUpdate (int filesScanned,
                                         int totalFilesFound,
                                         const juce::String& currentFile) = 0;

        /// Called once when the scan (including reconciliation) completes.
        /// NOT called when the scan is cancelled. Always on Message Thread.
        virtual void scanCompleted() = 0;
    };

    // -------------------------------------------------------------------------
    // Construction / destruction
    // -------------------------------------------------------------------------
    explicit WatchFolderScanner (TrackDatabase& db);
    ~WatchFolderScanner() override;

    WatchFolderScanner (const WatchFolderScanner&)            = delete;
    WatchFolderScanner& operator= (const WatchFolderScanner&) = delete;

    // -------------------------------------------------------------------------
    // Listener registration (Message Thread)
    // -------------------------------------------------------------------------
    void addListener    (Listener* listener);
    void removeListener (Listener* listener);

    // -------------------------------------------------------------------------
    // Scan control (Message Thread)
    // -------------------------------------------------------------------------

    /// Start a background scan of all rows in watched_folders.
    /// If a scan is already running it is cancelled first.
    void startScan();

    /// Signal the background thread to stop cleanly (blocks up to 2 s).
    void cancelScan();

    // -------------------------------------------------------------------------
    // Watched-folder management (Message Thread — uses db.getDbHandle())
    // -------------------------------------------------------------------------

    /// Insert a row into watched_folders and trigger an immediate scan.
    void addWatchedFolder (const juce::File& folder);

    /// Delete the row from watched_folders.
    /// Existing library_tracks records are NOT removed.
    void removeWatchedFolder (const juce::File& folder);

    /// Return all folder paths currently stored in watched_folders.
    juce::StringArray getWatchedFolders() const;

    /// Reset last_scanned_at to NULL for this folder, forcing a full rescan,
    /// then trigger startScan().
    void rescanFolder (const juce::File& folder);

    /// Set a callback invoked on the Message Thread during the reconciliation
    /// pass — fired after each batch of N row updates and once on completion.
    /// LibraryComponent uses this to repaint affected rows progressively
    /// (PRD-0039 AC-05).
    void setReconciliationProgressCallback (std::function<void()> callback);

private:
    // -------------------------------------------------------------------------
    // juce::Thread entry point
    // -------------------------------------------------------------------------
    void run() override;

    // -------------------------------------------------------------------------
    // Background-thread helpers (called only from run())
    // -------------------------------------------------------------------------
    void scanFolder (sqlite3* bgDb,
                     juce::AudioFormatManager& fmt,
                     const juce::String& folderPath,
                     int64_t             lastScannedAt,
                     int                 grandTotal);

    void ingestFile (sqlite3* bgDb,
                     juce::AudioFormatManager& fmt,
                     const juce::File& file,
                     int64_t           lastScannedAt);

    void runReconciliationPass (sqlite3* bgDb);

    // -------------------------------------------------------------------------
    // Utilities
    // -------------------------------------------------------------------------
    static juce::String computeSHA256    (const juce::File& file);
    static int          countAudioFiles  (const juce::File& folder);
    static bool         isSupportedAudio (const juce::File& file);

    void notifyProgress (int scanned, int total, const juce::String& currentFile);
    void notifyCompleted();

    // -------------------------------------------------------------------------
    // Members
    // -------------------------------------------------------------------------
    TrackDatabase&               db;
    juce::File                   dbFile;          ///< Cached for background open
    juce::ListenerList<Listener> listeners;
    std::atomic<int>             filesScannedAtomic { 0 };

    std::mutex                    reconciliationCallbackMutex;
    std::function<void()>         reconciliationProgressCallback;

    JUCE_DECLARE_WEAK_REFERENCEABLE (WatchFolderScanner)
};
