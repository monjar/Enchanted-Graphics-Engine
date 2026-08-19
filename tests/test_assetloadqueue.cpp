// The bookkeeping half of asynchronous asset loading.
//
// It knows nothing about assets or about a GPU, which is the point: what can
// go wrong here is two references to the same mesh each starting a load of it,
// or a result being reported twice, or one being lost between the thread that
// finished it and the thread that acts on it. All three are checkable without
// a device, and the last one is only checkable by actually using threads.

#include "assets/AssetLoadQueue.hpp"
#include "core/JobSystem.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

using ege::AssetLoadQueue;
using ege::Guid;
using ege::JobSystem;

namespace {

    Guid idOf(uint64_t value) {
        return Guid{value, value * 31u + 7u};
    }

}  // namespace

TEST_CASE("a second request for the same asset starts nothing") {
    // Two MeshRenderers referencing one mesh resolve on the same frame. The
    // first should load it; the second should find the load already running
    // and wait for it rather than loading it again.
    AssetLoadQueue queue;

    CHECK(queue.request(idOf(1)));
    CHECK_FALSE(queue.request(idOf(1)));
    CHECK_FALSE(queue.request(idOf(1)));
    CHECK(queue.inFlight() == 1);

    // A different asset is a different load.
    CHECK(queue.request(idOf(2)));
    CHECK(queue.inFlight() == 2);
}

TEST_CASE("a finished load can be requested again") {
    // Which is what a reload is: the asset was dropped from the cache and has
    // to come back.
    AssetLoadQueue queue;

    REQUIRE(queue.request(idOf(1)));
    queue.finish(idOf(1));
    CHECK(queue.inFlight() == 0);
    CHECK(queue.completed() == 1);

    CHECK(queue.request(idOf(1)));
    CHECK(queue.inFlight() == 1);
}

TEST_CASE("results are taken once") {
    AssetLoadQueue queue;

    REQUIRE(queue.request(idOf(1)));
    REQUIRE(queue.request(idOf(2)));
    queue.finish(idOf(1));
    queue.finish(idOf(2));

    const std::vector<Guid> first = queue.takeCompleted();
    CHECK(first.size() == 2);
    // Emptied by the taking: acting on the same result twice would re-point
    // the world's references at something that has not changed since.
    CHECK(queue.takeCompleted().empty());
    CHECK(queue.completed() == 0);
}

TEST_CASE("finishing something nobody asked for is ignored") {
    // Otherwise a double finish - a load retried after a failure, say -
    // reports an arrival that never happened.
    AssetLoadQueue queue;

    queue.finish(idOf(9));
    CHECK(queue.completed() == 0);

    REQUIRE(queue.request(idOf(9)));
    queue.finish(idOf(9));
    queue.finish(idOf(9));
    CHECK(queue.completed() == 1);
}

TEST_CASE("every load started is reported exactly once, whatever the thread") {
    // The real shape: a burst of requests, each finished on whichever worker
    // picked it up, collected by the thread that will act on them. Run under
    // the thread sanitizer in CI, where the interesting failure is not a wrong
    // count but the data race that produces one.
    AssetLoadQueue queue;
    JobSystem jobs;

    constexpr uint64_t assetCount = 200;
    std::vector<Guid> requested;
    for (uint64_t i = 0; i < assetCount; i++) {
        // Every id asked for twice, so half the requests find a load already
        // running and must not start a second.
        const bool started = queue.request(idOf(i));
        const bool again = queue.request(idOf(i));
        CHECK(started);
        CHECK_FALSE(again);
        requested.push_back(idOf(i));
    }
    REQUIRE(queue.inFlight() == assetCount);

    for (uint64_t i = 0; i < assetCount; i++) {
        jobs.submit([&queue, i]() { queue.finish(idOf(i)); });
    }
    jobs.waitForAll();

    CHECK(queue.inFlight() == 0);

    std::vector<Guid> collected = queue.takeCompleted();
    CHECK(collected.size() == assetCount);

    std::sort(collected.begin(), collected.end());
    std::sort(requested.begin(), requested.end());
    CHECK(collected == requested);
}
