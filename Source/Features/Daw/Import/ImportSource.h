#pragma once
//==============================================================================
// PRD-0098: ImportSource — the descriptor for an external audio file imported
// directly onto a DAW lane, plus the ref-counted registry that mints/de-dupes
// stable source ids and the session sample-rate reconciliation helper.
//
// An imported clip is a FIRST-CLASS timeline clip: it references its decoded
// source by a stable, opaque `sourceFileId` (keyed by content hash so identical
// re-imports collapse to one source, §1.5.2) exactly like a recorded clip, and
// re-resolves/persists identically through PRD-0095 / PRD-0097.
//
// This header is pure value/logic over juce_core / juce_data_structures: the
// actual decode/resample/waveform work (PRD-0003 / PRD-0006) is the caller's,
// and its result is handed in as an ImportedSourceDescriptor. Message/background
// thread only; no audio-thread path.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cmath>
#include <optional>
#include <unordered_map>

#include "../Session/SessionSchema.h"

namespace Daw::Import
{
    //--------------------------------------------------------------------------
    // Reconciles a native source length to the project sample rate (§1.5.5). The
    // decoded buffer is resampled at import, so the clip's session-rate length is
    // what the timeline math uses; the native rate is retained as display data.
    //--------------------------------------------------------------------------
    inline std::int64_t reconcileLengthToSessionRate (std::int64_t nativeLengthSamples,
                                                      double nativeSampleRate,
                                                      double sessionSampleRate) noexcept
    {
        if (nativeSampleRate <= 0.0 || sessionSampleRate <= 0.0)
            return nativeLengthSamples;
        if (juce::exactlyEqual (nativeSampleRate, sessionSampleRate))
            return nativeLengthSamples;

        return (std::int64_t) std::llround (
            (double) nativeLengthSamples * (sessionSampleRate / nativeSampleRate));
    }

    //--------------------------------------------------------------------------
    // The product of decoding + resampling an external file, ready to become a
    // clip. `sessionLengthSamples` is the length AFTER reconciliation to the
    // project rate (what crop/trim math in EPIC-0010 operates on).
    //--------------------------------------------------------------------------
    struct ImportedSourceDescriptor
    {
        juce::String sourceFileId;            // opaque, minted ("import:<hash>")
        juce::String contentHash;             // identity/relocation key (§1.5.2)
        juce::String lastKnownPath;           // relocation hint
        juce::String displayName;
        double       nativeSampleRate   { 0.0 };
        int          nativeChannelCount { 0 };
        std::int64_t nativeLengthSamples  { 0 };
        std::int64_t sessionLengthSamples { 0 };  // resampled length (session rate)

        // Mints the conventional opaque id from the content hash.
        static juce::String idForHash (const juce::String& contentHash)
        {
            return "import:" + contentHash;
        }
    };

    //==========================================================================
    // ImportSourceRegistry — ref-counted, content-hash-keyed registry of
    // imported sources. Guarantees:
    //   * identical files (same hash) collapse to ONE source (§1.5.2),
    //   * undoing an import does NOT evict a source still referenced by another
    //     clip (refcount > 0),
    //   * redo reattaches to the existing registered source rather than
    //     re-registering / re-decoding (§1.4).
    //==========================================================================
    class ImportSourceRegistry
    {
    public:
        ImportSourceRegistry() = default;

        // Registers (or reuses) a source for the descriptor's content hash and
        // returns its stable sourceFileId. Does NOT change the ref count — clip
        // placement acquires; undo releases.
        juce::String registerSource (ImportedSourceDescriptor desc)
        {
            const auto id = ImportedSourceDescriptor::idForHash (desc.contentHash);
            desc.sourceFileId = id;

            if (auto it = entries.find (id.toStdString()); it != entries.end())
                return id;                       // de-dupe: identical file already known

            entries.emplace (id.toStdString(), Entry { std::move (desc), 0 });
            return id;
        }

        void acquire (const juce::String& id)
        {
            if (auto it = entries.find (id.toStdString()); it != entries.end())
                ++it->second.refCount;
        }

        // Decrements the ref count. The descriptor is RETAINED (so a redo can
        // reattach without re-decoding); call evictUnreferenced() to reclaim.
        void release (const juce::String& id)
        {
            if (auto it = entries.find (id.toStdString()); it != entries.end())
                it->second.refCount = juce::jmax (0, it->second.refCount - 1);
        }

        int refCount (const juce::String& id) const
        {
            auto it = entries.find (id.toStdString());
            return it != entries.end() ? it->second.refCount : 0;
        }

        bool contains (const juce::String& id) const
        {
            return entries.find (id.toStdString()) != entries.end();
        }

        std::optional<ImportedSourceDescriptor> find (const juce::String& id) const
        {
            auto it = entries.find (id.toStdString());
            if (it == entries.end())
                return std::nullopt;
            return it->second.desc;
        }

        // Drops descriptors with no remaining references (optional reclamation).
        // Returns the number of entries evicted.
        int evictUnreferenced()
        {
            int evicted = 0;
            for (auto it = entries.begin(); it != entries.end();)
            {
                if (it->second.refCount <= 0)
                {
                    it = entries.erase (it);
                    ++evicted;
                }
                else
                    ++it;
            }
            return evicted;
        }

        int size() const { return (int) entries.size(); }

        // Emits a PRD-0095 SOURCE_REFS node (relocation hints) for every
        // registered source so imported clips persist + re-resolve identically
        // to recorded ones (PRD-0097).
        juce::ValueTree toSourceRefs() const
        {
            juce::ValueTree refs (Session::IDs::SOURCE_REFS);
            for (const auto& [key, e] : entries)
            {
                juce::ValueTree ref (Session::IDs::SOURCE_REF);
                ref.setProperty (Session::IDs::sourceFileId,  e.desc.sourceFileId,  nullptr);
                ref.setProperty (Session::IDs::lastKnownPath, e.desc.lastKnownPath, nullptr);
                ref.setProperty (Session::IDs::displayName,   e.desc.displayName,   nullptr);
                ref.setProperty (Session::IDs::sourceKind,
                                 Session::SourceKindStrings::kExternal, nullptr);
                refs.addChild (ref, -1, nullptr);
            }
            return refs;
        }

    private:
        struct Entry
        {
            ImportedSourceDescriptor desc;
            int                      refCount { 0 };
        };

        std::unordered_map<std::string, Entry> entries;
    };
}
