//==============================================================================
// PRD-0096 §1.5.8: SessionMenu implementation.
//==============================================================================

#include "SessionMenu.h"

namespace Daw::Session::Ui
{
SessionMenu::SessionMenu (SessionController& controllerIn)
    : controller (controllerIn)
{
    commandManager.registerAllCommandsForTarget (this);

    // The command manager must (a) always resolve THIS object as its target so
    // menu items and keypresses reach perform() — SessionMenu is not in the
    // keyboard-focus chain, so without an explicit first target the default
    // focus-based resolution never finds it — and (b) be watched by this
    // MenuBarModel so the Mac main menu dispatches addCommandItem entries through
    // it. Without these two calls the entire File menu + shortcuts are inert.
    commandManager.setFirstCommandTarget (this);
    setApplicationCommandManagerToWatch (&commandManager);
}

SessionMenu::~SessionMenu()
{
    commandManager.setFirstCommandTarget (nullptr);
    setApplicationCommandManagerToWatch (nullptr);
}

void SessionMenu::refresh()
{
    commandManager.commandStatusChanged();
    menuItemsChanged();
}

//==============================================================================
// MenuBarModel
//==============================================================================
juce::StringArray SessionMenu::getMenuBarNames()
{
    return { "File" };
}

juce::PopupMenu SessionMenu::getMenuForIndex (int /*topLevelMenuIndex*/,
                                              const juce::String& /*menuName*/)
{
    juce::PopupMenu menu;
    auto* cm = &commandManager;

    menu.addCommandItem (cm, CommandIDs::sessionNew,  "New");
    menu.addCommandItem (cm, CommandIDs::sessionOpen, "Open...");

    // Open Recent submenu (§1.4): populated from the persisted list; stale
    // entries (file no longer on disk) are greyed/disabled (§1.5.3). A
    // "Clear Recent" action sits at the foot.
    {
        juce::PopupMenu recentMenu;
        const auto recents = controller.recentSessions();

        if (recents.isEmpty())
        {
            recentMenu.addItem (1, "No Recent Sessions", false, false);
        }
        else
        {
            for (int i = 0; i < recents.size(); ++i)
            {
                const juce::File file (recents[i]);
                const bool exists = file.existsAsFile();
                recentMenu.addItem (CommandIDs::sessionRecentBase + i,
                                    file.getFileName(),
                                    /*isEnabled*/ exists,
                                    /*isTicked*/  false);
            }

            recentMenu.addSeparator();
            recentMenu.addCommandItem (cm, CommandIDs::sessionClearRecent, "Clear Recent");
        }

        menu.addSubMenu ("Open Recent", recentMenu);
    }

    menu.addSeparator();
    menu.addCommandItem (cm, CommandIDs::sessionSave,   "Save");
    menu.addCommandItem (cm, CommandIDs::sessionSaveAs, "Save As...");

    // PRD-0098: import an external audio file as a clip on the focused lane.
    menu.addSeparator();
    menu.addCommandItem (cm, CommandIDs::sessionImportAudio, "Import Audio File...");

    // PRD-0101: export the arrangement to an audio file.
    menu.addCommandItem (cm, CommandIDs::sessionExport, "Export...");

    return menu;
}

void SessionMenu::menuItemSelected (int menuItemID, int /*topLevelMenuIndex*/)
{
    // Recent slots are plain (non-command) items so they can be enabled/disabled
    // per-entry; route them here. Fixed commands flow through perform().
    if (menuItemID >= CommandIDs::sessionRecentBase)
        openRecentSlot (menuItemID - CommandIDs::sessionRecentBase);
}

//==============================================================================
// ApplicationCommandTarget
//==============================================================================
void SessionMenu::getAllCommands (juce::Array<juce::CommandID>& commands)
{
    commands.addArray ({
        CommandIDs::sessionNew,
        CommandIDs::sessionOpen,
        CommandIDs::sessionSave,
        CommandIDs::sessionSaveAs,
        CommandIDs::sessionClearRecent,
        CommandIDs::sessionImportAudio,
        CommandIDs::sessionExport
    });
}

void SessionMenu::getCommandInfo (juce::CommandID commandID,
                                  juce::ApplicationCommandInfo& result)
{
    // Platform modifier: Cmd on macOS, Ctrl on Windows/Linux.
    const auto cmd = juce::ModifierKeys::commandModifier;

    switch (commandID)
    {
        case CommandIDs::sessionNew:
            result.setInfo ("New", "Create a new, empty session", "File", 0);
            result.addDefaultKeypress ('n', cmd);
            break;

        case CommandIDs::sessionOpen:
            result.setInfo ("Open...", "Open an existing .soniksession", "File", 0);
            result.addDefaultKeypress ('o', cmd);
            break;

        case CommandIDs::sessionSave:
            result.setInfo ("Save", "Save the current session", "File", 0);
            result.addDefaultKeypress ('s', cmd);
            break;

        case CommandIDs::sessionSaveAs:
            result.setInfo ("Save As...", "Save the session to a new file", "File", 0);
            result.addDefaultKeypress ('s', cmd | juce::ModifierKeys::shiftModifier);
            break;

        case CommandIDs::sessionClearRecent:
            result.setInfo ("Clear Recent", "Empty the recent-sessions list", "File", 0);
            result.setActive (! controller.recentSessions().isEmpty());
            break;

        case CommandIDs::sessionImportAudio:
            result.setInfo ("Import Audio File...",
                            "Import an external audio file as a clip", "File", 0);
            result.addDefaultKeypress ('i', cmd);
            result.setActive (onImportAudioFile != nullptr);
            break;

        case CommandIDs::sessionExport:
            result.setInfo ("Export...",
                            "Export the arrangement to an audio file", "File", 0);
            result.addDefaultKeypress ('e', cmd);
            result.setActive (onExport != nullptr);
            break;

        default:
            break;
    }
}

bool SessionMenu::perform (const juce::ApplicationCommandTarget::InvocationInfo& info)
{
    switch (info.commandID)
    {
        case CommandIDs::sessionNew:
            controller.newSession ([this] (bool) { refresh(); });
            return true;

        case CommandIDs::sessionOpen:
            controller.open ([this] (bool) { refresh(); });
            return true;

        case CommandIDs::sessionSave:
            controller.save ([this] (bool) { refresh(); });
            return true;

        case CommandIDs::sessionSaveAs:
            controller.saveAs ([this] (bool) { refresh(); });
            return true;

        case CommandIDs::sessionClearRecent:
            controller.clearRecentSessions();
            refresh();
            return true;

        case CommandIDs::sessionImportAudio:
            if (onImportAudioFile)
                onImportAudioFile();
            return true;

        case CommandIDs::sessionExport:
            if (onExport)
                onExport();
            return true;

        default:
            return false;
    }
}

void SessionMenu::openRecentSlot (int slot)
{
    const auto recents = controller.recentSessions();
    if (slot < 0 || slot >= recents.size())
        return;

    const juce::File file (recents[slot]);

    // §1.5.3: a recent whose file no longer exists is removed lazily on click,
    // with a brief monochrome "file not found" notice (surfaced via the
    // controller's showError host hook by openFile's FileNotFound path).
    controller.openFile (file, [this] (bool) { refresh(); });
}

} // namespace Daw::Session::Ui
