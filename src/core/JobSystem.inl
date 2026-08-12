// Template definitions for core/JobSystem.hpp.

namespace ege {

    template<typename Fn, typename... Args>
    auto JobSystem::submit(Fn&& fn, Args&&... args)
        -> std::future<std::invoke_result_t<Fn, Args...>> {
        using Result = std::invoke_result_t<Fn, Args...>;

        // shared_ptr because std::function requires a copyable target and
        // packaged_task is move-only.
        auto task = std::make_shared<std::packaged_task<Result()>>(
            [callable = std::forward<Fn>(fn),
             arguments = std::make_tuple(std::forward<Args>(args)...)]() mutable -> Result {
                return std::apply(std::move(callable), std::move(arguments));
            });

        std::future<Result> result = task->get_future();

        {
            const std::lock_guard<std::mutex> lock{queueMutex};
            // Counted under the lock so it can never be observed as zero
            // between the push and the increment.
            outstanding.fetch_add(1, std::memory_order_relaxed);
            jobs.emplace([task] { (*task)(); });
        }
        workAvailable.notify_one();

        return result;
    }

    template<typename T>
    T JobSystem::waitFor(std::future<T>& future) {
        while (future.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            if (!tryRunPendingJob()) {
                // Nothing queued to help with; the work is already running on
                // another thread, so give up the rest of this time slice.
                std::this_thread::yield();
            }
        }
        return future.get();
    }

}  // namespace ege
