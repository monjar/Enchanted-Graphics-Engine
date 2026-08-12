#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace ege {

    // Thread pool for engine work.
    //
    // Asset importing, mipmap generation, mesh processing and shader
    // compilation are all embarrassingly parallel and all currently block the
    // frame. Phase 6 loads assets asynchronously on top of this.
    //
    // A single shared queue rather than per-worker deques with stealing: the
    // engine's jobs are coarse - decode a texture, parse a mesh - so contention
    // on one mutex is irrelevant next to the work each job does, and the
    // simpler structure is far easier to get right. If profiling ever shows the
    // queue is the bottleneck, that is the point to complicate it.
    class JobSystem {
    public:
        // Defaults to hardware concurrency minus one, leaving the main thread a
        // core of its own. Always at least one worker.
        explicit JobSystem(std::size_t workerCount = 0);

        // Waits for running jobs to finish. Queued but unstarted jobs are
        // dropped.
        ~JobSystem();

        JobSystem(const JobSystem&) = delete;
        JobSystem& operator=(const JobSystem&) = delete;

        // Queues work and hands back a future for its result.
        template<typename Fn, typename... Args>
        auto submit(Fn&& fn, Args&&... args) -> std::future<std::invoke_result_t<Fn, Args...>>;

        // Runs fn(i) for i in [begin, end), split across the workers, and
        // returns once every index has been processed.
        //
        // Calling from a worker would deadlock - the caller blocks while
        // holding a worker thread that the split work needs - so it asserts
        // against that rather than hanging.
        void parallelFor(
            std::size_t begin, std::size_t end, const std::function<void(std::size_t)>& fn);

        // Blocks until every submitted job has completed.
        void waitForAll();

        // Runs one queued job on the calling thread if there is one. Returns
        // false when the queue is empty.
        bool tryRunPendingJob();

        // Waits for a future while helping with queued work.
        //
        // Blocking on a future with future::get() from inside a job deadlocks a
        // fixed-size pool: the waiting job occupies a worker, and once every
        // worker is waiting there is nothing left to run the work they wait on.
        // Nested submission is the normal shape for asset loading - import a
        // scene, queue a job per mesh - so the pool has to survive it. waitFor
        // runs queued jobs on the calling thread until the future is ready, so
        // a blocked worker contributes instead of idling.
        //
        // Use this rather than future::get() anywhere inside a job.
        template<typename T>
        T waitFor(std::future<T>& future);

        std::size_t workerCount() const { return workers.size(); }

        // Jobs submitted but not yet finished.
        std::size_t pending() const { return outstanding.load(std::memory_order_relaxed); }

    private:
        void workerLoop();

        std::vector<std::thread> workers;
        std::queue<std::function<void()>> jobs;

        mutable std::mutex queueMutex;
        std::condition_variable workAvailable;
        std::condition_variable allDone;

        std::atomic<std::size_t> outstanding{0};
        bool stopping = false;
    };

}  // namespace ege

#include "core/JobSystem.inl"
