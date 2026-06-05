#include "ImportClipPlacer.h"

#include "../State/DawState.h"
#include "../Model/DawClip.h"

namespace Daw::Import
{
    ImportClipPlacer::ImportClipPlacer (juce::ValueTree dawBranch,
                                        juce::UndoManager& undoManager,
                                        ImportSourceRegistry& registry_)
        : daw      (std::move (dawBranch))
        , undo     (undoManager)
        , registry (registry_)
    {
    }

    std::int64_t ImportClipPlacer::laneEndSample (const juce::ValueTree& lane)
    {
        auto clips = lane.getChildWithName (DawIDs::clips);
        std::int64_t end = 0;

        for (int i = 0; i < clips.getNumChildren(); ++i)
        {
            auto clip = DawClip::fromValueTree (clips.getChild (i));
            end = juce::jmax (end, clip.timelineStartSample + clip.timelineLengthSamples());
        }
        return end;
    }

    juce::ValueTree ImportClipPlacer::placeClip (juce::ValueTree lane,
                                                 const ImportedSourceDescriptor& source,
                                                 std::int64_t dropTimelineSample,
                                                 SnapFn snap,
                                                 bool beginTransaction)
    {
        if (! lane.isValid() || source.sessionLengthSamples <= 0)
            return {};   // a degenerate/empty source never creates a clip

        auto clips = lane.getChildWithName (DawIDs::clips);
        if (! clips.isValid())
            return {};

        const std::int64_t snapped = snap ? snap (juce::jmax ((std::int64_t) 0, dropTimelineSample))
                                          : juce::jmax ((std::int64_t) 0, dropTimelineSample);

        DawClip clip;
        clip.clipId              = juce::Uuid();
        clip.laneId              = juce::Uuid (lane.getProperty (DawIDs::laneId).toString());
        clip.sourceFileId        = source.sourceFileId;
        clip.sourceStartSample   = 0;                              // crop = whole file
        clip.sourceEndSample     = source.sessionLengthSamples;    // crop = whole file
        clip.sourceLengthSamples = source.sessionLengthSamples;    // session-rate length
        clip.timelineStartSample = snapped;
        clip.gainDb              = 0.0f;

        if (beginTransaction)
            undo.beginNewTransaction ("Import Audio File");

        clips.addChild (DawClip::toValueTree (clip), -1, &undo);

        // Ref-count the source for this clip so undo can release it without
        // stranding a source still referenced elsewhere (§1.5.2).
        registry.acquire (source.sourceFileId);

        return DawClipModel::findClipNodeById (clips, clip.clipId);
    }

    std::vector<juce::ValueTree> ImportClipPlacer::placeSequential (
        juce::ValueTree lane,
        const std::vector<ImportedSourceDescriptor>& sources,
        std::int64_t dropTimelineSample,
        SnapFn snap)
    {
        std::vector<juce::ValueTree> placed;
        if (! lane.isValid() || sources.empty())
            return placed;

        std::int64_t base = snap ? snap (juce::jmax ((std::int64_t) 0, dropTimelineSample))
                                 : juce::jmax ((std::int64_t) 0, dropTimelineSample);

        // If the requested drop area overlaps existing material, append the whole
        // batch after the last clip on the lane (§1.5.7).
        const auto existingEnd = laneEndSample (lane);
        if (base < existingEnd)
            base = snap ? snap (existingEnd) : existingEnd;

        // One undo transaction for the entire multi-file drop.
        undo.beginNewTransaction ("Import Audio Files");

        std::int64_t cursor = base;
        for (const auto& src : sources)
        {
            if (src.sessionLengthSamples <= 0)
                continue;

            auto node = placeClip (lane, src, cursor, /*snap*/ nullptr, /*beginTransaction*/ false);
            if (node.isValid())
            {
                placed.push_back (node);
                cursor += src.sessionLengthSamples;   // next clip starts where this ends
            }
        }

        return placed;
    }
}
