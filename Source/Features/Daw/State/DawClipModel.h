#pragma once
//==============================================================================
// PRD-0063: DawClip serialization model — the property juce::Identifiers for
// every DawClip field, plus pure lookup helpers between a DawClip and a
// juce::ValueTree clip node. No audio buffers, no UI, no audio-thread access.
//
// The actual DawClip <-> ValueTree conversion lives as static members on
// DawClip (DawClip.h) and is driven by these identifiers.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "DawState.h"   // for DawIDs::clip / DawIDs::clips node types

namespace DawClipIDs
{
    #define DECLARE_CLIP_ID(name) const juce::Identifier name (#name);

    DECLARE_CLIP_ID (clipId)              // juce::Uuid, canonical string
    DECLARE_CLIP_ID (laneId)              // juce::Uuid of the owning lane, string
    DECLARE_CLIP_ID (sourceFileId)        // opaque stable id string (never a path)
    DECLARE_CLIP_ID (sourceStartSample)   // int64, project-rate index
    DECLARE_CLIP_ID (sourceEndSample)     // int64, project-rate index
    DECLARE_CLIP_ID (timelineStartSample) // int64, project-rate index
    DECLARE_CLIP_ID (sourceLengthSamples) // int64, total length of the source
    DECLARE_CLIP_ID (gainDb)              // float dB, default 0.0 (unity)

    #undef DECLARE_CLIP_ID
}

namespace DawClipModel
{
    /** True if the tree is a clip node of the expected type. */
    inline bool isClipNode (const juce::ValueTree& tree) noexcept
    {
        return tree.hasType (DawIDs::clip);
    }

    /** Finds a clip node within a "clips" container by its canonical Uuid id.
        Returns an invalid tree if not found. Message-thread only. */
    inline juce::ValueTree findClipNodeById (const juce::ValueTree& clipsContainer,
                                             const juce::Uuid& id)
    {
        const auto wanted = id.toString();
        for (int i = 0; i < clipsContainer.getNumChildren(); ++i)
        {
            auto c = clipsContainer.getChild (i);
            if (c.hasType (DawIDs::clip)
                && c.getProperty (DawClipIDs::clipId).toString() == wanted)
                return c;
        }
        return {};
    }
}
