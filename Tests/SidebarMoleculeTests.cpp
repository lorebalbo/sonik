#include <juce_gui_basics/juce_gui_basics.h>
#include "Features/Library/UI/SidebarMolecule.h"

class SidebarMoleculeTests final : public juce::UnitTest
{
public:
    SidebarMoleculeTests() : juce::UnitTest ("Sidebar Molecule", "Sonik") {}

    void runTest() override
    {
        beginTest ("Clicking a normal playlist survives sidebar rebuild");

        SidebarMolecule sidebar;
        sidebar.setBounds (0, 0, 220, 220);

        PlaylistInfo playlist;
        playlist.id = 42;
        playlist.name = "Crash Guard";
        playlist.type = "normal";
        playlist.trackCount = 3;
        sidebar.setPlaylists ({ playlist });
        sidebar.resized();

        int callbackCount = 0;
        int64_t selectedId = 0;
        juce::String selectedType;
        sidebar.onPlaylistSelected = [&] (int64_t id, juce::String type)
        {
            ++callbackCount;
            selectedId = id;
            selectedType = type;
        };

        SidebarItemAtom* playlistRow = nullptr;
        for (int i = 0; i < sidebar.getNumChildComponents(); ++i)
        {
            auto* row = dynamic_cast<SidebarItemAtom*> (sidebar.getChildComponent (i));
            if (row != nullptr && row->label == playlist.name)
            {
                playlistRow = row;
                break;
            }
        }

        expect (playlistRow != nullptr);
        if (playlistRow == nullptr)
            return;

        auto click = playlistRow->onClick;
        expect (static_cast<bool> (click));
        if (!click)
            return;

        click();

        expectEquals (callbackCount, 1);
        expectEquals (selectedId, static_cast<int64_t> (42));
        expectEquals (selectedType, juce::String ("normal"));

        beginTest ("Playlist section renders Preparation before History and normal playlists");

        SidebarMolecule orderedSidebar;
        orderedSidebar.setBounds (0, 0, 240, 260);

        PlaylistInfo later;
        later.id = 20;
        later.name = "Later";
        later.type = "normal";

        PlaylistInfo history;
        history.id = 1;
        history.name = "History";
        history.type = "history";
        history.trackCount = 7;

        PlaylistInfo earlier;
        earlier.id = 10;
        earlier.name = "Earlier";
        earlier.type = "normal";

        orderedSidebar.setPlaylists ({ later, history, earlier });
        orderedSidebar.resized();

        juce::StringArray labels;
        for (int i = 0; i < orderedSidebar.getNumChildComponents(); ++i)
        {
            auto* row = dynamic_cast<SidebarItemAtom*> (orderedSidebar.getChildComponent (i));
            if (row != nullptr)
                labels.add (row->label);
        }

        const int prepIndex = labels.indexOf ("PREPARATION LIST");
        const int historyIndex = labels.indexOf ("HISTORY");
        const int laterIndex = labels.indexOf ("Later");
        const int earlierIndex = labels.indexOf ("Earlier");

        expect (prepIndex >= 0, "Preparation List row should be present");
        expect (historyIndex >= 0, "History row should be present");
        expect (prepIndex < historyIndex, "Preparation List should render above History");
        expect (historyIndex < laterIndex, "History should render above normal playlists");
        expect (historyIndex < earlierIndex, "History should render above all normal playlists");

        beginTest ("Inline playlist creation submits trimmed text and can show duplicate-name error");

        SidebarMolecule inlineSidebar;
        inlineSidebar.setBounds (0, 0, 240, 260);
        inlineSidebar.addToDesktop (juce::ComponentPeer::windowIsTemporary);
        inlineSidebar.setVisible (true);

        int submitCount = 0;
        juce::String submittedName;
        inlineSidebar.onPlaylistCreateSubmitted = [&] (const juce::String& name)
        {
            ++submitCount;
            submittedName = name;
        };

        inlineSidebar.beginCreatePlaylist();
        inlineSidebar.resized();

        juce::TextEditor* editor = nullptr;
        for (int i = 0; i < inlineSidebar.getNumChildComponents(); ++i)
        {
            editor = dynamic_cast<juce::TextEditor*> (inlineSidebar.getChildComponent (i));
            if (editor != nullptr)
                break;
        }

        expect (editor != nullptr, "Inline editor should be mounted as a child component");
        if (editor != nullptr)
        {
            editor->setText ("  New Set  ", juce::dontSendNotification);
            editor->onReturnKey();
        }

        expectEquals (submitCount, 1);
        expectEquals (submittedName, juce::String ("New Set"));

        inlineSidebar.showPlaylistEditError ("A playlist with this name already exists");

        bool foundErrorLabel = false;
        for (int i = 0; i < inlineSidebar.getNumChildComponents(); ++i)
        {
            auto* label = dynamic_cast<juce::Label*> (inlineSidebar.getChildComponent (i));
            if (label != nullptr && label->getText() == "A playlist with this name already exists")
            {
                foundErrorLabel = true;
                break;
            }
        }

        expect (foundErrorLabel, "Duplicate-name error label should be visible");
        inlineSidebar.removeFromDesktop();
    }
};

static SidebarMoleculeTests sidebarMoleculeTests;