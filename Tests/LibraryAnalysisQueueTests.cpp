#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include "Features/Library/LibraryAnalysisQueue.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <memory>
#include <vector>

class LibraryAnalysisQueueTests final : public juce::UnitTest
{
public:
    LibraryAnalysisQueueTests() : juce::UnitTest ("Library Analysis Queue", "Sonik") {}

    void runTest() override
    {
        testAnalysisConcurrencyCap();
        testStemConcurrencyCap();
        testSeparatePoolsRunConcurrently();
        testUserTriggeredJumpsQueue();
        testCancelAllJobs();
        testStatusCallbackSequenceOnMessageThread();
        testFailedExecutorEmitsFailed();
        testDestructorCancelsAndJoins();
    }

private:
    using JobExecutor = LibraryAnalysisQueue::JobExecutor;
    using JobContext = LibraryAnalysisQueue::JobContext;
    using JobKind = LibraryAnalysisQueue::JobKind;
    using JobStatus = LibraryAnalysisQueue::JobStatus;
    using StatusUpdate = LibraryAnalysisQueue::StatusUpdate;

    static void updateMax (std::atomic<int>& maxValue, int candidate)
    {
        int observed = maxValue.load (std::memory_order_acquire);
        while (candidate > observed
               && ! maxValue.compare_exchange_weak (observed, candidate,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire))
        {
        }
    }

    static bool waitUntil (const std::function<bool()>& predicate, int timeoutMs = 3000)
    {
        const auto deadline = juce::Time::getMillisecondCounter()
                            + static_cast<juce::uint32> (timeoutMs);

        while (! predicate())
        {
            if (juce::Time::getMillisecondCounter() >= deadline)
                return false;

            juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
        }

        return true;
    }

    static JobExecutor immediateExecutor (bool result)
    {
        return [result] (const JobContext&)
        {
            return result;
        };
    }

    static JobExecutor blockingExecutor (juce::WaitableEvent& release,
                                         std::atomic<int>& started,
                                         std::atomic<int>& running,
                                         std::atomic<int>& maxRunning,
                                         std::atomic<int>& completed,
                                         std::atomic<int>& cancelled)
    {
        return [&release, &started, &running, &maxRunning, &completed, &cancelled]
               (const JobContext& ctx)
        {
            started.fetch_add (1, std::memory_order_acq_rel);
            const int nowRunning = running.fetch_add (1, std::memory_order_acq_rel) + 1;
            updateMax (maxRunning, nowRunning);

            while (! release.wait (10))
            {
                if (ctx.shouldExit && ctx.shouldExit())
                {
                    if (ctx.sharedCancel != nullptr)
                        ctx.sharedCancel->store (true, std::memory_order_release);
                    cancelled.fetch_add (1, std::memory_order_acq_rel);
                    running.fetch_sub (1, std::memory_order_acq_rel);
                    return false;
                }
            }

            running.fetch_sub (1, std::memory_order_acq_rel);
            completed.fetch_add (1, std::memory_order_acq_rel);
            return true;
        };
    }

    void testAnalysisConcurrencyCap()
    {
        beginTest ("enqueueAnalysis runs at most 2 jobs concurrently");

        juce::WaitableEvent release (true);
        std::atomic<int> started { 0 }, running { 0 }, maxRunning { 0 }, completed { 0 }, cancelled { 0 };
        LibraryAnalysisQueue queue (blockingExecutor (release, started, running, maxRunning, completed, cancelled),
                                    immediateExecutor (true));

        for (int i = 0; i < 5; ++i)
            queue.enqueueAnalysis (i + 1, juce::String ("/analysis/") + juce::String (i) + ".wav", "hash", false);

        expect (waitUntil ([&] { return started.load() == 2; }), "Expected exactly two analysis jobs to start");
        expect (maxRunning.load() <= 2, "Analysis concurrency exceeded 2");
        expectEquals (started.load(), 2, "Queued analysis jobs should wait for a free slot");

        release.signal();
        expect (waitUntil ([&] { return completed.load() == 5; }), "All analysis jobs should complete after release");
        expect (maxRunning.load() <= 2, "Analysis concurrency exceeded 2 after draining");
    }

    void testStemConcurrencyCap()
    {
        beginTest ("enqueueStemSeparation runs at most 1 job concurrently");

        juce::WaitableEvent release (true);
        std::atomic<int> started { 0 }, running { 0 }, maxRunning { 0 }, completed { 0 }, cancelled { 0 };
        LibraryAnalysisQueue queue (immediateExecutor (true),
                                    blockingExecutor (release, started, running, maxRunning, completed, cancelled));

        for (int i = 0; i < 4; ++i)
            queue.enqueueStemSeparation (i + 1, juce::String ("/stems/") + juce::String (i) + ".wav", "hash", false);

        expect (waitUntil ([&] { return started.load() == 1; }), "Expected one stem job to start");
        expectEquals (started.load(), 1, "Queued stem jobs should wait for the single stem slot");
        expect (maxRunning.load() <= 1, "Stem concurrency exceeded 1");

        release.signal();
        expect (waitUntil ([&] { return completed.load() == 4; }), "All stem jobs should complete after release");
        expect (maxRunning.load() <= 1, "Stem concurrency exceeded 1 after draining");
    }

    void testSeparatePoolsRunConcurrently()
    {
        beginTest ("2 analysis jobs and 1 stem job run concurrently in separate pools");

        juce::WaitableEvent releaseAnalysis (true), releaseStem (true);
        std::atomic<int> analysisStarted { 0 }, analysisRunning { 0 }, analysisMax { 0 }, analysisDone { 0 }, analysisCancelled { 0 };
        std::atomic<int> stemStarted { 0 }, stemRunning { 0 }, stemMax { 0 }, stemDone { 0 }, stemCancelled { 0 };

        LibraryAnalysisQueue queue (blockingExecutor (releaseAnalysis, analysisStarted, analysisRunning,
                                                      analysisMax, analysisDone, analysisCancelled),
                                    blockingExecutor (releaseStem, stemStarted, stemRunning,
                                                      stemMax, stemDone, stemCancelled));

        queue.enqueueAnalysis (1, "/a/1.wav", "a1", false);
        queue.enqueueAnalysis (2, "/a/2.wav", "a2", false);
        queue.enqueueStemSeparation (3, "/s/3.wav", "s3", false);

        expect (waitUntil ([&]
        {
            return analysisStarted.load() == 2 && stemStarted.load() == 1;
        }), "Expected two analysis jobs and one stem job to start together");

        expectEquals (analysisRunning.load(), 2, "Two analysis jobs should be active");
        expectEquals (stemRunning.load(), 1, "One stem job should be active alongside analysis jobs");

        releaseAnalysis.signal();
        releaseStem.signal();
        expect (waitUntil ([&] { return analysisDone.load() == 2 && stemDone.load() == 1; }),
                "Concurrent jobs should drain after release");
    }

    void testUserTriggeredJumpsQueue()
    {
        beginTest ("userTriggered=true jumps to the front of the pending queue");

        juce::WaitableEvent releaseTrackOne (true), releaseTrackTwo (true);
        std::mutex orderMutex;
        std::vector<int> executorOrder;

        auto priorityExecutor = [&] (const JobContext& ctx)
        {
            const auto trackId = static_cast<int> (ctx.trackId);
            {
                std::lock_guard<std::mutex> lock (orderMutex);
                executorOrder.push_back (trackId);
            }

            auto waitForRelease = [&] (juce::WaitableEvent& release)
            {
                while (! release.wait (10))
                {
                    if (ctx.shouldExit && ctx.shouldExit())
                    {
                        if (ctx.sharedCancel != nullptr)
                            ctx.sharedCancel->store (true, std::memory_order_release);
                        return false;
                    }
                }

                return true;
            };

            if (trackId == 1)
                return waitForRelease (releaseTrackOne);
            if (trackId == 2)
                return waitForRelease (releaseTrackTwo);

            return true;
        };

        LibraryAnalysisQueue queue (priorityExecutor, immediateExecutor (true));

        queue.enqueueAnalysis (1, "/prio/1.wav", "h1", false);
        queue.enqueueAnalysis (2, "/prio/2.wav", "h2", false);
        queue.enqueueAnalysis (3, "/prio/3.wav", "h3", false);

        expect (waitUntil ([&]
        {
            std::lock_guard<std::mutex> lock (orderMutex);
            return executorOrder.size() == 2;
        }), "Initial two analysis jobs should start");

        queue.enqueueAnalysis (99, "/prio/manual.wav", "hm", true);

        releaseTrackOne.signal();
        expect (waitUntil ([&]
        {
            std::lock_guard<std::mutex> lock (orderMutex);
            return executorOrder.size() >= 3;
        }), "One queued job should start after a slot opens");

        {
            std::lock_guard<std::mutex> lock (orderMutex);
            expectEquals (executorOrder[2], 99,
                          "Manual job should start before the older background pending job");
        }

        releaseTrackTwo.signal();
        expect (waitUntil ([&]
        {
            std::lock_guard<std::mutex> lock (orderMutex);
            return executorOrder.size() == 4;
        }), "Remaining background job should drain after the second slot opens");
    }

    void testCancelAllJobs()
    {
        beginTest ("cancelAllJobs drops pending jobs and signals running jobs promptly");

        juce::WaitableEvent release (true);
        std::atomic<int> started { 0 }, running { 0 }, maxRunning { 0 }, completed { 0 }, cancelled { 0 };
        LibraryAnalysisQueue queue (blockingExecutor (release, started, running, maxRunning, completed, cancelled),
                                    immediateExecutor (true));

        for (int i = 0; i < 5; ++i)
            queue.enqueueAnalysis (i + 1, juce::String ("/cancel/") + juce::String (i) + ".wav", "hash", false);

        expect (waitUntil ([&] { return started.load() == 2; }), "Expected two running jobs before cancellation");

        const double startMs = juce::Time::getMillisecondCounterHiRes();
        queue.cancelAllJobs();
        const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - startMs;

        expect (elapsedMs < 1000.0, "cancelAllJobs should return promptly");
        expectEquals (started.load(), 2, "Pending jobs should be dropped without starting");
        expectEquals (cancelled.load(), 2, "Running jobs should observe cancellation");
        expectEquals (completed.load(), 0, "Cancelled jobs should not report completion through the executor");
    }

    void testStatusCallbackSequenceOnMessageThread()
    {
        beginTest ("Status callback receives Queued -> Running -> Complete on the Message Thread");

        LibraryAnalysisQueue queue (immediateExecutor (true), immediateExecutor (true));
        std::vector<StatusUpdate> updates;
        bool allOnMessageThread = true;

        queue.setStatusCallback ([&] (const StatusUpdate& update)
        {
            allOnMessageThread = allOnMessageThread
                && juce::MessageManager::getInstance()->isThisTheMessageThread();
            updates.push_back (update);
        });

        queue.enqueueAnalysis (7, "/status/track.wav", "hash", true);

        expect (waitUntil ([&]
        {
            return std::any_of (updates.begin(), updates.end(), [] (const StatusUpdate& update)
            {
                return update.status == JobStatus::Complete;
            });
        }), "Expected completion status");

        expect (allOnMessageThread, "Every status callback should run on the Message Thread");
        expect (! updates.empty() && updates.front().status == JobStatus::Queued,
                "First status should be Queued");

        const auto runningIt = std::find_if (updates.begin(), updates.end(), [] (const StatusUpdate& update)
        {
            return update.status == JobStatus::Running && update.percent == 0;
        });
        const auto completeIt = std::find_if (updates.begin(), updates.end(), [] (const StatusUpdate& update)
        {
            return update.status == JobStatus::Complete;
        });

        expect (runningIt != updates.end(), "Running 0% status should be emitted");
        expect (completeIt != updates.end(), "Complete status should be emitted");
        expect (runningIt < completeIt, "Running should arrive before Complete");
    }

    void testFailedExecutorEmitsFailed()
    {
        beginTest ("Failed executor emits Failed status");

        LibraryAnalysisQueue queue (immediateExecutor (false), immediateExecutor (true));
        std::vector<StatusUpdate> updates;
        queue.setStatusCallback ([&] (const StatusUpdate& update) { updates.push_back (update); });

        queue.enqueueAnalysis (8, "/failed/track.wav", "hash", true);

        expect (waitUntil ([&]
        {
            return std::any_of (updates.begin(), updates.end(), [] (const StatusUpdate& update)
            {
                return update.status == JobStatus::Failed;
            });
        }), "Expected Failed status");
    }

    void testDestructorCancelsAndJoins()
    {
        beginTest ("Destructor cancels running jobs and joins safely");

        juce::WaitableEvent release (true);
        std::atomic<int> started { 0 }, running { 0 }, maxRunning { 0 }, completed { 0 }, cancelled { 0 };

        auto queue = std::make_unique<LibraryAnalysisQueue> (
            blockingExecutor (release, started, running, maxRunning, completed, cancelled),
            immediateExecutor (true));

        for (int i = 0; i < 4; ++i)
            queue->enqueueAnalysis (i + 1, juce::String ("/destructor/") + juce::String (i) + ".wav", "hash", false);

        expect (waitUntil ([&] { return started.load() == 2; }), "Expected two running jobs before destruction");

        const double startMs = juce::Time::getMillisecondCounterHiRes();
        queue.reset();
        const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - startMs;

        expect (elapsedMs < 1000.0, "Destructor should not hang on blocked executors");
        expectEquals (cancelled.load(), 2, "Running jobs should observe destructor cancellation");
        expectEquals (completed.load(), 0, "Destroyed queue should not complete blocked jobs");
    }
};

static LibraryAnalysisQueueTests libraryAnalysisQueueTests;