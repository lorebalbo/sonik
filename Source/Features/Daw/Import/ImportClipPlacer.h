#pragma once
//==============================================================================
// PRD-0098: ImportClipPlacer — turns decoded external sources into first-class
// DawClips on a target lane, as a single undoable transaction (PRD-0083).
//
// Each placed clip has its crop window initialised to the WHOLE file
// (cropStart = 0, cropEnd = sourceLengthSamples) and its source length in
// SESSION-RATE samples, so it is indistinguishable from a recorded clip and
// edits identically under EPIC-0010 (move / trim / un-crop / gain). The drop
// position is snapped to the active grid via an injected snap function
// (PRD-0074), and the registry is acquired/released so undo never strands a
// source still referenced elsewhere (§1.5.2).
//
// Pure logic over juce_core / juce_data_structures: no decoding, no audio
// thread. Message thread only.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <vector>

#include "ImportSource.h"

namespace Daw::Import
{
    class ImportClipPlacer
    {
    public:
        // Maps a raw timeline sample to a snapped one (grid snap on) or returns
        // it unchanged (snap off / override modifier held). PRD-0074.
        using SnapFn = std::function<std::int64_t (std::int64_t timelineSample)>;

        ImportClipPlacer (juce::ValueTree dawBranch,
                          juce::UndoManager& undoManager,
                          ImportSourceRegistry& registry);

        //----------------------------------------------------------------------
        // Places one imported source as a clip on `lane` at `dropTimelineSample`
        // (snapped). When `beginTransaction` is true a fresh undo transaction is
        // opened so a single Cmd-Z removes the clip; pass false to group several
        // placements (multi-file drop) into one transaction. Returns the created
        // clip node (invalid on failure, e.g. a degenerate zero-length source).
        //----------------------------------------------------------------------
        juce::ValueTree placeClip (juce::ValueTree lane,
                                   const ImportedSourceDescriptor& source,
                                   std::int64_t dropTimelineSample,
                                   SnapFn snap,
                                   bool beginTransaction = true);

        //----------------------------------------------------------------------
        // Places several sources as sequential, non-overlapping clips on one lane
        // starting at `dropTimelineSample` (snapped), each beginning where the
        // previous ends; if the drop area overlaps existing clips the batch is
        // appended after the last clip on the lane (§1.5.7). One undo transaction.
        //----------------------------------------------------------------------
        std::vector<juce::ValueTree> placeSequential (juce::ValueTree lane,
                                                      const std::vector<ImportedSourceDescriptor>& sources,
                                                      std::int64_t dropTimelineSample,
                                                      SnapFn snap);

        // The timeline sample at which the next clip on `lane` could be appended
        // without overlapping any existing clip (max clip end, or 0 if empty).
        static std::int64_t laneEndSample (const juce::ValueTree& lane);

    private:
        juce::ValueTree       daw;
        juce::UndoManager&    undo;
        ImportSourceRegistry& registry;
    };
}
