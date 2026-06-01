#pragma once
//==============================================================================
// EPIC-0010: ClipSourceResolver — maps a (contentHash, laneKind) pair to a
// freshly-opened juce::AudioFormatReader for the underlying source audio.
//
//   - "Original"      → the original library track file (DB reverse lookup).
//   - "Vocal"         → <stemCache>/<hash>/vocals.wav
//   - "Instrumental"  → sum of drums.wav + bass.wav + other.wav (SummingReader)
//
// Returns nullptr when the source cannot be located (missing file / stems not
// yet separated); callers treat nullptr as "play silence for this clip".
//
// MESSAGE / BACKGROUND THREAD ONLY.
//==============================================================================

#include <memory>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "SummingAudioFormatReader.h"
#include "../../Deck/Database/TrackDatabase.h"
#include "../../StemSeparation/StemCache.h"
#include "../../StemSeparation/StemData.h"

namespace Daw
{

class ClipSourceResolver
{
public:
    ClipSourceResolver (TrackDatabase& database, juce::AudioFormatManager& formatManager)
        : database_ (database)
        , formatManager_ (formatManager)
    {
    }

    std::unique_ptr<juce::AudioFormatReader> resolve (const juce::String& contentHash,
                                                      const juce::String& laneKind) const
    {
        if (contentHash.isEmpty())
            return nullptr;

        if (laneKind == "Instrumental")
            return resolveInstrumental (contentHash);

        if (laneKind == "Vocal")
            return openFile (stemFile (contentHash, StemData::Vocals));

        // Default / "Original": original source file via DB reverse lookup.
        const juce::String path = database_.getFilePathForContentHash (contentHash);
        if (path.isEmpty())
            return nullptr;

        return openFile (juce::File (path));
    }

private:
    juce::File stemFile (const juce::String& contentHash, int stemIndex) const
    {
        return StemCache::getCacheDirectory()
                   .getChildFile (contentHash)
                   .getChildFile (StemData::stemFilename (stemIndex));
    }

    std::unique_ptr<juce::AudioFormatReader> openFile (const juce::File& file) const
    {
        if (! file.existsAsFile())
            return nullptr;

        return std::unique_ptr<juce::AudioFormatReader> (formatManager_.createReaderFor (file));
    }

    std::unique_ptr<juce::AudioFormatReader> resolveInstrumental (const juce::String& contentHash) const
    {
        std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
        for (int stem : { StemData::Drums, StemData::Bass, StemData::Other })
        {
            if (auto r = openFile (stemFile (contentHash, stem)))
                readers.push_back (std::move (r));
        }

        if (readers.empty())
            return nullptr;

        return std::make_unique<SummingAudioFormatReader> (std::move (readers));
    }

    TrackDatabase&            database_;
    juce::AudioFormatManager& formatManager_;
};

} // namespace Daw
