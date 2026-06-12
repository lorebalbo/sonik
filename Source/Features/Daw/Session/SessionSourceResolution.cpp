#include "SessionSourceResolution.h"

#include "SessionSchema.h"
#include "../State/DawState.h"
#include "../State/DawClipModel.h"
#include "../Playback/ArrangementRecompileTrigger.h"
#include "../../Deck/Database/TrackDatabase.h"
#include "../../StemSeparation/StemSeparationManager.h"
#include "../../StemSeparation/StemCache.h"
#include "../../StemSeparation/StemData.h"

#include <sqlite3.h>

namespace Daw::Session
{
    //==========================================================================
    // Strategy construction. The three source kinds bind to:
    //   libraryById  -> TrackDatabase::getFilePathForContentHash (the DB reverse
    //                   lookup EPIC-0010 already uses; sourceFileId IS the
    //                   content hash in this codebase, so the DB id lookup and
    //                   content-hash match are the same authoritative query).
    //   pathExists   -> juce::File::existsAsFile (the resolver's default).
    //   hashMatch    -> the same DB lookup, tried last as a cheap re-check.
    //   stemByParentId -> the EPIC-0002 stem cache directory for the parent hash.
    //==========================================================================
    namespace
    {
        SourceIdResolver::Strategies makeStrategies (TrackDatabase& db)
        {
            SourceIdResolver::Strategies s;

            s.libraryById = [&db] (const juce::String& sourceFileId) -> std::optional<juce::String>
            {
                const auto path = db.getFilePathForContentHash (sourceFileId);
                if (path.isEmpty())
                    return std::nullopt;
                return path;
            };

            // pathExists keeps the resolver's File::existsAsFile default.

            s.hashMatch = [&db] (const juce::String& sourceFileId) -> std::optional<juce::String>
            {
                const auto path = db.getFilePathForContentHash (sourceFileId);
                if (path.isEmpty())
                    return std::nullopt;
                return path;
            };

            s.stemByParentId = [] (const juce::String& parentTrackId) -> std::optional<juce::String>
            {
                // A StemCache artefact is readable iff at least the vocals stem
                // WAV exists for the parent hash; ClipSourceResolver reads the
                // four stems from this same directory.
                const auto vocals = StemCache::getCacheDirectory()
                                        .getChildFile (parentTrackId)
                                        .getChildFile (StemData::stemFilename (StemData::Vocals));
                if (vocals.existsAsFile())
                    return vocals.getFullPathName();
                return std::nullopt;
            };

            return s;
        }

        // A clip's lane kind determines whether its sourceFileId is consumed as
        // the original library/external file or as a derived stem artefact. A
        // Vocal/Instrumental lane => StemCache; an Original lane => Library/
        // External (decided by whether the DB knows the hash).
        bool laneIsStem (const juce::String& laneKind)
        {
            return laneKind == "Vocal" || laneKind == "Instrumental";
        }
    }

    //==========================================================================
    SessionSourceResolution::SessionSourceResolution (juce::ValueTree dawBranch,
                                                      TrackDatabase& database,
                                                      StemSeparationManager& stemManager)
        : dawBranch_ (std::move (dawBranch)),
          database_ (database),
          stemManager_ (stemManager),
          resolver_ (makeStrategies (database))
    {
    }

    //==========================================================================
    void SessionSourceResolution::runResolutionPass (const juce::ValueTree& sourceRefs)
    {
        // The hint table may be empty (sessions saved before SOURCE_REFS were
        // populated, or a brand-new session). When it is, synthesise hints from
        // the live tree so kind classification still distinguishes stem lanes.
        juce::ValueTree hints = sourceRefs;
        if (! hints.isValid() || hints.getNumChildren() == 0)
            hints = buildSourceRefs();

        sources_ = resolver_.resolve (dawBranch_, hints);

        const auto missing = SourceIdResolver::missingOnly (sources_);
        juce::StringArray missingIds;
        for (const auto& m : missing)
            missingIds.add (m.sourceFileId);

        applyFlagsToClips (missingIds);
        requestRecompile();
    }

    void SessionSourceResolution::clearAndReresolve()
    {
        runResolutionPass (juce::ValueTree (IDs::SOURCE_REFS));
    }

    //==========================================================================
    bool SessionSourceResolution::areAllSourcesResolved() const noexcept
    {
        return SourceIdResolver::allResolved (sources_);
    }

    int SessionSourceResolution::missingSourceCount() const noexcept
    {
        return SourceIdResolver::missingCount (sources_);
    }

    std::vector<ResolvedSource> SessionSourceResolution::missingSources() const
    {
        return SourceIdResolver::missingOnly (sources_);
    }

    juce::StringArray SessionSourceResolution::missingClipIds() const
    {
        juce::StringArray out;

        // Collect the set of Missing source ids.
        juce::StringArray missingIds;
        for (const auto& s : sources_)
            if (s.state == ResolutionState::Missing)
                missingIds.add (s.sourceFileId);

        if (missingIds.isEmpty())
            return out;

        auto tracks = dawBranch_.getChildWithName (DawIDs::tracks);
        for (int t = 0; t < tracks.getNumChildren(); ++t)
        {
            auto lanes = tracks.getChild (t).getChildWithName (DawIDs::lanes);
            for (int l = 0; l < lanes.getNumChildren(); ++l)
            {
                auto clips = lanes.getChild (l).getChildWithName (DawIDs::clips);
                for (int c = 0; c < clips.getNumChildren(); ++c)
                {
                    auto clip = clips.getChild (c);
                    const auto id = clip.getProperty (DawClipIDs::sourceFileId).toString();
                    if (missingIds.contains (id))
                        out.add (clip.getProperty (DawClipIDs::clipId).toString());
                }
            }
        }

        return out;
    }

    //==========================================================================
    bool SessionSourceResolution::relocateSource (const juce::String& sourceFileId,
                                                  const juce::File& replacementFile,
                                                  bool* dedupRejected)
    {
        if (dedupRejected != nullptr)
            *dedupRejected = false;

        if (! replacementFile.existsAsFile())
            return false;

        auto* src = findSource (sourceFileId);
        if (src == nullptr)
            return false;

        const auto newPath = replacementFile.getFullPathName();
        auto* handle = database_.getDbHandle();
        if (handle == nullptr)
            return false;

        // PRD-0039 canonical-path dedup: reject a file already bound to a
        // DIFFERENT library row (a different content hash). Reusing the same
        // uniqueness semantics keeps a single relocation code path.
        {
            sqlite3_stmt* dup = nullptr;
            bool isDuplicate = false;
            if (sqlite3_prepare_v2 (handle,
                    "SELECT 1 FROM library_tracks WHERE file_path=? AND content_hash<>? LIMIT 1;",
                    -1, &dup, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text (dup, 1, newPath.toRawUTF8(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text (dup, 2, sourceFileId.toRawUTF8(), -1, SQLITE_TRANSIENT);
                isDuplicate = (sqlite3_step (dup) == SQLITE_ROW);
                sqlite3_finalize (dup);
            }

            if (isDuplicate)
            {
                if (dedupRejected != nullptr)
                    *dedupRejected = true;
                return false;
            }
        }

        // Canonical-path write, keyed by content hash (the source id). This binds
        // the new path to ALL clips referencing this source at once (§1.5.6),
        // because clips reference by id and playback resolves the id -> path
        // through this same DB row. Preserves analysis data (only file_path /
        // is_missing change), mirroring PRD-0039's library relocate.
        {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2 (handle,
                    "UPDATE library_tracks SET file_path=?, is_missing=0 WHERE content_hash=?;",
                    -1, &stmt, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text (stmt, 1, newPath.toRawUTF8(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text (stmt, 2, sourceFileId.toRawUTF8(), -1, SQLITE_TRANSIENT);
                sqlite3_step (stmt);
                sqlite3_finalize (stmt);
            }
        }

        // Rebind in the resolver model and clear the flag on the source's clips.
        if (! resolver_.applyRelocation (*src, newPath))
            return false;

        clearFlagForSource (sourceFileId);
        requestRecompile();
        return true;
    }

    //==========================================================================
    bool SessionSourceResolution::reDeriveStems (const juce::String& sourceFileId,
                                                 std::function<void (bool success)> onComplete)
    {
        // A StemCache source re-derives from its parent track. The parent is the
        // same content hash (the original library track the stems came from). The
        // parent must resolve first (§1.5.5); otherwise the caller relocates it.
        const auto parentPath = database_.getFilePathForContentHash (sourceFileId);
        if (parentPath.isEmpty() || ! juce::File (parentPath).existsAsFile())
            return false; // parent missing -> UI relocates the parent first

        // Re-run EPIC-0002 separation on the existing background thread; on
        // completion (message thread) re-resolve + clear flags.
        stemManager_.startSeparationForFile (parentPath, sourceFileId,
            [this, sourceFileId, onDone = std::move (onComplete)] (bool success)
            {
                juce::MessageManager::callAsync (
                    [this, sourceFileId, onDone, success]
                    {
                        bool resolved = false;
                        if (success)
                            resolved = reresolveSource (sourceFileId);
                        if (onDone)
                            onDone (success && resolved);
                    });
            });

        return true;
    }

    bool SessionSourceResolution::reresolveSource (const juce::String& sourceFileId)
    {
        auto* src = findSource (sourceFileId);
        if (src == nullptr)
            return false;

        resolver_.resolveOne (*src);
        if (src->state != ResolutionState::Resolved)
            return false;

        clearFlagForSource (sourceFileId);
        requestRecompile();
        return true;
    }

    //==========================================================================
    juce::ValueTree SessionSourceResolution::buildSourceRefs() const
    {
        juce::ValueTree refs (IDs::SOURCE_REFS);
        juce::StringArray seen;

        auto tracks = dawBranch_.getChildWithName (DawIDs::tracks);
        for (int t = 0; t < tracks.getNumChildren(); ++t)
        {
            auto lanes = tracks.getChild (t).getChildWithName (DawIDs::lanes);
            for (int l = 0; l < lanes.getNumChildren(); ++l)
            {
                auto laneNode = lanes.getChild (l);
                const auto laneKind = laneNode.getProperty (DawIDs::laneKind).toString();
                auto clips = laneNode.getChildWithName (DawIDs::clips);

                for (int c = 0; c < clips.getNumChildren(); ++c)
                {
                    const auto id = clips.getChild (c)
                                        .getProperty (DawClipIDs::sourceFileId).toString();
                    if (id.isEmpty() || seen.contains (id))
                        continue;
                    seen.add (id);

                    juce::ValueTree ref (IDs::SOURCE_REF);
                    ref.setProperty (IDs::sourceFileId, id, nullptr);

                    // Kind: a stem lane => StemCache; otherwise Library if the DB
                    // knows the hash, else External.
                    const auto knownPath = database_.getFilePathForContentHash (id);
                    juce::String kind;
                    if (laneIsStem (laneKind))
                        kind = SourceKindStrings::kStemCache;
                    else
                        kind = knownPath.isNotEmpty() ? SourceKindStrings::kLibrary
                                                      : SourceKindStrings::kExternal;
                    ref.setProperty (IDs::sourceKind, kind, nullptr);

                    // Last-known path hint: the original file for library/external,
                    // or (for a stem) the parent track file so the batch step can
                    // surface a meaningful broken location.
                    ref.setProperty (IDs::lastKnownPath, knownPath, nullptr);

                    // Display name + parent: for a stem the parent IS the same
                    // content hash; the resolver falls back to the id when empty.
                    const juce::String display = knownPath.isNotEmpty()
                        ? juce::File (knownPath).getFileName()
                        : id;
                    ref.setProperty (IDs::displayName, display, nullptr);
                    if (laneIsStem (laneKind))
                        ref.setProperty ("parentTrackId", id, nullptr);

                    refs.addChild (ref, -1, nullptr);
                }
            }
        }

        return refs;
    }

    //==========================================================================
    void SessionSourceResolution::applyFlagsToClips (const juce::StringArray& missingIds)
    {
        auto tracks = dawBranch_.getChildWithName (DawIDs::tracks);
        for (int t = 0; t < tracks.getNumChildren(); ++t)
        {
            auto lanes = tracks.getChild (t).getChildWithName (DawIDs::lanes);
            for (int l = 0; l < lanes.getNumChildren(); ++l)
            {
                auto clips = lanes.getChild (l).getChildWithName (DawIDs::clips);
                for (int c = 0; c < clips.getNumChildren(); ++c)
                {
                    auto clip = clips.getChild (c);
                    const auto id = clip.getProperty (DawClipIDs::sourceFileId).toString();
                    const bool missing = missingIds.contains (id);

                    // Only write when the state actually changes so we do not
                    // dirty the undo manager / churn listeners needlessly. nullptr
                    // UndoManager: the flag is transient session state, never an
                    // undoable edit and never persisted to disk.
                    const bool current = static_cast<bool> (clip.getProperty (DawClipIDs::missingSource));
                    if (current != missing)
                        clip.setProperty (DawClipIDs::missingSource, missing, nullptr);
                }
            }
        }
    }

    void SessionSourceResolution::clearFlagForSource (const juce::String& sourceFileId)
    {
        auto tracks = dawBranch_.getChildWithName (DawIDs::tracks);
        for (int t = 0; t < tracks.getNumChildren(); ++t)
        {
            auto lanes = tracks.getChild (t).getChildWithName (DawIDs::lanes);
            for (int l = 0; l < lanes.getNumChildren(); ++l)
            {
                auto clips = lanes.getChild (l).getChildWithName (DawIDs::clips);
                for (int c = 0; c < clips.getNumChildren(); ++c)
                {
                    auto clip = clips.getChild (c);
                    if (clip.getProperty (DawClipIDs::sourceFileId).toString() == sourceFileId
                        && static_cast<bool> (clip.getProperty (DawClipIDs::missingSource)))
                        clip.setProperty (DawClipIDs::missingSource, false, nullptr);
                }
            }
        }
    }

    void SessionSourceResolution::requestRecompile()
    {
        if (recompileTrigger_ != nullptr)
            recompileTrigger_->requestRecompile();
    }

    ResolvedSource* SessionSourceResolution::findSource (const juce::String& sourceFileId)
    {
        for (auto& s : sources_)
            if (s.sourceFileId == sourceFileId)
                return &s;
        return nullptr;
    }

    const ResolvedSource* SessionSourceResolution::findSource (const juce::String& sourceFileId) const
    {
        for (const auto& s : sources_)
            if (s.sourceFileId == sourceFileId)
                return &s;
        return nullptr;
    }
}
