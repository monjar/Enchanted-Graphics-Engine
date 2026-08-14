#include "script/BehaviorRegistry.hpp"

namespace ege {

    BehaviorRegistry& BehaviorRegistry::instance() {
        static BehaviorRegistry registry;
        return registry;
    }

    const BehaviorRegistry::Entry* BehaviorRegistry::find(std::string_view name) const {
        const auto found = byName.find(std::string{name});
        return found == byName.end() ? nullptr : &entries[found->second];
    }

    void BehaviorRegistry::clear() {
        entries.clear();
        byName.clear();
    }

}  // namespace ege
