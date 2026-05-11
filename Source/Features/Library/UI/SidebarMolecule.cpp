#include "SidebarMolecule.h"
#include "LibraryPalette.h"

SidebarMolecule::SidebarMolecule()
{
    rebuild();
}

void SidebarMolecule::paint (juce::Graphics& g)
{
    g.fillAll (LibraryPalette::containerLow());

    if (dragHoverIndex >= 0)
    {
        auto row = getLocalBounds().removeFromTop ((dragHoverIndex + 1) * SidebarItemAtom::kHeight)
                                .removeFromBottom (SidebarItemAtom::kHeight);
        g.setColour (LibraryPalette::primary());
        g.drawRect (row.reduced (2), 1);
    }
}

void SidebarMolecule::resized()
{
    auto b = getLocalBounds();
    for (auto& item : items)
        item->setBounds (b.removeFromTop (SidebarItemAtom::kHeight));

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
    dragHoverIndex = target.itemIndex;
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

    if (target.itemIndex >= 0 && onTrackPathDroppedOnPlaylist)
        onTrackPathDroppedOnPlaylist (target.playlistId, target.playlistType, details.description.toString());
}

// ---- private ---------------------------------------------------------------

void SidebarMolecule::rebuild()
{
    for (auto& it : items)
        removeChildComponent (it.get());
    items.clear();
    dropRows.clear();

    auto addItem = [this] (std::unique_ptr<SidebarItemAtom> item)
    {
        addAndMakeVisible (*item);
        items.push_back (std::move (item));
        return static_cast<int> (items.size()) - 1;
    };

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
    setActiveCollection();
    if (onCollectionSelected) onCollectionSelected();
}

void SidebarMolecule::selectFolder (const juce::String& path)
{
    const juce::String selectedPath = path;
    setActiveFolder (selectedPath);
    if (onFolderSelected) onFolderSelected (selectedPath);
}

void SidebarMolecule::selectPlaylist (int64_t id, const juce::String& type)
{
    const juce::String selectedType = type;
    setActivePlaylist (id, selectedType);
    if (onPlaylistSelected) onPlaylistSelected (id, selectedType);
}

SidebarMolecule::DropTargetRow SidebarMolecule::targetRowForY (int y) const
{
    if (items.empty())
        return {};

    const int itemIndex = juce::jlimit (0, static_cast<int> (items.size()) - 1,
                                        y / SidebarItemAtom::kHeight);
    for (const auto& row : dropRows)
        if (row.itemIndex == itemIndex)
            return row;

    return {};
}
