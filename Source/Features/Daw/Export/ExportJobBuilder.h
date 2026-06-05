#pragma once
//==============================================================================
// PRD-0101: ExportJobBuilder — the PRODUCTION glue that turns the live `daw`
// ValueTree into an ExportRunner::Job (a compiled ArrangementSnapshot at the
// chosen export rate + a per-clip ReaderProvider wrapping the EPIC-0010
// ClipSourceResolver). Message thread only; called by the dialog's buildJob hook.
//
// It adds NO render/encode logic: it reuses the EXISTING ArrangementCompiler
// (PRD-0079) to build the snapshot (so the offline render is identical to the
// live engine's view) and the EXISTING ClipSourceResolver (EPIC-0010) to open
// each clip's source reader. The only extra work is a sidecar hash table that
// lets the driver's ReaderProvider (keyed by the snapshot's 64-bit
// sourceFileId hash) recover the original source-id string + lane kind needed by
// the resolver — the compiler stores only the hash in the audio-thread ClipEvent.
//
// Missing-source clips (PRD-0097 `missingSource` flag) are EXCLUDED by the
// compiler already, so they never enter the snapshot; the dialog's PRD-0097 gate
// blocks the export up front, and this builder never silently substitutes them.
//==============================================================================

#include <functional>
#include <unordered_map>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "ExportOptions.h"
#include "ExportRunner.h"
#include "OfflineRenderDriver.h"
#include "../Playback/ArrangementCompiler.h"
#include "../Playback/ClipSourceResolver.h"
#include "../State/DawState.h"
#include "../State/DawClipModel.h"

namespace Daw::Export
{

class ExportJobBuilder
{
public:
    //--------------------------------------------------------------------------
    // @param dawBranch  The live "Daw" ValueTree branch (single source of truth).
    // @param database   The library DB (ClipSourceResolver reverse-lookup).
    // @param importPublisher  Optional PRD-0098 import publisher so imported clips
    //                  ("import:<hash>") resolve to their in-memory buffers.
    //--------------------------------------------------------------------------
    ExportJobBuilder (juce::ValueTree dawBranch,
                      TrackDatabase& database,
                      Daw::Import::ImportSourcePublisher* importPublisher = nullptr)
        : dawBranch_ (std::move (dawBranch)),
          database_ (database),
          importPublisher_ (importPublisher)
    {
        formatManager_.registerBasicFormats();
        sourceResolver_.setImportPublisher (importPublisher_);
    }

    //--------------------------------------------------------------------------
    // Build the Job for the given options. The snapshot is compiled at the export
    // sample rate (so positions are already at options.sampleRate and the driver
    // renders at that rate with no further conversion). The ReaderProvider opens
    // each source through the ClipSourceResolver, keyed back via the hash sidecar.
    //--------------------------------------------------------------------------
    ExportRunner::Job buildJob (const ExportOptions& options)
    {
        ExportRunner::Job job;
        job.options = options;

        // Build the hash -> (sourceId string, laneKind) sidecar by walking the
        // tree exactly as the compiler does (same djb2 hash), so the driver's
        // ReaderProvider can recover the resolver inputs from the ClipEvent hash.
        auto sidecar = std::make_shared<std::unordered_map<uint64_t, SourceRef>>();
        buildSidecar (*sidecar);

        // Compile the snapshot at the EXPORT rate. The handle resolver is a no-op
        // (-1): the offline driver assigns its own streamer slots from the
        // ReaderProvider, so the snapshot only needs geometry + the hash.
        const double scale = options.sampleRate / DawState::kProjectSampleRate;
        ArrangementCompiler compiler (
            [] (const ArrangementCompiler::ClipResolveRequest&) -> int32_t { return -1; },
            scale);
        compiler.compile (dawBranch_, job.snapshot);

        // ReaderProvider: map the ClipEvent's hash back to its source string +
        // lane, then resolve a fresh reader. nullptr => the driver renders that
        // clip as silence (defensive; the PRD-0097 gate already blocked Missing).
        auto& resolver = sourceResolver_;
        job.readerProvider =
            [sidecar, &resolver] (const ClipEvent& ev) -> std::unique_ptr<juce::AudioFormatReader>
            {
                auto it = sidecar->find (ev.sourceFileId);
                if (it == sidecar->end())
                    return nullptr;
                return resolver.resolve (it->second.sourceId, it->second.laneKind);
            };

        return job;
    }

private:
    struct SourceRef
    {
        juce::String sourceId;
        juce::String laneKind;
    };

    static uint64_t djb2 (const juce::String& s)
    {
        uint64_t h = 0;
        const auto* bytes = reinterpret_cast<const unsigned char*> (s.toRawUTF8());
        for (int i = 0; bytes[i] != 0; ++i)
            h = h * 31u + bytes[i];
        return h;
    }

    void buildSidecar (std::unordered_map<uint64_t, SourceRef>& out) const
    {
        if (! dawBranch_.isValid())
            return;

        auto tracksNode = dawBranch_.getChildWithName (DawIDs::tracks);
        if (! tracksNode.isValid())
            return;

        for (int t = 0; t < tracksNode.getNumChildren(); ++t)
        {
            auto trackNode = tracksNode.getChild (t);
            if (! trackNode.hasType (DawIDs::track)) continue;

            auto lanesNode = trackNode.getChildWithName (DawIDs::lanes);
            if (! lanesNode.isValid()) continue;

            for (int l = 0; l < lanesNode.getNumChildren(); ++l)
            {
                auto laneNode = lanesNode.getChild (l);
                if (! laneNode.hasType (DawIDs::lane)) continue;

                const juce::String laneKind =
                    laneNode.getProperty (DawIDs::laneKind).toString();

                auto clipsNode = laneNode.getChildWithName (DawIDs::clips);
                if (! clipsNode.isValid()) continue;

                for (int c = 0; c < clipsNode.getNumChildren(); ++c)
                {
                    auto clipNode = clipsNode.getChild (c);
                    if (! clipNode.hasType (DawIDs::clip)) continue;

                    // Skip Missing-source clips (excluded from the snapshot too).
                    if (static_cast<bool> (clipNode.getProperty (DawClipIDs::missingSource)))
                        continue;

                    const juce::String sourceId =
                        clipNode.getProperty (DawClipIDs::sourceFileId).toString();
                    if (sourceId.isEmpty())
                        continue;

                    out[djb2 (sourceId)] = { sourceId, laneKind };
                }
            }
        }
    }

    juce::ValueTree            dawBranch_;
    TrackDatabase&             database_;
    juce::AudioFormatManager   formatManager_;
    ClipSourceResolver         sourceResolver_ { database_, formatManager_ };
    Daw::Import::ImportSourcePublisher* importPublisher_ { nullptr };
};

} // namespace Daw::Export
