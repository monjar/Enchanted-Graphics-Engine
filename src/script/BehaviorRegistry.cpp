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
        // Deliberately not the registration count: it exists to measure what
        // a module load contributed, and a test clearing the registry between
        // cases has not un-registered anything that already happened.
    }

}  // namespace ege
