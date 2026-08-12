// Job system.
//
// Concurrency bugs do not reproduce reliably, so these lean on high iteration
// counts and on ThreadSanitizer/AddressSanitizer in CI rather than on any one
// run passing. The counters are atomic so a torn update shows up as a wrong
// total rather than as undefined behaviour the test cannot see.

#include "core/JobSystem.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <numeric>
#include <stdexcept>
#include <vector>

using ege::JobSystem;

TEST_CASE("the pool starts with at least one worker") {
    JobSystem jobs;
    CHECK(jobs.workerCount() >= 1);

    JobSystem explicitCount{4};
    CHECK(explicitCount.workerCount() == 4);
}

TEST_CASE("a submitted job runs and returns its result") {
    JobSystem jobs{2};

    std::future<int> answer = jobs.submit([] { return 42; });
    CHECK(answer.get() == 42);
}

TEST_CASE("arguments are forwarded to the job") {
    JobSystem jobs{2};

    std::future<int> sum = jobs.submit([](int a, int b) { return a + b; }, 17, 25);
    CHECK(sum.get() == 42);
}

TEST_CASE("every submitted job runs exactly once") {
    JobSystem jobs{4};
    constexpr int jobCount = 2000;

    std::atomic<int> counter{0};
    std::vector<std::future<void>> results;
    results.reserve(jobCount);

    for (int i = 0; i < jobCount; i++) {
        results.push_back(
            jobs.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); }));
    }

    for (auto& result : results) {
        result.get();
    }

    CHECK(counter.load() == jobCount);
}

TEST_CASE("waitForAll returns only once every job has finished") {
    JobSystem jobs{4};
    constexpr int jobCount = 500;

    std::atomic<int> finished{0};
    for (int i = 0; i < jobCount; i++) {
        jobs.submit([&finished] {
            // Enough work that the count is genuinely still climbing when
            // waitForAll is entered.
            volatile int spin = 0;
            for (int k = 0; k < 1000; k++) {
                spin += k;
            }
            finished.fetch_add(1, std::memory_order_relaxed);
        });
    }

    jobs.waitForAll();

    // The decrement happens after the job body, so this must be complete.
    CHECK(finished.load() == jobCount);
    CHECK(jobs.pending() == 0);
}

TEST_CASE("an exception in a job surfaces through its future") {
    JobSystem jobs{2};

    std::future<int> result = jobs.submit([]() -> int { throw std::runtime_error("job failed"); });

    CHECK_THROWS_AS(result.get(), std::runtime_error);
}

TEST_CASE("parallelFor visits every index exactly once") {
    JobSystem jobs{4};
    constexpr std::size_t count = 10000;

    std::vector<std::atomic<int>> visits(count);
    for (auto& visit : visits) {
        visit.store(0);
    }

    jobs.parallelFor(
        0, count, [&visits](std::size_t i) { visits[i].fetch_add(1, std::memory_order_relaxed); });

    for (std::size_t i = 0; i < count; i++) {
        CAPTURE(i);
        REQUIRE(visits[i].load() == 1);
    }
}

TEST_CASE("parallelFor computes the same result as a serial loop") {
    JobSystem jobs{4};
    constexpr std::size_t count = 5000;

    std::vector<std::size_t> parallel(count, 0);
    jobs.parallelFor(0, count, [&parallel](std::size_t i) { parallel[i] = i * i; });

    // Each index writes only its own slot, so no synchronisation is needed
    // beyond parallelFor's own join.
    for (std::size_t i = 0; i < count; i++) {
        REQUIRE(parallel[i] == i * i);
    }
}

TEST_CASE("parallelFor handles ranges smaller than the worker count") {
    JobSystem jobs{8};

    std::atomic<int> visited{0};
    jobs.parallelFor(
        0, 3, [&visited](std::size_t) { visited.fetch_add(1, std::memory_order_relaxed); });
    CHECK(visited.load() == 3);

    // A single element, and an empty range.
    visited.store(0);
    jobs.parallelFor(5, 6, [&visited](std::size_t i) {
        CHECK(i == 5);
        visited.fetch_add(1, std::memory_order_relaxed);
    });
    CHECK(visited.load() == 1);

    visited.store(0);
    jobs.parallelFor(
        4, 4, [&visited](std::size_t) { visited.fetch_add(1, std::memory_order_relaxed); });
    CHECK(visited.load() == 0);
}

TEST_CASE("parallelFor propagates an exception from a chunk") {
    JobSystem jobs{4};

    CHECK_THROWS_AS(
        jobs.parallelFor(
            0,
            1000,
            [](std::size_t i) {
                if (i == 750) {
                    throw std::runtime_error("chunk failed");
                }
            }),
        std::runtime_error);
}

TEST_CASE("nested submits from inside a job complete") {
    // Jobs that queue more jobs are the normal shape for asset loading: import
    // a scene, which queues a job per mesh.
    JobSystem jobs{4};

    std::atomic<int> inner{0};
    std::vector<std::future<void>> outer;

    for (int i = 0; i < 20; i++) {
        outer.push_back(jobs.submit([&jobs, &inner] {
            std::vector<std::future<void>> children;
            for (int k = 0; k < 10; k++) {
                children.push_back(
                    jobs.submit([&inner] { inner.fetch_add(1, std::memory_order_relaxed); }));
            }
            // waitFor, not get(). A worker blocking on get() here occupies a
            // thread the children need, and with enough outer jobs every
            // worker blocks at once and the pool deadlocks - which is exactly
            // what this test did before waitFor existed.
            for (auto& child : children) {
                jobs.waitFor(child);
            }
        }));
    }

    for (auto& result : outer) {
        result.get();
    }

    CHECK(inner.load() == 200);
}

TEST_CASE("destroying the pool with queued work does not hang or crash") {
    std::atomic<int> ran{0};
    {
        JobSystem jobs{2};
        for (int i = 0; i < 100; i++) {
            jobs.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
        }
        // Destructor runs here. Unstarted jobs are dropped by design, so the
        // count is not asserted - what matters is that this terminates.
    }
    CHECK(ran.load() >= 0);
}
