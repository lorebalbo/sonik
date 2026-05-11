#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

class LibraryAnalysisQueue final
{
public:
    enum class JobKind   { Analysis, StemSeparation };
    enum class JobStatus { Queued, Running, Complete, Failed };

    struct StatusUpdate
    {
        int64_t      trackId = 0;
        juce::String filePath;
        JobKind      kind = JobKind::Analysis;
        JobStatus    status = JobStatus::Queued;
        int          percent = 0;
    };

    /// Context passed to a JobExecutor.  Decouples executors from the queue's
    /// internal QueueJob representation so executors can live outside this
    /// module (PRD-0038 AC-34).
    struct JobContext
    {
        int64_t                            trackId = 0;
        juce::String                       filePath;
        juce::String                       contentHash;
        std::shared_ptr<std::atomic<bool>> sharedCancel;
        std::function<void (int)>          progress;
        std::function<bool()>              shouldExit;
    };

    using StatusCallback = std::function<void (const StatusUpdate&)>;
    using JobExecutor    = std::function<bool (const JobContext&)>;

    LibraryAnalysisQueue (JobExecutor analysisExecutor, JobExecutor stemExecutor);
    ~LibraryAnalysisQueue();

    LibraryAnalysisQueue (const LibraryAnalysisQueue&) = delete;
    LibraryAnalysisQueue& operator= (const LibraryAnalysisQueue&) = delete;

    void enqueueAnalysis       (int64_t trackId, juce::String filePath, juce::String contentHash, bool userTriggered);
    void enqueueStemSeparation (int64_t trackId, juce::String filePath, juce::String contentHash, bool userTriggered);

    void cancelAllJobs();

    void setStatusCallback (StatusCallback cb);

private:
    struct PendingJob
    {
        int64_t      trackId = 0;
        juce::String filePath;
        juce::String contentHash;
        bool         userTriggered = false;
    };

    class QueueJob;

    struct LaneState
    {
        explicit LaneState (int maxWorkersIn) : maxWorkers (maxWorkersIn) {}

        std::mutex             mutex;
        std::deque<PendingJob> pending;
        int                    running = 0;
        const int              maxWorkers;
    };

    void enqueue (JobKind kind, PendingJob job);
    void startNextJobs (JobKind kind);
    void handleJobFinished (JobKind kind);
    void emitStatus (const PendingJob& job, JobKind kind, JobStatus status, int percent);

    LaneState& laneFor (JobKind kind) noexcept;
    juce::ThreadPool& poolFor (JobKind kind) noexcept;
    const JobExecutor& executorFor (JobKind kind) const noexcept;

    JobExecutor analysisExecutor;
    JobExecutor stemExecutor;

    juce::ThreadPool analysisPool { 2 };
    juce::ThreadPool stemPool     { 1 };
    LaneState analysisLane { 2 };
    LaneState stemLane     { 1 };

    std::mutex callbackMutex;
    StatusCallback statusCallback;

    std::atomic<bool> acceptingJobs { true };
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
};