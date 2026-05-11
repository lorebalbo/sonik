#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <utility>

// Library-internal headers (module boundary: only DeckIdentifiers.h crosses
// into another feature)
#include "../LibraryQueryThread.h"   // LibraryTrackRow, QueryParams, DeckAwareFilterState
#include "../LibraryAnalysisService.h"
#include "../WatchFolderScanner.h"   // WatchFolderScanner::Listener
#include "Features/Deck/DeckIdentifiers.h"

#include "FilterBarMolecule.h"
#include "SidebarMolecule.h"
#include "TrackTableMolecule.h"

// TrackDatabase is available transitively via LibraryQueryThread.h.

/// Organism: top-level Library UI component.
/// Constructor takes exactly juce::ValueTree& rootState and TrackDatabase& db.
/// All deck state is observed exclusively via ValueTree::Listener.
class LibraryComponent final : public juce::Component,
                                public juce::ValueTree::Listener,
                                private juce::Timer,
                                public WatchFolderScanner::Listener
{
public:
    LibraryComponent (juce::ValueTree& rootState, TrackDatabase& db);
    ~LibraryComponent() override;

    void paint   (juce::Graphics& g) override;
    void resized () override;
    bool keyPressed (const juce::KeyPress& key) override;

    /// Register this component as a WatchFolderScanner listener (called by
    /// SonikApplication after the scanner is created).
    void setScanner (WatchFolderScanner& scanner);

    // WatchFolderScanner::Listener
    void scanProgressUpdate (int filesScanned, int total,
                              const juce::String& currentFile) override;
    void scanCompleted () override;
    void savePreparationListBeforeQuit (std::function<void(bool)> completion);

private:
    // juce::ValueTree::Listener
    void valueTreePropertyChanged (juce::ValueTree& tree,
                                   const juce::Identifier& property) override;
    void valueTreeChildAdded      (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved    (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged   (juce::ValueTree&) override {}

    // juce::Timer — debounce for deck state changes
    void timerCallback () override;

    void dispatchCurrentQuery (bool immediate = false);
    void rebuildDeckAwareFilter ();
    void updateResultBuffer (std::vector<LibraryTrackRow> rows);
    void updatePlayingTrack ();
    void refreshSidebarFolders ();
    void refreshSidebarPlaylists ();
    QueryParams buildQueryParams () const;

    void showContextMenu (int rowIndex, juce::Point<int> screenPos);
    void promptForPlaylistName (const juce::String& title,
                                const juce::String& initialName,
                                std::function<void(const juce::String&)> onSubmit);
    void showPlaylistHeaderMenu (juce::Point<int> screenPos);
    void showPlaylistMenu (int64_t playlistId, const juce::String& type, juce::Point<int> screenPos);
    void handlePlaylistCreate (const juce::String& name);
    void handlePlaylistRename (int64_t playlistId, const juce::String& name);
    void handlePlaylistMutationResult (bool ok, const juce::String& message, int64_t playlistId);
    void addSelectedRowsToPlaylist (int64_t playlistId);
    void addSelectedRowsToPreparation ();
    void addTrackPathToPlaylist (int64_t playlistId, const juce::String& type, const juce::String& filePath);
    void removeSelectedPlaylistEntries ();
    void movePlaylistEntry (int64_t entryId, int newRowIndex);
    void loadTrackToDeck (int rowIndex, const juce::String& deckId);
    void doLoadToDeck    (int rowIndex, const juce::String& deckId);
    void loadFocusedDeckTrack (int rowIndex);
    void analyzeTrack (int rowIndex);
    void setTrackRating  (int64_t trackId, int newRating);
    void incrementPlayCount (const juce::String& filePath);

    // ---- members -----------------------------------------------------------
    juce::ValueTree              rootState;
    TrackDatabase&               db;
    LibraryAnalysisService       analysisService;
    WatchFolderScanner*          scanner = nullptr;

    std::unique_ptr<LibraryQueryThread> queryThread;

    SidebarMolecule    sidebar;
    FilterBarMolecule  filterBar;
    TrackTableMolecule trackTable;
    juce::Label        scanProgressLabel;

    QueryParams          currentParams;
    DeckAwareFilterState deckFilter;
    juce::String         currentSidebarContext; // "collection" | "folder:PATH" | "playlist:ID"
    std::vector<PlaylistInfo> playlistCache;
    std::vector<int64_t>      preparationTrackIds;
    std::vector<int64_t>      pendingCreatePlaylistTrackIds;
    bool                 scanning = false;
    int                  activeAnalysisJobs = 0;

    // Play-count tracking (PRD-0034 AC-16): deckId → last filePath whose play was counted
    std::map<juce::String, juce::String> lastCountedPathPerDeck;
    std::map<juce::String, juce::String> lastPlaybackStatusPerDeck;

    // Deck-aware smart filters (PRD-0035)
    bool                 halfTimeEnabled   = false;
    bool                 showingEmptyState = false;
    juce::Label          emptyStateLabel;
    juce::TextButton     clearFiltersButton;
    std::unique_ptr<juce::PropertiesFile> filterPropsFile;
    void loadFilterState ();
    void saveFilterState ();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibraryComponent)
};
