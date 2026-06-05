#pragma once
//==============================================================================
// PRD-0098: AudioFileImporter — the background import pipeline that turns an
// external audio file into a first-class DAW clip.
//
// Per import, on a BACKGROUND thread (juce::ThreadPool):
//   1. validate + decode the file, REUSING the PRD-0003 supported-format set
//      (MP3/FLAC/WAV/AIFF) and rejection rules (a single AudioFormatManager,
//      no second whitelist);
//   2. normalise channels to stereo-interleaved float (mono -> dual-mono,
//      multi -> L/R downmix), exactly like PRD-0003 step 8;
//   3. resample the buffer to the SESSION sample rate (PRD-0003 step 13 /
//      §1.5.5), so sourceLengthSamples is already in session-rate terms;
//   4. compute the content hash (same heuristic the deck loader uses) and build
//      an ImportedSourceDescriptor;
// then, on the MESSAGE thread:
//   5. register the source in the ref-counted ImportSourceRegistry (de-duping
//      byte-identical re-imports),
//   6. publish the baked buffer to the engine via the atomic-swap
//      ImportSourcePublisher (the clip becomes audible only here),
//   7. kick off PRD-0006 waveform generation keyed by the clip's sourceFileId,
//   8. place the clip(s) via ImportClipPlacer as ONE undo transaction.
//
// A "decoding..." placeholder id is reported the instant the import starts and
// cleared on success/failure so the lane can show a progressive affordance. A
// validation/decode failure reports a transient, monochrome error string and
// creates NO clip.
//
// AUDIO-THREAD CONTRACT: nothing here runs on the audio thread. Decode/resample/
// hash/waveform all run on the pool; placement + publish run on the message
// thread. The engine only ever sees the source through the atomic publish.
//==============================================================================

#include <functional>
#include <memory>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "ImportSource.h"
#include "ImportSourcePublisher.h"
#include "ImportClipPlacer.h"
#include "../../Waveform/WaveformAnalyzer.h"

class TrackDatabase;

namespace Daw::Import
{
    class AudioFileImporter
    {
    public:
        // A pending placement on a specific lane at a specific (already snapped or
        // raw) timeline sample. The SnapFn is captured per request so import
        // inherits the live grid/snap-toggle state of the drop (PRD-0074).
        struct Target
        {
            juce::ValueTree                lane;
            std::int64_t                   dropTimelineSample { 0 };
            ImportClipPlacer::SnapFn       snap;
        };

        // Host callbacks (all invoked on the MESSAGE thread).
        struct Callbacks
        {
            // The session/project sample rate every baked buffer is reconciled to.
            std::function<double()> getSessionSampleRate;

            // Shown a "decoding..." placeholder should appear / disappear at the
            // drop position for the given lane. token uniquely identifies the
            // in-flight import so begin/end pair up.
            std::function<void (juce::ValueTree lane,
                                std::int64_t dropSample,
                                int token)>           onDecodeBegan;
            std::function<void (int token)>            onDecodeEnded;

            // A transient, monochrome error notice (no clip created).
            std::function<void (const juce::String& message)> onError;

            // Fired after the clip(s) are placed (e.g. to force a recompile so the
            // engine picks up the freshly published source).
            std::function<void()>                      onClipsPlaced;
        };

        AudioFileImporter (ImportSourceRegistry& registry,
                           ImportSourcePublisher& publisher,
                           ImportClipPlacer& placer,
                           TrackDatabase& database,
                           Callbacks callbacks);

        ~AudioFileImporter();

        AudioFileImporter (const AudioFileImporter&)            = delete;
        AudioFileImporter& operator= (const AudioFileImporter&) = delete;

        // Begins importing a single file onto one lane (drag-drop single file or
        // menu import). Message thread.
        void importFile (const juce::File& file, Target target);

        // Begins importing several files onto ONE lane as sequential,
        // non-overlapping clips in the given order (§1.5.7 multi-file drop). The
        // batch decodes off-thread; placement is a single undo transaction.
        void importFiles (const juce::Array<juce::File>& files, Target target);

        // The supported-extension set, delegated to the PRD-0003 decoder so there
        // is a single source of truth for "what Sonik can decode".
        static bool isSupportedExtension (const juce::String& extension);

        // The native-chooser wildcard ("*.wav;*.flac;...") for the menu import.
        static juce::String supportedFormatsWildcard();

    private:
        class ImportJob;

        // The product of decoding one file on the background thread.
        struct DecodeResult
        {
            bool                     ok { false };
            juce::String             errorMessage;
            ImportedSourceDescriptor descriptor;
            AudioBufferHolder::Ptr   bakedBuffer;   // session-rate stereo float
        };

        // Decodes + resamples + hashes one file (background thread). Pure: no
        // model mutation, no publish.
        static DecodeResult decodeOne (juce::AudioFormatManager& formatManager,
                                       const juce::File& file,
                                       double sessionSampleRate);

        // Content-hash heuristic (same as AudioFileLoader: MD5 of first 64 KB +
        // file size) so imported files de-dup consistently with library tracks.
        static juce::String computeContentHash (const juce::File& file);

        // Message-thread finalisation for a completed batch: register, publish,
        // analyse, place. `targetDrop`/`snap` come from the originating Target.
        void finalisePlacement (std::vector<DecodeResult> results,
                                juce::ValueTree lane,
                                std::int64_t dropSample,
                                ImportClipPlacer::SnapFn snap,
                                int token);

        ImportSourceRegistry&   registry_;
        ImportSourcePublisher&  publisher_;
        ImportClipPlacer&       placer_;
        WaveformAnalyzer        waveformAnalyzer_;
        Callbacks               callbacks_;

        juce::ThreadPool        threadPool_ { 2 };
        std::atomic<int>        nextToken_  { 1 };
    };
}
