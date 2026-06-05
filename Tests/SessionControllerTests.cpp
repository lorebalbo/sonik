//==============================================================================
// PRD-0096: SessionControllerTests — Save-with-path vs Save-As routing, dirty
// set/clear across mutate/save/open/new, the unsaved-changes prompt's three
// branches (Cancel aborts), recent-list promotion/dedup/cap, and model swap on
// open. A fake serializer and a real on-temp-disk PropertiesFile are injected so
// no native dialogs are needed. JUCE UnitTest, category "Sonik".
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <memory>

#include "Features/Daw/State/DawState.h"
#include "Features/Daw/Session/SessionController.h"
#include "Features/Daw/Session/SessionSerializer.h"

using namespace Daw::Session;

//==============================================================================
// A deterministic serializer fake: records save calls and returns a configured
// document/result on load — no disk, no XML.
//==============================================================================
class FakeSerializer final : public SessionSerializer
{
public:
    SaveResult save (const juce::ValueTree& daw, const SessionMetadata& m,
                     const juce::File& target) override
    {
        ++saveCount;
        lastSavedDaw = daw.createCopy();
        lastMetadata = m;
        lastTarget   = target;

        SaveResult r;
        if (failSave)
        {
            r.error   = SaveError::IoFailure;
            r.message = "forced failure";
        }
        else
        {
            r.error       = SaveError::None;
            r.writtenPath = normaliseTarget (target);
        }
        return r;
    }

    LoadResult load (const juce::File& f) const override
    {
        ++loadCount;
        lastLoaded = f;
        return nextLoad;
    }

    // Test state.
    int                     saveCount { 0 };
    bool                    failSave  { false };
    juce::ValueTree         lastSavedDaw;
    SessionMetadata         lastMetadata;
    juce::File              lastTarget;
    mutable int             loadCount { 0 };
    mutable juce::File      lastLoaded;
    LoadResult              nextLoad;
};

class SessionControllerTests final : public juce::UnitTest
{
public:
    SessionControllerTests() : juce::UnitTest ("Session Controller", "Sonik") {}

    void runTest() override
    {
        testDirtyTracking();
        testSaveAsRoutingAndSaveWithPath();
        testSaveAsCancel();
        testOpenSwapsModel();
        testOpenFailureLeavesModelUntouched();
        testNewPromptBranches();
        testRecentPromoteDedupCap();
    }

private:
    //==========================================================================
    struct ScopedTempDir
    {
        juce::File dir;
        ScopedTempDir()
        {
            dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile ("sonik_ctrl_test_" + juce::Uuid().toString());
            dir.createDirectory();
        }
        ~ScopedTempDir() { dir.deleteRecursively(); }
    };

    //==========================================================================
    // A fixture bundling everything a controller needs + recorded host activity.
    //==========================================================================
    struct Fixture
    {
        ScopedTempDir                         tmp;
        std::unique_ptr<juce::PropertiesFile> props;
        juce::UndoManager                     undo;
        juce::ValueTree                       daw { DawState::createDawBranch() };
        FakeSerializer                        serializer;
        std::unique_ptr<SessionController>    controller;

        // Recorded host activity / configurable responses.
        int          openedCount { 0 };
        int          newCount    { 0 };
        int          errorCount  { 0 };
        juce::File   chooserReturns;            // what chooseFile delivers
        int          chooserCalls { 0 };
        bool         haveChoice { false };
        UnsavedChoice promptChoice { UnsavedChoice::Cancel };
        int          promptCalls { 0 };

        Fixture()
        {
            juce::PropertiesFile::Options o;
            o.applicationName = "SonikCtrlTest";
            o.filenameSuffix  = "settings";
            props = std::make_unique<juce::PropertiesFile> (
                tmp.dir.getChildFile ("prefs.settings"), o);

            SessionController::Host host;
            host.captureMetadata = [] { return SessionMetadata{}; };
            host.onSessionOpened = [this] (const SessionDocument&) { ++openedCount; };
            host.onNewSession    = [this] { ++newCount; };
            host.showError       = [this] (const juce::String&, const juce::String&) { ++errorCount; };
            host.chooseFile = [this] (ChooserPurpose, juce::File, juce::String,
                                      std::function<void (juce::File)> onChosen)
            {
                ++chooserCalls;
                onChosen (chooserReturns);
            };
            host.promptUnsavedChanges = [this] (std::function<void (UnsavedChoice)> cb)
            {
                ++promptCalls;
                cb (promptChoice);
            };

            controller = std::make_unique<SessionController> (
                daw, serializer, undo, *props, std::move (host));
        }

        // Performs one undoable mutation so the model becomes dirty.
        void mutate (const juce::String& key = "k", int value = 1)
        {
            undo.beginNewTransaction ("mutate");
            daw.setProperty (key, value, &undo);
        }
    };

    //==========================================================================
    void testDirtyTracking()
    {
        beginTest ("dirty derives from the undo change index vs saved baseline");

        Fixture f;
        expect (! f.controller->isDirty(), "fresh session is clean");

        f.mutate();
        expect (f.controller->isDirty(), "model mutation sets dirty");

        f.controller->markSavedBaseline();
        expect (! f.controller->isDirty(), "saving clears dirty");

        f.mutate ("k2", 2);
        expect (f.controller->isDirty(), "further mutation is dirty again");

        f.undo.undo();   // back to the saved baseline
        expect (! f.controller->isDirty(), "undo back to baseline is clean again");
    }

    //==========================================================================
    void testSaveAsRoutingAndSaveWithPath()
    {
        beginTest ("untitled Save routes to Save As; subsequent Save skips the chooser");

        Fixture f;
        f.mutate();
        f.chooserReturns = f.tmp.dir.getChildFile ("My Set");   // no extension

        bool ok = false;
        f.controller->save ([&] (bool s) { ok = s; });

        expect (ok, "save succeeded");
        expectEquals (f.chooserCalls, 1, "untitled save opened the chooser");
        expectEquals (f.serializer.saveCount, 1);
        expect (f.controller->hasPath(), "path adopted");
        expectEquals (f.controller->currentSessionPath().getFileName(),
                      juce::String ("My Set.soniksession"), "extension normalised");
        expect (! f.controller->isDirty(), "dirty cleared after save");
        expectEquals (f.controller->recentSessions()[0],
                      f.controller->currentSessionPath().getFullPathName(), "promoted to recents");

        // Mutate and Save again: a path exists, so no chooser is shown.
        f.mutate ("k2", 9);
        const int chooserBefore = f.chooserCalls;
        f.controller->save();
        expectEquals (f.chooserCalls, chooserBefore, "Save with a path skips the chooser");
        expectEquals (f.serializer.saveCount, 2, "wrote again");
        expect (! f.controller->isDirty());
    }

    //==========================================================================
    void testSaveAsCancel()
    {
        beginTest ("cancelling the Save As chooser aborts with no side effects");

        Fixture f;
        f.mutate();
        f.chooserReturns = juce::File();   // user cancelled

        bool result = true;
        f.controller->save ([&] (bool s) { result = s; });

        expect (! result, "save reports failure");
        expectEquals (f.serializer.saveCount, 0, "nothing written");
        expect (! f.controller->hasPath(), "still untitled");
        expect (f.controller->isDirty(), "still dirty");
    }

    //==========================================================================
    void testOpenSwapsModel()
    {
        beginTest ("open swaps in the loaded model, restores baseline, fires onSessionOpened");

        Fixture f;

        // Configure the fake to return a document whose daw has one track.
        auto loadedDaw = DawState::createDawBranch();
        DawState::ensureTrackForDeck (loadedDaw, 3);
        LoadResult lr;
        lr.error = LoadError::None;
        lr.document.daw = loadedDaw;
        f.serializer.nextLoad = lr;

        auto file = f.tmp.dir.getChildFile ("opened.soniksession");
        bool ok = false;
        f.controller->openFile (file, [&] (bool s) { ok = s; });

        expect (ok, "open succeeded");
        expectEquals (f.openedCount, 1, "onSessionOpened fired");
        expect (f.controller->currentSessionPath() == file, "path adopted");
        expect (! f.controller->isDirty(), "freshly opened session is clean");

        auto tracks = f.daw.getChildWithName (DawIDs::tracks);
        expectEquals (tracks.getNumChildren(), 1, "live model now holds the opened track");
        expectEquals ((int) tracks.getChild (0).getProperty (DawIDs::deckIndex), 3);
    }

    //==========================================================================
    void testOpenFailureLeavesModelUntouched()
    {
        beginTest ("a failed open never mutates the live model");

        Fixture f;
        DawState::ensureTrackForDeck (f.daw, 1);   // existing work
        const auto before = f.daw.createCopy();

        LoadResult lr;
        lr.error   = LoadError::CorruptFile;
        lr.message = "bad";
        f.serializer.nextLoad = lr;

        bool ok = true;
        f.controller->openFile (f.tmp.dir.getChildFile ("bad.soniksession"),
                                [&] (bool s) { ok = s; });

        expect (! ok, "open reports failure");
        expectEquals (f.errorCount, 1, "error surfaced");
        expect (f.daw.isEquivalentTo (before), "live model untouched");
        expect (! f.controller->hasPath(), "path not adopted");
    }

    //==========================================================================
    void testNewPromptBranches()
    {
        beginTest ("New on a dirty session honours Cancel / Don't Save / Save");

        // Cancel: aborts, model unchanged.
        {
            Fixture f;
            DawState::ensureTrackForDeck (f.daw, 0);
            f.mutate();
            f.promptChoice = UnsavedChoice::Cancel;

            bool settled = true;
            f.controller->newSession ([&] (bool s) { settled = s; });
            expect (! settled, "cancel aborts");
            expectEquals (f.newCount, 0, "no new model installed");
            expectEquals (f.daw.getChildWithName (DawIDs::tracks).getNumChildren(), 1,
                          "model retained");
        }

        // Don't Save: discards and installs an empty model.
        {
            Fixture f;
            DawState::ensureTrackForDeck (f.daw, 0);
            f.mutate();
            f.promptChoice = UnsavedChoice::DontSave;

            bool ok = false;
            f.controller->newSession ([&] (bool s) { ok = s; });
            expect (ok, "proceeds");
            expectEquals (f.newCount, 1, "empty model installed");
            expectEquals (f.daw.getChildWithName (DawIDs::tracks).getNumChildren(), 0,
                          "model emptied");
            expect (! f.controller->hasPath(), "path cleared");
            expect (! f.controller->isDirty());
        }

        // Save: writes first (path exists), then installs empty model.
        {
            Fixture f;
            f.chooserReturns = f.tmp.dir.getChildFile ("set.soniksession");
            f.controller->save();              // adopt a path so Save won't re-open chooser
            const int savesBefore = f.serializer.saveCount;
            f.mutate();
            f.promptChoice = UnsavedChoice::Save;

            bool ok = false;
            f.controller->newSession ([&] (bool s) { ok = s; });
            expect (ok, "save-then-new proceeds");
            expect (f.serializer.saveCount > savesBefore, "saved before discarding");
            expectEquals (f.newCount, 1, "new empty model installed");
        }
    }

    //==========================================================================
    void testRecentPromoteDedupCap()
    {
        beginTest ("recent list is most-recent-first, de-duplicated and capped at 10");

        Fixture f;

        // Load 12 distinct files; each open promotes to the recents.
        auto loadedDaw = DawState::createDawBranch();
        LoadResult lr; lr.error = LoadError::None; lr.document.daw = loadedDaw;
        f.serializer.nextLoad = lr;

        juce::Array<juce::File> files;
        for (int i = 0; i < 12; ++i)
        {
            auto file = f.tmp.dir.getChildFile ("s" + juce::String (i) + ".soniksession");
            files.add (file);
            f.controller->openFile (file);
        }

        auto recents = f.controller->recentSessions();
        expectEquals (recents.size(), SessionController::kMaxRecent, "capped at 10");
        expectEquals (recents[0], files[11].getFullPathName(), "most-recent first");
        expect (! recents.contains (files[0].getFullPathName()), "oldest beyond cap dropped");

        // Re-open an already-listed file: it is promoted, not duplicated.
        f.controller->openFile (files[5]);
        recents = f.controller->recentSessions();
        expectEquals (recents[0], files[5].getFullPathName(), "re-open promotes to front");
        int occurrences = 0;
        for (const auto& r : recents)
            if (r == files[5].getFullPathName()) ++occurrences;
        expectEquals (occurrences, 1, "no duplicate entry");
        expectEquals (recents.size(), SessionController::kMaxRecent, "still capped");

        f.controller->clearRecentSessions();
        expectEquals (f.controller->recentSessions().size(), 0, "clear empties the list");
    }
};

static SessionControllerTests sessionControllerTests;
