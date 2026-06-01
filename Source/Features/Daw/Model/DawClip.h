#pragma once
//==============================================================================
// PRD-0063: DawClip — the non-destructive clip value object.
//
// A clip *references* audio by stable id; it NEVER holds an audio buffer, a raw
// pointer to audio, or any JUCE UI reference. It is a copyable value type that
// depends only on juce_core / juce_data_structures (juce::Uuid, juce::ValueTree)
// and compiles/links with nothing else (AC: pure value object).
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>

#include "../State/DawClipModel.h"   // field identifiers + node-type id

struct DawClip
{
    juce::Uuid   clipId;                       // minted once, never content-derived
    juce::Uuid   laneId;                       // owning lane's stable id
    juce::String sourceFileId;                 // opaque stable id (lib id / stem key)
    std::int64_t sourceStartSample   { 0 };    // project-rate index into the source
    std::int64_t sourceEndSample     { 0 };    // project-rate index into the source
    std::int64_t timelineStartSample { 0 };    // project-rate index on the timeline
    std::int64_t sourceLengthSamples { 0 };    // total length of the referenced source
    float        gainDb              { 0.0f }; // per-clip trim in dB, unity default

    /** Timeline length is exactly the cropped source span (1:1, no stretch). */
    std::int64_t timelineLengthSamples() const noexcept
    {
        return sourceEndSample - sourceStartSample;
    }

    //--------------------------------------------------------------------------
    // Serialization — bit-exact round-trip for all eight fields. No filesystem
    // path is ever stored; only ids (canonical Uuid strings) and sample indices.
    //--------------------------------------------------------------------------
    static juce::ValueTree toValueTree (const DawClip& clip)
    {
        juce::ValueTree node (DawIDs::clip);
        node.setProperty (DawClipIDs::clipId,              clip.clipId.toString(),       nullptr);
        node.setProperty (DawClipIDs::laneId,              clip.laneId.toString(),       nullptr);
        node.setProperty (DawClipIDs::sourceFileId,        clip.sourceFileId,            nullptr);
        node.setProperty (DawClipIDs::sourceStartSample,   clip.sourceStartSample,       nullptr);
        node.setProperty (DawClipIDs::sourceEndSample,     clip.sourceEndSample,         nullptr);
        node.setProperty (DawClipIDs::timelineStartSample, clip.timelineStartSample,     nullptr);
        node.setProperty (DawClipIDs::sourceLengthSamples, clip.sourceLengthSamples,     nullptr);
        node.setProperty (DawClipIDs::gainDb,              clip.gainDb,                  nullptr);
        return node;
    }

    static DawClip fromValueTree (const juce::ValueTree& node)
    {
        DawClip clip;
        clip.clipId              = juce::Uuid (node.getProperty (DawClipIDs::clipId).toString());
        clip.laneId              = juce::Uuid (node.getProperty (DawClipIDs::laneId).toString());
        clip.sourceFileId        = node.getProperty (DawClipIDs::sourceFileId).toString();
        clip.sourceStartSample   = static_cast<std::int64_t> (node.getProperty (DawClipIDs::sourceStartSample));
        clip.sourceEndSample     = static_cast<std::int64_t> (node.getProperty (DawClipIDs::sourceEndSample));
        clip.timelineStartSample = static_cast<std::int64_t> (node.getProperty (DawClipIDs::timelineStartSample));
        clip.sourceLengthSamples = static_cast<std::int64_t> (node.getProperty (DawClipIDs::sourceLengthSamples));
        clip.gainDb              = static_cast<float> (static_cast<double> (node.getProperty (DawClipIDs::gainDb)));
        return clip;
    }
};
