#include "core/JobSystem.hpp"

#include "core/Assert.hpp"
#include "core/Log.hpp"

#include <algorithm>

namespace ege {

    namespace {

        // Set on worker threads so parallelFor can refuse to run on one, where
        // it would block a thread the work it is waiting for needs.
        thread_local bool isWorkerThread = false;

    }  // namespace

    JobSystem::JobSystem(std::size_t workerCount) {
        if (workerCount == 0) {
            const unsigned hardware = std::thread::hardware_concurrency();
            // Leave the main thread a core. hardware_concurrency returns 0 when
            // it cannot tell, hence the floor.
            workerCount = hardware > 1 ? static_cast<std::size_t>(hardware - 1) : 1;
        }

        workers.reserve(workerCount);
        for (std::size_t i = 0; i < workerCount; i++) {
            workers.emplace_back([this] { workerLoop(); });
        }

        EGE_INFO("Job system started with {} worker(s)", workers.size());
    }

    JobSystem::~JobSystem() {
        {
            const std::lock_guard<std::mutex> lock{queueMutex};
            stopping = true;
        }
        workAvailable.notify_all();

        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void JobSystem::workerLoop() {
        isWorkerThread = true;

        while (true) {
            std::function<void()> job;

            {
                std::unique_lock<std::mutex> lock{queueMutex};
                workAvailable.wait(lock, [this] { return stopping || !jobs.empty(); });

                // Queued but unstarted work is dropped on shutdown; anything
                // that must complete should be waited on first.
                if (stopping && jobs.empty()) {
                    return;
                }

                job = std::move(jobs.front());
                jobs.pop();
            }

            job();

            // Decremented after the job has fully run, so waitForAll cannot
            // return while a job is still executing.
            if (outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                const std::lock_guard<std::mutex> lock{queueMutex};
                allDone.notify_all();
            }
        }
    }

    bool JobSystem::tryRunPendingJob() {
        std::function<void()> job;

        {
            const std::lock_guard<std::mutex> lock{queueMutex};
            if (jobs.empty()) {
                return false;
            }
            job = std::move(jobs.front());
            jobs.pop();
        }

        job();

        if (outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            const std::lock_guard<std::mutex> lock{queueMutex};
            allDone.notify_all();
        }
        return true;
    }

    void JobSystem::waitForAll() {
        std::unique_lock<std::mutex> lock{queueMutex};
        allDone.wait(lock, [this] { return outstanding.load(std::memory_order_acquire) == 0; });
    }

    void JobSystem::parallelFor(
        std::size_t begin, std::size_t end, const std::function<void(std::size_t)>& fn) {
        EGE_VERIFY(
            !isWorkerThread,
            "parallelFor called from a worker thread, which would deadlock: the caller "
            "blocks while occupying a thread the split work needs");

        if (end <= begin) {
            return;
        }

        const std::size_t count = end - begin;
        // One chunk per worker plus the calling thread, which participates
        // rather than idling.
        const std::size_t chunks = std::min(count, workers.size() + 1);
        const std::size_t chunkSize = (count + chunks - 1) / chunks;

        std::vector<std::future<void>> results;
        results.reserve(chunks);

        // The calling thread takes the first chunk, so submit the rest.
        for (std::size_t chunk = 1; chunk < chunks; chunk++) {
            const std::size_t chunkBegin = begin + chunk * chunkSize;
            const std::size_t chunkEnd = std::min(chunkBegin + chunkSize, end);
            if (chunkBegin >= chunkEnd) {
                break;
            }
            results.push_back(submit([&fn, chunkBegin, chunkEnd] {
                for (std::size_t i = chunkBegin; i < chunkEnd; i++) {
                    fn(i);
                }
            }));
        }

        const std::size_t ownEnd = std::min(begin + chunkSize, end);
        for (std::size_t i = begin; i < ownEnd; i++) {
            fn(i);
        }

        // waitFor rather than get(), so the calling thread keeps helping with
        // queued work, and so an exception thrown inside a chunk still
        // propagates to the caller.
        for (std::future<void>& result : results) {
            waitFor(result);
        }
    }

}  // namespace ege
