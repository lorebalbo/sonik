#include "LibraryAnalysisService.h"
#include "Features/KeyDetection/KeyUtils.h"
#include <memory>

namespace
{
bool isSupportedAudioFile (const juce::File& file)
{
    const auto ext = file.getFileExtension().toLowerCase();
    return ext == ".mp3" || ext == ".flac" || ext == ".wav"
        || ext == ".aiff" || ext == ".aif" || ext == ".ogg" || ext == ".m4a";
}
}

struct LibraryAnalysisService::AnalysisState
{
    juce::String filePath;
    CompletionCallback callback;
    bool beatDone = false;
    bool keyDone = false;
    bool changed = false;
};

class LibraryAnalysisService::DecodeJob final : public juce::ThreadPoolJob
{
public:
    using DecodeCallback = std::function<void (const juce::String&, const juce::String&, AudioBufferHolder::Ptr)>;

    DecodeJob (juce::AudioFormatManager& manager,
               juce::String path,
               juce::String hash,
               DecodeCallback cb)
        : juce::ThreadPoolJob ("LibraryTrackDecode"),
          formatManager (manager),
          filePath (std::move (path)),
          contentHash (std::move (hash)),
          callback (std::move (cb))
    {
    }

    JobStatus runJob() override
    {
        const juce::File file (filePath);
        if (!file.existsAsFile() || !isSupportedAudioFile (file) || shouldExit())
        {
            deliver (nullptr);
            return jobHasFinished;
        }

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        {
            deliver (nullptr);
            return jobHasFinished;
        }

        const int64_t totalFrames = reader->lengthInSamples;
        const int channels = juce::jlimit (1, 2, static_cast<int> (reader->numChannels));
        juce::AudioBuffer<float> decoded (channels, static_cast<int> (totalFrames));
        decoded.clear();

        reader->read (&decoded, 0, static_cast<int> (totalFrames), 0,
                      true, channels > 1);

        if (shouldExit())
        {
            deliver (nullptr);
            return jobHasFinished;
        }

        AudioBufferHolder::Ptr holder (
            new AudioBufferHolder (std::move (decoded), reader->sampleRate, totalFrames));
        deliver (holder);
        return jobHasFinished;
    }

private:
    void deliver (AudioBufferHolder::Ptr holder)
    {
        auto cb = callback;
        auto path = filePath;
        auto hash = contentHash;
        juce::MessageManager::callAsync ([cb, path, hash, holder]() mutable
        {
            if (cb)
                cb (path, hash, holder);
        });
    }

    juce::AudioFormatManager& formatManager;
    juce::String filePath;
    juce::String contentHash;
    DecodeCallback callback;
};

LibraryAnalysisService::LibraryAnalysisService (TrackDatabase& database)
    : db (database),
      beatGridAnalyzer (database),
      keyDetectionAnalyzer (database)
{
    formatManager.registerBasicFormats();
}

LibraryAnalysisService::~LibraryAnalysisService()
{
    decodePool.removeAllJobs (true, 5000);
}

void LibraryAnalysisService::analyzeTrack (const juce::String& filePath,
                                           const juce::String& contentHash,
                                           CompletionCallback callback)
{
    if (filePath.isEmpty())
    {
        if (callback)
            juce::MessageManager::callAsync ([callback] { callback ({}, false); });
        return;
    }

    juce::WeakReference<LibraryAnalysisService> weakThis (this);
    auto completionCallback = std::move (callback);
    auto* job = new DecodeJob (formatManager, filePath, contentHash,
        [weakThis, movedCompletion = std::move (completionCallback)] (const juce::String& path,
                                                                      const juce::String& hash,
                                                                      AudioBufferHolder::Ptr holder) mutable
        {
            if (auto* self = weakThis.get())
                self->runAnalyzers (path, hash, holder, std::move (movedCompletion));
        });
    decodePool.addJob (job, true);
}

void LibraryAnalysisService::runAnalyzers (const juce::String& filePath,
                                           const juce::String& contentHash,
                                           AudioBufferHolder::Ptr holder,
                                           CompletionCallback callback)
{
    if (holder == nullptr)
    {
        if (callback)
            callback (filePath, false);
        return;
    }

    auto state = std::make_shared<AnalysisState>();
    state->filePath = filePath;
    state->callback = std::move (callback);

    auto finish = [state] (bool changed)
    {
        state->changed = state->changed || changed;
        if (state->beatDone && state->keyDone && state->callback)
            state->callback (state->filePath, state->changed);
    };

    const auto analysisHash = contentHash.isNotEmpty() ? contentHash : filePath;
    juce::WeakReference<LibraryAnalysisService> weakThis (this);

    beatGridAnalyzer.analyze (analysisHash, filePath, holder,
        [weakThis, filePath, contentHash, state, finish]
        (const juce::String&, BeatGridData::Ptr data) mutable
        {
            bool changed = false;
            if (auto* self = weakThis.get(); self != nullptr && data != nullptr && data->bpm > 0.0)
            {
                self->db.updateLibraryTrackBpm (filePath, contentHash, data->bpm);
                changed = true;
            }

            state->beatDone = true;
            finish (changed);
        });

    keyDetectionAnalyzer.analyze (analysisHash, filePath, holder,
        [weakThis, filePath, contentHash, state, finish]
        (const juce::String&, int keyIndex, float) mutable
        {
            bool changed = false;
            if (auto* self = weakThis.get(); self != nullptr && keyIndex >= 0)
            {
                self->db.updateLibraryTrackKey (filePath, contentHash,
                                                KeyUtils::toCamelot (keyIndex),
                                                KeyUtils::toCamelotIndex (keyIndex));
                changed = true;
            }

            state->keyDone = true;
            finish (changed);
        });
}