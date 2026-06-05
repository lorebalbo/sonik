#include "SessionController.h"

#include "../State/DawState.h"

namespace Daw::Session
{
    const char* SessionController::kRecentSessionsKey = "recentDawSessions";

    SessionController::SessionController (juce::ValueTree liveDawBranch,
                                          SessionSerializer& serializer_,
                                          juce::UndoManager& undoManager,
                                          juce::PropertiesFile& properties_,
                                          Host host_)
        : liveDaw    (std::move (liveDawBranch))
        , serializer (serializer_)
        , undo       (undoManager)
        , properties (properties_)
        , host       (std::move (host_))
    {
        markSavedBaseline();
    }

    void SessionController::markSavedBaseline()
    {
        // Capture the model's content at this save/open/new point. A later
        // mutate-then-undo back to here compares equal => clean again (§1.5.1).
        savedSnapshot = liveDaw.createCopy();
        notifyTitle();
    }

    bool SessionController::isDirty() const noexcept
    {
        return ! liveDaw.isEquivalentTo (savedSnapshot);
    }

    //==========================================================================
    juce::String SessionController::displayTitle() const
    {
        return hasPath() ? currentPath.getFileName() : juce::String ("Untitled");
    }

    juce::String SessionController::titleWithDirtyMarker() const
    {
        return isDirty() ? displayTitle() + " " + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa2"))
                         : displayTitle();
    }

    void SessionController::notifyTitle()
    {
        if (host.onTitleChanged)
            host.onTitleChanged (displayTitle(), isDirty());
    }

    //==========================================================================
    // Recent sessions: stored as newline-joined absolute paths under a single
    // PropertiesFile key, most-recent-first, de-duplicated, capped.
    //==========================================================================
    juce::StringArray SessionController::recentSessions() const
    {
        juce::StringArray list;
        list.addLines (properties.getValue (kRecentSessionsKey, {}));
        list.removeEmptyStrings();
        return list;
    }

    void SessionController::writeRecents (const juce::StringArray& list)
    {
        properties.setValue (kRecentSessionsKey, list.joinIntoString ("\n"));
        properties.saveIfNeeded();
    }

    void SessionController::promoteRecent (const juce::File& file)
    {
        if (file == juce::File())
            return;

        auto list = recentSessions();
        const auto full = file.getFullPathName();
        list.removeString (full, true);     // de-duplicate (case-insensitive)
        list.insert (0, full);              // most-recent-first
        while (list.size() > kMaxRecent)
            list.remove (list.size() - 1);  // cap
        writeRecents (list);
    }

    void SessionController::clearRecentSessions()
    {
        writeRecents ({});
    }

    //==========================================================================
    juce::File SessionController::suggestedSaveDirectory() const
    {
        if (hasPath())
            return currentPath.getParentDirectory();

        auto recents = recentSessions();
        if (! recents.isEmpty())
        {
            juce::File mostRecent (recents[0]);
            if (mostRecent.getParentDirectory().isDirectory())
                return mostRecent.getParentDirectory();
        }

        return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    }

    //==========================================================================
    void SessionController::doSaveTo (const juce::File& target, std::function<void (bool)> onDone)
    {
        const auto metadata = host.captureMetadata ? host.captureMetadata() : SessionMetadata{};
        const auto result   = serializer.save (liveDaw, metadata, target);

        if (! result.ok())
        {
            if (host.showError)
                host.showError ("Could not save session", result.message);
            if (onDone) onDone (false);
            return;
        }

        currentPath = result.writtenPath;
        markSavedBaseline();             // clears the dirty flag + updates the title
        promoteRecent (currentPath);
        if (onDone) onDone (true);
    }

    void SessionController::save (std::function<void (bool)> onDone)
    {
        if (! hasPath())
        {
            saveAs (std::move (onDone));  // untitled session falls through to Save As
            return;
        }
        doSaveTo (currentPath, std::move (onDone));
    }

    void SessionController::saveAs (std::function<void (bool)> onDone)
    {
        if (! host.chooseFile)
        {
            if (onDone) onDone (false);
            return;
        }

        const auto suggestedName = (hasPath() ? currentPath.getFileNameWithoutExtension()
                                              : juce::String ("Untitled"))
                                   + kSessionFileExtension;

        host.chooseFile (ChooserPurpose::SaveAs, suggestedSaveDirectory(), suggestedName,
            [this, onDone] (juce::File chosen)
            {
                if (chosen == juce::File())     // cancelled
                {
                    if (onDone) onDone (false);
                    return;
                }
                doSaveTo (SessionSerializer::normaliseTarget (chosen), onDone);
            });
    }

    //==========================================================================
    void SessionController::openFile (const juce::File& file, std::function<void (bool)> onDone)
    {
        // Deserialize into a STAGING document first; the live model is swapped in
        // ONLY on full success, so a corrupt/future-version file can never
        // partially mutate the DJ's current work (§1.5.7).
        const auto result = serializer.load (file);

        if (! result.ok())
        {
            if (host.showError)
                host.showError ("Could not open session", result.message);
            if (onDone) onDone (false);     // live model left completely untouched
            return;
        }

        replaceLiveModel (result.document.daw);
        currentPath = file;
        undo.clearUndoHistory();            // the opened state is the new baseline
        markSavedBaseline();
        promoteRecent (file);

        if (host.onSessionOpened)
            host.onSessionOpened (result.document);   // UI rebuild + view-state restore

        if (onDone) onDone (true);
    }

    void SessionController::open (std::function<void (bool)> onDone)
    {
        promptIfDirtyThen (
            [this, onDone]
            {
                if (! host.chooseFile)
                {
                    if (onDone) onDone (false);
                    return;
                }
                host.chooseFile (ChooserPurpose::Open, suggestedSaveDirectory(), {},
                    [this, onDone] (juce::File chosen)
                    {
                        if (chosen == juce::File())
                        {
                            if (onDone) onDone (false);
                            return;
                        }
                        openFile (chosen, onDone);
                    });
            },
            onDone);
    }

    //==========================================================================
    void SessionController::newSession (std::function<void (bool)> onDone)
    {
        promptIfDirtyThen (
            [this, onDone]
            {
                installEmptyModel();
                currentPath = juce::File();
                undo.clearUndoHistory();
                markSavedBaseline();
                if (host.onNewSession)
                    host.onNewSession();
                if (onDone) onDone (true);
            },
            onDone);
    }

    //==========================================================================
    void SessionController::confirmQuit (std::function<void (bool)> cb)
    {
        if (! isDirty())
        {
            if (cb) cb (true);
            return;
        }

        if (! host.promptUnsavedChanges)
        {
            if (cb) cb (true);
            return;
        }

        host.promptUnsavedChanges (
            [this, cb] (UnsavedChoice choice)
            {
                switch (choice)
                {
                    case UnsavedChoice::Cancel:
                        if (cb) cb (false);     // veto the quit
                        break;
                    case UnsavedChoice::DontSave:
                        if (cb) cb (true);
                        break;
                    case UnsavedChoice::Save:
                        save ([cb] (bool ok) { if (cb) cb (ok); }); // quit only if save succeeded
                        break;
                }
            });
    }

    //==========================================================================
    void SessionController::promptIfDirtyThen (std::function<void()> proceed,
                                               std::function<void (bool)> onSettled)
    {
        if (! isDirty())
        {
            proceed();
            return;
        }

        if (! host.promptUnsavedChanges)
        {
            proceed();
            return;
        }

        host.promptUnsavedChanges (
            [this, proceed, onSettled] (UnsavedChoice choice)
            {
                switch (choice)
                {
                    case UnsavedChoice::Cancel:
                        if (onSettled) onSettled (false);   // abort with zero side effects
                        break;
                    case UnsavedChoice::DontSave:
                        proceed();
                        break;
                    case UnsavedChoice::Save:
                        // Save-then-act: a cancelled/failed save aborts the whole
                        // transition (§1.5.2) — never discard on a half save.
                        save ([proceed, onSettled] (bool ok)
                        {
                            if (ok) proceed();
                            else if (onSettled) onSettled (false);
                        });
                        break;
                }
            });
    }

    //==========================================================================
    void SessionController::replaceLiveModel (const juce::ValueTree& loadedDaw)
    {
        // copyPropertiesAndChildrenFrom replaces the target's content in place,
        // preserving the live branch NODE IDENTITY so existing ValueTree
        // listeners (the DAW UI) keep observing the same object.
        if (loadedDaw.isValid())
            liveDaw.copyPropertiesAndChildrenFrom (loadedDaw, nullptr);
        else
            installEmptyModel();
    }

    void SessionController::installEmptyModel()
    {
        liveDaw.copyPropertiesAndChildrenFrom (DawState::createDawBranch(), nullptr);
    }
}
