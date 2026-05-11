#include "TrackTableMolecule.h"
#include "LibraryPalette.h"

static constexpr int kRowHeight = 24;

TrackTableMolecule::TrackTableMolecule()
{
    tableBox.setModel (this);
    tableBox.setRowHeight (kRowHeight);
    tableBox.setMultipleSelectionEnabled (true);
    tableBox.setRowSelectedOnMouseDown (true);
    tableBox.setOutlineThickness (0);

    auto& hdr = tableBox.getHeader();

    hdr.setColour (juce::TableHeaderComponent::backgroundColourId,
                   LibraryPalette::containerHighest());
    hdr.setColour (juce::TableHeaderComponent::textColourId,
                   LibraryPalette::primary());
    hdr.setColour (juce::TableHeaderComponent::outlineColourId,
                   LibraryPalette::primary());
    hdr.setColour (juce::TableHeaderComponent::highlightColourId,
                   LibraryPalette::containerHighest());

    // Columns — widths chosen to match spec
    const int ns = juce::TableHeaderComponent::notSortable;
    const int df = juce::TableHeaderComponent::defaultFlags;

    hdr.addColumn ("",         ColIndicator, 24,   24,  24,  ns);
    hdr.addColumn ("TITLE",    ColTitle,    200,   40,  800, df);
    hdr.addColumn ("ARTIST",   ColArtist,   140,   40,  400, df);
    hdr.addColumn ("BPM",      ColBpm,       60,   40,  120, df);
    hdr.addColumn ("KEY",      ColKey,       48,   40,  100, df);
    hdr.addColumn ("DURATION", ColDuration,  72,   40,  120, df);
    hdr.addColumn ("RATING",   ColRating,    80,   80,   80, ns);
    hdr.addColumn ("PLAYS",    ColPlayed,    48,   48,   80, df);
    hdr.addColumn ("STATUS",   ColStatus,    96,   80,  140, ns);

    hdr.addListener (this);

    tableBox.setColour (juce::ListBox::backgroundColourId, LibraryPalette::surface());
    tableBox.setColour (juce::ListBox::outlineColourId,    LibraryPalette::primary());

    addAndMakeVisible (tableBox);
}

TrackTableMolecule::~TrackTableMolecule()
{
    tableBox.getHeader().removeListener (this);
    tableBox.setModel (nullptr);
}

void TrackTableMolecule::paint (juce::Graphics& g)
{
    if (resultBuffer.empty())
    {
        g.fillAll (LibraryPalette::surface());
        g.setColour (LibraryPalette::primary());
        g.setFont (LibraryPalette::bodyFont());
        g.drawText ("No tracks found.",
                    getLocalBounds().withTrimmedTop (tableBox.getHeaderHeight()),
                    juce::Justification::centred);
    }
}

void TrackTableMolecule::paintOverChildren (juce::Graphics& g)
{
    if (!playlistReorderEnabled || dropIndicatorRow < 0)
        return;

    const int y = tableBox.getHeaderHeight() + dropIndicatorRow * kRowHeight;
    g.setColour (LibraryPalette::primary());
    g.fillRect (2, y - 1, juce::jmax (0, getWidth() - 4), 2);
}

void TrackTableMolecule::resized()
{
    tableBox.setBounds (getLocalBounds());
}

void TrackTableMolecule::setPlayingTrack (const juce::String& path)
{
    playingFilePath = path;
    tableBox.repaint();
}

void TrackTableMolecule::updateContent()
{
    tableBox.updateContent();
    currentSelection = tableBox.getSelectedRows();
    selectionBeforeLastChange = currentSelection;
    repaint();
}

void TrackTableMolecule::setRowStatus (int64_t trackId, juce::String status)
{
    if (trackId <= 0)
        return;

    if (status.isEmpty())
    {
        rowStatusOverride.erase (trackId);
        repaint();
        return;
    }

    if (! isAllowedStatus (status))
        return;

    rowStatusOverride[trackId] = std::move (status);
    tableBox.repaint();
}

int TrackTableMolecule::getSelectedRow() const
{
    return tableBox.getSelectedRow();
}

std::vector<int> TrackTableMolecule::getSelectedRows() const
{
    std::vector<int> rows;
    const auto selected = tableBox.getSelectedRows();
    for (int i = 0; i < selected.size(); ++i)
        rows.push_back (selected[i]);
    return rows;
}

void TrackTableMolecule::selectRow (int row)
{
    tableBox.selectRow (row);
}

void TrackTableMolecule::scrollToRow (int row)
{
    tableBox.scrollToEnsureRowIsOnscreen (row);
}

void TrackTableMolecule::setPlaylistReorderEnabled (bool enabled)
{
    playlistReorderEnabled = enabled;
    if (!enabled)
        dropIndicatorRow = -1;
}

bool TrackTableMolecule::isInterestedInDragSource (const juce::DragAndDropTarget::SourceDetails& details)
{
    // Only accept reorder drags when in playlist reorder mode.
    if (playlistReorderEnabled)
        return details.description.toString().startsWith ("REORDER:");

    return false;
}

void TrackTableMolecule::itemDragMove (const juce::DragAndDropTarget::SourceDetails& details)
{
    if (!playlistReorderEnabled)
        return;

    const int rowY = details.localPosition.y - tableBox.getHeaderHeight();
    const int targetRow = juce::jlimit (0, static_cast<int> (resultBuffer.size()),
                                        (rowY + kRowHeight / 2) / kRowHeight);

    if (targetRow != dropIndicatorRow)
    {
        dropIndicatorRow = targetRow;
        repaint();
    }
}

void TrackTableMolecule::itemDragExit (const juce::DragAndDropTarget::SourceDetails&)
{
    if (dropIndicatorRow >= 0)
    {
        dropIndicatorRow = -1;
        repaint();
    }
}

void TrackTableMolecule::itemDropped (const juce::DragAndDropTarget::SourceDetails& details)
{
    dropIndicatorRow = -1;
    repaint();

    if (!playlistReorderEnabled || onPlaylistEntryDropped == nullptr)
        return;

    // Extract the entryId encoded by getDragSourceDescription.
    const auto description = details.description.toString();
    if (!description.startsWith ("REORDER:"))
        return;

    const auto entryId = description.fromFirstOccurrenceOf ("REORDER:", false, false).getLargeIntValue();
    if (entryId <= 0)
        return;

    const int rowY = details.localPosition.y - tableBox.getHeaderHeight();
    const int targetRow = juce::jlimit (0, static_cast<int> (resultBuffer.size()),
                                        (rowY + kRowHeight / 2) / kRowHeight);
    onPlaylistEntryDropped (entryId, targetRow);
}

// =============================================================================
// juce::TableListBoxModel
// =============================================================================

int TrackTableMolecule::getNumRows()
{
    return static_cast<int> (resultBuffer.size());
}

void TrackTableMolecule::paintRowBackground (juce::Graphics& g,
                                              int rowNumber,
                                              int /*width*/, int /*height*/,
                                              bool rowIsSelected)
{
    if (rowNumber < 0 || rowNumber >= static_cast<int> (resultBuffer.size()))
        return;

    const bool playing = (playingFilePath.isNotEmpty()
                          && resultBuffer[static_cast<size_t> (rowNumber)].filePath == playingFilePath);

    if (playing || rowIsSelected)
        g.fillAll (LibraryPalette::primary());
    else if (rowNumber % 2 == 0)
        g.fillAll (LibraryPalette::surface());
    else
        g.fillAll (LibraryPalette::containerLow());
}

void TrackTableMolecule::paintCell (juce::Graphics& g,
                                     int rowNumber, int columnId,
                                     int width, int height,
                                     bool rowIsSelected)
{
    if (rowNumber < 0 || rowNumber >= static_cast<int> (resultBuffer.size()))
        return;

    const auto& row     = resultBuffer[static_cast<size_t> (rowNumber)];
    const bool  playing = (playingFilePath.isNotEmpty()
                           && row.filePath == playingFilePath);
    const bool  inverted = playing || rowIsSelected;
    const auto  textCol = inverted ? LibraryPalette::surface() : LibraryPalette::primary();

    const juce::Rectangle<int> cell    { 0, 0, width, height };
    const juce::Rectangle<int> padded  = cell.reduced (3, 0);

    g.setColour (textCol);
    g.setFont (LibraryPalette::bodyFont (11.0f));

    switch (columnId)
    {
        case ColIndicator:
            if (playing)
                g.drawText ("[>]", cell, juce::Justification::centred);
            else if (row.isMissing)
                g.drawText ("!", cell, juce::Justification::centred);
            else if (playlistReorderEnabled && row.playlistEntryId > 0)
                g.drawText ("||", cell, juce::Justification::centred);
            break;

        case ColTitle:
            g.drawText (row.title.isNotEmpty()
                            ? row.title
                            : juce::File (row.filePath).getFileNameWithoutExtension(),
                        padded, juce::Justification::centredLeft, true);
            break;

        case ColArtist:
            g.drawText (row.artist, padded, juce::Justification::centredLeft, true);
            break;

        case ColBpm:
            g.drawText (row.bpm > 0.0
                            ? juce::String::formatted ("%.2f", row.bpm)
                            : "-",
                        padded, juce::Justification::centredLeft);
            break;

        case ColKey:
            g.drawText (row.key.isNotEmpty() ? row.key : "-",
                        padded, juce::Justification::centredLeft);
            break;

        case ColDuration:
            g.drawText (formatDuration (row.durationSeconds),
                        padded, juce::Justification::centredLeft);
            break;

        case ColRating:
            RatingAtom::drawGlyphs (g, cell, row.rating, inverted);
            break;

        case ColPlayed:
        {
            // Show total play count from DB; draw a small dot prefix when played this session.
            const bool playedNow = (playedThisSession.count (row.filePath) > 0);
            juce::String txt;
            if (playedNow)
                txt = juce::CharPointer_UTF8 ("\xe2\x80\xa2 "); // bullet U+2022
            txt += row.playCount > 0 ? juce::String (row.playCount) : "-";
            g.drawText (txt, padded, juce::Justification::centredRight);
            break;
        }

        case ColStatus:
        {
            juce::String status;
            if (auto it = rowStatusOverride.find (row.id); it != rowStatusOverride.end())
                status = it->second;
            else
                status = (row.bpm > 0.0 && row.keyIndex >= 0) ? "Complete" : "Unanalyzed";

            g.drawText (status, padded, juce::Justification::centredLeft, true);
            break;
        }

        default:
            break;
    }
}

void TrackTableMolecule::cellClicked (int rowNumber, int columnId,
                                       const juce::MouseEvent& e)
{
    if (rowNumber < 0 || rowNumber >= static_cast<int> (resultBuffer.size()))
        return;

    if (e.mods.isRightButtonDown())
    {
        if (!tableBox.isRowSelected (rowNumber))
            tableBox.selectRow (rowNumber);

        if (onRowRightClicked)
            onRowRightClicked (rowNumber, e.getPosition());
        return;
    }

    if (e.mods.isAltDown() && !e.mods.isShiftDown() && !e.mods.isCommandDown() && !e.mods.isCtrlDown())
    {
        auto restoredSelection = selectionBeforeLastChange;
        const auto rowRange = juce::Range<int>::withStartAndLength (rowNumber, 1);

        if (restoredSelection.contains (rowNumber))
            restoredSelection.removeRange (rowRange);
        else
            restoredSelection.addRange (rowRange);

        tableBox.setSelectedRows (restoredSelection);
    }

    if (columnId == ColRating)
    {
        constexpr int kGS = RatingAtom::kGlyphSize;
        constexpr int kGP = RatingAtom::kGlyphGap;
        constexpr int kTW = RatingAtom::kTotalWidth;
        // e.x is row-relative, so subtract the column's left edge within the row.
        const int colIdx  = tableBox.getHeader().getIndexOfColumnId (ColRating, true);
        const int colXOff = tableBox.getHeader().getColumnPosition (colIdx).getX();
        const int colW    = tableBox.getHeader().getColumnWidth (ColRating);
        const int startX  = colXOff + (colW - kTW) / 2;
        const int relX    = e.x - startX;
        const int idx     = relX / (kGS + kGP);

        if (idx >= 0 && idx < 5)
        {
            const int curRating = resultBuffer[static_cast<size_t> (rowNumber)].rating;
            const int newRating = (idx + 1 == curRating) ? 0 : (idx + 1);
            resultBuffer[static_cast<size_t> (rowNumber)].rating = newRating; // mutate in-buffer for instant feedback
            tableBox.repaintRow (rowNumber);
            if (onRatingChanged)
                onRatingChanged (rowNumber, newRating);
        }
    }
}

void TrackTableMolecule::cellDoubleClicked (int rowNumber, int /*columnId*/,
                                             const juce::MouseEvent&)
{
    if (rowNumber >= 0 && rowNumber < static_cast<int> (resultBuffer.size()))
        if (onRowDoubleClicked)
            onRowDoubleClicked (rowNumber);
}

void TrackTableMolecule::backgroundClicked (const juce::MouseEvent&)
{
    tableBox.deselectAllRows();
}

juce::String TrackTableMolecule::getCellTooltip (int rowNumber, int columnId)
{
    if (columnId == ColIndicator
        && rowNumber >= 0
        && rowNumber < static_cast<int> (resultBuffer.size()))
    {
        if (resultBuffer[static_cast<size_t> (rowNumber)].isMissing)
            return "File not found on disk.";
    }
    return {};
}

juce::var TrackTableMolecule::getDragSourceDescription (const juce::SparseSet<int>& rows)
{
    // When playlist reorder is active, encode "REORDER:<entryId>" so itemDropped
    // can use the stable entryId rather than the current selection.
    if (playlistReorderEnabled && rows.size() == 1)
    {
        const int row = rows[0];
        if (row >= 0 && row < static_cast<int> (resultBuffer.size()))
        {
            const auto entryId = resultBuffer[static_cast<size_t> (row)].playlistEntryId;
            if (entryId > 0)
                return juce::var ("REORDER:" + juce::String (entryId));
        }
    }

    juce::StringArray paths;
    for (int i = 0; i < rows.size(); ++i)
    {
        const int row = rows[i];
        if (row >= 0 && row < static_cast<int> (resultBuffer.size()))
            paths.add (resultBuffer[static_cast<size_t> (row)].filePath);
    }

    return paths.isEmpty() ? juce::var{} : juce::var (paths.joinIntoString ("\n"));
}

void TrackTableMolecule::selectedRowsChanged (int /*lastRowSelected*/)
{
    selectionBeforeLastChange = currentSelection;
    currentSelection = tableBox.getSelectedRows();
}

// juce::TableHeaderComponent::Listener
void TrackTableMolecule::tableSortOrderChanged (juce::TableHeaderComponent* hdr)
{
    if (hdr == nullptr) return;
    const int  colId  = hdr->getSortColumnId();
    const bool fwd    = hdr->isSortedForwards();
    activeSortColumn  = colId;
    sortAscending     = fwd;
    if (onSortChanged)
        onSortChanged (colId, fwd);
}

// ---- static helpers --------------------------------------------------------

juce::String TrackTableMolecule::formatDuration (double seconds)
{
    const int total = static_cast<int> (seconds);
    const int secs  = total % 60;
    const int mins  = (total / 60) % 60;
    const int hours = total / 3600;

    if (hours > 0)
        return juce::String::formatted ("%d:%02d:%02d", hours, mins, secs);

    return juce::String::formatted ("%d:%02d", mins, secs);
}

bool TrackTableMolecule::isAllowedStatus (const juce::String& status)
{
    if (status == "Unanalyzed" || status == "Queued" || status == "Queued (Stems)"
        || status == "Complete" || status == "Stem Complete" || status == "Failed"
        || status == "Stem Failed")
        return true;

    if (status.startsWith ("Analyzing ") && status.endsWithChar ('%'))
        return status.fromFirstOccurrenceOf ("Analyzing ", false, false)
                     .dropLastCharacters (1).getIntValue() >= 0;

    if (status.startsWith ("Separating Stems ") && status.endsWithChar ('%'))
        return status.fromFirstOccurrenceOf ("Separating Stems ", false, false)
                     .dropLastCharacters (1).getIntValue() >= 0;

    return false;
}
