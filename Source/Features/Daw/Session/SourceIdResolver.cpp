#include "SourceIdResolver.h"

#include "../State/DawState.h"
#include "../Model/DawClip.h"

#include <unordered_map>

namespace Daw::Session
{
    SourceIdResolver::SourceIdResolver (Strategies s)
        : strategies (std::move (s))
    {
    }

    SourceKind SourceIdResolver::classifyKind (const juce::String& sourceFileId,
                                               const juce::String& kindHint)
    {
        if (kindHint == SourceKindStrings::kLibrary)   return SourceKind::Library;
        if (kindHint == SourceKindStrings::kExternal)  return SourceKind::External;
        if (kindHint == SourceKindStrings::kStemCache) return SourceKind::StemCache;

        // No hint: infer from the id prefix convention.
        if (sourceFileId.startsWith ("stem:")) return SourceKind::StemCache;
        if (sourceFileId.startsWith ("lib:"))  return SourceKind::Library;
        return SourceKind::External;
    }

    //==========================================================================
    void SourceIdResolver::resolveOne (ResolvedSource& src) const
    {
        src.state        = ResolutionState::Missing;
        src.resolvedPath = {};

        // StemCache resolves by re-deriving from its parent; the cache lookup is
        // keyed by the parent track id (§1.5.5).
        if (src.kind == SourceKind::StemCache)
        {
            if (strategies.stemByParentId != nullptr)
            {
                const auto parent = src.parentTrackId.isNotEmpty() ? src.parentTrackId
                                                                   : src.sourceFileId;
                if (auto p = strategies.stemByParentId (parent); p.has_value()
                        && strategies.pathExists (*p))
                {
                    src.state        = ResolutionState::Resolved;
                    src.resolvedPath = *p;
                }
            }
            return;
        }

        // (1) Library DB id lookup.
        if (strategies.libraryById != nullptr)
            if (auto p = strategies.libraryById (src.sourceFileId); p.has_value()
                    && strategies.pathExists (*p))
            {
                src.state        = ResolutionState::Resolved;
                src.resolvedPath = *p;
                return;
            }

        // (2) Stored last-known path.
        if (src.lastKnownPath.isNotEmpty() && strategies.pathExists (src.lastKnownPath))
        {
            src.state        = ResolutionState::Resolved;
            src.resolvedPath = src.lastKnownPath;
            return;
        }

        // (3) Content-hash match against the library (most expensive, last).
        if (strategies.hashMatch != nullptr)
            if (auto p = strategies.hashMatch (src.sourceFileId); p.has_value()
                    && strategies.pathExists (*p))
            {
                src.state        = ResolutionState::Resolved;
                src.resolvedPath = *p;
            }
    }

    //==========================================================================
    std::vector<ResolvedSource> SourceIdResolver::resolve (const juce::ValueTree& daw,
                                                           const juce::ValueTree& sourceRefs) const
    {
        // Build the hint table keyed by sourceFileId.
        struct Hint { juce::String kind, lastKnownPath, displayName, parentTrackId; };
        std::unordered_map<std::string, Hint> hints;

        if (sourceRefs.isValid())
            for (int i = 0; i < sourceRefs.getNumChildren(); ++i)
            {
                auto ref = sourceRefs.getChild (i);
                if (! ref.hasType (IDs::SOURCE_REF))
                    continue;

                const auto id = ref.getProperty (IDs::sourceFileId).toString();
                if (id.isEmpty())
                    continue;

                Hint h;
                h.kind          = ref.getProperty (IDs::sourceKind).toString();
                h.lastKnownPath = ref.getProperty (IDs::lastKnownPath).toString();
                h.displayName   = ref.getProperty (IDs::displayName).toString();
                h.parentTrackId = ref.getProperty ("parentTrackId").toString();
                hints[id.toStdString()] = std::move (h);
            }

        // Walk every clip, collecting distinct sourceFileIds with clip counts and
        // preserving first-seen order for a stable UI.
        std::vector<ResolvedSource> result;
        std::unordered_map<std::string, std::size_t> indexById;

        auto tracks = daw.getChildWithName (DawIDs::tracks);
        for (int t = 0; t < tracks.getNumChildren(); ++t)
        {
            auto lanes = tracks.getChild (t).getChildWithName (DawIDs::lanes);
            for (int l = 0; l < lanes.getNumChildren(); ++l)
            {
                auto clips = lanes.getChild (l).getChildWithName (DawIDs::clips);
                for (int c = 0; c < clips.getNumChildren(); ++c)
                {
                    const auto id = clips.getChild (c)
                                        .getProperty (DawClipIDs::sourceFileId).toString();
                    if (id.isEmpty())
                        continue;

                    auto it = indexById.find (id.toStdString());
                    if (it != indexById.end())
                    {
                        ++result[it->second].clipCount;
                        continue;
                    }

                    ResolvedSource src;
                    src.sourceFileId = id;
                    src.clipCount    = 1;

                    if (auto h = hints.find (id.toStdString()); h != hints.end())
                    {
                        src.kind          = classifyKind (id, h->second.kind);
                        src.lastKnownPath = h->second.lastKnownPath;
                        src.displayName   = h->second.displayName.isNotEmpty()
                                                ? h->second.displayName : id;
                        src.parentTrackId = h->second.parentTrackId;
                    }
                    else
                    {
                        src.kind        = classifyKind (id, {});
                        src.displayName = id;
                    }

                    resolveOne (src);

                    indexById[id.toStdString()] = result.size();
                    result.push_back (std::move (src));
                }
            }
        }

        return result;
    }

    //==========================================================================
    bool SourceIdResolver::applyRelocation (ResolvedSource& src, const juce::String& path) const
    {
        if (path.isEmpty() || ! strategies.pathExists (path))
            return false;

        src.state         = ResolutionState::Resolved;
        src.resolvedPath  = path;
        src.lastKnownPath = path; // persisted as the new hint on the next save
        return true;
    }

    //==========================================================================
    bool SourceIdResolver::allResolved (const std::vector<ResolvedSource>& sources) noexcept
    {
        for (const auto& s : sources)
            if (s.state != ResolutionState::Resolved)
                return false;
        return true;
    }

    std::vector<ResolvedSource> SourceIdResolver::missingOnly (const std::vector<ResolvedSource>& sources)
    {
        std::vector<ResolvedSource> out;
        for (const auto& s : sources)
            if (s.state == ResolutionState::Missing)
                out.push_back (s);
        return out;
    }

    int SourceIdResolver::missingCount (const std::vector<ResolvedSource>& sources) noexcept
    {
        int n = 0;
        for (const auto& s : sources)
            if (s.state == ResolutionState::Missing)
                ++n;
        return n;
    }

    juce::StringArray SourceIdResolver::resolvedSourceIds (const std::vector<ResolvedSource>& sources)
    {
        juce::StringArray ids;
        for (const auto& s : sources)
            if (s.state == ResolutionState::Resolved)
                ids.add (s.sourceFileId);
        return ids;
    }
}
