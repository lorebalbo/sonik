#pragma once
//==============================================================================
// PRD-0096 §1.5.8: the central session command surface. A single authoritative
// binding table lives in a juce::ApplicationCommandManager, so the File-menu
// items and the keyboard shortcuts invoke identical command IDs (no divergent
// per-view KeyPress handlers for session lifecycle).
//
// SessionMenu is BOTH the juce::MenuBarModel (it builds the File menu, including
// the dynamic "Open Recent" submenu) and the juce::ApplicationCommandTarget
// (it routes New / Open / Save / Save As and the per-slot recent commands to
// the injected SessionController). It owns the command manager and registers
// the four platform-standard shortcuts:
//   Cmd/Ctrl+N        -> New
//   Cmd/Ctrl+O        -> Open
//   Cmd/Ctrl+S        -> Save
//   Shift+Cmd/Ctrl+S  -> Save As
//
// The command target is installed at the foot of the focus chain (the main
// window is the first command target), so these never shadow the deck /
// transport KeyPress handlers in MainContentComponent, which consume their keys
// before the command manager sees them.
//
// Message/UI thread only.
//==============================================================================

#include <juce_gui_extra/juce_gui_extra.h>

#include "../SessionController.h"

namespace Daw::Session::Ui
{
    //--------------------------------------------------------------------------
    // Command IDs. The recent-session entries occupy a contiguous range above
    // the fixed commands so the menu can map a clicked slot back to its path.
    //--------------------------------------------------------------------------
    namespace CommandIDs
    {
        enum
        {
            sessionNew        = 0x53'00'01, // 'S' namespace, avoids JUCE std ids
            sessionOpen       = 0x53'00'02,
            sessionSave       = 0x53'00'03,
            sessionSaveAs     = 0x53'00'04,
            sessionClearRecent = 0x53'00'05,
            sessionImportAudio = 0x53'00'06, // PRD-0098: Import Audio File...
            sessionExport      = 0x53'00'07, // PRD-0101: Export...

            // Per-slot recent commands: sessionRecentBase + i for slot i.
            sessionRecentBase = 0x53'01'00
        };
    }

    class SessionMenu final : public juce::MenuBarModel,
                              public juce::ApplicationCommandTarget
    {
    public:
        explicit SessionMenu (SessionController& controller);
        ~SessionMenu() override;

        // The owned command manager — the host installs it as the application
        // key-press target and (on macOS) builds the main menu from this model.
        juce::ApplicationCommandManager& getCommandManager() noexcept { return commandManager; }

        // Rebuild the menu (e.g. after the recent list or dirty state changes)
        // so "Open Recent" and the Save enablement reflect the live controller.
        void refresh();

        // PRD-0098: fired when "Import Audio File..." is chosen (menu or its
        // shortcut). The host opens the native chooser + runs the import pipeline.
        // When unset the menu item is present but inert.
        std::function<void()> onImportAudioFile;

        // PRD-0101: fired when "Export..." (Cmd/Ctrl+E) is chosen. The host opens
        // the ExportDialog over the current arrangement. When unset the menu item
        // is present but inert.
        std::function<void()> onExport;

        //---- juce::MenuBarModel ------------------------------------------------
        juce::StringArray getMenuBarNames() override;
        juce::PopupMenu    getMenuForIndex (int topLevelMenuIndex,
                                            const juce::String& menuName) override;
        void               menuItemSelected (int menuItemID,
                                             int topLevelMenuIndex) override;

        //---- juce::ApplicationCommandTarget -----------------------------------
        juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
        void getAllCommands (juce::Array<juce::CommandID>& commands) override;
        void getCommandInfo (juce::CommandID commandID,
                             juce::ApplicationCommandInfo& result) override;
        bool perform (const juce::ApplicationCommandTarget::InvocationInfo& info) override;

    private:
        void openRecentSlot (int slot);

        SessionController& controller;
        juce::ApplicationCommandManager commandManager;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionMenu)
    };
}
