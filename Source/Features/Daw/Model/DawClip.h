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
#include <cmath>

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
    double       sourceBpm           { 0.0 };  // clip's original (native) BPM; 0 => no stretch
    bool         keyLock             { false }; // deck key lock at capture => pitch-preserved stretch

    /** Cropped source span in source samples (before any tempo stretch). */
    std::int64_t sourceSpanSamples() const noexcept
    {
        return sourceEndSample - sourceStartSample;
    }

    /** Timeline length once the clip is time-stretched from its original BPM to
        `masterBpm`. With no known source BPM (or no master) the mapping is 1:1.
        128-BPM clip on a 140 master => span * 128/140 (shorter, plays faster). */
    std::int64_t timelineLengthSamples (double masterBpm = 0.0) const noexcept
    {
        const std::int64_t span = sourceEndSample - sourceStartSample;
        if (sourceBpm <= 0.0 || masterBpm <= 0.0)
            return span;
        return static_cast<std::int64_t> (std::llround (static_cast<double> (span)
                                                        * (sourceBpm / masterBpm)));
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
        node.setProperty (DawClipIDs::sourceStartSample,   (juce::int64) clip.sourceStartSample,   nullptr);
        node.setProperty (DawClipIDs::sourceEndSample,     (juce::int64) clip.sourceEndSample,     nullptr);
        node.setProperty (DawClipIDs::timelineStartSample, (juce::int64) clip.timelineStartSample, nullptr);
        node.setProperty (DawClipIDs::sourceLengthSamples, (juce::int64) clip.sourceLengthSamples, nullptr);
        node.setProperty (DawClipIDs::gainDb,              clip.gainDb,                  nullptr);
        node.setProperty (DawClipIDs::sourceBpm,           clip.sourceBpm,               nullptr);
        node.setProperty (DawClipIDs::keyLock,             clip.keyLock,                 nullptr);
        return node;
    }

    static DawClip fromValueTree (const juce::ValueTree& node)
    {
        DawClip clip;
        clip.clipId              = juce::Uuid (node.getProperty (DawClipIDs::clipId).toString());
        clip.laneId              = juce::Uuid (node.getProperty (DawClipIDs::laneId).toString());
        clip.sourceFileId        = node.getProperty (DawClipIDs::sourceFileId).toString();
        // Go through juce::int64 explicitly: juce::var exposes an operator int64
        // but no operator for the platform's std::int64_t when that is a distinct
        // type (long vs long long on LP64 Linux), so a direct static_cast would be
        // ambiguous there. This is a no-op on macOS where the two types coincide.
        clip.sourceStartSample   = static_cast<std::int64_t> (static_cast<juce::int64> (node.getProperty (DawClipIDs::sourceStartSample)));
        clip.sourceEndSample     = static_cast<std::int64_t> (static_cast<juce::int64> (node.getProperty (DawClipIDs::sourceEndSample)));
        clip.timelineStartSample = static_cast<std::int64_t> (static_cast<juce::int64> (node.getProperty (DawClipIDs::timelineStartSample)));
        clip.sourceLengthSamples = static_cast<std::int64_t> (static_cast<juce::int64> (node.getProperty (DawClipIDs::sourceLengthSamples)));
        clip.gainDb              = static_cast<float> (static_cast<double> (node.getProperty (DawClipIDs::gainDb)));
        clip.sourceBpm           = static_cast<double> (node.getProperty (DawClipIDs::sourceBpm, 0.0));
        clip.keyLock             = static_cast<bool> (node.getProperty (DawClipIDs::keyLock, false));
        return clip;
    }
};
