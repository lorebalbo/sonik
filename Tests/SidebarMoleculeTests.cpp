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
    }
};

static SidebarMoleculeTests sidebarMoleculeTests;