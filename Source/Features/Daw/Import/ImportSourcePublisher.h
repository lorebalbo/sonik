#pragma once
//==============================================================================
// PRD-0098: ImportSourcePublisher — the atomic-publish bridge that makes an
// imported, session-rate-baked audio buffer audible to the EPIC-0010 playback
// engine WITHOUT ever touching the audio thread on the import path.
//
// An imported source is decoded + resampled + normalised on a background thread
// into an in-memory, session-rate, stereo-interleaved-float AudioBufferHolder.
// That holder is "published" here keyed by the clip's opaque sourceFileId
// ("import:<hash>"). Publication is a single std::shared_ptr swap behind a
// short message/background-thread mutex (PRD-0003 swap pattern): the engine's
// ClipSourceResolver looks the id up and obtains a fresh, read-only
// AudioFormatReader over the published buffer. Until a source is published, the
// resolver returns nullptr and the streamer plays silence (a Missing source is
// NEVER read on the audio thread).
//
// The published buffer is reference-counted (AudioBufferHolder::Ptr), so a
// reader created from it keeps the PCM alive for as long as a streamer holds it,
// even if the entry is later replaced. Re-imports of a byte-identical file
// collapse to the same id (the registry de-dupes) and re-publish the same data.
//
// THREADING: publish()/withdraw() run on the message/background thread.
// makeReader() runs on the message thread (the playback compiler's resolver).
// No method here is called from the audio thread.
//==============================================================================

#include <memory>
#include <mutex>
#include <unordered_map>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "../../AudioEngine/AudioBufferHolder.h"

namespace Daw::Import
{
    //==========================================================================
    // BufferAudioFormatReader — a juce::AudioFormatReader over an in-memory,
    // ref-counted AudioBufferHolder. This is the "reader" the ClipStreamer
    // pre-rolls from; it performs no disk I/O. The holder is retained for the
    // reader's whole lifetime so the PCM cannot be freed mid-stream.
    //==========================================================================
    class BufferAudioFormatReader final : public juce::AudioFormatReader
    {
    public:
        explicit BufferAudioFormatReader (AudioBufferHolder::Ptr holder)
            : juce::AudioFormatReader (nullptr, "ImportBuffer"),
              holder_ (std::move (holder))
        {
            const auto& buf = holder_->getBuffer();
            sampleRate            = holder_->getSampleRate();
            bitsPerSample         = 32;
            usesFloatingPointData = true;
            lengthInSamples       = holder_->getNumFrames();
            numChannels           = (unsigned int) juce::jmax (1, buf.getNumChannels());
        }

        bool readSamples (int* const* destChannels,
                          int numDestChannels,
                          int startOffsetInDestBuffer,
                          juce::int64 startSampleInFile,
                          int numSamples) override
        {
            const auto& buf       = holder_->getBuffer();
            const int   srcChans  = buf.getNumChannels();
            const juce::int64 total = holder_->getNumFrames();

            for (int dest = 0; dest < numDestChannels; ++dest)
            {
                auto* out = reinterpret_cast<float*> (destChannels[dest]);
                if (out == nullptr)
                    continue;

                // Mirror the last available source channel (mono -> dual-mono is
                // already baked, but be defensive).
                const int srcChan = juce::jmin (dest, juce::jmax (0, srcChans - 1));
                const float* src  = (srcChans > 0) ? buf.getReadPointer (srcChan) : nullptr;

                for (int i = 0; i < numSamples; ++i)
                {
                    const juce::int64 pos = startSampleInFile + i;
                    float v = 0.0f;
                    if (src != nullptr && pos >= 0 && pos < total)
                        v = src[pos];
                    out[startOffsetInDestBuffer + i] = v;
                }
            }
            return true;
        }

    private:
        AudioBufferHolder::Ptr holder_;
    };

    //==========================================================================
    // ImportSourcePublisher
    //==========================================================================
    class ImportSourcePublisher
    {
    public:
        ImportSourcePublisher() = default;

        ImportSourcePublisher (const ImportSourcePublisher&)            = delete;
        ImportSourcePublisher& operator= (const ImportSourcePublisher&) = delete;

        // Atomically publishes (or replaces) the baked session-rate buffer for an
        // imported source id. Message/background thread.
        void publish (const juce::String& sourceFileId, AudioBufferHolder::Ptr holder)
        {
            if (sourceFileId.isEmpty() || holder == nullptr)
                return;

            std::lock_guard<std::mutex> lk (mutex_);
            buffers_[sourceFileId] = std::move (holder);
        }

        // Drops a published buffer (e.g. on registry eviction). Any reader already
        // handed out keeps its own ref to the holder, so an in-flight stream is
        // never invalidated by a withdraw. Message/background thread.
        void withdraw (const juce::String& sourceFileId)
        {
            std::lock_guard<std::mutex> lk (mutex_);
            buffers_.erase (sourceFileId);
        }

        bool contains (const juce::String& sourceFileId) const
        {
            std::lock_guard<std::mutex> lk (mutex_);
            return buffers_.find (sourceFileId) != buffers_.end();
        }

        // Returns a fresh reader over the published buffer, or nullptr when the
        // source has not been published yet (engine plays silence). Message thread.
        std::unique_ptr<juce::AudioFormatReader> makeReader (const juce::String& sourceFileId) const
        {
            AudioBufferHolder::Ptr holder;
            {
                std::lock_guard<std::mutex> lk (mutex_);
                auto it = buffers_.find (sourceFileId);
                if (it == buffers_.end())
                    return nullptr;
                holder = it->second;
            }
            if (holder == nullptr || holder->getNumFrames() <= 0)
                return nullptr;

            return std::make_unique<BufferAudioFormatReader> (std::move (holder));
        }

    private:
        mutable std::mutex mutex_;
        std::unordered_map<juce::String, AudioBufferHolder::Ptr> buffers_;
    };
}
