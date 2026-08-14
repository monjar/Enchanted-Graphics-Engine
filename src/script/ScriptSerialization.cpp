#include "script/ScriptSerialization.hpp"

#include "core/Log.hpp"
#include "reflect/Serialization.hpp"
#include "script/BehaviorRegistry.hpp"
#include "script/Script.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace ege {

    namespace {

        nlohmann::json writeScript(const void* instance) {
            const auto* script = static_cast<const Script*>(instance);
            const Serializer& serializer = Serializer::instance();

            nlohmann::json behaviors = nlohmann::json::array();
            for (const Script::Slot& slot : script->behaviors) {
                nlohmann::json record = nlohmann::json::object();
                record["type"] = slot.behavior;

                const BehaviorRegistry::Entry* entry =
                    BehaviorRegistry::instance().find(slot.behavior);
                if (entry != nullptr && entry->type != nullptr && slot.instance != nullptr) {
                    record["fields"] = serializer.write(*entry->type, slot.instance.get());
                } else if (!slot.savedFields.empty()) {
                    // No instance to read from - the behaviour is not in this
                    // build - so the fields go back exactly as they came in.
                    // Opening a scene in a build missing one of its behaviours
                    // must not be how that behaviour's data gets deleted.
                    record["fields"] = nlohmann::json::parse(slot.savedFields, nullptr, false);
                }

                behaviors.push_back(std::move(record));
            }

            nlohmann::json out = nlohmann::json::object();
            out["behaviors"] = std::move(behaviors);
            return out;
        }

        void readScript(void* instance, const nlohmann::json& json) {
            auto* script = static_cast<Script*>(instance);
            script->behaviors.clear();

            const auto behaviors = json.find("behaviors");
            if (behaviors == json.end() || !behaviors->is_array()) {
                return;
            }

            const Serializer& serializer = Serializer::instance();
            for (const nlohmann::json& record : *behaviors) {
                Script::Slot slot{};
                slot.behavior = record.value("type", std::string{});
                if (slot.behavior.empty()) {
                    continue;
                }

                const auto fields = record.find("fields");
                if (fields != record.end()) {
                    slot.savedFields = fields->dump();
                }

                // The instance is built here rather than at play time, so the
                // inspector has fields to show and edit while the scene is
                // merely open. onSpawn stays unrun until Play - having a
                // behaviour is not the same as running one.
                const BehaviorRegistry::Entry* entry =
                    BehaviorRegistry::instance().find(slot.behavior);
                if (entry != nullptr) {
                    slot.instance = entry->create();
                    if (entry->type != nullptr && fields != record.end()) {
                        serializer.read(*entry->type, slot.instance.get(), *fields);
                    }
                }

                script->behaviors.push_back(std::move(slot));
            }
        }

    }  // namespace

    void registerScriptSerializer() {
        Serializer::instance().registerLeaf<Script>(&writeScript, &readScript);
    }

}  // namespace ege
