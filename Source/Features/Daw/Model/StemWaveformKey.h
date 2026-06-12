#pragma once
//==============================================================================
// Stem waveform-cache key helper.
//
// A DawClip references its source audio by a stable `sourceFileId` (the original
// track's content hash) regardless of which lane it lives on. The Original,
// Instrumental and Vocal lanes therefore share ONE sourceFileId even though each
// lane reproduces DIFFERENT audio (the original, the summed instrumental, or the
// isolated vocal).
//
// The PRD-0006 waveform cache is keyed by an opaque string, so we qualify the
// base id with the lane kind to give each stem lane its OWN cache entry:
//
//   Original      -> "<hash>"
//   Instrumental  -> "<hash>::instrumental"  (sum of drums + bass + other)
//   Vocal         -> "<hash>::vocals"
//
// The producer (stem-separation completion) analyses the matching stem audio and
// stores it under exactly these keys; the consumer (ClipBlock) resolves the same
// key so each clip draws the waveform of its own audio content.
//
// Pure string helper — no JUCE UI / audio-thread dependency.
//==============================================================================

#include <juce_core/juce_core.h>

namespace Daw
{
    /** Builds the waveform-cache key for a clip's audio content from its base
        sourceFileId and its lane kind ("Original" / "Instrumental" / "Vocal").
        An empty sourceFileId yields an empty key (resolves to a placeholder). */
    inline juce::String stemWaveformKey (const juce::String& sourceFileId,
                                         const juce::String& laneKind)
    {
        if (sourceFileId.isEmpty())
            return {};

        // Imported sources (PRD-0098 "import:<hash>") are not stem-separated, so a
        // clip placed on any lane reproduces the whole imported file — keep its own
        // peaks rather than qualifying it into a stem key that would never resolve.
        if (sourceFileId.startsWith ("import:"))
            return sourceFileId;

        if (laneKind == "Instrumental") return sourceFileId + "::instrumental";
        if (laneKind == "Vocal")        return sourceFileId + "::vocals";

        return sourceFileId; // Original / unknown -> the pristine source waveform
    }
}
