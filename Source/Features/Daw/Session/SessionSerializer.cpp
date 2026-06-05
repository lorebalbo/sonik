#include "SessionSerializer.h"

namespace Daw::Session
{
    //==========================================================================
    // Small node read/write helpers (keep buildSessionTree / documentFromTree
    // readable and symmetric).
    //==========================================================================
    namespace
    {
        juce::ValueTree makeMasterGridNode (const MasterGridRef& g)
        {
            juce::ValueTree mg (IDs::MASTER_GRID);
            mg.setProperty (IDs::bpm,                g.bpm,                                  nullptr);
            mg.setProperty (IDs::downbeatSample,     (juce::int64) g.downbeatSample,         nullptr);
            mg.setProperty (IDs::timeSigNumerator,   g.timeSigNumerator,                     nullptr);
            mg.setProperty (IDs::timeSigDenominator, g.timeSigDenominator,                   nullptr);
            mg.setProperty (IDs::playheadSample,     (juce::int64) g.playheadSample,         nullptr);
            mg.setProperty (IDs::loopStartSample,    (juce::int64) g.loopStartSample,        nullptr);
            mg.setProperty (IDs::loopEndSample,      (juce::int64) g.loopEndSample,          nullptr);
            mg.setProperty (IDs::loopEnabled,        g.loopEnabled,                          nullptr);
            return mg;
        }

        MasterGridRef readMasterGridNode (const juce::ValueTree& mg)
        {
            MasterGridRef g;
            if (! mg.isValid())
                return g;

            g.bpm                = (double)       mg.getProperty (IDs::bpm,                0.0);
            g.downbeatSample     = (juce::int64)  mg.getProperty (IDs::downbeatSample,     (juce::int64) 0);
            g.timeSigNumerator   = (int)          mg.getProperty (IDs::timeSigNumerator,   4);
            g.timeSigDenominator = (int)          mg.getProperty (IDs::timeSigDenominator, 4);
            g.playheadSample     = (juce::int64)  mg.getProperty (IDs::playheadSample,     (juce::int64) 0);
            g.loopStartSample    = (juce::int64)  mg.getProperty (IDs::loopStartSample,    (juce::int64) 0);
            g.loopEndSample      = (juce::int64)  mg.getProperty (IDs::loopEndSample,      (juce::int64) 0);
            g.loopEnabled        = (bool)         mg.getProperty (IDs::loopEnabled,        false);
            return g;
        }

        juce::ValueTree makeViewStateNode (const ViewState& v)
        {
            juce::ValueTree vs (IDs::VIEW_STATE);
            if (v.zoomSamplesPerPixel.has_value())
                vs.setProperty (IDs::zoomSamplesPerPixel, *v.zoomSamplesPerPixel, nullptr);
            if (v.scrollStartSample.has_value())
                vs.setProperty (IDs::scrollStartSample, (juce::int64) *v.scrollStartSample, nullptr);
            if (v.selectedClipId.isNotEmpty())
                vs.setProperty (IDs::selectedClipId, v.selectedClipId, nullptr);
            return vs;
        }

        ViewState readViewStateNode (const juce::ValueTree& vs)
        {
            ViewState v;
            if (! vs.isValid())
                return v;

            if (vs.hasProperty (IDs::zoomSamplesPerPixel))
                v.zoomSamplesPerPixel = (double) vs.getProperty (IDs::zoomSamplesPerPixel);
            if (vs.hasProperty (IDs::scrollStartSample))
                v.scrollStartSample = (juce::int64) vs.getProperty (IDs::scrollStartSample);
            v.selectedClipId = vs.getProperty (IDs::selectedClipId, juce::String()).toString();
            return v;
        }
    }

    //==========================================================================
    juce::File SessionSerializer::normaliseTarget (const juce::File& target)
    {
        if (target.getFileExtension().equalsIgnoreCase (kSessionFileExtension))
            return target;

        // withFileExtension replaces an existing (different) extension or appends
        // one when absent — never producing a double extension.
        return target.withFileExtension (kSessionFileExtension);
    }

    //==========================================================================
    juce::ValueTree SessionSerializer::buildSessionTree (const juce::ValueTree& daw,
                                                         const SessionMetadata& metadata) const
    {
        juce::ValueTree root (IDs::SONIK_SESSION);
        root.setProperty (IDs::schemaVersion,     kCurrentSchemaVersion,                       nullptr);
        root.setProperty (IDs::projectSampleRate, metadata.projectSampleRate,                  nullptr);
        root.setProperty (IDs::appVersion,        metadata.appVersion,                         nullptr);
        root.setProperty (IDs::savedAtUtc,        juce::Time::getCurrentTime().toISO8601 (true), nullptr);

        root.addChild (makeMasterGridNode (metadata.masterGrid), -1, nullptr);
        root.addChild (makeViewStateNode  (metadata.viewState),  -1, nullptr);

        // SOURCE_REFS: verbatim copy of the caller-supplied hint table, or an
        // empty container when none is provided.
        if (metadata.sourceRefs.isValid() && metadata.sourceRefs.hasType (IDs::SOURCE_REFS))
            root.addChild (metadata.sourceRefs.createCopy(), -1, nullptr);
        else
            root.addChild (juce::ValueTree (IDs::SOURCE_REFS), -1, nullptr);

        // The `daw` branch is persisted as a VERBATIM structural copy: every
        // lane / clip / automation node (and any unknown future node) is carried
        // through unchanged. The caller's live tree is never mutated.
        root.addChild (daw.createCopy(), -1, nullptr);

        return root;
    }

    //==========================================================================
    SessionDocument SessionSerializer::documentFromTree (const juce::ValueTree& sessionRoot)
    {
        SessionDocument d;
        d.projectSampleRate = (double) sessionRoot.getProperty (IDs::projectSampleRate,
                                                                DawState::kProjectSampleRate);
        d.masterGrid = readMasterGridNode (sessionRoot.getChildWithName (IDs::MASTER_GRID));
        d.viewState  = readViewStateNode  (sessionRoot.getChildWithName (IDs::VIEW_STATE));

        // Hand back standalone copies so the caller can swap them straight into
        // the live model without sharing structure with the parsed root.
        auto refs = sessionRoot.getChildWithName (IDs::SOURCE_REFS);
        d.sourceRefs = refs.isValid() ? refs.createCopy() : juce::ValueTree (IDs::SOURCE_REFS);

        auto daw = sessionRoot.getChildWithName (IDs::DAW());
        d.daw = daw.isValid() ? daw.createCopy() : juce::ValueTree();

        d.loadedFromVersion = (int) sessionRoot.getProperty (IDs::schemaVersion,
                                                             kCurrentSchemaVersion);
        return d;
    }

    //==========================================================================
    SaveError SessionSerializer::writeAtomically (const juce::File& target,
                                                  const juce::String& xml) const
    {
        // A TemporaryFile is created alongside `target` (same directory, same
        // volume) so the final move is an atomic same-volume rename. Until that
        // rename succeeds, any pre-existing `target` is never touched (§1.5.5).
        juce::TemporaryFile temp (target);

        if (! temp.getFile().replaceWithText (xml))
            return SaveError::IoFailure; // temp auto-deleted by dtor; target intact

        // Atomic-write integrity test seam: bail BEFORE the rename so a forced
        // mid-write failure leaves the original file byte-for-byte intact.
        if (simulateWriteFailureForTest)
            return SaveError::IoFailure;

        if (! temp.overwriteTargetFileWithTemporary())
            return SaveError::IoFailure; // rename failed; original still intact

        return SaveError::None;
    }

    //==========================================================================
    SaveResult SessionSerializer::save (const juce::ValueTree& daw,
                                        const SessionMetadata& metadata,
                                        const juce::File& target)
    {
        SaveResult r;

        if (! daw.isValid() || ! daw.hasType (DawIDs::Daw))
        {
            r.error   = SaveError::InvalidInput;
            r.message = "save() requires a valid Daw branch";
            return r;
        }

        const auto normalised = normaliseTarget (target);
        r.writtenPath = normalised;

        if (! normalised.getParentDirectory().createDirectory())
        {
            // createDirectory returns a Result; if the dir already exists it is
            // treated as success by JUCE. A hard failure here is an IO failure.
            if (! normalised.getParentDirectory().isDirectory())
            {
                r.error   = SaveError::IoFailure;
                r.message = "Destination directory could not be created";
                return r;
            }
        }

        const auto tree = buildSessionTree (daw, metadata);
        const auto xml  = tree.toXmlString();

        r.error = writeAtomically (normalised, xml);
        if (r.error != SaveError::None)
            r.message = "Atomic write failed; any existing session left intact";

        return r;
    }

    //==========================================================================
    LoadResult SessionSerializer::load (const juce::File& source) const
    {
        LoadResult r;

        if (! source.existsAsFile())
        {
            r.error   = LoadError::FileNotFound;
            r.message = "Session file does not exist";
            return r;
        }

        const auto xml = source.loadFileAsString();
        std::unique_ptr<juce::XmlElement> parsed (juce::parseXML (xml));

        if (parsed == nullptr)
        {
            r.error   = LoadError::CorruptFile;
            r.message = "Session file is not valid XML";
            return r;
        }

        auto tree = juce::ValueTree::fromXml (*parsed);

        if (! tree.isValid() || ! tree.hasType (IDs::SONIK_SESSION))
        {
            r.error   = LoadError::CorruptFile;
            r.message = "Session file is not a SONIK_SESSION document";
            return r;
        }

        if (! tree.hasProperty (IDs::schemaVersion))
        {
            r.error   = LoadError::CorruptFile;
            r.message = "Session file is missing its schemaVersion";
            return r;
        }

        const int version = (int) tree.getProperty (IDs::schemaVersion, 0);

        if (version <= 0)
        {
            r.error   = LoadError::CorruptFile;
            r.message = "Session file has an invalid schemaVersion";
            return r;
        }

        if (version > kCurrentSchemaVersion)
        {
            r.error   = LoadError::UnsupportedFutureVersion;
            r.message = "Session was written by a newer build (schemaVersion "
                          + juce::String (version) + " > "
                          + juce::String (kCurrentSchemaVersion) + ")";
            return r;
        }

        if (! sessionMigrator.canMigrate (version, kCurrentSchemaVersion))
        {
            r.error   = LoadError::CorruptFile;
            r.message = "No migration path from schemaVersion " + juce::String (version);
            return r;
        }

        bool migrated = false;
        tree = sessionMigrator.migrate (tree, version, kCurrentSchemaVersion, migrated);

        if (! migrated)
        {
            r.error   = LoadError::CorruptFile;
            r.message = "Migration failed";
            return r;
        }

        r.document = documentFromTree (tree);
        r.document.loadedFromVersion = version;
        return r;
    }
}
