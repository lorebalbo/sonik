//==============================================================================
// PRD-0098: AudioFileImporter — background import pipeline implementation.
//
// See AudioFileImporter.h for the per-import contract. The decode/resample/
// normalise/hash work mirrors the PRD-0003 deck loader (AudioFileLoader) so
// there is a single behavioural source of truth for "what Sonik can decode" and
// how it is reconciled to the session rate; the model mutation (register +
// publish + analyse + place) runs on the message thread only.
//==============================================================================

#include "AudioFileImporter.h"

#include <cmath>

#include <juce_cryptography/juce_cryptography.h>

#include "../../Deck/Database/TrackDatabase.h"
#include "../../AudioEngine/AudioFileLoader.h"

namespace Daw::Import
{
    //==========================================================================
    // ImportJob — one background decode of a batch of files for a single target.
    // The decode is pure (no model mutation); the message-thread continuation
    // does the register/publish/analyse/place as one undo transaction.
    //==========================================================================
    class AudioFileImporter::ImportJob final : public juce::ThreadPoolJob
    {
    public:
        ImportJob (AudioFileImporter& owner,
                   juce::Array<juce::File> files,
                   juce::ValueTree lane,
                   std::int64_t dropSample,
                   ImportClipPlacer::SnapFn snap,
                   double sessionRate,
                   int token)
            : juce::ThreadPoolJob ("ImportJob"),
              owner_ (owner),
              files_ (std::move (files)),
              lane_ (std::move (lane)),
              dropSample_ (dropSample),
              snap_ (std::move (snap)),
              sessionRate_ (sessionRate),
              token_ (token)
        {
        }

        JobStatus runJob() override
        {
            // A fresh AudioFormatManager per job: thread-confined, registers the
            // SAME basic formats the PRD-0003 deck loader uses (single whitelist).
            juce::AudioFormatManager formatManager;
            formatManager.registerBasicFormats();

            std::vector<DecodeResult> results;
            results.reserve ((size_t) files_.size());

            juce::String firstError;

            for (const auto& file : files_)
            {
                if (shouldExit())
                    return jobHasFinished;

                auto r = decodeOne (formatManager, file, sessionRate_);
                if (r.ok)
                    results.push_back (std::move (r));
                else if (firstError.isEmpty())
                    firstError = r.errorMessage;
            }

            if (shouldExit())
                return jobHasFinished;

            // Hand the decoded batch back to the message thread for placement.
            auto* ownerPtr = &owner_;
            auto  lane      = lane_;
            auto  drop      = dropSample_;
            auto  snap      = snap_;
            auto  token     = token_;
            auto  error     = firstError;

            // Move the results into a shared payload so the async lambda is
            // copyable (juce::MessageManager::callAsync requires a copyable
            // std::function; DecodeResult holds a move-only ref-counted buffer).
            auto payload = std::make_shared<std::vector<DecodeResult>> (std::move (results));

            juce::MessageManager::callAsync (
                [ownerPtr, payload, lane, drop, snap, token, error]() mutable
                {
                    if (ownerPtr->callbacks_.onDecodeEnded)
                        ownerPtr->callbacks_.onDecodeEnded (token);

                    if (payload->empty())
                    {
                        if (ownerPtr->callbacks_.onError)
                            ownerPtr->callbacks_.onError (
                                error.isNotEmpty() ? error
                                                   : juce::String ("File could not be decoded"));
                        return;
                    }

                    ownerPtr->finalisePlacement (std::move (*payload), lane, drop, snap, token);
                });

            return jobHasFinished;
        }

    private:
        AudioFileImporter&        owner_;
        juce::Array<juce::File>   files_;
        juce::ValueTree           lane_;
        std::int64_t              dropSample_;
        ImportClipPlacer::SnapFn  snap_;
        double                    sessionRate_;
        int                       token_;
    };

    //==========================================================================
    AudioFileImporter::AudioFileImporter (ImportSourceRegistry& registry,
                                          ImportSourcePublisher& publisher,
                                          ImportClipPlacer& placer,
                                          TrackDatabase& database,
                                          Callbacks callbacks)
        : registry_ (registry),
          publisher_ (publisher),
          placer_ (placer),
          waveformAnalyzer_ (database),
          callbacks_ (std::move (callbacks))
    {
    }

    AudioFileImporter::~AudioFileImporter()
    {
        threadPool_.removeAllJobs (true, 5000);
    }

    //==========================================================================
    void AudioFileImporter::importFile (const juce::File& file, Target target)
    {
        juce::Array<juce::File> one;
        one.add (file);
        importFiles (one, std::move (target));
    }

    void AudioFileImporter::importFiles (const juce::Array<juce::File>& files, Target target)
    {
        if (files.isEmpty() || ! target.lane.isValid())
            return;

        const double sessionRate = (callbacks_.getSessionSampleRate
                                        ? callbacks_.getSessionSampleRate()
                                        : 0.0);

        const int token = nextToken_.fetch_add (1);

        // Surface the "decoding..." placeholder at the drop position immediately
        // (message thread) so the lane shows the import is in flight.
        if (callbacks_.onDecodeBegan)
            callbacks_.onDecodeBegan (target.lane, target.dropTimelineSample, token);

        auto* job = new ImportJob (*this,
                                   files,
                                   target.lane,
                                   target.dropTimelineSample,
                                   target.snap,
                                   sessionRate,
                                   token);
        threadPool_.addJob (job, true); // pool takes ownership
    }

    //==========================================================================
    bool AudioFileImporter::isSupportedExtension (const juce::String& extension)
    {
        // SINGLE SOURCE OF TRUTH (PRD-0098 §contract): the importer accepts
        // EXACTLY the PRD-0003 deck-loader set (MP3/FLAC/WAV/AIFF). We delegate to
        // AudioFileLoader::isSupportedExtension rather than re-deriving the set
        // from registerBasicFormats() — that would silently accept extra codecs
        // (e.g. Ogg Vorbis when JUCE_USE_OGGVORBIS is on) that the deck loader
        // rejects, creating two divergent whitelists. Normalise to a leading-dot,
        // lowercase form first.
        juce::String ext = extension.trim().toLowerCase();
        if (ext.isEmpty())
            return false;
        if (! ext.startsWithChar ('.'))
            ext = "." + ext;

        return AudioFileLoader::isSupportedExtension (ext);
    }

    juce::String AudioFileImporter::supportedFormatsWildcard()
    {
        // Build the native-chooser wildcard from the same canonical set the gate
        // accepts, so the file picker can never offer a format the importer (and
        // deck loader) would then reject.
        static const char* const exts[] = { ".mp3", ".flac", ".wav", ".aiff", ".aif" };

        juce::StringArray wildcards;
        for (auto* e : exts)
            wildcards.addIfNotAlreadyThere (juce::String ("*") + e);

        return wildcards.joinIntoString (";");
    }

    //==========================================================================
    // decodeOne — REUSES the PRD-0003 (AudioFileLoader) decode/validate/normalise/
    // resample/hash logic faithfully. There is no second whitelist and no second
    // downmix: validation goes through the same AudioFormatManager, channel
    // normalisation follows mono->dual-mono / multi->L/R, and resampling uses the
    // same LagrangeInterpolator step toward the target (session) rate.
    //==========================================================================
    AudioFileImporter::DecodeResult AudioFileImporter::decodeOne (
        juce::AudioFormatManager& formatManager,
        const juce::File& file,
        double sessionSampleRate)
    {
        DecodeResult out;

        // ---- 1. Validate (mirrors AudioFileLoader::LoadJob::validate) --------
        if (! file.existsAsFile())
        {
            out.errorMessage = "File does not exist: " + file.getFullPathName();
            return out;
        }
        if (file.getSize() == 0)
        {
            out.errorMessage = "File is empty: " + file.getFileName();
            return out;
        }

        // Enforce the SAME extension whitelist as the gate / deck loader so the
        // decode path cannot smuggle in a codec (e.g. .ogg) that the deck loader
        // rejects, even if it bypassed the UI gate (single rejection rule).
        if (! isSupportedExtension (file.getFileExtension()))
        {
            out.errorMessage = "Unsupported file format";
            return out;
        }

        // ---- 2. Open reader (the AudioFormatManager IS the format whitelist) --
        std::unique_ptr<juce::AudioFormatReader> reader (
            formatManager.createReaderFor (file));

        if (reader == nullptr)
        {
            out.errorMessage = "Unsupported file format";
            return out;
        }

        const std::int64_t totalFrames = reader->lengthInSamples;
        const int          srcChannels = (int) reader->numChannels;
        const double       nativeRate  = reader->sampleRate;

        if (totalFrames <= 0 || srcChannels <= 0 || nativeRate <= 0.0)
        {
            out.errorMessage = "File could not be decoded";
            return out;
        }

        // Large-file guard (PRD-0003 §1.5.6): the deck loader logs a warning for
        // very large files and proceeds with a full in-memory decode; mirror that
        // so import and deck share the same memory-resident clip-source model.
        if (file.getSize() > 2LL * 1024 * 1024 * 1024)
            DBG ("Warning: imported file exceeds 2 GB — " + file.getFullPathName());

        // ---- 3. Decode into at-least-stereo buffer ---------------------------
        const int readChannels = juce::jmax (srcChannels, 2);
        juce::AudioBuffer<float> decoded (readChannels, (int) totalFrames);
        decoded.clear();
        reader->read (&decoded, 0, (int) totalFrames, 0, true, true);

        // ---- 4. Channel normalisation (mirrors AudioFileLoader step 8) -------
        if (srcChannels == 1)
        {
            // Mono -> dual-mono. readChannels >= 2, so channel 1 exists.
            decoded.copyFrom (1, 0, decoded, 0, 0, (int) totalFrames);
        }
        else if (srcChannels > 2)
        {
            // Multi-channel -> L=ch0, R=ch1 downmix.
            juce::AudioBuffer<float> stereo (2, (int) totalFrames);
            stereo.copyFrom (0, 0, decoded, 0, 0, (int) totalFrames);
            stereo.copyFrom (1, 0, decoded, 1, 0, (int) totalFrames);
            decoded = std::move (stereo);
        }
        // srcChannels == 2: keep as-is (already exactly stereo).

        // ---- 5. Resample to the session rate (mirrors AudioFileLoader step 9) -
        std::int64_t outputFrames = totalFrames;
        double       bakedRate    = nativeRate;

        if (sessionSampleRate > 0.0 && std::abs (nativeRate - sessionSampleRate) > 0.01)
        {
            const double ratio = nativeRate / sessionSampleRate;
            outputFrames = (std::int64_t) std::ceil ((double) totalFrames / ratio);

            juce::AudioBuffer<float> resampled (2, (int) outputFrames);
            resampled.clear();

            for (int ch = 0; ch < 2; ++ch)
            {
                juce::LagrangeInterpolator interpolator;
                interpolator.process (ratio,
                                      decoded.getReadPointer (ch),
                                      resampled.getWritePointer (ch),
                                      (int) outputFrames);
            }

            decoded   = std::move (resampled);
            bakedRate = sessionSampleRate;
        }

        // ---- 6. Content hash (SAME heuristic the deck loader / library use) ---
        const juce::String contentHash = computeContentHash (file);
        if (contentHash.isEmpty())
        {
            out.errorMessage = "File could not be read";
            return out;
        }

        // ---- 7. Bake the session-rate buffer + build the descriptor ----------
        auto holder = AudioBufferHolder::Ptr (
            new AudioBufferHolder (std::move (decoded), bakedRate, outputFrames));

        ImportedSourceDescriptor desc;
        desc.contentHash         = contentHash;
        desc.sourceFileId        = ImportedSourceDescriptor::idForHash (contentHash);
        desc.lastKnownPath       = file.getFullPathName();
        desc.displayName         = file.getFileName();
        desc.nativeSampleRate    = nativeRate;
        desc.nativeChannelCount  = srcChannels;
        desc.nativeLengthSamples = totalFrames;
        desc.sessionLengthSamples = outputFrames;

        out.ok          = true;
        out.descriptor  = std::move (desc);
        out.bakedBuffer = std::move (holder);
        return out;
    }

    //==========================================================================
    // computeContentHash — identical heuristic to AudioFileLoader::
    // computeContentHash (MD5 of first 64 KB + file size) so an imported file
    // collapses/de-dups consistently with library tracks (§1.5.2).
    //==========================================================================
    juce::String AudioFileImporter::computeContentHash (const juce::File& file)
    {
        juce::FileInputStream stream (file);
        if (stream.failedToOpen())
            return {};

        constexpr int hashBytes = 65536;
        juce::MemoryBlock block;
        auto bytesToRead = juce::jmin (stream.getTotalLength(),
                                       (juce::int64) hashBytes);
        block.setSize ((size_t) bytesToRead);
        stream.read (block.getData(), (int) bytesToRead);

        juce::int64 fileSize = file.getSize();
        block.append (&fileSize, sizeof (fileSize));

        return juce::MD5 (block.getData(), block.getSize()).toHexString();
    }

    //==========================================================================
    void AudioFileImporter::finalisePlacement (std::vector<DecodeResult> results,
                                               juce::ValueTree lane,
                                               std::int64_t dropSample,
                                               ImportClipPlacer::SnapFn snap,
                                               int /*token*/)
    {
        if (results.empty() || ! lane.isValid())
            return;

        // 1. Register every decoded source in the ref-counted registry (de-dupes
        //    byte-identical re-imports) and publish its baked buffer so the engine
        //    can resolve it via the publisher's atomic reader (the clip becomes
        //    audible only here). Then kick off waveform analysis keyed by the
        //    clip's sourceFileId so ClipBlock's WaveformSource finds it.
        std::vector<ImportedSourceDescriptor> sources;
        sources.reserve (results.size());

        for (auto& r : results)
        {
            const auto id = registry_.registerSource (r.descriptor);
            r.descriptor.sourceFileId = id;

            // Publish the baked PCM (atomic swap). A re-import of an identical
            // file simply re-publishes the same data under the same id.
            publisher_.publish (id, r.bakedBuffer);

            // Waveform generation (PRD-0006), keyed by the clip's sourceFileId so
            // the lane renderer resolves it through the same id ClipBlock uses.
            // A placeholder shows until the analysis result lands; on completion
            // we request a recompile/repaint via onClipsPlaced.
            if (r.bakedBuffer != nullptr)
            {
                auto onClipsPlaced = callbacks_.onClipsPlaced;
                waveformAnalyzer_.analyze (
                    id, r.bakedBuffer,
                    [onClipsPlaced] (const juce::String&, WaveformData::Ptr)
                    {
                        if (onClipsPlaced)
                            onClipsPlaced();
                    });
            }

            sources.push_back (r.descriptor);
        }

        // 2. Place the clip(s) as ONE undo transaction. Single file uses
        //    placeClip (opens its own transaction); multi-file uses
        //    placeSequential (non-overlapping, one transaction, §1.5.7).
        if (sources.size() == 1)
            placer_.placeClip (lane, sources.front(), dropSample, snap, /*beginTransaction*/ true);
        else
            placer_.placeSequential (lane, sources, dropSample, snap);

        // 3. Trigger a recompile so the engine picks up the freshly published
        //    sources and the new clip nodes enter the arrangement snapshot.
        if (callbacks_.onClipsPlaced)
            callbacks_.onClipsPlaced();
    }
}
