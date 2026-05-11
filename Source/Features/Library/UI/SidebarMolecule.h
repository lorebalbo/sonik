#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <vector>
#include <utility>
#include "SidebarItemAtom.h"
#include "../LibraryQueryThread.h"

/// Molecule: left-side navigation panel. Three top-level items (COLLECTION,
/// FOLDERS, PLAYLISTS) with inline-expandable sub-items. Background #f3f3f4.
/// Active item uses full binary inversion.
class SidebarMolecule final : public juce::Component,
                              public juce::DragAndDropTarget
{
public:
    std::function<void()>                    onCollectionSelected;
    std::function<void (const juce::String&)> onFolderSelected;
    std::function<void (int64_t, juce::String)> onPlaylistSelected;
    std::function<void (juce::Point<int>)>   onPlaylistHeaderMenuRequested;
    std::function<void (int64_t, juce::String, juce::Point<int>)> onPlaylistMenuRequested;
    std::function<void (int64_t, juce::String)> onPlaylistDoubleClicked;
    std::function<void (int64_t, juce::String, juce::String)> onTrackPathDroppedOnPlaylist;
    std::function<void (const juce::String&)> onPlaylistCreateSubmitted;
    std::function<void (int64_t, const juce::String&)> onPlaylistRenameSubmitted;

    SidebarMolecule();

    void paint   (juce::Graphics& g) override;
    void resized () override;

    /// Called by LibraryComponent after querying the DB.
    void setFolders   (const juce::StringArray& paths);
    void setPlaylists (const std::vector<PlaylistInfo>& items);
    void setPreparationCount (int count);

    void setActiveCollection ();
    void setActiveFolder     (const juce::String& path);
    void setActivePlaylist   (int64_t id, const juce::String& type);

    void beginCreatePlaylist ();
    void beginRenamePlaylist (int64_t id, const juce::String& type, const juce::String& currentName);
    void cancelPlaylistEdit ();
    void showPlaylistEditError (const juce::String& message);

    bool isInterestedInDragSource (const juce::DragAndDropTarget::SourceDetails& details) override;
    void itemDragEnter (const juce::DragAndDropTarget::SourceDetails& details) override;
    void itemDragMove  (const juce::DragAndDropTarget::SourceDetails& details) override;
    void itemDragExit  (const juce::DragAndDropTarget::SourceDetails& details) override;
    void itemDropped   (const juce::DragAndDropTarget::SourceDetails& details) override;

private:
    enum class ActiveSection { Collection, Folders, Playlists };

    struct DropTargetRow
    {
        int          rowIndex = -1;
        int64_t      playlistId = 0;
        juce::String playlistType;
    };

    enum class PlaylistEditMode { None, Create, Rename };

    void rebuild       ();
    void scheduleRebuild ();
    void selectCollection ();
    void selectFolder     (const juce::String& path);
    void selectPlaylist   (int64_t id, const juce::String& type);
    DropTargetRow targetRowForY (int y) const;
    void configureInlineEditor ();
    void submitInlineEdit ();

    juce::StringArray                          folderPaths;
    std::vector<PlaylistInfo>                  playlists;
    int                                        preparationCount = 0;

    ActiveSection activeSection   = ActiveSection::Collection;
    juce::String  activeFolder;
    int64_t       activePlaylistId = -1;
    juce::String  activePlaylistType;

    bool foldersExpanded   = false;
    bool playlistsExpanded = true;
    int dragHoverIndex = -1;

    std::vector<std::unique_ptr<SidebarItemAtom>> items;
    std::vector<int> itemRows;
    std::vector<DropTargetRow> dropRows;

    juce::TextEditor inlineEditor;
    juce::Label      inlineErrorLabel;
    PlaylistEditMode playlistEditMode = PlaylistEditMode::None;
    int64_t          editingPlaylistId = 0;
    juce::String     editingOriginalName;
    juce::String     editErrorText;
    int              inlineEditorRow = -1;
    int              inlineErrorRow = -1;
    bool             editSubmitInFlight = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SidebarMolecule)
};
