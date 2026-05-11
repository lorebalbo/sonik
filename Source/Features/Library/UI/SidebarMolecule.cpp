#include "SidebarMolecule.h"
#include "LibraryPalette.h"

SidebarMolecule::SidebarMolecule()
{
    configureInlineEditor();
    rebuild();
}

void SidebarMolecule::paint (juce::Graphics& g)
{
    g.fillAll (LibraryPalette::containerLow());

    if (dragHoverIndex >= 0)
    {
        auto row = getLocalBounds().withY (dragHoverIndex * SidebarItemAtom::kHeight)
                                  .withHeight (SidebarItemAtom::kHeight);
        g.setColour (LibraryPalette::primary());
        g.drawRect (row.reduced (2), 1);
    }
}

void SidebarMolecule::resized()
{
    const auto b = getLocalBounds();
    for (size_t i = 0; i < items.size(); ++i)
    {
        const int row = i < itemRows.size() ? itemRows[i] : static_cast<int> (i);
        items[i]->setBounds (0, row * SidebarItemAtom::kHeight,
                             b.getWidth(), SidebarItemAtom::kHeight);
    }

    if (inlineEditorRow >= 0)
    {
        inlineEditor.setBounds (6, inlineEditorRow * SidebarItemAtom::kHeight + 3,
                                juce::jmax (0, b.getWidth() - 12),
                                SidebarItemAtom::kHeight - 6);
        inlineEditor.toFront (false);
    }

    if (inlineErrorRow >= 0)
    {
        inlineErrorLabel.setBounds (20, inlineErrorRow * SidebarItemAtom::kHeight,
                                    juce::jmax (0, b.getWidth() - 24),
                                    SidebarItemAtom::kHeight);
        inlineErrorLabel.toFront (false);
    }

}

void SidebarMolecule::setFolders (const juce::StringArray& paths)
{
    folderPaths = paths;
    rebuild();
    resized();
    repaint();
}

void SidebarMolecule::setPlaylists (const std::vector<PlaylistInfo>& pl)
{
    playlists = pl;
    rebuild();
    resized();
    repaint();
}

void SidebarMolecule::beginCreatePlaylist()
{
    playlistsExpanded = true;
    playlistEditMode = PlaylistEditMode::Create;
    editingPlaylistId = 0;
    editingOriginalName = {};
    editErrorText = {};
    editSubmitInFlight = false;
    inlineEditor.setText ({}, juce::dontSendNotification);

    rebuild();
    resized();
    repaint();

    inlineEditor.grabKeyboardFocus();
    inlineEditor.selectAll();
}

void SidebarMolecule::beginRenamePlaylist (int64_t id, const juce::String& type, const juce::String& currentName)
{
    if (type != "normal")
        return;

    playlistsExpanded = true;
    playlistEditMode = PlaylistEditMode::Rename;
    editingPlaylistId = id;
    editingOriginalName = currentName;
    editErrorText = {};
    editSubmitInFlight = false;
    inlineEditor.setText (currentName, juce::dontSendNotification);

    rebuild();
    resized();
    repaint();

    inlineEditor.grabKeyboardFocus();
    inlineEditor.selectAll();
}

void SidebarMolecule::cancelPlaylistEdit()
{
    playlistEditMode = PlaylistEditMode::None;
    editingPlaylistId = 0;
    editingOriginalName = {};
    editErrorText = {};
    editSubmitInFlight = false;
    inlineEditor.setText ({}, juce::dontSendNotification);

    rebuild();
    resized();
    repaint();
}

void SidebarMolecule::showPlaylistEditError (const juce::String& message)
{
    if (playlistEditMode == PlaylistEditMode::None)
        return;

    editSubmitInFlight = false;
    editErrorText = message.isNotEmpty()
        ? message
        : "A playlist with this name already exists";

    rebuild();
    resized();
    repaint();

    inlineEditor.grabKeyboardFocus();
    inlineEditor.selectAll();
}

void SidebarMolecule::setPreparationCount (int count)
{
    preparationCount = juce::jmax (0, count);
    rebuild();
    resized();
    repaint();
}

void SidebarMolecule::setActiveCollection()
{
    activeSection      = ActiveSection::Collection;
    activeFolder       = {};
    activePlaylistId   = -1;
    activePlaylistType = {};
    rebuild();
    resized();
    repaint();
}

void SidebarMolecule::setActiveFolder (const juce::String& path)
{
    activeSection      = ActiveSection::Folders;
    activeFolder       = path;
    activePlaylistId   = -1;
    activePlaylistType = {};
    rebuild();
    resized();
    repaint();
}

void SidebarMolecule::setActivePlaylist (int64_t id, const juce::String& type)
{
    activeSection      = ActiveSection::Playlists;
    activeFolder       = {};
    activePlaylistId   = id;
    activePlaylistType = type;
    rebuild();
    resized();
    repaint();
}

bool SidebarMolecule::isInterestedInDragSource (const juce::DragAndDropTarget::SourceDetails& details)
{
    return details.description.toString().isNotEmpty();
}

void SidebarMolecule::itemDragEnter (const juce::DragAndDropTarget::SourceDetails& details)
{
    itemDragMove (details);
}

void SidebarMolecule::itemDragMove (const juce::DragAndDropTarget::SourceDetails& details)
{
    const auto target = targetRowForY (details.localPosition.y);
    dragHoverIndex = target.rowIndex;
    repaint();
}

void SidebarMolecule::itemDragExit (const juce::DragAndDropTarget::SourceDetails&)
{
    dragHoverIndex = -1;
    repaint();
}

void SidebarMolecule::itemDropped (const juce::DragAndDropTarget::SourceDetails& details)
{
    const auto target = targetRowForY (details.localPosition.y);
    dragHoverIndex = -1;
    repaint();

    if (target.rowIndex >= 0 && onTrackPathDroppedOnPlaylist)
        onTrackPathDroppedOnPlaylist (target.playlistId, target.playlistType, details.description.toString());
}

// ---- private ---------------------------------------------------------------

void SidebarMolecule::rebuild()
{
    for (auto& it : items)
        removeChildComponent (it.get());
    items.clear();
    itemRows.clear();
    dropRows.clear();
    inlineEditorRow = -1;
    inlineErrorRow = -1;

    int nextRow = 0;
    auto addItem = [this, &nextRow] (std::unique_ptr<SidebarItemAtom> item)
    {
        const int rowIndex = nextRow++;
        addAndMakeVisible (*item);
        items.push_back (std::move (item));
        itemRows.push_back (rowIndex);
        return rowIndex;
    };

    auto addInlineEditorRow = [this, &nextRow]
    {
        inlineEditorRow = nextRow++;
        inlineEditor.setVisible (true);

        if (editErrorText.isNotEmpty())
        {
            inlineErrorRow = nextRow++;
            inlineErrorLabel.setText (editErrorText, juce::dontSendNotification);
            inlineErrorLabel.setVisible (true);
        }
        else
        {
            inlineErrorLabel.setVisible (false);
        }
    };

    inlineEditor.setVisible (playlistEditMode != PlaylistEditMode::None);
    inlineErrorLabel.setVisible (false);

    // COLLECTION
    {
        auto item = std::make_unique<SidebarItemAtom>();
        item->label    = "COLLECTION";
        item->isActive = (activeSection == ActiveSection::Collection);
        item->onClick  = [this] { selectCollection(); };
        addItem (std::move (item));
    }

    // FOLDERS header
    {
        auto item  = std::make_unique<SidebarItemAtom>();
        item->label   = foldersExpanded ? "FOLDERS -" : "FOLDERS +";
        item->isActive = (activeSection == ActiveSection::Folders && activeFolder.isEmpty());
        item->onClick  = [this]
        {
            foldersExpanded = !foldersExpanded;
            rebuild();
            resized();
            repaint();
        };
        addItem (std::move (item));

        if (foldersExpanded)
        {
            for (int i = 0; i < folderPaths.size(); ++i)
            {
                const juce::String path = folderPaths[i];
                auto sub  = std::make_unique<SidebarItemAtom>();
                sub->label      = juce::File (path).getFileName();
                sub->isActive   = (activeSection == ActiveSection::Folders
                                   && activeFolder == path);
                sub->isIndented = true;
                sub->onClick    = [this, path] { selectFolder (path); };
                addItem (std::move (sub));
            }
        }
    }

    // PLAYLISTS header
    {
        auto item = std::make_unique<SidebarItemAtom>();
        item->label   = playlistsExpanded ? "PLAYLISTS -" : "PLAYLISTS +";
        item->isActive = false;
        item->onClick  = [this]
        {
            playlistsExpanded = !playlistsExpanded;
            rebuild();
            resized();
            repaint();
        };
        item->onRightClick = [this] (juce::Point<int> screenPos)
        {
            if (onPlaylistHeaderMenuRequested)
                onPlaylistHeaderMenuRequested (screenPos);
        };
        addItem (std::move (item));

        if (playlistsExpanded)
        {
            if (playlistEditMode == PlaylistEditMode::Create)
                addInlineEditorRow();

            auto preparation = std::make_unique<SidebarItemAtom>();
            preparation->label = "PREPARATION LIST";
            preparation->secondaryLabel = juce::String (preparationCount);
            preparation->isIndented = true;
            preparation->isActive = (activeSection == ActiveSection::Playlists
                                     && activePlaylistType == "preparation");
            preparation->onClick = [this] { selectPlaylist (-1, "preparation"); };
            const int prepRow = addItem (std::move (preparation));
            dropRows.push_back ({ prepRow, -1, "preparation" });

            for (const auto& playlist : playlists)
            {
                if (playlist.isHistory())
                {
                    auto history = std::make_unique<SidebarItemAtom>();
                    history->label = "HISTORY";
                    history->secondaryLabel = juce::String (playlist.trackCount);
                    history->isIndented = true;
                    history->isActive = (activeSection == ActiveSection::Playlists
                                         && activePlaylistId == playlist.id
                                         && activePlaylistType == "history");
                    const int64_t capturedId = playlist.id;
                    history->onClick = [this, capturedId] { selectPlaylist (capturedId, "history"); };
                    addItem (std::move (history));
                    break;
                }
            }

            for (const auto& playlist : playlists)
            {
                if (!playlist.isNormal())
                    continue;

                if (playlistEditMode == PlaylistEditMode::Rename
                    && editingPlaylistId == playlist.id)
                {
                    addInlineEditorRow();
                    continue;
                }

                auto sub = std::make_unique<SidebarItemAtom>();
                sub->label      = playlist.name;
                sub->secondaryLabel = juce::String (playlist.trackCount);
                sub->isActive   = (activeSection == ActiveSection::Playlists
                                   && activePlaylistId == playlist.id
                                   && activePlaylistType == "normal");
                sub->isIndented = true;
                const int64_t capturedId = playlist.id;
                const juce::String capturedType = playlist.type;
                sub->onClick = [this, capturedId, capturedType] { selectPlaylist (capturedId, capturedType); };
                sub->onDoubleClick = [this, capturedId, capturedType]
                {
                    if (onPlaylistDoubleClicked)
                        onPlaylistDoubleClicked (capturedId, capturedType);
                };
                sub->onRightClick = [this, capturedId, capturedType] (juce::Point<int> screenPos)
                {
                    if (onPlaylistMenuRequested)
                        onPlaylistMenuRequested (capturedId, capturedType, screenPos);
                };
                const int rowIndex = addItem (std::move (sub));
                dropRows.push_back ({ rowIndex, capturedId, capturedType });
            }
        }
    }
}

void SidebarMolecule::selectCollection()
{
    activeSection      = ActiveSection::Collection;
    activeFolder       = {};
    activePlaylistId   = -1;
    activePlaylistType = {};
    if (onCollectionSelected) onCollectionSelected();
    scheduleRebuild();
}

void SidebarMolecule::selectFolder (const juce::String& path)
{
    const juce::String selectedPath = path;
    activeSection      = ActiveSection::Folders;
    activeFolder       = selectedPath;
    activePlaylistId   = -1;
    activePlaylistType = {};
    if (onFolderSelected) onFolderSelected (selectedPath);
    scheduleRebuild();
}

void SidebarMolecule::selectPlaylist (int64_t id, const juce::String& type)
{
    const juce::String selectedType = type;
    activeSection      = ActiveSection::Playlists;
    activeFolder       = {};
    activePlaylistId   = id;
    activePlaylistType = selectedType;
    if (onPlaylistSelected) onPlaylistSelected (id, selectedType);
    scheduleRebuild();
}

void SidebarMolecule::scheduleRebuild()
{
    juce::Component::SafePointer<SidebarMolecule> safeThis (this);
    juce::MessageManager::callAsync ([safeThis]
    {
        if (safeThis == nullptr)
            return;

        safeThis->rebuild();
        safeThis->resized();
        safeThis->repaint();
    });
}

SidebarMolecule::DropTargetRow SidebarMolecule::targetRowForY (int y) const
{
    if (items.empty())
        return {};

    const int rowIndex = juce::jmax (0, y / SidebarItemAtom::kHeight);
    for (const auto& row : dropRows)
        if (row.rowIndex == rowIndex)
            return row;

    return {};
}

void SidebarMolecule::configureInlineEditor()
{
    inlineEditor.setVisible (false);
    inlineEditor.setFont (LibraryPalette::bodyFont (12.0f));
    inlineEditor.setColour (juce::TextEditor::backgroundColourId, LibraryPalette::surface());
    inlineEditor.setColour (juce::TextEditor::textColourId, LibraryPalette::primary());
    inlineEditor.setColour (juce::TextEditor::outlineColourId, LibraryPalette::primary());
    inlineEditor.setColour (juce::TextEditor::focusedOutlineColourId, LibraryPalette::primary());
    inlineEditor.setColour (juce::TextEditor::highlightColourId, LibraryPalette::primary());
    inlineEditor.setColour (juce::TextEditor::highlightedTextColourId, LibraryPalette::surface());
    inlineEditor.setSelectAllWhenFocused (true);
    inlineEditor.setInputRestrictions (96);
    inlineEditor.onReturnKey = [this] { submitInlineEdit(); };
    inlineEditor.onEscapeKey = [this] { cancelPlaylistEdit(); };
    inlineEditor.onFocusLost = [this]
    {
        if (editSubmitInFlight)
            return;

        if (playlistEditMode == PlaylistEditMode::Rename)
            submitInlineEdit();
        else if (playlistEditMode == PlaylistEditMode::Create)
            cancelPlaylistEdit();
    };
    inlineEditor.onTextChange = [this]
    {
        if (editErrorText.isNotEmpty())
        {
            editErrorText = {};
            rebuild();
            resized();
            repaint();
        }
    };
    addChildComponent (inlineEditor);

    inlineErrorLabel.setVisible (false);
    inlineErrorLabel.setFont (LibraryPalette::bodyFont (10.0f));
    inlineErrorLabel.setColour (juce::Label::textColourId, LibraryPalette::primary());
    inlineErrorLabel.setJustificationType (juce::Justification::centredLeft);
    addChildComponent (inlineErrorLabel);
}

void SidebarMolecule::submitInlineEdit()
{
    if (playlistEditMode == PlaylistEditMode::None || editSubmitInFlight)
        return;

    const auto submittedName = inlineEditor.getText().trim();
    if (submittedName.isEmpty())
    {
        cancelPlaylistEdit();
        return;
    }

    editSubmitInFlight = true;

    if (playlistEditMode == PlaylistEditMode::Create)
    {
        if (onPlaylistCreateSubmitted)
            onPlaylistCreateSubmitted (submittedName);
        else
            editSubmitInFlight = false;
        return;
    }

    if (submittedName == editingOriginalName)
    {
        cancelPlaylistEdit();
        return;
    }

    if (onPlaylistRenameSubmitted)
        onPlaylistRenameSubmitted (editingPlaylistId, submittedName);
    else
        editSubmitInFlight = false;
}
