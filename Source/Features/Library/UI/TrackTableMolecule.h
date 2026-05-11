#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <set>
#include <vector>
#include "RatingAtom.h"
#include "../LibraryQueryThread.h"

/// Molecule: virtualized track table backed by juce::TableListBox.
/// paintCell() reads ONLY from resultBuffer — no DB or I/O calls ever.
/// Row height: 24 px. Zebra (#f9f9f9 / #f3f3f4). Playing row: full inversion.
class TrackTableMolecule final : public juce::Component,
                                  public juce::TableListBoxModel,
                                  public juce::DragAndDropTarget,
                                  private juce::TableHeaderComponent::Listener
{
public:
    // ---- Result buffer (swapped on Message Thread after each query) --------
    std::vector<LibraryTrackRow> resultBuffer;
    juce::String                 playingFilePath;
    std::set<juce::String>       playedThisSession;
    int                          activeSortColumn = 0;
    bool                         sortAscending    = true;

    // ---- Callbacks ---------------------------------------------------------
    std::function<void (int rowIndex)>                       onRowDoubleClicked;
    std::function<void (int rowIndex, juce::Point<int> pos)> onRowRightClicked;
    std::function<void (int rowIndex, int newRating)>        onRatingChanged;
    std::function<void (int columnId, bool ascending)>       onSortChanged;
    std::function<void (int64_t entryId, int newRowIndex)>   onPlaylistEntryDropped;

    // ---- Column IDs --------------------------------------------------------
    enum ColumnId
    {
        ColIndicator = 1,
        ColTitle     = 2,
        ColArtist    = 3,
        ColBpm       = 4,
        ColKey       = 5,
        ColDuration  = 6,
        ColRating    = 7,
        ColPlayed    = 8
    };

    TrackTableMolecule();
    ~TrackTableMolecule() override;

    void paint   (juce::Graphics& g) override;
    void resized () override;

    void setPlayingTrack (const juce::String& path);
    void updateContent   ();
    int  getSelectedRow  () const;
    std::vector<int> getSelectedRows () const;
    void selectRow       (int row);
    void scrollToRow     (int row);
    void setPlaylistReorderEnabled (bool enabled);

    bool isInterestedInDragSource (const juce::DragAndDropTarget::SourceDetails& details) override;
    void itemDropped (const juce::DragAndDropTarget::SourceDetails& details) override;

    // juce::TableListBoxModel
    int  getNumRows () override;
    void paintRowBackground (juce::Graphics& g, int rowNumber, int width,
                              int height, bool rowIsSelected) override;
    void paintCell  (juce::Graphics& g, int rowNumber, int columnId,
                     int width, int height, bool rowIsSelected) override;
    void cellClicked       (int rowNumber, int columnId,
                             const juce::MouseEvent& e) override;
    void cellDoubleClicked (int rowNumber, int columnId,
                             const juce::MouseEvent& e) override;
    void backgroundClicked (const juce::MouseEvent&) override;
    juce::String getCellTooltip (int rowNumber, int columnId) override;
    juce::var    getDragSourceDescription (const juce::SparseSet<int>& rows) override;
    void selectedRowsChanged (int lastRowSelected) override;

    // juce::TableHeaderComponent::Listener
    void tableColumnsChanged       (juce::TableHeaderComponent*) override {}
    void tableColumnsResized       (juce::TableHeaderComponent*) override {}
    void tableSortOrderChanged     (juce::TableHeaderComponent* hdr) override;
    void tableColumnDraggingChanged(juce::TableHeaderComponent*, int) override {}

private:
    static juce::String formatDuration (double seconds);

    juce::TableListBox tableBox;
    juce::SparseSet<int> selectionBeforeLastChange;
    juce::SparseSet<int> currentSelection;
    bool playlistReorderEnabled = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackTableMolecule)
};
