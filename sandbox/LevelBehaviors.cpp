// The sandbox game.
//
// Seven behaviours, and between them they are a level with a goal, obstacles,
// pickups and a way to lose. Every one of them is written against nothing but
// the engine's public headers and compiled into the module the engine loads
// at runtime - the same module as Pulse, and for the same reason: this is
// where a game's code would live, and the engine must not have to know
// anything about it.
//
// That is the claim the v0.6 milestone makes, so it is worth being precise
// about what it cost. Building this needed two changes to the engine and
// neither was gameplay: the primitives it ships had to be catalogued before a
// scene is loaded rather than while one is built, and there had to be a way
// to open a scene file from the command line. Everything below is project
// code.

#include "LevelEvents.hpp"
#include "core/Log.hpp"
#include "physics/PhysicsComponents.hpp"
#include "platform/Input.hpp"
#include "reflect/BuiltinTypes.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "script/Behavior.hpp"
#include "script/BehaviorRegistry.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace sandbox {

    namespace {

        // Whether an entity is on a named collision layer. The level asks
        // this about almost everything it touches, because "the player" is a
        // layer rather than a name: a level that matched on names would break
        // the day something was renamed in the inspector.
        bool onLayer(ege::Entity entity, const std::string& layer) {
            if (layer.empty() || !entity.alive()) {
                return layer.empty();
            }
            const ege::PhysicsLayer* found = entity.find<ege::PhysicsLayer>();
            return found != nullptr && found->name == layer;
        }

    }  // namespace

    // ---- The things you collect --------------------------------------------

    // Taken by being walked into, and gone once it is.
    //
    // A trigger rather than a solid body, and that is not a detail: a coin
    // you can bump into is a coin you can push around the level and a coin
    // that stops you dead when you were aiming past it. What a pickup wants
    // is a volume that notices you.
    //
    // It knows nothing about how many there are, what happens when they are
    // all gone, or whether anything is counting at all - the same collectible
    // works in a level with no goal in it.
    class Collectible : public ege::Behavior {
    public:
        std::string collectedBy = "Character";
        // Radians per second, so a collectible reads as a thing to pick up
        // rather than as scenery that happens to be small.
        float spinRate = 2.f;

        void onTriggerEnter(ege::Entity other) override {
            if (taken || !onLayer(other, collectedBy)) {
                return;
            }
            // Guarded, because arriving and despawning do not happen in the
            // same instant: a second arrival in the same tick would announce
            // a collectible that is already spoken for.
            taken = true;
            raise(Collected{other});
            self().despawn();
        }

        void onFixedTick(float deltaSeconds) override {
            ege::Transform* transform = self().find<ege::Transform>();
            if (transform == nullptr) {
                return;
            }
            transform->rotation.y += spinRate * deltaSeconds;
            ege::hierarchy::markDirty(world(), self().id());
        }

    private:
        bool taken = false;
    };

    // ---- The way to lose ----------------------------------------------------

    // A volume under the level. Anything of the player's layer that reaches
    // it has fallen off, which is the whole fail condition: a floor with a
    // hole in it is an obstacle, and an obstacle you cannot fail is scenery.
    class Pit : public ege::Behavior {
    public:
        std::string catches = "Character";

        void onTriggerEnter(ege::Entity other) override {
            if (onLayer(other, catches)) {
                raise(Fell{});
            }
        }
    };

    // Puts the player back where they started when they fall.
    //
    // On the player rather than on the pit, because being returned somewhere
    // is something that happens to *you*: a level with two pits would
    // otherwise have to say where the start is twice.
    class RespawnOnFall : public ege::Behavior {
    public:
        // Seconds between falling and reappearing, so the two read as cause
        // and effect rather than as a teleport.
        float pause = 0.6f;

        void onSpawn() override {
            const ege::Transform* transform = self().find<ege::Transform>();
            start = transform != nullptr ? transform->translation : glm::vec3{0.f};
            on<Fell>([this](const Fell&) { returnHome(); });
        }

        std::string onSaveState() override {
            return std::to_string(start.x) + " " + std::to_string(start.y) + " " +
                   std::to_string(start.z);
        }

        void onReload(const std::string& state) override {
            // onSpawn first, for the subscription - and it will read the
            // player's *current* position as the start, which is exactly the
            // wrong answer if they have wandered off. So the real start is
            // put back over it.
            onSpawn();
            std::istringstream reader{state};
            glm::vec3 saved{};
            if (reader >> saved.x >> saved.y >> saved.z) {
                start = saved;
            }
        }

    private:
        void returnHome() {
            after(pause, [this]() {
                ege::Transform* transform = self().find<ege::Transform>();
                ege::CharacterController* controller = self().find<ege::CharacterController>();
                if (transform == nullptr) {
                    return;
                }
                transform->translation = start;
                ege::hierarchy::markDirty(world(), self().id());
                if (controller != nullptr) {
                    // Otherwise the player arrives at the start still falling
                    // at whatever speed the pit gave them, and immediately
                    // falls through the floor they were put back on.
                    controller->velocity = glm::vec3{0.f};
                    controller->jump = false;
                }
            });
        }

        glm::vec3 start{0.f};
    };

    // ---- The rules ----------------------------------------------------------

    // Counts what has been collected, counts what it has cost, and decides
    // when the level is over either way.
    //
    // The only thing in the level that knows there is a level. Everything
    // else raises what happened to it and gets on with its own job.
    class LevelRules : public ege::Behavior {
    public:
        int needed = 3;
        int lives = 3;

        void onSpawn() override {
            collected = 0;
            livesLeft = lives;
            over = false;

            on<Collected>([this](const Collected&) {
                if (over) {
                    return;
                }
                collected++;
                EGE_INFO("Collected {} of {}", collected, needed);
                if (collected >= needed) {
                    EGE_INFO("The gate is open");
                    raise(GateOpens{});
                }
            });

            on<Fell>([this](const Fell&) {
                if (over) {
                    return;
                }
                livesLeft--;
                if (livesLeft > 0) {
                    EGE_INFO("Fell. {} live(s) left", livesLeft);
                    return;
                }
                over = true;
                EGE_INFO("Out of lives: the level is lost");
                raise(LevelLost{});
            });

            on<LevelWon>([this](const LevelWon&) { over = true; });
        }

        // A level's progress is worth more than a clean slate: editing the
        // rules while somebody is playing should not take away what they have
        // already done.
        std::string onSaveState() override {
            return std::to_string(collected) + " " + std::to_string(livesLeft) + " " +
                   std::to_string(over ? 1 : 0);
        }

        void onReload(const std::string& state) override {
            onSpawn();
            std::istringstream reader{state};
            int savedCollected = 0;
            int savedLives = 0;
            int savedOver = 0;
            if (reader >> savedCollected >> savedLives >> savedOver) {
                collected = savedCollected;
                livesLeft = savedLives;
                over = savedOver != 0;
            }
        }

        int score() const { return collected; }

        int remaining() const { return livesLeft; }

    private:
        int collected = 0;
        int livesLeft = 3;
        bool over = false;
    };

    // ---- The gate and the exit ---------------------------------------------

    // Slides out of the way, once, when the gate opens. It has never heard of
    // a collectible.
    class GateSlides : public ege::Behavior {
    public:
        glm::vec3 opening{0.f, 1.f, 0.f};
        float speed = 1.2f;

        void onSpawn() override {
            openness = 0.f;
            moving = false;
            const ege::Transform* transform = self().find<ege::Transform>();
            shut = transform != nullptr ? transform->translation : glm::vec3{0.f};
            on<GateOpens>([this](const GateOpens&) { moving = true; });
        }

        void onFixedTick(float deltaSeconds) override {
            if (!moving || openness >= 1.f) {
                return;
            }
            ege::Transform* transform = self().find<ege::Transform>();
            if (transform == nullptr) {
                return;
            }
            const float travel = glm::length(opening);
            const float step = travel > 0.f ? speed * deltaSeconds / travel : 1.f;
            openness = std::min(openness + step, 1.f);
            transform->translation = shut + opening * openness;
            ege::hierarchy::markDirty(world(), self().id());
        }

    private:
        glm::vec3 shut{0.f};
        float openness = 0.f;
        bool moving = false;
    };

    // The end of the level: a pad that only counts once the gate is open.
    //
    // It listens for the gate rather than asking the rules how many things
    // are collected, so that "the way out" is a thing that opens rather than
    // a number this has to agree with.
    class ExitPad : public ege::Behavior {
    public:
        std::string opensFor = "Character";

        void onSpawn() override {
            open = false;
            won = false;
            reached = 0;
            on<GateOpens>([this](const GateOpens&) { open = true; });
            on<Collected>([this](const Collected&) { reached++; });
        }

        void onTriggerEnter(ege::Entity other) override {
            if (won || !open || !onLayer(other, opensFor)) {
                return;
            }
            won = true;
            EGE_INFO("Level complete with {} collected", reached);
            raise(LevelWon{reached});
        }

    private:
        bool open = false;
        bool won = false;
        int reached = 0;
    };

    // ---- Playing it without a player ----------------------------------------

    // Walks the level's route, and stands down the moment a human touches
    // anything.
    //
    // It drives the character through exactly the fields a player would, so
    // the controller cannot tell the difference - which is what makes a
    // recorded run a recording of the game rather than of an animation. The
    // standing-down is what lets one scene file be both the recording and the
    // thing you play: press a key and it is yours.
    class ScriptedRun : public ege::Behavior {
    public:
        // Where to walk, in order: "x z; x z; x z". A string rather than a
        // list of points because the serializer's leaves are scalars and
        // vectors and it has no containers yet - so a project that wants a
        // list either encodes one or waits for the engine. Encoding one is
        // three lines and needs nobody's permission, which is the right
        // trade for a route that changes when the level does.
        std::string route;
        float arriveRadius = 0.35f;
        bool run = false;
        // Stops at the last one rather than going round again, because a
        // level is finished rather than circled.
        bool loop = false;

        void onSpawn() override {
            target = 0;
            takenOver = false;
            waypoints.clear();

            // A leg that ended in the pit is a leg this route is not going to
            // finish. Giving up on it and trying the next one is what stops a
            // scripted run walking off the same edge until the level is lost
            // - and it is also how the route can be *written* to fall on
            // purpose, which is the only way a recording shows what losing a
            // life looks like.
            on<Fell>([this](const Fell&) { target++; });

            // "x z; x z" - the height is the floor's business, not the
            // route's.
            std::istringstream reader{route};
            std::string leg;
            while (std::getline(reader, leg, ';')) {
                std::istringstream point{leg};
                float x = 0.f;
                float z = 0.f;
                if (point >> x >> z) {
                    waypoints.push_back(glm::vec3{x, 0.f, z});
                }
            }
        }

        void onFixedTick(float) override {
            ege::CharacterController* controller = self().find<ege::CharacterController>();
            const ege::Transform* transform = self().find<ege::Transform>();
            if (controller == nullptr || transform == nullptr) {
                return;
            }

            if (!takenOver && somebodyIsPlaying()) {
                // Once, and for good: a run that resumed the moment the
                // player let go of the stick would be fighting them.
                takenOver = true;
                EGE_INFO("Taking over from the scripted run");
            }
            if (takenOver || waypoints.empty()) {
                return;
            }

            if (target >= static_cast<int>(waypoints.size())) {
                if (!loop) {
                    controller->move = glm::vec3{0.f};
                    return;
                }
                target = 0;
            }

            const glm::vec3 here = transform->translation;
            glm::vec3 towards = waypoints[static_cast<std::size_t>(target)] - here;
            towards.y = 0.f;
            if (glm::length(towards) <= arriveRadius) {
                target++;
                return;
            }

            controller->move = glm::normalize(towards);
            controller->run = run;
        }

    private:
        bool somebodyIsPlaying() const {
            const ege::Input* input = world().input();
            if (input == nullptr) {
                return false;
            }
            const bool moving = input->axis("MoveLeft", "MoveRight") != 0.f ||
                                input->axis("MoveBackward", "MoveForward") != 0.f ||
                                glm::length(input->leftStick()) > 0.2f;
            return moving || input->isActionDown("Jump");
        }

        std::vector<glm::vec3> waypoints;
        int target = 0;
        bool takenOver = false;
    };

}  // namespace sandbox

EGE_REFLECT(sandbox::Collectible)
EGE_FIELD(collectedBy).tooltip("The collision layer that may take it");
EGE_FIELD(spinRate).range(0.f, 12.f).tooltip("Radians per second it turns");
EGE_REFLECT_END()

EGE_REFLECT(sandbox::Pit)
EGE_FIELD(catches).tooltip("The collision layer that can fall in");
EGE_REFLECT_END()

EGE_REFLECT(sandbox::RespawnOnFall)
EGE_FIELD(pause).range(0.f, 5.f).tooltip("Seconds between falling and reappearing");
EGE_REFLECT_END()

EGE_REFLECT(sandbox::LevelRules)
EGE_FIELD(needed).range(1.f, 32.f).tooltip("How many collectibles open the gate");
EGE_FIELD(lives).range(1.f, 9.f).tooltip("How many falls the level allows");
EGE_REFLECT_END()

EGE_REFLECT(sandbox::GateSlides)
EGE_FIELD(opening).tooltip("How far it travels from where it started");
EGE_FIELD(speed).range(0.f, 20.f).tooltip("Units per second");
EGE_REFLECT_END()

EGE_REFLECT(sandbox::ExitPad)
EGE_FIELD(opensFor).tooltip("The collision layer that may finish the level");
EGE_REFLECT_END()

EGE_REFLECT(sandbox::ScriptedRun)
EGE_FIELD(route).tooltip("Where to walk: 'x z; x z; x z'");
EGE_FIELD(arriveRadius).range(0.05f, 3.f).tooltip("How close counts as arrived");
EGE_FIELD(run).tooltip("Whether it runs rather than walks");
EGE_FIELD(loop).tooltip("Whether it starts again at the end");
EGE_REFLECT_END()

EGE_BEHAVIOR(sandbox::Collectible)
EGE_BEHAVIOR(sandbox::Pit)
EGE_BEHAVIOR(sandbox::RespawnOnFall)
EGE_BEHAVIOR(sandbox::LevelRules)
EGE_BEHAVIOR(sandbox::GateSlides)
EGE_BEHAVIOR(sandbox::ExitPad)
EGE_BEHAVIOR(sandbox::ScriptedRun)
