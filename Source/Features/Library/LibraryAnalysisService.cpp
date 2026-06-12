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

bool isCancelled (const LibraryAnalysisService::CancellationFlag& cancelFlag)
{
    return cancelFlag != nullptr && cancelFlag->load (std::memory_order_acquire);
}
}

struct LibraryAnalysisService::AnalysisState
{
    juce::String filePath;
    juce::String contentHash;
    CompletionCallback callback;
    CancellationFlag cancelFlag;
    bool beatDone = false;
    bool keyDone = false;
    bool finished = false;
    bool changed = false;
    bool hasBpm = false;
    bool hasKey = false;
    double bpm = 0.0;
    juce::String key;
    int keyIndex = -1;
};

class LibraryAnalysisService::DecodeJob final : public juce::ThreadPoolJob
{
public:
    using DecodeCallback = std::function<void (const juce::String&, const juce::String&, AudioBufferHolder::Ptr)>;

    DecodeJob (juce::AudioFormatManager& manager,
               juce::String path,
               juce::String hash,
                             CancellationFlag cancel,
               DecodeCallback cb)
        : juce::ThreadPoolJob ("LibraryTrackDecode"),
          formatManager (manager),
          filePath (std::move (path)),
          contentHash (std::move (hash)),
                    cancelFlag (std::move (cancel)),
          callback (std::move (cb))
    {
    }

    JobStatus runJob() override
    {
        const juce::File file (filePath);
        if (!file.existsAsFile() || !isSupportedAudioFile (file))
        {
            deliver (nullptr);
            return jobHasFinished;
        }

        if (shouldExit() || isCancelled (cancelFlag))
            return jobHasFinished;

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        {
            deliver (nullptr);
            return jobHasFinished;
        }

        if (shouldExit() || isCancelled (cancelFlag))
            return jobHasFinished;

        const int64_t totalFrames = reader->lengthInSamples;
        const int channels = juce::jlimit (1, 2, static_cast<int> (reader->numChannels));
        juce::AudioBuffer<float> decoded (channels, static_cast<int> (totalFrames));
        decoded.clear();

        reader->read (&decoded, 0, static_cast<int> (totalFrames), 0,
                      true, channels > 1);

        if (shouldExit() || isCancelled (cancelFlag))
            return jobHasFinished;

        AudioBufferHolder::Ptr holder (
            new AudioBufferHolder (std::move (decoded), reader->sampleRate, totalFrames));
        deliver (holder);
        return jobHasFinished;
    }

private:
    void deliver (AudioBufferHolder::Ptr holder)
    {
        if (isCancelled (cancelFlag))
            return;

        auto callbackCopy = callback;
        auto path = filePath;
        auto hash = contentHash;
        juce::MessageManager::callAsync ([callbackCopy, path, hash, holder]() mutable
        {
            if (callbackCopy)
                callbackCopy (path, hash, holder);
        });
    }

    juce::AudioFormatManager& formatManager;
    juce::String filePath;
    juce::String contentHash;
    CancellationFlag cancelFlag;
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
    analyzeTrack (filePath, contentHash, std::move (callback), nullptr, nullptr);
}

void LibraryAnalysisService::analyzeTrack (const juce::String& filePath,
                                           const juce::String& contentHash,
                                           CompletionCallback callback,
                                           CancellationFlag cancelFlag,
                                           ProgressCallback progressCallback)
{
    if (filePath.isEmpty())
    {
        if (callback)
            juce::MessageManager::callAsync ([callback] { callback ({}, false); });
        return;
    }

    juce::WeakReference<LibraryAnalysisService> weakThis (this);
    auto analyzerCancelFlag = cancelFlag;
    auto* job = new DecodeJob (formatManager, filePath, contentHash, cancelFlag,
        [weakThis,
         movedCompletion = std::move (callback),
         cancelForAnalyzers = std::move (analyzerCancelFlag),
         progressFn = std::move (progressCallback)] (const juce::String& path,
                                                     const juce::String& hash,
                                                     AudioBufferHolder::Ptr holder) mutable
        {
            if (auto* self = weakThis.get())
                self->runAnalyzers (path, hash, holder, std::move (movedCompletion),
                                    cancelForAnalyzers, std::move (progressFn));
        });

    decodePool.addJob (job, true);
}

void LibraryAnalysisService::runAnalyzers (const juce::String& filePath,
                                           const juce::String& contentHash,
                                           AudioBufferHolder::Ptr holder,
                                           CompletionCallback callback,
                                           CancellationFlag cancelFlag,
                                           ProgressCallback progressCallback)
{
    if (isCancelled (cancelFlag))
    {
        if (callback)
            callback (filePath, false);
        return;
    }

    if (holder == nullptr)
    {
        if (callback)
            callback (filePath, false);
        return;
    }

    auto state = std::make_shared<AnalysisState>();
    state->filePath = filePath;
    state->contentHash = contentHash;
    state->callback = std::move (callback);
    state->cancelFlag = cancelFlag;

    if (progressCallback)
        progressCallback (50);

    juce::WeakReference<LibraryAnalysisService> weakThis (this);

    auto finish = [state, weakThis]
    {
        if (! state->beatDone || ! state->keyDone || state->finished)
            return;

        state->finished = true;

        if (! isCancelled (state->cancelFlag))
        {
            if (auto* self = weakThis.get())
            {
                if (state->hasBpm)
                {
                    self->db.updateLibraryTrackBpm (state->filePath, state->contentHash, state->bpm);
                    state->changed = true;
                }

                if (state->hasKey)
                {
                    self->db.updateLibraryTrackKey (state->filePath, state->contentHash,
                                                    state->key, state->keyIndex);
                    state->changed = true;
                }
            }
        }

        if (state->callback)
            state->callback (state->filePath, state->hasBpm && state->hasKey);
    };

    const auto analysisHash = contentHash.isNotEmpty() ? contentHash : filePath;

    beatGridAnalyzer.analyze (analysisHash, filePath, holder,
        [state, finish]
        (const juce::String&, BeatGridData::Ptr data) mutable
        {
            if (! isCancelled (state->cancelFlag) && data != nullptr && data->bpm > 0.0)
            {
                state->bpm = data->bpm;
                state->hasBpm = true;
            }

            state->beatDone = true;
            finish();
        },
        cancelFlag);

    keyDetectionAnalyzer.analyze (analysisHash, filePath, holder,
        [state, finish]
        (const juce::String&, int keyIndex, float) mutable
        {
            if (! isCancelled (state->cancelFlag) && keyIndex >= 0)
            {
                state->key = KeyUtils::toCamelot (keyIndex);
                state->keyIndex = KeyUtils::toCamelotIndex (keyIndex);
                state->hasKey = state->key.isNotEmpty() && state->keyIndex >= 0;
            }

            state->keyDone = true;
            finish();
        },
        cancelFlag);
}