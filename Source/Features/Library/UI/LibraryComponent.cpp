#include "LibraryComponent.h"
#include "LibraryPalette.h"
#include "LibraryStatusText.h"
#include <sqlite3.h>
#include <algorithm>

static constexpr int kSidebarWidth     = 200;
static constexpr int kFilterBarHeight  = 23; // Figma: 23 px row, no outer frame
static constexpr int kLibraryGap       = 12; // gap between filter bar / sidebar / table
static constexpr int kLibraryPadding   = 12; // outer frame padding
static constexpr int kLibraryMargin    = 8;  // gap between window edges and outer frame
static constexpr int kScanLabelHeight  = 16;

namespace
{
bool isAnalysisComplete (const juce::String& status)
{
    return status == "done" || status == "completed";
}

int canonicalKeyToCamelotIndex (int canonicalKey)
{
    if (canonicalKey < 0 || canonicalKey > 23)
        return -1;

    static constexpr int pitchToCamelotMajor[] = { 8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1 };
    static constexpr int pitchToCamelotMinor[] = { 5, 12, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10 };

    const int pitchClass = canonicalKey / 2;
    const bool isMajor = (canonicalKey % 2) == 0;
    const int number = isMajor ? pitchToCamelotMajor[pitchClass]
                               : pitchToCamelotMinor[pitchClass];

    return isMajor ? number + 11 : number - 1;
}

class LibraryPopupLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override
    {
        g.fillAll (LibraryPalette::surface());

        // JUCE owns the popup window chrome, so painting outside the menu bounds
        // is not reliable. The dithered 2px offset shadow is drawn as an
        // internal right/bottom band, preserving the 1-bit checker pattern.
        g.setColour (LibraryPalette::primary());
        for (int y = 2; y < height; ++y)
            for (int x = juce::jmax (0, width - 2); x < width; ++x)
                if (((x + y) & 1) == 0)
                    g.fillRect (x, y, 1, 1);

        for (int y = juce::jmax (0, height - 2); y < height; ++y)
            for (int x = 2; x < width; ++x)
                if (((x + y) & 1) == 0)
                    g.fillRect (x, y, 1, 1);
    }

    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area,
                            bool isSeparator, bool isActive, bool isHighlighted,
                            bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText,
                            const juce::Drawable* icon, const juce::Colour* textColour) override
    {
        juce::ignoreUnused (shortcutKeyText, icon, textColour);

        if (isSeparator)
        {
            g.setColour (LibraryPalette::primary());
            const auto y = area.getCentreY();
            for (int x = area.getX() + 8; x < area.getRight() - 8; x += 2)
                g.fillRect (x, y, 1, 1);
            return;
        }

        const auto itemArea = area.reduced (2, 0);
        const bool inverted = isActive && isHighlighted;
        g.setColour (inverted ? LibraryPalette::primary() : LibraryPalette::surface());
        g.fillRect (itemArea);

        auto textColourToUse = inverted ? LibraryPalette::surface() : LibraryPalette::primary();
        if (! isActive)
            textColourToUse = LibraryPalette::containerHighest();

        g.setColour (textColourToUse);
        g.setFont (juce::Font (LibraryPalette::bodyFont (12.0f)));

        auto textArea = itemArea.reduced (10, 0);
        if (isTicked)
            g.drawText ("*", textArea.removeFromLeft (12), juce::Justification::centred);
        else
            textArea.removeFromLeft (12);

        if (hasSubMenu)
        {
            auto arrowArea = textArea.removeFromRight (14);
            g.drawText (">", arrowArea, juce::Justification::centred);
        }

        g.drawText (text, textArea, juce::Justification::centredLeft, true);
    }
};

LibraryPopupLookAndFeel libraryPopupLnf;
}

// =============================================================================
// Construction / destruction
// =============================================================================

LibraryComponent::LibraryComponent (juce::ValueTree& rootStateRef,
                                     TrackDatabase&   dbRef,
                                     LibraryAnalysisQueue& queueRef)
    : rootState (rootStateRef)
    , db        (dbRef)
    , analysisQueue (queueRef)
{
    currentSidebarContext = "collection";

    // ---- Scan progress label ------------------------------------------------
    scanProgressLabel.setFont (juce::Font (LibraryPalette::bodyFont (11.0f)));
    scanProgressLabel.setColour (juce::Label::textColourId, LibraryPalette::primary());
    scanProgressLabel.setJustificationType (juce::Justification::centredLeft);
    scanProgressLabel.setVisible (false);
    addAndMakeVisible (scanProgressLabel);

    // ---- Child components ---------------------------------------------------
    addAndMakeVisible (sidebar);
    addAndMakeVisible (filterBar);
    addAndMakeVisible (trackTable);

    // ---- Sidebar callbacks --------------------------------------------------
    sidebar.onCollectionSelected = [this]
    {
        currentSidebarContext = "collection";
        dispatchCurrentQuery (true);
    };

    sidebar.onFolderSelected = [this] (const juce::String& path)
    {
        currentSidebarContext = "folder:" + path;
        dispatchCurrentQuery (true);
    };

    sidebar.onPlaylistSelected = [this] (int64_t id, juce::String type)
    {
        currentSidebarContext = (type == "preparation")
                                    ? "preparation"
                                    : "playlist:" + juce::String (id) + ":" + type;
        dispatchCurrentQuery (true);
    };

    sidebar.onPlaylistHeaderMenuRequested = [this] (juce::Point<int> screenPos)
    {
        showPlaylistHeaderMenu (screenPos);
    };

    sidebar.onPlaylistMenuRequested = [this] (int64_t playlistId, juce::String type, juce::Point<int> screenPos)
    {
        showPlaylistMenu (playlistId, type, screenPos);
    };

    sidebar.onPlaylistDoubleClicked = [this] (int64_t playlistId, juce::String type)
    {
        if (type == "normal")
        {
            for (const auto& playlist : playlistCache)
            {
                if (playlist.id == playlistId)
                {
                    sidebar.beginRenamePlaylist (playlistId, type, playlist.name);
                    break;
                }
            }
        }
    };

    sidebar.onPlaylistCreateSubmitted = [this] (const juce::String& name)
    {
        handlePlaylistCreate (name);
    };

    sidebar.onPlaylistRenameSubmitted = [this] (int64_t playlistId, const juce::String& name)
    {
        handlePlaylistRename (playlistId, name);
    };

    sidebar.onTrackPathDroppedOnPlaylist = [this] (int64_t playlistId, juce::String type, juce::String filePath)
    {
        addTrackPathToPlaylist (playlistId, type, filePath);
    };

    // ---- FilterBar callbacks ------------------------------------------------
    filterBar.onSearchChanged = [this] (const juce::String& text)
    {
        QueryParams np = LibraryQueryThread::parseSearchString (text);
        np.sortColumn      = currentParams.sortColumn;
        np.sortAscending   = currentParams.sortAscending;
        np.showMissingOnly = currentParams.showMissingOnly;
        currentParams      = std::move (np);
        dispatchCurrentQuery (true);
    };

    filterBar.onKeyMatchToggled = [this] (bool active)
    {
        deckFilter.keyMatchActive = active;
        rebuildDeckAwareFilter();
        dispatchCurrentQuery (true);
        saveFilterState();
    };

    filterBar.onBpmMatchToggled = [this] (bool active)
    {
        deckFilter.bpmMatchActive = active;
        rebuildDeckAwareFilter();
        dispatchCurrentQuery (true);
        saveFilterState();
    };

    filterBar.onBpmVisionChanged = [this] (double vision)
    {
        deckFilter.bpmVision = vision;
        rebuildDeckAwareFilter();
        if (deckFilter.bpmMatchActive)
            dispatchCurrentQuery (true);
        saveFilterState();
    };

    // NOTE: 1/2 BPM and MISSING-only toggles were removed from the FilterBar per
    // the Figma Library spec. The underlying state remains in this component so
    // backend filtering logic is preserved; both flags simply stay defaulted off.

    // ---- TrackTable callbacks -----------------------------------------------
    trackTable.onRowDoubleClicked = [this] (int rowIndex)
    {
        loadFocusedDeckTrack (rowIndex);
    };

    trackTable.onRowRightClicked = [this] (int rowIndex, juce::Point<int> pos)
    {
        showContextMenu (rowIndex,
                         trackTable.localPointToGlobal (pos));
    };

    trackTable.onRatingChanged = [this] (int rowIndex, int newRating)
    {
        if (rowIndex >= 0
            && rowIndex < static_cast<int> (trackTable.resultBuffer.size()))
        {
            setTrackRating (trackTable.resultBuffer[static_cast<size_t> (rowIndex)].id, newRating);
        }
    };

    trackTable.onSortChanged = [this] (int columnId, bool ascending)
    {
        juce::String col;
        switch (columnId)
        {
            case TrackTableMolecule::ColTitle:    col = "title";            break;
            case TrackTableMolecule::ColArtist:   col = "artist";           break;
            case TrackTableMolecule::ColAlbum:    col = "album";            break;
            case TrackTableMolecule::ColBpm:      col = "bpm";              break;
            case TrackTableMolecule::ColKey:      col = "key_index";        break;
            case TrackTableMolecule::ColDuration: col = "duration_seconds"; break;
            case TrackTableMolecule::ColRating:   col = "rating";           break;
            default:                               col = "date_added";       break;
        }
        currentParams.sortColumn    = col;
        currentParams.sortAscending = ascending;
        dispatchCurrentQuery (true);
    };

    trackTable.onPlaylistEntryDropped = [this] (int64_t entryId, int newRowIndex)
    {
        movePlaylistEntry (entryId, newRowIndex);
    };

    analysisQueue.setStatusCallback (
        [safeThis = juce::Component::SafePointer<LibraryComponent> (this)]
        (const LibraryAnalysisQueue::StatusUpdate& update)
        {
            if (safeThis != nullptr)
                safeThis->handleAnalysisQueueStatus (update);
        });

    // ---- Query thread -------------------------------------------------------
    queryThread = std::make_unique<LibraryQueryThread> (db);
    queryThread->setResultCallback ([this] (std::vector<LibraryTrackRow> rows)
    {
        updateResultBuffer (std::move (rows));
    });

    // ---- Filter state persistence -------------------------------------------
    {
        juce::PropertiesFile::Options opts;
        opts.applicationName     = "Sonik";
        opts.filenameSuffix      = ".settings";
        opts.folderName          = "Sonik";
        opts.osxLibrarySubFolder = "Application Support";
        filterPropsFile = std::make_unique<juce::PropertiesFile> (opts);
    }

    // ---- Empty state panel --------------------------------------------------
    emptyStateLabel.setFont (juce::Font (LibraryPalette::bodyFont (13.0f)));
    emptyStateLabel.setColour (juce::Label::textColourId, LibraryPalette::primary());
    emptyStateLabel.setJustificationType (juce::Justification::centred);
    emptyStateLabel.setText ("No tracks match the current deck filters.\n"
                              "Try widening BPM VISION or loading a different track.",
                              juce::dontSendNotification);
    emptyStateLabel.setVisible (false);
    addAndMakeVisible (emptyStateLabel);

    clearFiltersButton.setButtonText ("Clear Filters");
    clearFiltersButton.setColour (juce::TextButton::buttonColourId,   LibraryPalette::surface());
    clearFiltersButton.setColour (juce::TextButton::textColourOffId,  LibraryPalette::primary());
    clearFiltersButton.setColour (juce::TextButton::buttonOnColourId, LibraryPalette::primary());
    clearFiltersButton.setColour (juce::TextButton::textColourOnId,   LibraryPalette::surface());
    clearFiltersButton.onClick = [this]
    {
        deckFilter.keyMatchActive = false;
        deckFilter.bpmMatchActive = false;
        filterBar.setKeyMatchActive (false);
        filterBar.setBpmMatchActive (false);
        rebuildDeckAwareFilter();
        dispatchCurrentQuery (true);
        saveFilterState();
    };
    clearFiltersButton.setVisible (false);
    addAndMakeVisible (clearFiltersButton);

    // ---- ValueTree listening ------------------------------------------------
    rootState.addListener (this);

    // ---- Populate sidebar ---------------------------------------------------
    refreshSidebarFolders();
    refreshSidebarPlaylists();
    refreshMissingCount();

    // ---- Initial query (full library, date_added DESC) ----------------------
    currentParams.sortColumn    = "date_added";
    currentParams.sortAscending = false;
    dispatchCurrentQuery (true);

    // Load persisted filter state (must come after wiring all callbacks)
    loadFilterState();

    setWantsKeyboardFocus (true);
}

LibraryComponent::~LibraryComponent()
{
    analysisQueue.setStatusCallback (nullptr);
    rootState.removeListener (this);
    if (scanner != nullptr)
        scanner->removeListener (this);
    queryThread.reset();
}

// =============================================================================
// paint / resized / keyPressed
// =============================================================================

void LibraryComponent::paint (juce::Graphics& g)
{
    // Outer chassis fill (#e5e5e5) sits between the window edges and the
    // outer Library frame border, and shows through the 12 px gaps between
    // the filter bar / sidebar / table regions.
    g.fillAll (LibraryPalette::chassis());

    // Outer Library frame: 2px #2d2d2d border, inset by kLibraryMargin so it
    // is not flush with the window edges.
    g.setColour (LibraryPalette::primary());
    g.drawRect (getLocalBounds().reduced (kLibraryMargin), 2);
}

void LibraryComponent::resized()
{
    // 8 px breathing room between the window edges and the outer Library frame,
    // then 12 px of padding inside the frame border.
    auto b = getLocalBounds().reduced (kLibraryMargin)
                             .reduced (kLibraryPadding, kLibraryPadding);

    if (scanning || activeAnalysisJobs > 0)
    {
        scanProgressLabel.setVisible (true);
        scanProgressLabel.setBounds (b.removeFromTop (kScanLabelHeight));
    }
    else
    {
        scanProgressLabel.setVisible (false);
    }

    // Figma Library layout: filter bar (framed) at the top, then 12 px gap,
    // then sidebar (framed) on the left, 12 px gap, then table (framed) on the right.
    filterBar.setBounds (b.removeFromTop (kFilterBarHeight));
    b.removeFromTop (kLibraryGap);

    auto sidebarBounds = b.removeFromLeft (kSidebarWidth);
    sidebar.setBounds (sidebarBounds);
    b.removeFromLeft (kLibraryGap);

    if (showingEmptyState)
    {
        trackTable.setVisible (false);
        emptyStateLabel.setVisible (true);
        clearFiltersButton.setVisible (true);
        emptyStateLabel.setBounds (b.withTrimmedBottom (48));
        clearFiltersButton.setBounds (b.removeFromBottom (48)
                                        .withSizeKeepingCentre (120, 32));
    }
    else
    {
        emptyStateLabel.setVisible (false);
        clearFiltersButton.setVisible (false);
        trackTable.setVisible (true);
        trackTable.setBounds (b);
    }
}

bool LibraryComponent::keyPressed (const juce::KeyPress& key)
{
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'f')
    {
        filterBar.focusSearchBar();
        return true;
    }

    if (key == juce::KeyPress::returnKey)
    {
        const int row = trackTable.getSelectedRow();
        if (row >= 0) loadFocusedDeckTrack (row);
        return true;
    }

    if (key == juce::KeyPress::upKey)
    {
        const int row = juce::jmax (0, trackTable.getSelectedRow() - 1);
        trackTable.selectRow (row);
        trackTable.scrollToRow (row);
        return true;
    }

    if (key == juce::KeyPress::downKey)
    {
        const int maxRow = static_cast<int> (trackTable.resultBuffer.size()) - 1;
        const int row    = juce::jmin (maxRow, trackTable.getSelectedRow() + 1);
        if (row >= 0) { trackTable.selectRow (row); trackTable.scrollToRow (row); }
        return true;
    }

    return false;
}

// =============================================================================
// WatchFolderScanner::Listener
// =============================================================================

void LibraryComponent::setScanner (WatchFolderScanner& s)
{
    scanner = &s;
    s.addListener (this);

    // PRD-0039 AC-05: progressive UI updates as the reconciliation pass flips
    // is_missing on individual rows. The callback fires on the Message Thread.
    s.setReconciliationProgressCallback (
        [safeThis = juce::Component::SafePointer<LibraryComponent> (this)]
        {
            if (safeThis == nullptr)
                return;
            safeThis->refreshMissingCount();
            safeThis->dispatchCurrentQuery (true);
        });
}

void LibraryComponent::scanProgressUpdate (int filesScanned, int total,
                                            const juce::String& currentFile)
{
    scanning = true;
    const juce::String msg = "Scanning: "
        + juce::String (filesScanned) + " / " + juce::String (total)
        + "  " + currentFile.fromLastOccurrenceOf ("/", false, false).substring (0, 48);
    scanProgressLabel.setText (msg, juce::dontSendNotification);
    resized();
}

void LibraryComponent::scanCompleted()
{
    scanning = false;
    resized();
    refreshSidebarFolders();
    refreshSidebarPlaylists();
    refreshMissingCount();
    dispatchCurrentQuery (true);
}

void LibraryComponent::savePreparationListBeforeQuit (std::function<void(bool)> completionIn)
{
    if (queryThread == nullptr || preparationTrackIds.empty())
    {
        if (completionIn)
            completionIn (true);
        return;
    }

    auto quitCompletion = std::make_shared<std::function<void(bool)>> (std::move (completionIn));
    const int trackCount = static_cast<int> (preparationTrackIds.size());

    auto options = juce::MessageBoxOptions()
        .withIconType (juce::MessageBoxIconType::WarningIcon)
        .withTitle ("Preparation List")
        .withMessage ("Your Preparation List has " + juce::String (trackCount)
                      + " track(s) and will be lost. Export to a playlist before quitting?")
        .withButton ("Export to Playlist")
        .withButton ("Discard")
        .withButton ("Cancel");

    juce::AlertWindow::showAsync (options,
        [safeThis = juce::Component::SafePointer<LibraryComponent> (this), quitCompletion]
        (int result) mutable
        {
            if (safeThis == nullptr)
            {
                if (*quitCompletion)
                    (*quitCompletion) (false);
                return;
            }

            if (result == 1)
            {
                const auto idsToExport = safeThis->preparationTrackIds;
                safeThis->promptForPlaylistName ("Export Preparation List", "Preparation",
                    [safeThis, quitCompletion, idsToExport] (const juce::String& name) mutable
                    {
                        if (safeThis == nullptr || safeThis->queryThread == nullptr)
                        {
                            if (*quitCompletion)
                                (*quitCompletion) (false);
                            return;
                        }

                        safeThis->queryThread->createPlaylistWithTracks (name, idsToExport,
                            [safeThis, quitCompletion] (bool ok, juce::String message, int64_t) mutable
                            {
                                if (safeThis != nullptr && ok)
                                {
                                    safeThis->preparationTrackIds.clear();
                                    safeThis->sidebar.setPreparationCount (0);
                                    if (safeThis->currentSidebarContext == "preparation")
                                        safeThis->dispatchCurrentQuery (true);
                                }
                                else if (safeThis != nullptr)
                                {
                                    juce::AlertWindow::showMessageBoxAsync (
                                        juce::MessageBoxIconType::WarningIcon,
                                        "Playlist",
                                        message.isNotEmpty() ? message : "Playlist export failed.",
                                        "OK",
                                        safeThis);
                                }

                                if (*quitCompletion)
                                    (*quitCompletion) (ok);
                            });
                    },
                    [quitCompletion]
                    {
                        if (*quitCompletion)
                            (*quitCompletion) (false);
                    });
                return;
            }

            if (result == 2)
            {
                safeThis->preparationTrackIds.clear();
                safeThis->sidebar.setPreparationCount (0);
                if (safeThis->currentSidebarContext == "preparation")
                    safeThis->dispatchCurrentQuery (true);

                if (*quitCompletion)
                    (*quitCompletion) (true);
                return;
            }

            if (*quitCompletion)
                (*quitCompletion) (false);
        });
}

// =============================================================================
// MIDI control interface
// =============================================================================

void LibraryComponent::scrollLibraryUp()
{
    const int current = trackTable.getSelectedRow();
    const int total   = static_cast<int> (trackTable.resultBuffer.size());
    if (total <= 0) return;
    const int next = (current <= 0) ? 0 : current - 1;
    trackTable.selectRow (next);
    trackTable.scrollToRow (next);
}

void LibraryComponent::scrollLibraryDown()
{
    const int current = trackTable.getSelectedRow();
    const int total   = static_cast<int> (trackTable.resultBuffer.size());
    if (total <= 0) return;
    const int next = (current < 0) ? 0 : std::min (current + 1, total - 1);
    trackTable.selectRow (next);
    trackTable.scrollToRow (next);
}

void LibraryComponent::loadSelectedTrackToDeck (int deckIndex)
{
    const int rowIndex = trackTable.getSelectedRow();
    if (rowIndex < 0) return;

    auto decksNode = rootState.getChildWithName (IDs::Decks);
    if (deckIndex < 0 || deckIndex >= decksNode.getNumChildren()) return;

    const auto deckId = decksNode.getChild (deckIndex).getProperty (IDs::id).toString();
    if (deckId.isEmpty()) return;

    loadTrackToDeck (rowIndex, deckId);
}

// =============================================================================
// ValueTree::Listener
// =============================================================================

void LibraryComponent::valueTreePropertyChanged (juce::ValueTree&         tree,
                                                  const juce::Identifier& property)
{
    // We care about changes on Deck nodes or their immediate children
    // (BeatGrid, TrackMetadata, KeyInfo).
    const juce::ValueTree& parent = tree.getParent();
    const bool isDeckLevel  = (tree.getType() == IDs::Deck);
    const bool isDeckChild  = parent.isValid() && (parent.getType() == IDs::Deck);

    if (!isDeckLevel && !isDeckChild)
        return;

    // Playing-track indicator: update whenever file or loaded-path or status changes.
    if (property == IDs::filePath || property == IDs::playbackStatus || property == IDs::loadedFilePath)
        updatePlayingTrack();

    if (isDeckChild && property == IDs::analysisStatus
        && (tree.hasType (IDs::BeatGrid) || tree.hasType (IDs::KeyInfo))
        && isAnalysisComplete (tree.getProperty (IDs::analysisStatus).toString()))
    {
        rebuildDeckAwareFilter();
        dispatchCurrentQuery (true);
        return;
    }

    // Play-count increment: fire exactly once per new track the first time it plays (AC-16)
    if (isDeckLevel && property == IDs::playbackStatus)
    {
        const auto newStatus = tree.getProperty (IDs::playbackStatus).toString();
        const auto dId       = tree.getProperty (IDs::id).toString();
        const auto oldStatus = lastPlaybackStatusPerDeck[dId];
        lastPlaybackStatusPerDeck[dId] = newStatus;

        if (newStatus == "playing")
        {
            const auto loadedPath = tree.getProperty (IDs::loadedFilePath).toString();
            if (loadedPath.isNotEmpty() && loadedPath != lastCountedPathPerDeck[dId])
            {
                incrementPlayCount (loadedPath);
                lastCountedPathPerDeck[dId] = loadedPath;
            }

            if (loadedPath.isNotEmpty() && oldStatus != "playing")
            {
                queryThread->appendHistoryEntryForFilePath (loadedPath,
                    [safeThis = juce::Component::SafePointer<LibraryComponent> (this)]
                    (bool, juce::String, int64_t)
                    {
                        if (safeThis != nullptr)
                        {
                            safeThis->refreshSidebarPlaylists();
                            if (safeThis->currentSidebarContext.contains (":history"))
                                safeThis->dispatchCurrentQuery (true);
                        }
                    });
            }
        }
    }

    // Key change → immediate re-dispatch (no debounce)
    if (property == IDs::keyIndex)
    {
        rebuildDeckAwareFilter();
        if (deckFilter.keyMatchActive)
            dispatchCurrentQuery (true);
        return;
    }

    // New track loaded → immediate
    if (property == IDs::loadedFilePath)
    {
        rebuildDeckAwareFilter();
        if (deckFilter.bpmMatchActive || deckFilter.keyMatchActive)
            dispatchCurrentQuery (true);
        return;
    }

    // BPM / speed changes → 150 ms debounce
    if (property == IDs::speedMultiplier
        || property == IDs::bpm
        || property == IDs::playbackStatus)
    {
        rebuildDeckAwareFilter();
        if (deckFilter.bpmMatchActive || deckFilter.keyMatchActive)
            dispatchCurrentQuery (false); // debounced (150 ms)
    }
}

// =============================================================================
// Timer — debounce for deck state changes
// =============================================================================

void LibraryComponent::timerCallback()
{
    stopTimer();
    queryThread->dispatchQuery (buildQueryParams());
}

// =============================================================================
// Private helpers
// =============================================================================

void LibraryComponent::dispatchCurrentQuery (bool immediate)
{
    if (immediate)
    {
        stopTimer();
        queryThread->dispatchQuery (buildQueryParams());
    }
    else
    {
        startTimer (150);
    }
}

void LibraryComponent::rebuildDeckAwareFilter()
{
    deckFilter.bpmWindows.clear();
    deckFilter.compatibleKeyIndices.clear();

    auto decksNode = rootState.getChildWithName (IDs::Decks);
    for (int i = 0; i < decksNode.getNumChildren(); ++i)
    {
        auto deckNode  = decksNode.getChild (i);
        auto beatGrid  = deckNode.getChildWithName (IDs::BeatGrid);
        auto keyInfo   = deckNode.getChildWithName (IDs::KeyInfo);
        auto trackMeta = deckNode.getChildWithName (IDs::TrackMetadata);

        // Skip decks with no track loaded
        if (!trackMeta.isValid()
            || trackMeta.getProperty (IDs::filePath).toString().isEmpty())
            continue;

        // BPM window: effectiveBpm = storedBpm * speedMultiplier
        // Only when BeatGrid analysis is completed (AC-3.2)
        if (deckFilter.bpmMatchActive)
        {
            const juce::String bgStatus = beatGrid.isValid()
                ? beatGrid.getProperty (IDs::analysisStatus).toString()
                : juce::String{};

            if (isAnalysisComplete (bgStatus))
            {
                const double storedBpm = static_cast<double> (
                    beatGrid.getProperty (IDs::bpm));
                const double speed = static_cast<double> (
                    deckNode.getProperty (IDs::speedMultiplier, 1.0));
                const double effective = storedBpm * speed;

                if (effective > 0.0)
                {
                    const double v = deckFilter.bpmVision;
                    deckFilter.bpmWindows.add ({ effective - v, effective + v });

                    if (halfTimeEnabled)
                    {
                        deckFilter.bpmWindows.add ({ effective * 0.5 - v, effective * 0.5 + v });
                        deckFilter.bpmWindows.add ({ effective * 2.0 - v, effective * 2.0 + v });
                    }
                }
            }
        }

        // Key compatibility: Camelot Wheel adjacency (fixed boundary wrapping)
        // Only when KeyInfo analysis is completed (AC-3.2)
        if (deckFilter.keyMatchActive && keyInfo.isValid())
        {
            const juce::String kiStatus =
                keyInfo.getProperty (IDs::analysisStatus).toString();

            if (isAnalysisComplete (kiStatus))
            {
                const int canonicalKey = static_cast<int> (
                    keyInfo.getProperty (IDs::keyIndex, -1));
                const int ki = canonicalKeyToCamelotIndex (canonicalKey);

                if (ki >= 0 && ki < 24)
                {
                    const int letter = ki / 12;   // 0 = A ring, 1 = B ring
                    const int num    = ki % 12;   // 0-11

                    deckFilter.compatibleKeyIndices.addIfNotAlreadyThere (ki);
                    deckFilter.compatibleKeyIndices.addIfNotAlreadyThere ((num + 1)  % 12 + letter * 12); // CW
                    deckFilter.compatibleKeyIndices.addIfNotAlreadyThere ((num + 11) % 12 + letter * 12); // CCW
                    deckFilter.compatibleKeyIndices.addIfNotAlreadyThere (ki < 12 ? ki + 12 : ki - 12);  // relative
                }
            }
        }
    }

    // Compute suspended state and update FilterBar visual
    const bool keySuspended = deckFilter.keyMatchActive
                               && deckFilter.compatibleKeyIndices.isEmpty();
    const bool bpmSuspended = deckFilter.bpmMatchActive
                               && deckFilter.bpmWindows.isEmpty();
    filterBar.setSuspended (keySuspended || bpmSuspended);
}

void LibraryComponent::updatePlayingTrack()
{
    juce::String playingPath;

    auto decksNode = rootState.getChildWithName (IDs::Decks);
    for (int i = 0; i < decksNode.getNumChildren(); ++i)
    {
        auto deckNode = decksNode.getChild (i);
        if (deckNode.getProperty (IDs::playbackStatus).toString() == "playing")
        {
            auto meta = deckNode.getChildWithName (IDs::TrackMetadata);
            if (meta.isValid())
            {
                playingPath = meta.getProperty (IDs::filePath).toString();
                break;
            }
        }
    }

    trackTable.setPlayingTrack (playingPath);
}

void LibraryComponent::updateResultBuffer (std::vector<LibraryTrackRow> rows)
{
    updatePlayingTrack();
    trackTable.setPlaylistReorderEnabled (currentSidebarContext.contains (":normal"));
    trackTable.resultBuffer = std::move (rows);

    const bool filtersActive = deckFilter.keyMatchActive || deckFilter.bpmMatchActive;
    const bool nowEmpty      = trackTable.resultBuffer.empty() && filtersActive;
    if (nowEmpty != showingEmptyState)
    {
        showingEmptyState = nowEmpty;
        resized();
    }

    trackTable.updateContent();
}

void LibraryComponent::refreshSidebarFolders()
{
    auto*  handle = db.getDbHandle();
    juce::StringArray paths;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2 (handle,
                             "SELECT folder_path FROM watched_folders ORDER BY folder_path",
                             -1, &stmt, nullptr) == SQLITE_OK)
    {
        while (sqlite3_step (stmt) == SQLITE_ROW)
        {
            const auto* txt = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 0));
            if (txt) paths.add (juce::String::fromUTF8 (txt));
        }
        sqlite3_finalize (stmt);
    }

    sidebar.setFolders (paths);
}

void LibraryComponent::refreshSidebarPlaylists()
{
    if (queryThread == nullptr)
        return;

    queryThread->requestPlaylists (
        [safeThis = juce::Component::SafePointer<LibraryComponent> (this)]
        (std::vector<PlaylistInfo> playlists)
        {
            if (safeThis == nullptr)
                return;

            safeThis->playlistCache = std::move (playlists);
            safeThis->sidebar.setPreparationCount (static_cast<int> (safeThis->preparationTrackIds.size()));
            safeThis->sidebar.setPlaylists (safeThis->playlistCache);
        });
}

void LibraryComponent::refreshMissingCount()
{
    if (queryThread == nullptr)
        return;

    queryThread->countMissingTracks (
        [safeThis = juce::Component::SafePointer<LibraryComponent> (this)] (int count)
        {
            if (safeThis != nullptr)
                safeThis->sidebar.setMissingCount (count);
        });
}

QueryParams LibraryComponent::buildQueryParams() const
{
    QueryParams p  = currentParams;
    p.deckFilter   = deckFilter;

    if (currentSidebarContext.startsWith ("folder:"))
    {
        p.folderPath = currentSidebarContext.fromFirstOccurrenceOf ("folder:", false, false);
    }
    else if (currentSidebarContext == "preparation")
    {
        p.playlistType = "preparation";
        p.preparationTrackIds = preparationTrackIds;
    }
    else if (currentSidebarContext.startsWith ("playlist:"))
    {
        const auto tail = currentSidebarContext.fromFirstOccurrenceOf ("playlist:", false, false);
        p.playlistId = tail.upToFirstOccurrenceOf (":", false, false).getLargeIntValue();
        p.playlistType = tail.fromFirstOccurrenceOf (":", false, false);
    }

    return p;
}

// ---- Track loading ---------------------------------------------------------

void LibraryComponent::loadFocusedDeckTrack (int rowIndex)
{
    auto decksNode = rootState.getChildWithName (IDs::Decks);

    // Prefer the first deck (by index) whose loadedFilePath is empty (AC-08)
    for (int i = 0; i < decksNode.getNumChildren(); ++i)
    {
        auto d = decksNode.getChild (i);
        if (d.getProperty (IDs::loadedFilePath).toString().isEmpty())
        {
            loadTrackToDeck (rowIndex, d.getProperty (IDs::id).toString());
            return;
        }
    }

    // Fallback: deck at index 0
    if (decksNode.getNumChildren() > 0)
    {
        const auto fallbackId = decksNode.getChild (0).getProperty (IDs::id).toString();
        loadTrackToDeck (rowIndex, fallbackId.isNotEmpty() ? fallbackId : "A");
    }
}

void LibraryComponent::loadTrackToDeck (int rowIndex, const juce::String& deckId)
{
    if (rowIndex < 0 || rowIndex >= static_cast<int> (trackTable.resultBuffer.size()))
        return;

    // File existence and format checks are the deck's responsibility (AC-09/AC-13 handled in DeckShellComponent)
    doLoadToDeck (rowIndex, deckId);
}

void LibraryComponent::doLoadToDeck (int rowIndex, const juce::String& deckId)
{
    if (rowIndex < 0 || rowIndex >= static_cast<int> (trackTable.resultBuffer.size()))
        return;

    const auto& row       = trackTable.resultBuffer[static_cast<size_t> (rowIndex)];
    auto        decksNode = rootState.getChildWithName (IDs::Decks);

    for (int i = 0; i < decksNode.getNumChildren(); ++i)
    {
        auto deckNode = decksNode.getChild (i);
        if (deckNode.getProperty (IDs::id).toString() == deckId)
        {
            // Single write: pendingLoadPath. DeckShellComponent handles everything else.
            deckNode.setProperty (IDs::pendingLoadPath, row.filePath, nullptr);

            // Mark as played this session for the session indicator column
            trackTable.playedThisSession.insert (row.filePath);
            trackTable.repaint();
            break;
        }
    }
}

// ---- Context menu ----------------------------------------------------------

void LibraryComponent::showContextMenu (int rowIndex, juce::Point<int> screenPos)
{
    if (rowIndex < 0 || rowIndex >= static_cast<int> (trackTable.resultBuffer.size()))
        return;

    auto selectedRows = trackTable.getSelectedRows();
    if (selectedRows.empty()
        || std::find (selectedRows.begin(), selectedRows.end(), rowIndex) == selectedRows.end())
        selectedRows = { rowIndex };

    std::sort (selectedRows.begin(), selectedRows.end());
    selectedRows.erase (std::unique (selectedRows.begin(), selectedRows.end()), selectedRows.end());

    std::vector<LibraryTrackRow> rows;
    rows.reserve (selectedRows.size());
    for (auto selectedRow : selectedRows)
        if (selectedRow >= 0 && selectedRow < static_cast<int> (trackTable.resultBuffer.size()))
            rows.push_back (trackTable.resultBuffer[static_cast<size_t> (selectedRow)]);

    if (rows.empty())
        return;

    constexpr int kRelocate = 10;
    constexpr int kRemove = 11;
    constexpr int kAnalyze = 12;
    constexpr int kForceAnalyze = 13;
    constexpr int kSeparateStems = 14;
    constexpr int kPlaylistPreparation = 1001;
    constexpr int kPlaylistNew = 1002;
    constexpr int kPlaylistBase = 2000;
    constexpr int kRatingBase = 3000;
    constexpr int kDeckBase = 4000;

    auto menu = std::make_shared<juce::PopupMenu>();
    menu->setLookAndFeel (&libraryPopupLnf);

    std::vector<int64_t> playlistIds;
    auto addPlaylistSubMenu = [&]
    {
        juce::PopupMenu playlistMenu;
        playlistMenu.setLookAndFeel (&libraryPopupLnf);
        playlistMenu.addItem (kPlaylistPreparation, "Preparation List");

        playlistIds.clear();
        for (const auto& playlist : playlistCache)
        {
            if (! playlist.isNormal())
                continue;

            const int itemId = kPlaylistBase + static_cast<int> (playlistIds.size());
            playlistIds.push_back (playlist.id);
            playlistMenu.addItem (itemId, playlist.name);
        }

        playlistMenu.addSeparator();
        playlistMenu.addItem (kPlaylistNew, "New Playlist...");
        menu->addSubMenu ("Add to Playlist...", playlistMenu);
    };

    auto addRatingSubMenu = [&]
    {
        juce::PopupMenu ratingMenu;
        ratingMenu.setLookAndFeel (&libraryPopupLnf);
        for (int rating = 1; rating <= 5; ++rating)
            ratingMenu.addItem (kRatingBase + rating,
                                juce::String (rating) + (rating == 1 ? " Star" : " Stars"));
        menu->addSubMenu ("Rate...", ratingMenu);
    };

    std::vector<juce::String> deckIds;
    auto decksNode = rootState.getChildWithName (IDs::Decks);
    for (int i = 0; i < decksNode.getNumChildren(); ++i)
        deckIds.push_back (decksNode.getChild (i).getProperty (IDs::id).toString());

    if (rows.size() == 1 && rows.front().isMissing != 0)
    {
        // PRD-0039 AC-26 supersedes PRD-0038 AC-11: missing rows get Relocate
        // and Remove at the TOP, then the standard single-row items below.
        // "Load to Deck N" entries are shown but disabled — loading a missing
        // file is invalid until the row is relocated.
        const auto& row = rows.front();
        menu->addItem (kRelocate, "Relocate File...");
        menu->addItem (kRemove,   "Remove from Library");
        menu->addSeparator();

        for (int i = 0; i < static_cast<int> (deckIds.size()); ++i)
            menu->addItem (kDeckBase + i, "Load to Deck " + juce::String (i + 1), false);

        menu->addSeparator();
        const bool analyzed = isRowAnalyzed (row);
        menu->addItem (kAnalyze, "Analyze Track", ! analyzed);
        if (analyzed)
            menu->addItem (kForceAnalyze, "Force Re-analyze");

        const bool stemsExist = row.contentHash.isNotEmpty() && db.hasStemRecord (row.contentHash);
        menu->addItem (kSeparateStems, "Separate Stems", ! stemsExist);
        addPlaylistSubMenu();
        addRatingSubMenu();
    }
    else if (rows.size() == 1)
    {
        const auto& row = rows.front();
        for (int i = 0; i < static_cast<int> (deckIds.size()); ++i)
            menu->addItem (kDeckBase + i, "Load to Deck " + juce::String (i + 1));

        menu->addSeparator();
        const bool analyzed = isRowAnalyzed (row);
        menu->addItem (kAnalyze, "Analyze Track", ! analyzed);
        if (analyzed)
            menu->addItem (kForceAnalyze, "Force Re-analyze");

        const bool stemsExist = row.contentHash.isNotEmpty() && db.hasStemRecord (row.contentHash);
        menu->addItem (kSeparateStems, "Separate Stems", ! stemsExist);
        addPlaylistSubMenu();
        addRatingSubMenu();
        menu->addSeparator();
        menu->addItem (kRemove, "Remove from Library");
    }
    else
    {
        const int count = static_cast<int> (rows.size());
        const bool allAnalyzed = std::all_of (rows.begin(), rows.end(), isRowAnalyzed);
        menu->addItem (kAnalyze, "Analyze Tracks (" + juce::String (count) + ")", ! allAnalyzed);
        if (allAnalyzed)
            menu->addItem (kForceAnalyze, "Force Re-analyze (" + juce::String (count) + ")");

        menu->addItem (kSeparateStems, "Separate Stems (" + juce::String (count) + ")");
        addPlaylistSubMenu();
        addRatingSubMenu();
        menu->addSeparator();
        menu->addItem (kRemove, "Remove from Library (" + juce::String (count) + ")");
    }

    const auto targetArea = juce::Rectangle<int> { screenPos.x, screenPos.y, 1, 1 };
    juce::Component::SafePointer<LibraryComponent> safeThis (this);

    menu->showMenuAsync (
        juce::PopupMenu::Options{}.withTargetScreenArea (targetArea),
        [safeThis, menu, rows = std::move (rows), rowIndex, deckIds = std::move (deckIds),
         playlistIds = std::move (playlistIds)] (int choice) mutable
        {
            menu->setLookAndFeel (nullptr);
            if (safeThis == nullptr || choice == 0)
                return;

            if (choice >= kDeckBase && choice < kDeckBase + static_cast<int> (deckIds.size()))
            {
                safeThis->loadTrackToDeck (rowIndex, deckIds[static_cast<size_t> (choice - kDeckBase)]);
                return;
            }

            if (choice == kAnalyze)
            {
                safeThis->queueAnalysisRows (rows, false);
                return;
            }

            if (choice == kForceAnalyze)
            {
                safeThis->queueAnalysisRows (rows, true);
                return;
            }

            if (choice == kSeparateStems)
            {
                safeThis->queueStemRows (rows);
                return;
            }

            if (choice == kRelocate && rows.size() == 1)
            {
                safeThis->relocateTrackFile (rows.front().id, rows.front().filePath);
                return;
            }

            if (choice == kRemove)
            {
                std::vector<int64_t> ids;
                ids.reserve (rows.size());
                for (const auto& row : rows)
                    ids.push_back (row.id);
                safeThis->removeTracksFromLibrary (ids);
                return;
            }

            if (choice == kPlaylistPreparation)
            {
                for (const auto& row : rows)
                    safeThis->preparationTrackIds.push_back (row.id);
                safeThis->sidebar.setPreparationCount (static_cast<int> (safeThis->preparationTrackIds.size()));
                if (safeThis->currentSidebarContext == "preparation")
                    safeThis->dispatchCurrentQuery (true);
                return;
            }

            if (choice == kPlaylistNew)
            {
                safeThis->pendingCreatePlaylistTrackIds.clear();
                for (const auto& row : rows)
                    safeThis->pendingCreatePlaylistTrackIds.push_back (row.id);
                safeThis->sidebar.beginCreatePlaylist();
                return;
            }

            if (choice >= kPlaylistBase && choice < kPlaylistBase + static_cast<int> (playlistIds.size()))
            {
                if (safeThis->queryThread == nullptr)
                    return;

                std::vector<int64_t> ids;
                ids.reserve (rows.size());
                for (const auto& row : rows)
                    ids.push_back (row.id);

                const auto playlistId = playlistIds[static_cast<size_t> (choice - kPlaylistBase)];
                safeThis->queryThread->addTracksToPlaylist (playlistId, std::move (ids),
                    [safeThis] (bool ok, juce::String message, int64_t id)
                    {
                        if (safeThis != nullptr)
                            safeThis->handlePlaylistMutationResult (ok, message, id);
                    });
                return;
            }

            if (choice > kRatingBase && choice <= kRatingBase + 5)
                safeThis->setTrackRatingForRows (rows, choice - kRatingBase);
        });
}

bool LibraryComponent::isRowAnalyzed (const LibraryTrackRow& row) noexcept
{
    return row.bpm > 0.0 && row.keyIndex >= 0;
}

juce::String LibraryComponent::statusTextForUpdate (const LibraryAnalysisQueue::StatusUpdate& update)
{
    return SonikLibraryUi::statusTextForUpdate (update);
}

void LibraryComponent::handleAnalysisQueueStatus (const LibraryAnalysisQueue::StatusUpdate& update)
{
    trackTable.setRowStatus (update.trackId, statusTextForUpdate (update));
    trackTable.updateContent();

    const bool terminal = update.status == LibraryAnalysisQueue::JobStatus::Complete
                       || update.status == LibraryAnalysisQueue::JobStatus::Failed;
    if (terminal)
    {
        activeAnalysisJobs = juce::jmax (0, activeAnalysisJobs - 1);
        if (update.kind == LibraryAnalysisQueue::JobKind::Analysis
            && update.status == LibraryAnalysisQueue::JobStatus::Complete)
            dispatchCurrentQuery (true);
    }

    if (activeAnalysisJobs > 0)
        scanProgressLabel.setText ("Analysis Queue: " + juce::String (activeAnalysisJobs) + " job(s)",
                                   juce::dontSendNotification);

    resized();
}

void LibraryComponent::queueAnalysisRows (const std::vector<LibraryTrackRow>& rows, bool force)
{
    if (rows.empty())
        return;

    for (const auto& row : rows)
    {
        if (force)
            clearAnalysisCache (row);

        trackTable.setRowStatus (row.id, "Queued");
        analysisQueue.enqueueAnalysis (row.id, row.filePath, row.contentHash, true);
        ++activeAnalysisJobs;
    }

    scanProgressLabel.setText ("Analysis Queue: " + juce::String (activeAnalysisJobs) + " job(s)",
                               juce::dontSendNotification);
    resized();
}

void LibraryComponent::queueStemRows (const std::vector<LibraryTrackRow>& rows)
{
    if (rows.empty())
        return;

    for (const auto& row : rows)
    {
        trackTable.setRowStatus (row.id, "Queued (Stems)");
        analysisQueue.enqueueStemSeparation (row.id, row.filePath, row.contentHash, true);
        ++activeAnalysisJobs;
    }

    scanProgressLabel.setText ("Analysis Queue: " + juce::String (activeAnalysisJobs) + " job(s)",
                               juce::dontSendNotification);
    resized();
}

void LibraryComponent::relocateTrackFile (int64_t trackId, const juce::String& currentPath)
{
    // PRD-0039 AC-15: no file format filter is applied — AudioFileLoader
    // validates the format at dispatch time.
    auto chooser = std::make_shared<juce::FileChooser> (
        "Choose replacement file",
        juce::File{},
        juce::String());

    chooser->launchAsync (
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis = juce::Component::SafePointer<LibraryComponent> (this), trackId, currentPath, chooser]
        (const juce::FileChooser& fc)
        {
            if (safeThis == nullptr)
                return;

            const auto result = fc.getResult();
            if (! result.existsAsFile())
                return;

            const auto newPath = result.getFullPathName();
            auto* handle = safeThis->db.getDbHandle();

            // PRD-0039 AC-16 / AC-33: deduplication check against other rows.
            {
                sqlite3_stmt* dup = nullptr;
                bool isDuplicate = false;
                if (sqlite3_prepare_v2 (handle,
                        "SELECT 1 FROM library_tracks WHERE file_path=? AND id<>? LIMIT 1;",
                        -1, &dup, nullptr) == SQLITE_OK)
                {
                    sqlite3_bind_text  (dup, 1, newPath.toRawUTF8(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64 (dup, 2, trackId);
                    isDuplicate = (sqlite3_step (dup) == SQLITE_ROW);
                    sqlite3_finalize (dup);
                }

                if (isDuplicate)
                {
                    juce::AlertWindow::showMessageBoxAsync (
                        juce::MessageBoxIconType::WarningIcon,
                        "Relocate Track",
                        "This file is already in your library.",
                        "OK",
                        safeThis);
                    return;
                }
            }

            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2 (handle,
                    "UPDATE library_tracks SET file_path=?, is_missing=0 WHERE id=?",
                    -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text  (stmt, 1, newPath.toRawUTF8(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64 (stmt, 2, trackId);
                sqlite3_step (stmt);
                sqlite3_finalize (stmt);
            }

            juce::ignoreUnused (currentPath);
            safeThis->refreshMissingCount();
            safeThis->dispatchCurrentQuery (true);
        });
}

void LibraryComponent::removeTracksFromLibrary (const std::vector<int64_t>& trackIds)
{
    if (trackIds.empty())
        return;

    // PRD-0039 AC-21 / AC-22: when the removal includes any missing rows, show
    // a nested confirmation step before destructively deleting. This action
    // also wipes playlist_tracks entries (AC-23), so the warning is required.
    bool anyMissing = false;
    for (const auto& row : trackTable.resultBuffer)
        if (std::find (trackIds.begin(), trackIds.end(), row.id) != trackIds.end()
            && row.isMissing != 0)
        {
            anyMissing = true;
            break;
        }

    auto performDeletion = [this, trackIds]
    {
        auto* handle = db.getDbHandle();
        bool ok = sqlite3_exec (handle, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK;

        sqlite3_stmt* deletePlaylist = nullptr;
        sqlite3_stmt* deleteTrack = nullptr;

        ok = ok && sqlite3_prepare_v2 (handle,
                                       "DELETE FROM playlist_tracks WHERE track_id=?",
                                       -1, &deletePlaylist, nullptr) == SQLITE_OK;
        ok = ok && sqlite3_prepare_v2 (handle,
                                       "DELETE FROM library_tracks WHERE id=?",
                                       -1, &deleteTrack, nullptr) == SQLITE_OK;

        for (auto trackId : trackIds)
        {
            if (! ok)
                break;

            sqlite3_reset (deletePlaylist);
            sqlite3_clear_bindings (deletePlaylist);
            sqlite3_bind_int64 (deletePlaylist, 1, trackId);
            ok = sqlite3_step (deletePlaylist) == SQLITE_DONE;

            sqlite3_reset (deleteTrack);
            sqlite3_clear_bindings (deleteTrack);
            sqlite3_bind_int64 (deleteTrack, 1, trackId);
            ok = ok && sqlite3_step (deleteTrack) == SQLITE_DONE;

            trackTable.setRowStatus (trackId, {});
        }

        if (deletePlaylist != nullptr)
            sqlite3_finalize (deletePlaylist);
        if (deleteTrack != nullptr)
            sqlite3_finalize (deleteTrack);

        if (ok)
            ok = sqlite3_exec (handle, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK;

        if (! ok)
            sqlite3_exec (handle, "ROLLBACK;", nullptr, nullptr, nullptr);

        std::set<int64_t> removedIds (trackIds.begin(), trackIds.end());
        preparationTrackIds.erase (
            std::remove_if (preparationTrackIds.begin(), preparationTrackIds.end(),
                            [&removedIds] (int64_t id) { return removedIds.count (id) > 0; }),
            preparationTrackIds.end());

        refreshSidebarPlaylists();
        refreshMissingCount();
        sidebar.setPreparationCount (static_cast<int> (preparationTrackIds.size()));
        dispatchCurrentQuery (true);
    };

    if (! anyMissing)
    {
        performDeletion();
        return;
    }

    auto options = juce::MessageBoxOptions()
        .withIconType (juce::MessageBoxIconType::WarningIcon)
        .withTitle ("Remove from Library")
        .withMessage ("This will also remove the track from all playlists. "
                      "This action cannot be undone.")
        .withButton ("Confirm Remove")
        .withButton ("Cancel")
        .withAssociatedComponent (this);

    juce::AlertWindow::showAsync (options,
        [safeThis = juce::Component::SafePointer<LibraryComponent> (this),
         performDeletion = std::move (performDeletion)] (int result) mutable
        {
            if (safeThis == nullptr || result != 1)
                return;
            performDeletion();
        });
}

void LibraryComponent::setTrackRatingForRows (const std::vector<LibraryTrackRow>& rows, int newRating)
{
    for (const auto& row : rows)
    {
        setTrackRating (row.id, newRating);
        for (auto& bufferedRow : trackTable.resultBuffer)
            if (bufferedRow.id == row.id)
                bufferedRow.rating = newRating;
    }

    trackTable.updateContent();
}

void LibraryComponent::clearAnalysisCache (const LibraryTrackRow& row)
{
    SonikLibraryUi::clearAnalysisCache (db, row.filePath, row.contentHash);
}

void LibraryComponent::promptForPlaylistName (const juce::String& title,
                                             const juce::String& initialName,
                                             std::function<void(const juce::String&)> onSubmit,
                                             std::function<void()> onCancel)
{
    auto* alert = new juce::AlertWindow (title, {}, juce::MessageBoxIconType::NoIcon, this);
    alert->addTextEditor ("playlistName", initialName, "Name", false);
    alert->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    alert->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    if (auto* editor = alert->getTextEditor ("playlistName"))
        editor->setSelectAllWhenFocused (true);

    juce::Component::SafePointer<LibraryComponent> safeThis (this);
    juce::Component::SafePointer<juce::AlertWindow> safeAlert (alert);

    alert->enterModalState (true,
        juce::ModalCallbackFunction::create (
            [safeThis, safeAlert, submitCallback = std::move (onSubmit), cancelCallback = std::move (onCancel)] (int result) mutable
            {
                if (safeThis == nullptr || safeAlert == nullptr)
                {
                    if (cancelCallback)
                        cancelCallback();
                    return;
                }

                if (result != 1)
                {
                    if (cancelCallback)
                        cancelCallback();
                    return;
                }

                const auto name = safeAlert->getTextEditorContents ("playlistName").trim();
                if (name.isEmpty())
                {
                    juce::AlertWindow::showMessageBoxAsync (
                        juce::MessageBoxIconType::WarningIcon,
                        "Playlist Name",
                        "Playlist name is required.",
                        "OK",
                        safeThis);
                    if (cancelCallback)
                        cancelCallback();
                    return;
                }

                submitCallback (name);
            }),
        true);

    juce::MessageManager::callAsync ([safeAlert]
    {
        if (safeAlert != nullptr)
            if (auto* editor = safeAlert->getTextEditor ("playlistName"))
                editor->grabKeyboardFocus();
    });
}

void LibraryComponent::showPlaylistHeaderMenu (juce::Point<int> screenPos)
{
    juce::PopupMenu menu;
    menu.addItem (1, "New Playlist");

    const auto targetArea = juce::Rectangle<int> { screenPos.x, screenPos.y, 1, 1 };
    juce::Component::SafePointer<LibraryComponent> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options{}.withTargetScreenArea (targetArea),
        [safeThis] (int choice)
        {
            if (safeThis != nullptr && choice == 1)
            {
                safeThis->pendingCreatePlaylistTrackIds.clear();
                safeThis->sidebar.beginCreatePlaylist();
            }
        });
}

void LibraryComponent::showPlaylistMenu (int64_t playlistId, const juce::String& type, juce::Point<int> screenPos)
{
    if (type != "normal")
        return;

    juce::PopupMenu menu;
    menu.addItem (1, "Rename");
    menu.addItem (2, "Delete");

    const auto targetArea = juce::Rectangle<int> { screenPos.x, screenPos.y, 1, 1 };
    juce::Component::SafePointer<LibraryComponent> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options{}.withTargetScreenArea (targetArea),
        [safeThis, playlistId] (int choice)
        {
            if (safeThis == nullptr)
                return;

            if (choice == 1)
            {
                for (const auto& playlist : safeThis->playlistCache)
                {
                    if (playlist.id == playlistId)
                    {
                        safeThis->sidebar.beginRenamePlaylist (playlistId, "normal", playlist.name);
                        break;
                    }
                }
                return;
            }

            if (choice == 2)
            {
                juce::String playlistName = "this playlist";
                for (const auto& playlist : safeThis->playlistCache)
                {
                    if (playlist.id == playlistId)
                    {
                        playlistName = playlist.name;
                        break;
                    }
                }

                auto options = juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::WarningIcon)
                    .withTitle ("Delete Playlist")
                    .withMessage ("Delete '" + playlistName + "'? Tracks in your library will not be affected.")
                    .withButton ("Delete")
                    .withButton ("Cancel");

                juce::AlertWindow::showAsync (options,
                    [safeThis, playlistId] (int result)
                    {
                        if (safeThis == nullptr || result != 1 || safeThis->queryThread == nullptr)
                            return;

                        safeThis->queryThread->deletePlaylist (playlistId,
                            [safeThis, playlistId] (bool ok, juce::String message, int64_t)
                            {
                                if (safeThis == nullptr)
                                    return;

                                if (!ok)
                                {
                                    juce::AlertWindow::showMessageBoxAsync (
                                        juce::MessageBoxIconType::WarningIcon,
                                        "Playlist",
                                        message.isNotEmpty() ? message : "Playlist update failed.",
                                        "OK",
                                        safeThis);
                                    return;
                                }

                                if (safeThis->currentSidebarContext.startsWith ("playlist:" + juce::String (playlistId) + ":"))
                                {
                                    safeThis->currentSidebarContext = "collection";
                                    safeThis->sidebar.setActiveCollection();
                                }

                                safeThis->refreshSidebarPlaylists();
                                safeThis->dispatchCurrentQuery (true);
                            });
                    });
            }
        });
}

void LibraryComponent::handlePlaylistCreate (const juce::String& name)
{
    if (queryThread == nullptr)
        return;

    auto trackIds = pendingCreatePlaylistTrackIds;

    auto callback = [safeThis = juce::Component::SafePointer<LibraryComponent> (this)]
        (bool ok, juce::String message, int64_t playlistId)
        {
            if (safeThis == nullptr)
                return;

            if (!ok)
            {
                safeThis->handlePlaylistNameEditFailure (message, true);
                return;
            }

            safeThis->pendingCreatePlaylistTrackIds.clear();
            safeThis->handlePlaylistMutationResult (ok, message, playlistId);
        };

    if (trackIds.empty())
    {
        queryThread->createPlaylist (name, std::move (callback));
        return;
    }

    queryThread->createPlaylistWithTracks (name, std::move (trackIds),
        std::move (callback));
}

void LibraryComponent::handlePlaylistRename (int64_t playlistId, const juce::String& name)
{
    if (queryThread == nullptr)
        return;

    pendingCreatePlaylistTrackIds.clear();

    queryThread->renamePlaylist (playlistId, name,
        [safeThis = juce::Component::SafePointer<LibraryComponent> (this)]
        (bool ok, juce::String message, int64_t id)
        {
            if (safeThis == nullptr)
                return;

            if (!ok)
            {
                safeThis->handlePlaylistNameEditFailure (message, false);
                return;
            }

            safeThis->handlePlaylistMutationResult (ok, message, id);
        });
}

void LibraryComponent::handlePlaylistMutationResult (bool ok, const juce::String& message, int64_t playlistId)
{
    if (!ok)
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            "Playlist",
            message.isNotEmpty() ? message : "Playlist update failed.",
            "OK",
            this);
        return;
    }

    sidebar.cancelPlaylistEdit();
    refreshSidebarPlaylists();
    if (playlistId > 0)
    {
        currentSidebarContext = "playlist:" + juce::String (playlistId) + ":normal";
        sidebar.setActivePlaylist (playlistId, "normal");
    }
    dispatchCurrentQuery (true);
}

void LibraryComponent::handlePlaylistNameEditFailure (const juce::String& message, bool keepCreateEditorOpen)
{
    const bool duplicate = message.containsIgnoreCase ("already exists");

    if (keepCreateEditorOpen && duplicate)
    {
        sidebar.showPlaylistEditError ("A playlist with this name already exists");
        return;
    }

    sidebar.cancelPlaylistEdit();

    if (!duplicate)
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            "Playlist",
            message.isNotEmpty() ? message : "Playlist update failed.",
            "OK",
            this);
    }
}

void LibraryComponent::addSelectedRowsToPlaylist (int64_t playlistId)
{
    std::vector<int64_t> trackIds;
    auto selectedRows = trackTable.getSelectedRows();
    if (selectedRows.empty())
        selectedRows.push_back (trackTable.getSelectedRow());

    for (auto rowIndex : selectedRows)
        if (rowIndex >= 0 && rowIndex < static_cast<int> (trackTable.resultBuffer.size()))
            trackIds.push_back (trackTable.resultBuffer[static_cast<size_t> (rowIndex)].id);

    if (trackIds.empty() || queryThread == nullptr)
        return;

    queryThread->addTracksToPlaylist (playlistId, std::move (trackIds),
        [safeThis = juce::Component::SafePointer<LibraryComponent> (this)]
        (bool ok, juce::String message, int64_t id)
        {
            if (safeThis != nullptr)
                safeThis->handlePlaylistMutationResult (ok, message, id);
        });
}

void LibraryComponent::addSelectedRowsToPreparation()
{
    auto selectedRows = trackTable.getSelectedRows();
    if (selectedRows.empty())
        selectedRows.push_back (trackTable.getSelectedRow());

    for (auto rowIndex : selectedRows)
        if (rowIndex >= 0 && rowIndex < static_cast<int> (trackTable.resultBuffer.size()))
            preparationTrackIds.push_back (trackTable.resultBuffer[static_cast<size_t> (rowIndex)].id);

    sidebar.setPreparationCount (static_cast<int> (preparationTrackIds.size()));
    if (currentSidebarContext == "preparation")
        dispatchCurrentQuery (true);
}

void LibraryComponent::addTrackPathToPlaylist (int64_t playlistId, const juce::String& type,
                                               const juce::String& filePath)
{
    juce::StringArray draggedPaths;
    draggedPaths.addLines (filePath);
    draggedPaths.removeEmptyStrings();

    if (type == "preparation")
    {
        for (const auto& path : draggedPaths)
        {
            for (const auto& row : trackTable.resultBuffer)
            {
                if (row.filePath == path)
                {
                    preparationTrackIds.push_back (row.id);
                    break;
                }
            }
        }

        sidebar.setPreparationCount (static_cast<int> (preparationTrackIds.size()));
        if (currentSidebarContext == "preparation")
            dispatchCurrentQuery (true);
        return;
    }

    if (type != "normal" || queryThread == nullptr)
        return;

    std::vector<int64_t> trackIds;
    for (const auto& path : draggedPaths)
    {
        for (const auto& row : trackTable.resultBuffer)
        {
            if (row.filePath == path)
            {
                trackIds.push_back (row.id);
                break;
            }
        }
    }

    if (!trackIds.empty())
    {
        queryThread->addTracksToPlaylist (playlistId, std::move (trackIds),
            [safeThis = juce::Component::SafePointer<LibraryComponent> (this)]
            (bool ok, juce::String message, int64_t id)
            {
                if (safeThis != nullptr)
                    safeThis->handlePlaylistMutationResult (ok, message, id);
            });
        return;
    }

    queryThread->addFilePathToPlaylist (playlistId, draggedPaths.isEmpty() ? filePath : draggedPaths[0],
        [safeThis = juce::Component::SafePointer<LibraryComponent> (this)]
        (bool ok, juce::String message, int64_t id)
        {
            if (safeThis != nullptr)
                safeThis->handlePlaylistMutationResult (ok, message, id);
        });
}

void LibraryComponent::removeSelectedPlaylistEntries()
{
    if (!currentSidebarContext.contains (":normal"))
        return;

    const auto tail = currentSidebarContext.fromFirstOccurrenceOf ("playlist:", false, false);
    const auto playlistId = tail.upToFirstOccurrenceOf (":", false, false).getLargeIntValue();

    std::vector<int64_t> entryIds;
    auto selectedRows = trackTable.getSelectedRows();
    if (selectedRows.empty())
        selectedRows.push_back (trackTable.getSelectedRow());

    for (auto rowIndex : selectedRows)
        if (rowIndex >= 0 && rowIndex < static_cast<int> (trackTable.resultBuffer.size()))
            entryIds.push_back (trackTable.resultBuffer[static_cast<size_t> (rowIndex)].playlistEntryId);

    if (entryIds.empty() || queryThread == nullptr)
        return;

    queryThread->removePlaylistEntries (playlistId, std::move (entryIds),
        [safeThis = juce::Component::SafePointer<LibraryComponent> (this)]
        (bool ok, juce::String message, int64_t id)
        {
            if (safeThis != nullptr)
                safeThis->handlePlaylistMutationResult (ok, message, id);
        });
}

void LibraryComponent::removeSelectedPreparationEntries()
{
    if (currentSidebarContext != "preparation")
        return;

    auto selectedRows = trackTable.getSelectedRows();
    if (selectedRows.empty())
        selectedRows.push_back (trackTable.getSelectedRow());

    std::vector<int> indices;
    for (auto rowIndex : selectedRows)
    {
        if (rowIndex < 0 || rowIndex >= static_cast<int> (trackTable.resultBuffer.size()))
            continue;

        const auto& row = trackTable.resultBuffer[static_cast<size_t> (rowIndex)];
        const int index = row.playlistPosition > 0 ? row.playlistPosition - 1 : rowIndex;
        if (index >= 0 && index < static_cast<int> (preparationTrackIds.size()))
            indices.push_back (index);
    }

    if (indices.empty())
        return;

    std::sort (indices.begin(), indices.end());
    indices.erase (std::unique (indices.begin(), indices.end()), indices.end());

    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
        preparationTrackIds.erase (preparationTrackIds.begin() + *it);

    sidebar.setPreparationCount (static_cast<int> (preparationTrackIds.size()));
    dispatchCurrentQuery (true);
}

void LibraryComponent::movePlaylistEntry (int64_t entryId, int newRowIndex)
{
    if (!currentSidebarContext.contains (":normal") || queryThread == nullptr)
        return;

    const auto tail = currentSidebarContext.fromFirstOccurrenceOf ("playlist:", false, false);
    const auto playlistId = tail.upToFirstOccurrenceOf (":", false, false).getLargeIntValue();

    previewPlaylistEntryMove (entryId, newRowIndex);

    queryThread->movePlaylistEntry (playlistId, entryId, newRowIndex,
        [safeThis = juce::Component::SafePointer<LibraryComponent> (this)]
        (bool ok, juce::String message, int64_t id)
        {
            if (safeThis == nullptr)
                return;

            if (!ok)
            {
                juce::AlertWindow::showMessageBoxAsync (
                    juce::MessageBoxIconType::WarningIcon,
                    "Playlist",
                    message.isNotEmpty() ? message : "Playlist update failed.",
                    "OK",
                    safeThis);
                safeThis->dispatchCurrentQuery (true);
                return;
            }

            safeThis->refreshSidebarPlaylists();
        });
}

void LibraryComponent::previewPlaylistEntryMove (int64_t entryId, int newRowIndex)
{
    auto& rows = trackTable.resultBuffer;
    const auto it = std::find_if (rows.begin(), rows.end(), [entryId] (const LibraryTrackRow& row)
    {
        return row.playlistEntryId == entryId;
    });

    if (it == rows.end())
        return;

    auto movedRow = std::move (*it);
    rows.erase (it);

    const int clampedIndex = juce::jlimit (0, static_cast<int> (rows.size()), newRowIndex);
    rows.insert (rows.begin() + clampedIndex, std::move (movedRow));

    for (int i = 0; i < static_cast<int> (rows.size()); ++i)
        rows[static_cast<size_t> (i)].playlistPosition = i + 1;

    trackTable.updateContent();
    trackTable.selectRow (juce::jlimit (0, static_cast<int> (rows.size()) - 1, clampedIndex));
}

// ---- Play count DB write ---------------------------------------------------

void LibraryComponent::incrementPlayCount (const juce::String& filePath)
{
    auto* handle = db.getDbHandle();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2 (handle,
                             "UPDATE library_tracks SET play_count = play_count + 1 WHERE file_path = ?",
                             -1, &stmt, nullptr) == SQLITE_OK)
    {
        const std::string ps = filePath.toStdString();
        sqlite3_bind_text (stmt, 1, ps.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step      (stmt);
        sqlite3_finalize  (stmt);
    }
    // Refresh the result buffer so the PLAYS column shows the updated count.
    dispatchCurrentQuery (true);
}

// ---- Rating DB write -------------------------------------------------------

void LibraryComponent::setTrackRating (int64_t trackId, int newRating)
{
    auto* handle = db.getDbHandle();
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2 (handle,
                             "UPDATE library_tracks SET rating=? WHERE id=?",
                             -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int   (stmt, 1, newRating);
        sqlite3_bind_int64 (stmt, 2, trackId);
        sqlite3_step       (stmt);
        sqlite3_finalize   (stmt);
    }
}

// ---- Filter state persistence (PRD-0035) -----------------------------------

void LibraryComponent::loadFilterState()
{
    if (!filterPropsFile) return;

    deckFilter.keyMatchActive = filterPropsFile->getBoolValue   ("keyMatchActive", false);
    deckFilter.bpmMatchActive = filterPropsFile->getBoolValue   ("bpmMatchActive", false);
    deckFilter.bpmVision      = filterPropsFile->getDoubleValue ("bpmVision",       6.0);
    halfTimeEnabled           = filterPropsFile->getBoolValue   ("halfTimeEnabled", false);
    const bool missingOnly    = filterPropsFile->getBoolValue   ("showMissingOnly", false);

    filterBar.setKeyMatchActive (deckFilter.keyMatchActive);
    filterBar.setBpmMatchActive (deckFilter.bpmMatchActive);
    filterBar.setBpmVisionValue (deckFilter.bpmVision);
    currentParams.showMissingOnly = missingOnly;

    rebuildDeckAwareFilter();
}

void LibraryComponent::saveFilterState()
{
    if (!filterPropsFile) return;

    filterPropsFile->setValue ("keyMatchActive", deckFilter.keyMatchActive);
    filterPropsFile->setValue ("bpmMatchActive", deckFilter.bpmMatchActive);
    filterPropsFile->setValue ("bpmVision",      deckFilter.bpmVision);
    filterPropsFile->setValue ("halfTimeEnabled", halfTimeEnabled);
    filterPropsFile->setValue ("showMissingOnly", currentParams.showMissingOnly);
    filterPropsFile->saveIfNeeded();
}
