#include "LibraryAnalysisQueue.h"

#include <juce_events/juce_events.h>
#include <utility>

class LibraryAnalysisQueue::QueueJob final : public juce::ThreadPoolJob
{
public:
    QueueJob (LibraryAnalysisQueue& ownerIn, JobKind kindIn, PendingJob jobIn)
        : juce::ThreadPoolJob ((kindIn == JobKind::Analysis ? "LibraryAnalysis_" : "LibraryStemSeparation_")
                               + juce::String (jobIn.trackId)),
          owner (ownerIn),
          kind (kindIn),
          job (std::move (jobIn)),
          sharedCancel (std::make_shared<std::atomic<bool>> (false))
    {
    }

    JobStatus runJob() override
    {
        struct FinishGuard final
        {
            LibraryAnalysisQueue& queue;
            JobKind jobKind;
            ~FinishGuard() { queue.handleJobFinished (jobKind); }
        } finishGuard { owner, kind };

        owner.emitStatus (job, kind, LibraryAnalysisQueue::JobStatus::Running, 0);

        if (shouldExit())
        {
            sharedCancel->store (true, std::memory_order_release);
            return jobHasFinished;
        }

        owner.emitStatus (job, kind, LibraryAnalysisQueue::JobStatus::Running, 50);

        const auto& executor = owner.executorFor (kind);

        JobContext ctx;
        ctx.trackId      = job.trackId;
        ctx.filePath     = job.filePath;
        ctx.contentHash  = job.contentHash;
        ctx.sharedCancel = sharedCancel;
        ctx.progress     = makeProgressCallback();
        ctx.shouldExit   = [this]() { return shouldExit(); };

        const bool ok = executor ? executor (ctx) : false;

        if (shouldExit() || sharedCancel->load (std::memory_order_acquire))
            return jobHasFinished;

          owner.emitStatus (job, kind,
                                  ok ? LibraryAnalysisQueue::JobStatus::Complete
                                      : LibraryAnalysisQueue::JobStatus::Failed,
                                  100);
        return jobHasFinished;
    }

private:
    std::function<void(int)> makeProgressCallback()
    {
        auto aliveFlag = owner.alive;
        auto* queue = &owner;
        auto jobForStatus = job;
        const auto statusKind = kind;
        auto cancel = sharedCancel;

        return [aliveFlag, queue, jobForStatus = std::move (jobForStatus), statusKind, cancel] (int percent)
        {
            if (! aliveFlag->load (std::memory_order_acquire)
                || cancel->load (std::memory_order_acquire))
                return;

            queue->emitStatus (jobForStatus, statusKind, LibraryAnalysisQueue::JobStatus::Running,
                               juce::jlimit (0, 100, percent));
        };
    }

    LibraryAnalysisQueue& owner;
    JobKind kind;
    PendingJob job;
    std::shared_ptr<std::atomic<bool>> sharedCancel;
};

LibraryAnalysisQueue::LibraryAnalysisQueue (JobExecutor analysisExecutorIn,
                                            JobExecutor stemExecutorIn)
    : analysisExecutor (std::move (analysisExecutorIn)),
      stemExecutor (std::move (stemExecutorIn))
{
}

LibraryAnalysisQueue::~LibraryAnalysisQueue()
{
    alive->store (false, std::memory_order_release);
    cancelAllJobs();
    setStatusCallback (nullptr);
}

void LibraryAnalysisQueue::enqueueAnalysis (int64_t trackId,
                                            juce::String filePath,
                                            juce::String contentHash,
                                            bool userTriggered)
{
    enqueue (JobKind::Analysis, { trackId, std::move (filePath), std::move (contentHash), userTriggered });
}

void LibraryAnalysisQueue::enqueueStemSeparation (int64_t trackId,
                                                  juce::String filePath,
                                                  juce::String contentHash,
                                                  bool userTriggered)
{
    enqueue (JobKind::StemSeparation, { trackId, std::move (filePath), std::move (contentHash), userTriggered });
}

void LibraryAnalysisQueue::cancelAllJobs()
{
    acceptingJobs.store (false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock (analysisLane.mutex);
        analysisLane.pending.clear();
    }

    {
        std::lock_guard<std::mutex> lock (stemLane.mutex);
        stemLane.pending.clear();
    }

    analysisPool.removeAllJobs (true, 5000);
    stemPool.removeAllJobs (true, 5000);
}

void LibraryAnalysisQueue::setStatusCallback (StatusCallback cb)
{
    std::lock_guard<std::mutex> lock (callbackMutex);
    statusCallback = std::move (cb);
}

void LibraryAnalysisQueue::enqueue (JobKind kind, PendingJob job)
{
    if (! acceptingJobs.load (std::memory_order_acquire)
        || ! alive->load (std::memory_order_acquire))
        return;

    emitStatus (job, kind, JobStatus::Queued, 0);

    auto& lane = laneFor (kind);
    {
        std::lock_guard<std::mutex> lock (lane.mutex);
        if (job.userTriggered)
            lane.pending.push_front (std::move (job));
        else
            lane.pending.push_back (std::move (job));
    }

    startNextJobs (kind);
}

void LibraryAnalysisQueue::startNextJobs (JobKind kind)
{
    std::vector<PendingJob> jobsToStart;
    auto& lane = laneFor (kind);

    {
        std::lock_guard<std::mutex> lock (lane.mutex);
        while (acceptingJobs.load (std::memory_order_acquire)
               && lane.running < lane.maxWorkers
               && ! lane.pending.empty())
        {
            jobsToStart.push_back (std::move (lane.pending.front()));
            lane.pending.pop_front();
            ++lane.running;
        }
    }

    auto& pool = poolFor (kind);
    for (auto& pendingJob : jobsToStart)
        pool.addJob (new QueueJob (*this, kind, std::move (pendingJob)), true);
}

void LibraryAnalysisQueue::handleJobFinished (JobKind kind)
{
    auto& lane = laneFor (kind);
    {
        std::lock_guard<std::mutex> lock (lane.mutex);
        lane.running = juce::jmax (0, lane.running - 1);
    }

    startNextJobs (kind);
}

void LibraryAnalysisQueue::emitStatus (const PendingJob& job,
                                       JobKind kind,
                                       JobStatus status,
                                       int percent)
{
    StatusCallback callbackCopy;
    {
        std::lock_guard<std::mutex> lock (callbackMutex);
        callbackCopy = statusCallback;
    }

    if (! callbackCopy)
        return;

    StatusUpdate statusUpdate;
    statusUpdate.trackId = job.trackId;
    statusUpdate.filePath = job.filePath;
    statusUpdate.kind = kind;
    statusUpdate.status = status;
    statusUpdate.percent = juce::jlimit (0, 100, percent);

    auto aliveFlag = alive;
    juce::MessageManager::callAsync ([aliveFlag, callback = std::move (callbackCopy), statusUpdate = std::move (statusUpdate)]() mutable
    {
        if (aliveFlag->load (std::memory_order_acquire) && callback)
            callback (statusUpdate);
    });
}

LibraryAnalysisQueue::LaneState& LibraryAnalysisQueue::laneFor (JobKind kind) noexcept
{
    return kind == JobKind::Analysis ? analysisLane : stemLane;
}

juce::ThreadPool& LibraryAnalysisQueue::poolFor (JobKind kind) noexcept
{
    return kind == JobKind::Analysis ? analysisPool : stemPool;
}

const LibraryAnalysisQueue::JobExecutor& LibraryAnalysisQueue::executorFor (JobKind kind) const noexcept
{
    return kind == JobKind::Analysis ? analysisExecutor : stemExecutor;
}