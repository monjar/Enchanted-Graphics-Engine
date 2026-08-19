#include "assets/AssetLoadQueue.hpp"

#include <algorithm>

namespace ege {

    bool AssetLoadQueue::request(Guid id) {
        const std::lock_guard<std::mutex> lock{mutex};
        // insert reports whether it did anything, which is exactly the
        // question being asked: did this call start the load, or find one
        // already running?
        return outstanding.insert(id).second;
    }

    void AssetLoadQueue::finish(Guid id) {
        const std::lock_guard<std::mutex> lock{mutex};
        // Only counted as done if it was outstanding. Finishing something
        // nobody asked for would leave a result the taker cannot explain, and
        // finishing twice would report it twice.
        if (outstanding.erase(id) == 0) {
            return;
        }
        done.push_back(id);
    }

    std::vector<Guid> AssetLoadQueue::takeCompleted() {
        const std::lock_guard<std::mutex> lock{mutex};
        std::vector<Guid> taken;
        taken.swap(done);
        return taken;
    }

    std::size_t AssetLoadQueue::inFlight() const {
        const std::lock_guard<std::mutex> lock{mutex};
        return outstanding.size();
    }

    std::size_t AssetLoadQueue::completed() const {
        const std::lock_guard<std::mutex> lock{mutex};
        return done.size();
    }

    void AssetLoadQueue::clear() {
        const std::lock_guard<std::mutex> lock{mutex};
        outstanding.clear();
        done.clear();
    }

}  // namespace ege
