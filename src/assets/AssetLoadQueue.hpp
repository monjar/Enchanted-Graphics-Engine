#pragma once

#include "core/Guid.hpp"

#include <cstddef>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace ege {

    // Which assets are being loaded, and which have arrived.
    //
    // Loading an asset used to happen on whichever thread asked for it, which
    // meant the frame that first referenced a mesh paid for reading it off
    // disk, decoding it and uploading it. The job system has existed since
    // Phase 1 and the asset database was designed around this happening
    // elsewhere; what stood in the way was a single command pool, which is now
    // one per thread.
    //
    // This is the bookkeeping half, and it is deliberately device-free: it
    // does not know what an asset is, only that something was asked for, that
    // at most one request per id may be outstanding, and that results are
    // collected by whoever asks for them rather than pushed at anyone. Two
    // references to the same mesh resolving on the same frame must not start
    // two loads of it - which is the property this exists to hold and the one
    // the tests pin.
    class AssetLoadQueue {
    public:
        // Registers a request. False when one is already outstanding for this
        // id, in which case the caller has nothing to do: whoever started the
        // first load will finish it.
        bool request(Guid id);

        // Records a load as done. Called from whichever thread ran it.
        void finish(Guid id);

        // Takes everything that has finished since the last call, and empties
        // the list. Called by the thread that wants to act on the results -
        // in practice the one that re-points the world's references at them.
        std::vector<Guid> takeCompleted();

        // How many loads are outstanding: requested and not yet finished.
        std::size_t inFlight() const;

        // How many finished loads are waiting to be taken.
        std::size_t completed() const;

        // Forgets everything, for a database being cleared out from under it.
        void clear();

    private:
        mutable std::mutex mutex;
        std::unordered_set<Guid> outstanding;
        std::vector<Guid> done;
    };

}  // namespace ege
