#pragma once
//==============================================================================
// PRD-0096: SessionController — owns the Save / Save As / Open / New session
// lifecycle for the DAW, tracks the unsaved-changes ("dirty") state, guards
// every destructive transition with a Save / Don't Save / Cancel prompt, and
// maintains the de-duplicated, capped Recent Sessions list.
//
// The controller is GUI-agnostic: every platform/UI concern (the FileChooser,
// the modal prompt, the error dialog, the DAW UI rebuild + view-state restore,
// and the window-title update) is injected as a std::function `Host` hook, so
// the whole lifecycle is unit-testable headless with a fake serializer and an
// in-memory PropertiesFile (§1.4). It depends only on juce_core /
// juce_data_structures + the PRD-0095 serializer and PRD-0083 UndoManager.
//
// Dirty state (§1.5.1): the controller captures a lightweight structural
// snapshot of the `daw` model at each Save / Open / New and reports
// isDirty() == ! liveDaw.isEquivalentTo (savedSnapshot). juce::UndoManager
// (PRD-0083) exposes no public monotonic undo-position accessor in this JUCE
// version, so rather than adding one (or latching a naive boolean listener that
// never un-latches on undo), the snapshot comparison is used: it correctly
// reports CLEAN after the user mutates and then undoes back to the saved point,
// which is exactly the property the PRD requires.
//
// THREADING: message thread only. File I/O happens inside the injected
// serializer (off the audio thread); no processBlock path is touched.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <functional>

#include "SessionSerializer.h"

namespace Daw::Session
{
    enum class UnsavedChoice { Save, DontSave, Cancel };
    enum class ChooserPurpose { SaveAs, Open };

    class SessionController
    {
    public:
        //----------------------------------------------------------------------
        // Injected host hooks. Async hooks (chooseFile / promptUnsavedChanges)
        // deliver their result via a callback so the controller works equally
        // with a real modal FileChooser and a deterministic test fake.
        //----------------------------------------------------------------------
        struct Host
        {
            // Gather master-grid / view-state / source-ref metadata from the live
            // model + UI at the moment of a save.
            std::function<SessionMetadata()> captureMetadata;

            // A session was opened: rebuild the DAW UI from the document and
            // restore its persisted view state (§1.4 / §1.5.5).
            std::function<void (const SessionDocument&)> onSessionOpened;

            // A fresh empty session was installed (New): rebuild the empty UI.
            std::function<void()> onNewSession;

            // Async native file chooser. The implementation calls `onChosen` with
            // an invalid juce::File when the user cancels.
            std::function<void (ChooserPurpose purpose,
                                juce::File suggestedDirectory,
                                juce::String suggestedName,
                                std::function<void (juce::File)> onChosen)> chooseFile;

            // Async Save / Don't Save / Cancel modal.
            std::function<void (std::function<void (UnsavedChoice)> onChoice)> promptUnsavedChanges;

            // Monochrome error dialog (e.g. unreadable / corrupt / future-version).
            std::function<void (const juce::String& title, const juce::String& message)> showError;

            // The title and/or dirty marker changed; the UI + window title update.
            std::function<void (const juce::String& displayTitle, bool dirty)> onTitleChanged;
        };

        static constexpr int kMaxRecent = 10;
        static const char*   kRecentSessionsKey;  // PropertiesFile key

        SessionController (juce::ValueTree liveDawBranch,
                           SessionSerializer& serializer,
                           juce::UndoManager& undoManager,
                           juce::PropertiesFile& properties,
                           Host host);

        //----------------------------------------------------------------------
        // Lifecycle actions. `onDone(success)` (optional) fires when the action
        // (including any async dialog) settles; a cancelled transition reports
        // success == false.
        //----------------------------------------------------------------------
        void save     (std::function<void (bool)> onDone = {});
        void saveAs   (std::function<void (bool)> onDone = {});
        void open     (std::function<void (bool)> onDone = {});   // dirty-guarded
        void openFile (const juce::File& file, std::function<void (bool)> onDone = {}); // Open Recent
        void newSession (std::function<void (bool)> onDone = {}); // dirty-guarded

        // Application-quit guard: cb(true) => quit may proceed.
        void confirmQuit (std::function<void (bool mayQuit)> cb);

        //----------------------------------------------------------------------
        // State.
        //----------------------------------------------------------------------
        bool       isDirty() const noexcept;
        bool       hasPath() const noexcept             { return currentPath != juce::File(); }
        juce::File currentSessionPath() const noexcept  { return currentPath; }

        // Bare display title: "Untitled" or the session file name.
        juce::String displayTitle() const;
        // Title with the unsaved marker: "My Set.soniksession •".
        juce::String titleWithDirtyMarker() const;

        //----------------------------------------------------------------------
        // Recent sessions (most-recent-first, de-duplicated, capped at kMaxRecent).
        //----------------------------------------------------------------------
        juce::StringArray recentSessions() const;
        void clearRecentSessions();

        // Re-baselines dirty tracking to the current undo position (used after a
        // successful save/open/new). Public for hosts that mutate the model
        // out-of-band and need to re-sync.
        void markSavedBaseline();

    private:
        void doSaveTo (const juce::File& target, std::function<void (bool)> onDone);
        void replaceLiveModel (const juce::ValueTree& loadedDaw);
        void installEmptyModel();
        void promptIfDirtyThen (std::function<void()> proceed,
                                std::function<void (bool)> onSettled);
        void promoteRecent (const juce::File& file);
        void writeRecents (const juce::StringArray& list);
        juce::File suggestedSaveDirectory() const;
        void notifyTitle();

        juce::ValueTree       liveDaw;
        SessionSerializer&    serializer;
        juce::UndoManager&    undo;
        juce::PropertiesFile& properties;
        Host                  host;
        juce::File            currentPath;        // invalid => Untitled
        juce::ValueTree       savedSnapshot;      // model content at the last save/open/new

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionController)
    };
}
