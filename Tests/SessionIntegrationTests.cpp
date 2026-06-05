//==============================================================================
// PRD-0096: SessionIntegrationTests — the GUI-adjacent integration layer that
// sits on top of the (separately, exhaustively tested) SessionController logic
// core. These tests cover only the deterministic, headless-reachable surface:
//
//   1. DawPanel view-state round-trip (capture/restore are inverse) and the
//      deterministic fit-to-width / scroll-to-start fallback for absent or
//      out-of-range persisted values (§1.5.5).
//   2. SessionMenu: the registered command IDs, their names + platform key
//      shortcuts (Cmd/Ctrl+N/O/S, Shift+Cmd/Ctrl+S), the dynamic Open Recent
//      submenu (including the greyed stale entry + Clear Recent enablement), and
//      that invoking a command id routes to the injected SessionController.
//   3. The monochrome session dialogs (showUnsavedChangesPrompt /
//      showSessionError) construct + paint into an offscreen image without
//      crashing and produce non-trivial ink, and route their button result.
//
// JUCE UnitTest, category "Sonik". No native dialogs / no disk dialogs.
//==============================================================================

#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>
#include <optional>

#include "Features/Daw/State/DawState.h"
#include "Features/Daw/Model/MasterGridService.h"
#include "Features/Daw/Transform/TimelineTransform.h"
#include "Features/Daw/Ui/Organisms/DawPanel.h"
#include "Features/Daw/Session/SessionController.h"
#include "Features/Daw/Session/SessionSerializer.h"
#include "Features/Daw/Session/Ui/SessionMenu.h"
#include "Features/Daw/Session/Ui/SessionDialogs.h"
#include "Features/Sync/MasterClockPublisher.h"

using namespace Daw::Session;
using namespace Daw::Session::Ui;

//==============================================================================
// A minimal serializer fake (the SessionController logic itself is covered by
// SessionControllerTests; here it only needs to satisfy the constructor and the
// command-routing assertions).
//==============================================================================
namespace
{
    class CountingSerializer final : public SessionSerializer
    {
    public:
        SaveResult save (const juce::ValueTree&, const SessionMetadata&,
                         const juce::File& target) override
        {
            ++saveCount;
            SaveResult r;
            r.error       = SaveError::None;
            r.writtenPath = normaliseTarget (target);
            return r;
        }

        LoadResult load (const juce::File& f) const override
        {
            ++loadCount;
            lastLoaded = f;
            return nextLoad;
        }

        int                saveCount { 0 };
        mutable int        loadCount { 0 };
        mutable juce::File lastLoaded;
        LoadResult         nextLoad;
    };
}

class SessionIntegrationTests final : public juce::UnitTest
{
public:
    SessionIntegrationTests() : juce::UnitTest ("Session Integration (PRD-0096)", "Sonik") {}

    void runTest() override
    {
        testViewStateRoundTrip();
        testViewStateFallbacks();
        testMenuCommandRegistration();
        testMenuKeyShortcuts();
        testOpenRecentSubmenu();
        testCommandRoutesToController();
        testDialogsPaintAndRoute();
    }

private:
    //==========================================================================
    // Shared scaffolding.
    //==========================================================================
    struct ScopedTempDir
    {
        juce::File dir;
        ScopedTempDir()
        {
            dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile ("sonik_integ_test_" + juce::Uuid().toString());
            dir.createDirectory();
        }
        ~ScopedTempDir() { dir.deleteRecursively(); }
    };

    // Builds a headless DawPanel exactly the way the existing DAW UI tests do:
    // a MasterClockPublisher feeding a MasterGridService, a daw branch, and a
    // null deck resolver. Sized so the transform has a real viewport width.
    struct PanelFixture
    {
        MasterClockPublisher                  publisher;
        std::unique_ptr<Daw::MasterGridService> grid;
        juce::ValueTree                       root { juce::Identifier ("SonikState") };
        juce::ValueTree                       dawBranch;
        std::unique_ptr<Daw::DawPanel>        panel;

        PanelFixture()
        {
            publisher.publish ({ 120.0, 120.0, 0, true });
            grid = std::make_unique<Daw::MasterGridService> (publisher, [] { return 44100.0; });
            dawBranch = DawState::getOrCreateDawBranch (root);
            panel = std::make_unique<Daw::DawPanel> (*grid, dawBranch,
                                                     [] (int) { return juce::ValueTree(); });
            panel->setSize (1000, panel->getPreferredHeight());
        }
    };

    //==========================================================================
    // 1a. View-state capture/restore are inverse operations.
    //==========================================================================
    void testViewStateRoundTrip()
    {
        beginTest ("DawPanel view-state capture and restore are inverse operations");

        PanelFixture f;
        auto& t = f.panel->getTransform();

        // Establish a non-default view: zoom in and scroll right.
        t.setPixelsPerBeat (200.0);
        t.setLeftEdgeSample (1'234'567);
        f.panel->resized();

        const double       capturedZoom   = f.panel->captureViewZoomSamplesPerPixel();
        const std::int64_t capturedScroll = f.panel->captureViewScrollStartSample();

        expect (capturedZoom > 0.0, "captured zoom is positive");

        // Mutate the live transform away from the captured state.
        t.setPixelsPerBeat (50.0);
        t.setLeftEdgeSample (0);
        expect (std::abs (f.panel->captureViewZoomSamplesPerPixel() - capturedZoom) > 1.0e-6,
                "zoom actually changed before restore");

        // Restore the captured pair: the transform returns to the captured view.
        f.panel->restoreViewState (capturedZoom, capturedScroll);

        expectWithinAbsoluteError (f.panel->captureViewZoomSamplesPerPixel(),
                                   capturedZoom, 1.0e-6);
        // The transform clamps the left edge to its valid scroll range; the
        // captured scroll was itself produced by the same transform, so the
        // round-trip is exact.
        expectEquals ((int) f.panel->captureViewScrollStartSample(),
                      (int) capturedScroll, "scroll start restored exactly");
    }

    //==========================================================================
    // 1b. Deterministic fallbacks when persisted values are absent / invalid.
    //==========================================================================
    void testViewStateFallbacks()
    {
        beginTest ("DawPanel restoreViewState falls back to fit-to-width / scroll-to-start");

        // Absent values: fit-to-width default zoom + scroll to the min left edge.
        {
            PanelFixture f;
            auto& t = f.panel->getTransform();
            t.setPixelsPerBeat (777.0);
            t.setLeftEdgeSample (9'000'000);

            f.panel->restoreViewState (std::nullopt, std::nullopt);

            expectWithinAbsoluteError (t.getPixelsPerBeat(),
                                       Daw::DawPanel::kDefaultPixelsPerBeat, 1.0e-6);
            expectEquals ((int) t.getLeftEdgeSample(), (int) t.minLeftEdgeSample(),
                          "absent scroll falls back to the min left edge");
        }

        // Non-positive / non-finite zoom: same fit-to-width fallback.
        {
            PanelFixture f;
            auto& t = f.panel->getTransform();
            f.panel->restoreViewState (0.0, std::nullopt);
            expectWithinAbsoluteError (t.getPixelsPerBeat(),
                                       Daw::DawPanel::kDefaultPixelsPerBeat, 1.0e-6,
                                       "non-positive zoom rejected");

            f.panel->restoreViewState (-12.0, std::nullopt);
            expectWithinAbsoluteError (t.getPixelsPerBeat(),
                                       Daw::DawPanel::kDefaultPixelsPerBeat, 1.0e-6,
                                       "negative zoom rejected");
        }

        // Out-of-range scroll collapses harmlessly to the nearest valid edge,
        // never beyond the transform's scroll bounds.
        {
            PanelFixture f;
            auto& t = f.panel->getTransform();
            // A valid zoom + an absurd scroll far past content end.
            f.panel->restoreViewState (f.panel->captureViewZoomSamplesPerPixel(),
                                       (std::int64_t) 1'000'000'000'000LL);
            expect (t.getLeftEdgeSample() <= t.maxLeftEdgeSample(),
                    "scroll clamped to max left edge");
            expect (t.getLeftEdgeSample() >= t.minLeftEdgeSample(),
                    "scroll clamped to min left edge");
        }
    }

    //==========================================================================
    // A fixture providing a real SessionController over a temp PropertiesFile so
    // the menu can be exercised. Records the controller actions reached.
    //==========================================================================
    struct MenuFixture
    {
        ScopedTempDir                         tmp;
        std::unique_ptr<juce::PropertiesFile> props;
        juce::UndoManager                     undo;
        juce::ValueTree                       daw { DawState::createDawBranch() };
        CountingSerializer                    serializer;
        std::unique_ptr<SessionController>    controller;
        std::unique_ptr<SessionMenu>          menu;

        // Recorded host activity.
        int          newCount    { 0 };
        int          openedCount { 0 };
        int          chooserCalls { 0 };
        juce::File   chooserReturns;
        UnsavedChoice promptChoice { UnsavedChoice::DontSave };

        MenuFixture()
        {
            juce::PropertiesFile::Options o;
            o.applicationName = "SonikMenuTest";
            o.filenameSuffix  = "settings";
            props = std::make_unique<juce::PropertiesFile> (
                tmp.dir.getChildFile ("prefs.settings"), o);

            // Default load result: a valid empty document.
            LoadResult lr; lr.error = LoadError::None; lr.document.daw = DawState::createDawBranch();
            serializer.nextLoad = lr;

            SessionController::Host host;
            host.captureMetadata = [] { return SessionMetadata{}; };
            host.onSessionOpened = [this] (const SessionDocument&) { ++openedCount; };
            host.onNewSession    = [this] { ++newCount; };
            host.showError       = [] (const juce::String&, const juce::String&) {};
            host.chooseFile = [this] (ChooserPurpose, juce::File, juce::String,
                                      std::function<void (juce::File)> onChosen)
            {
                ++chooserCalls;
                onChosen (chooserReturns);
            };
            host.promptUnsavedChanges = [this] (std::function<void (UnsavedChoice)> cb)
            {
                cb (promptChoice);
            };

            controller = std::make_unique<SessionController> (
                daw, serializer, undo, *props, std::move (host));
            menu = std::make_unique<SessionMenu> (*controller);
        }

        // Seed the persisted recent list directly via the controller (open
        // promotes the path; here we use a configured load result).
        void seedRecent (const juce::File& file)
        {
            controller->openFile (file);
        }
    };

    static juce::ApplicationCommandInfo infoFor (SessionMenu& menu, juce::CommandID id)
    {
        juce::ApplicationCommandInfo info (id);
        menu.getCommandInfo (id, info);
        return info;
    }

    static bool isCommandActive (const juce::ApplicationCommandInfo& info)
    {
        return (info.flags & juce::ApplicationCommandInfo::isDisabled) == 0;
    }

    //==========================================================================
    // 2a. The expected command IDs are registered with the right names.
    //==========================================================================
    void testMenuCommandRegistration()
    {
        beginTest ("SessionMenu registers the expected command IDs with correct names");

        MenuFixture f;

        juce::Array<juce::CommandID> commands;
        f.menu->getAllCommands (commands);

        expect (commands.contains (CommandIDs::sessionNew),        "New registered");
        expect (commands.contains (CommandIDs::sessionOpen),       "Open registered");
        expect (commands.contains (CommandIDs::sessionSave),       "Save registered");
        expect (commands.contains (CommandIDs::sessionSaveAs),     "Save As registered");
        expect (commands.contains (CommandIDs::sessionClearRecent),"Clear Recent registered");
        expect (commands.contains (CommandIDs::sessionImportAudio),"Import Audio registered (PRD-0098)");
        expectEquals (commands.size(), 6, "exactly the six fixed session commands");

        expectEquals (infoFor (*f.menu, CommandIDs::sessionNew).shortName,    juce::String ("New"));
        expectEquals (infoFor (*f.menu, CommandIDs::sessionOpen).shortName,   juce::String ("Open..."));
        expectEquals (infoFor (*f.menu, CommandIDs::sessionSave).shortName,   juce::String ("Save"));
        expectEquals (infoFor (*f.menu, CommandIDs::sessionSaveAs).shortName, juce::String ("Save As..."));
        expectEquals (infoFor (*f.menu, CommandIDs::sessionClearRecent).shortName,
                      juce::String ("Clear Recent"));
        expectEquals (infoFor (*f.menu, CommandIDs::sessionImportAudio).shortName,
                      juce::String ("Import Audio File..."));

        // The menu bar exposes a single "File" menu.
        expectEquals (f.menu->getMenuBarNames().size(), 1);
        expectEquals (f.menu->getMenuBarNames()[0], juce::String ("File"));
    }

    //==========================================================================
    // 2b. The platform-standard key shortcuts are bound.
    //==========================================================================
    void testMenuKeyShortcuts()
    {
        beginTest ("SessionMenu binds Cmd/Ctrl+N/O/S and Shift+Cmd/Ctrl+S");

        MenuFixture f;
        const auto cmd = juce::ModifierKeys::commandModifier;

        auto hasKey = [&] (juce::CommandID id, const juce::KeyPress& expected)
        {
            const auto info = infoFor (*f.menu, id);
            for (const auto& k : info.defaultKeypresses)
                if (k == expected)
                    return true;
            return false;
        };

        expect (hasKey (CommandIDs::sessionNew,  juce::KeyPress ('n', cmd, 0)),
                "New = Cmd/Ctrl+N");
        expect (hasKey (CommandIDs::sessionOpen, juce::KeyPress ('o', cmd, 0)),
                "Open = Cmd/Ctrl+O");
        expect (hasKey (CommandIDs::sessionSave, juce::KeyPress ('s', cmd, 0)),
                "Save = Cmd/Ctrl+S");
        expect (hasKey (CommandIDs::sessionSaveAs,
                        juce::KeyPress ('s', cmd | juce::ModifierKeys::shiftModifier, 0)),
                "Save As = Shift+Cmd/Ctrl+S");

        // Save and Save As share the base key but are disambiguated by Shift:
        // the plain-S binding must NOT carry the shift modifier.
        expect (! hasKey (CommandIDs::sessionSave,
                          juce::KeyPress ('s', cmd | juce::ModifierKeys::shiftModifier, 0)),
                "Save is not the Shift variant");
    }

    //==========================================================================
    // 2c. The Open Recent submenu reflects the recents list, greys stale
    //     entries, and the Clear Recent enablement tracks the list.
    //==========================================================================
    void testOpenRecentSubmenu()
    {
        beginTest ("SessionMenu Open Recent reflects recents, greys stale entries, Clear Recent enablement");

        MenuFixture f;

        // Empty list: Clear Recent is inactive.
        expect (! isCommandActive (infoFor (*f.menu, CommandIDs::sessionClearRecent)),
                "Clear Recent disabled when list empty");

        // Promote one real (on-disk) file and one stale (never-created) file.
        auto realFile = f.tmp.dir.getChildFile ("real.soniksession");
        realFile.replaceWithText ("placeholder"); // exists on disk
        auto staleFile = f.tmp.dir.getChildFile ("ghost.soniksession"); // never created

        f.seedRecent (staleFile);   // promoted first, ends up lower
        f.seedRecent (realFile);    // promoted to slot 0

        const auto recents = f.controller->recentSessions();
        expectEquals (recents.size(), 2, "two recents tracked");
        expectEquals (recents[0], realFile.getFullPathName(), "most-recent first");

        // Clear Recent is now active.
        expect (isCommandActive (infoFor (*f.menu, CommandIDs::sessionClearRecent)),
                "Clear Recent enabled when list non-empty");

        // Build the File menu and locate the Open Recent submenu.
        auto fileMenu = f.menu->getMenuForIndex (0, "File");

        const juce::PopupMenu* recentSub = nullptr;
        juce::PopupMenu::MenuItemIterator it (fileMenu);
        while (it.next())
            if (it.getItem().text == "Open Recent" && it.getItem().subMenu != nullptr)
                recentSub = it.getItem().subMenu.get();

        expect (recentSub != nullptr, "Open Recent submenu present");

        // Inspect the recent slots: real entry enabled, stale entry greyed; a
        // Clear Recent item exists at the foot.
        bool realEnabled = false, realFound = false;
        bool staleDisabled = false, staleFound = false;
        bool clearRecentPresent = false;

        juce::PopupMenu::MenuItemIterator sit (*recentSub);
        while (sit.next())
        {
            const auto& item = sit.getItem();
            if (item.text == realFile.getFileName())  { realFound = true;  realEnabled = item.isEnabled; }
            if (item.text == staleFile.getFileName()) { staleFound = true; staleDisabled = ! item.isEnabled; }
            if (item.text == "Clear Recent")          { clearRecentPresent = true; }
        }

        expect (realFound,   "real recent entry listed");
        expect (realEnabled, "existing-file recent entry is enabled");
        expect (staleFound,  "stale recent entry listed");
        expect (staleDisabled, "missing-file recent entry is greyed/disabled");
        expect (clearRecentPresent, "Clear Recent action at the foot of the submenu");
    }

    //==========================================================================
    // 2d. Invoking a command id routes through perform() to the controller.
    //==========================================================================
    void testCommandRoutesToController()
    {
        beginTest ("Invoking a SessionMenu command routes to the SessionController action");

        // New routes to the controller's newSession (clean session, no prompt).
        {
            MenuFixture f;
            const int newsBefore = f.newCount;

            juce::ApplicationCommandTarget::InvocationInfo info (CommandIDs::sessionNew);
            const bool handled = f.menu->perform (info);

            expect (handled, "perform handled the New command");
            expectEquals (f.newCount, newsBefore + 1, "New command installed a fresh model");
        }

        // Save on an untitled, dirty session routes to Save As -> the chooser.
        {
            MenuFixture f;
            f.undo.beginNewTransaction ("mutate");
            f.daw.setProperty ("k", 1, &f.undo);     // make it dirty
            f.chooserReturns = f.tmp.dir.getChildFile ("set.soniksession");

            juce::ApplicationCommandTarget::InvocationInfo info (CommandIDs::sessionSave);
            expect (f.menu->perform (info), "perform handled the Save command");
            expectEquals (f.chooserCalls, 1, "untitled Save routed through the chooser");
            expectEquals (f.serializer.saveCount, 1, "the model was serialized");
            expect (f.controller->hasPath(), "path adopted after Save As");
        }

        // Clear Recent routes through and empties the list.
        {
            MenuFixture f;
            f.seedRecent (f.tmp.dir.getChildFile ("a.soniksession"));
            expect (f.controller->recentSessions().size() > 0, "list seeded");

            juce::ApplicationCommandTarget::InvocationInfo info (CommandIDs::sessionClearRecent);
            expect (f.menu->perform (info), "perform handled Clear Recent");
            expectEquals (f.controller->recentSessions().size(), 0, "recents cleared");
        }

        // An unknown command id is not handled.
        {
            MenuFixture f;
            juce::ApplicationCommandTarget::InvocationInfo info (0xDEAD);
            expect (! f.menu->perform (info), "unknown command ignored");
        }
    }

    //==========================================================================
    // 3. The monochrome dialogs construct + paint offscreen without crashing
    //    and route their button result. enterModalState is non-blocking, so we
    //    present, paint the live modal component, then dismiss it deterministically.
    //==========================================================================
    void testDialogsPaintAndRoute()
    {
        beginTest ("Session dialogs paint offscreen without crashing and route their result");

        // The dialogs self-delete via MessageManager::callAsync and enter modal
        // state. We host them inside an offscreen parent component (the
        // `parent != nullptr` branch of presentModal, which adds the dialog as a
        // child WITHOUT creating a native desktop peer), paint that child, then
        // dismiss it. The parent must outlive every dialog dismissal, so a single
        // parent is reused and message pumping happens before it is destroyed.
        struct Host : public juce::Component { Host() { setSize (900, 600); } };
        auto host = std::make_unique<Host>();
        // Put the host on a temporary desktop peer so the presenter's
        // grabKeyboardFocus() has a showing component to focus (otherwise JUCE
        // asserts "can only be focused when on screen"). windowIsTemporary keeps
        // it lightweight and off any real display.
        host->addToDesktop (juce::ComponentPeer::windowIsTemporary);
        host->setVisible (true);

        // Finds the dialog component the presenter just added as a child of the
        // host's top-level (it is the most-recently-added visible child).
        auto findDialog = [&host]() -> juce::Component*
        {
            for (int i = host->getNumChildComponents(); --i >= 0;)
                if (auto* c = host->getChildComponent (i))
                    if (c->isVisible() && c->getWidth() > 0 && c->getHeight() > 0)
                        return c;
            return nullptr;
        };

        // Collects the dialog's MonoButton children (they derive from juce::Button)
        // in left-to-right add order (Cancel/Don't-Save/Save, or OK).
        auto buttonsOf = [] (juce::Component* dialog)
        {
            juce::Array<juce::Button*> result;
            if (dialog != nullptr)
                for (int i = 0; i < dialog->getNumChildComponents(); ++i)
                    if (auto* b = dynamic_cast<juce::Button*> (dialog->getChildComponent (i)))
                        result.add (b);
            return result;
        };

        auto paintInk = [] (juce::Component* comp) -> long
        {
            if (comp == nullptr)
                return 0;
            juce::Image img (juce::Image::ARGB, comp->getWidth(), comp->getHeight(), true);
            {
                juce::Graphics g (img);
                comp->paintEntireComponent (g, false); // must not crash
            }
            long inked = 0;
            for (int y = 0; y < img.getHeight(); y += 3)
                for (int x = 0; x < img.getWidth(); x += 3)
                    if (img.getPixelAt (x, y).getBrightness() < 0.9f)
                        ++inked;
            return inked;
        };

        // A button's production onClick == the dialog's private dismissWith: it
        // fires the result callback synchronously, exits the modal state, removes
        // the dialog from its parent, and then schedules `delete this` via
        // MessageManager::callAsync. We let that deferred self-delete run by
        // draining the dispatch loop at the end of the test (see below): we MUST
        // NOT manually `delete dialog` here, because the queued callAsync would
        // then double-free it the next time ANY test pumps the message loop.
        auto clickDismiss = [] (juce::Button* button)
        {
            if (button != nullptr && button->onClick)
                button->onClick();           // synchronous dismissWith + routing
        };

        // --- Unsaved-changes prompt: present, paint, dismiss via the first
        // button (Cancel, §1.5.2) and verify the routed choice.
        {
            UnsavedChoice received = UnsavedChoice::Save;
            bool got = false;
            showUnsavedChangesPrompt (host.get(), "My Set.soniksession",
                                      [&] (UnsavedChoice c) { received = c; got = true; });

            auto* dialog = findDialog();
            expect (dialog != nullptr, "unsaved-changes prompt was added to the host");
            expect (paintInk (dialog) > 50, "prompt produced non-trivial monochrome ink");

            auto btns = buttonsOf (dialog);
            expectEquals (btns.size(), 3, "prompt offers Cancel / Don't Save / Save");
            clickDismiss (btns.isEmpty() ? nullptr : btns.getFirst()); // index 0 == Cancel

            expect (got, "prompt delivered a choice on dismissal");
            expect (received == UnsavedChoice::Cancel,
                    "first button (Cancel) maps to UnsavedChoice::Cancel (§1.5.2)");
        }

        // --- Error notice: present, paint, dismiss via OK, confirm the callback.
        {
            bool dismissed = false;
            showSessionError (host.get(), "Cannot Open Session",
                              "The file could not be read.",
                              [&] { dismissed = true; });

            auto* dialog = findDialog();
            expect (dialog != nullptr, "error notice was added to the host");
            expect (paintInk (dialog) > 50, "error notice produced non-trivial monochrome ink");

            auto btns = buttonsOf (dialog);
            expectEquals (btns.size(), 1, "error notice offers a single OK button");
            clickDismiss (btns.isEmpty() ? nullptr : btns.getFirst());

            expect (dismissed, "error notice fired its onDismissed callback");
        }

        // Both dialogs exited modal state cleanly on dismissal.
        expectEquals (juce::ModalComponentManager::getInstance()->getNumModalComponents(), 0,
                      "no modal dialogs left presented");

        // Drive the deferred `delete this` callAsync each dialog queued in
        // dismissWith, so the orphaned dialogs are reclaimed HERE (while the host
        // is alive) instead of lingering in the queue as a double-free landmine
        // for the next test that pumps the loop.
        if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
            mm->runDispatchLoopUntil (200);

        host.reset();
    }
};

static SessionIntegrationTests sessionIntegrationTests;
