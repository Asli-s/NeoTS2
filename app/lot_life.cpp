#include "lot_life.hpp"
#include "lot_life_clips.hpp"

#include "diag/log.hpp"
#include "scene/look_at.hpp"
#include "scene/pool_water.hpp"
#include "sim/object_model.hpp"
#include "sim/route_goals.hpp"
#include "sim/snap.hpp"
#include "sim/turn.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <exception>
#include <format>
#include <functional>
#include <set>
#include <string>

namespace app {

namespace {

// Object data 2 and 3: the object something is held by and its slot there.
constexpr std::size_t kContainerField = 2, kSlotField = 3;

// How many times real time the speed clock runs at each speed.
float clockRate(ts2::sim::Speed speed) {
    return speed == ts2::sim::Speed::Ultra ? 21.0f : speed == ts2::sim::Speed::High ? 3.0f : 1.0f;
}

// How far ahead a walk's way is read where the Sim hasn't moved yet (route slivers shorter than this don't turn it).
constexpr float kRouteAhead = 0.01f;

// Sets `held` to the scene instances of the objects Sims hold, with what hangs in their slots (see
// LotLife::hangInAnimated). It is made again only when they change, as it is asked for every frame.
void heldInstances(const ts2::scene::Scene& scene, std::set<std::size_t>& held) {
    thread_local std::vector<std::size_t> now;
    now.clear();
    for (const auto& actor : scene.actors) {
        for (const auto& h : actor.held) {
            now.push_back(h.instance);
        }
    }
    // Objects nest a few deep at most (food in a bowl in the hand). The cap stops a broken lot's
    // containers that hold each other.
    constexpr int kMaxNesting = 8;
    for (int depth = 0; depth < kMaxNesting; ++depth) {
        const std::size_t found = now.size();
        for (const auto& actor : scene.objectActors) {
            const bool inHand = std::any_of(actor.skinned.begin(), actor.skinned.end(), [&](const ts2::scene::Actor::Skinned& s) {
                return std::find(now.begin(), now.end(), s.instance) != now.end();
            });
            for (const auto& h : actor.held) {
                if (inHand && std::find(now.begin(), now.end(), h.instance) == now.end()) {
                    now.push_back(h.instance);
                }
            }
        }
        if (now.size() == found) {
            break;
        }
    }
    std::sort(now.begin(), now.end());
    now.erase(std::unique(now.begin(), now.end()), now.end());
    if (!std::equal(now.begin(), now.end(), held.begin(), held.end())) {
        held = std::set<std::size_t>(now.begin(), now.end());
    }
}

// Where a Sim is drawn: where the simulation has it, a walking Sim as far along its route as it gets
// by `nowMs` (the simulation moves it a step at a time), facing the route's way there. A Sim turning
// in place stays at the heading it turns from, and its turn clip turns the body, as the game draws it.
struct SimFrame {
    float x = 0, y = 0;  // route coordinates
    float heading = 0;
    std::optional<float> pathHeading;  // while walking
    float walked = 0;                  // along the route, while walking
};
SimFrame simFrame(const ts2::sim::SimObject& sim, double nowMs) {
    SimFrame f;
    f.x = static_cast<float>(sim.x) / 65536.0f;
    f.y = static_cast<float>(sim.y) / 65536.0f;
    f.heading = sim.heading ? *sim.heading : ts2::sim::compassHeading(sim.data[1]);
    if (sim.turning) {
        f.heading = sim.turning->start;
    }
    // A walk's turns on the spot are drawn as turns, from where they start.
    if (sim.walking && sim.walking->turn) {
        f.heading = sim.walking->turn->from;
    }
    if (sim.walking && ts2::sim::alongPlan(*sim.walking)) {
        const auto& walk = *sim.walking;
        f.walked = ts2::sim::walkedAt(walk.plan, static_cast<float>(std::max(0.0, nowMs - walk.startMs)));
        const auto at = ts2::sim::walkPoint(walk, f.walked);
        f.x = at.x;
        f.y = at.y;
        // Where the walk hasn't moved yet, the route's way just ahead. A step keeps the Sim's facing.
        const auto way = at.heading ? at.heading : ts2::sim::walkPoint(walk, kRouteAhead).heading;
        if (way && !walk.plan.keepsFacing) {
            f.heading = *way;
            f.pathHeading = way;
        }
    }
    return f;
}

// How high a Sim stands at (x, y) on its level: on the floor, or, swimming (person data 0x40), at
// the pool water's surface, which the swim clips hang the body below.
float standHeight(const ts2::scene::Actor& actor, const ts2::sim::SimObject& sim, std::int32_t level, float x, float y) {
    constexpr std::int32_t kLevelBelowGround = 0;
    if (sim.personData[ts2::sim::kSwimmingPersonData] != 0) {
        return ts2::scene::poolwater::surfaceHeight(actor.floorHeight(kLevelBelowGround, x, y));
    }
    return actor.floorHeight(level, x, y);
}

// Draws a Sim in an object's slot (a seat, a stair step) at the slot's bone and turned as it is, as the
// game does, so a stair's slots stand a step higher each. `slotAt` places an object's slot (see
// LotLife::slotPlace) and `partSlot` one of a multi-tile part drawn by its lead (a double bed's far side).
// A Sim carried in another Sim's slot hangs at that slot's bone once posed (see Actor::carrier), and
// until then stands at its last pose (`pose`), else at its carrier's place.
void placeSim(ts2::scene::Actor& actor, const ts2::sim::SimObject& sim, const SimFrame& frame, const ts2::scene::Scene& scene,
              const std::function<std::optional<ts2::scene::Transform>(const ts2::scene::PlacedObject&, std::size_t slot)>& slotAt,
              const std::function<std::optional<ts2::scene::Transform>(std::int16_t part, std::size_t slot)>& partSlot,
              const ts2::scene::ScenePose* pose) {
    actor.carrier = 0;
    if (sim.x == ts2::sim::kOffLot || sim.y == ts2::sim::kOffLot) {
        // Out of the world (a Sim gone onto a school bus): not drawn, as an unplaced object isn't.
        actor.place.m.fill(0.0f);
        return;
    }
    if (!actor.floorHeight) {
        return;
    }
    if (const std::int16_t holder = sim.data[kContainerField]; holder > 0 && sim.data[kSlotField] >= 0) {
        const auto carrier = std::find_if(scene.actors.begin(), scene.actors.end(),
                                          [&](const ts2::scene::Actor& a) { return a.object == static_cast<std::uint32_t>(holder); });
        if (carrier != scene.actors.end() && &*carrier != &actor) {
            actor.carrier = carrier->object;
            actor.carrierSlot = static_cast<std::size_t>(sim.data[kSlotField]);
            // Just snapped in, it hangs at the slot's bone as the carrier was last posed, so the clips it
            // starts there have the slot as their place, as the game's do.
            actor.place = carrier->place;
            if (pose != nullptr) {
                const auto index = static_cast<std::size_t>(carrier - scene.actors.begin());
                const auto nodes = std::find_if(pose->nodes.begin(), pose->nodes.end(), [&](const auto& n) { return n.first == index; });
                const int node = actor.carrierSlot < carrier->slotNodes.size() ? carrier->slotNodes[actor.carrierSlot] : -1;
                if (const auto last = pose->carried.find(actor.object); last != pose->carried.end()) {
                    actor.place = last->second;
                } else if (nodes != pose->nodes.end() && node >= 0 && static_cast<std::size_t>(node) < nodes->second.size()) {
                    actor.place = carrier->place * nodes->second[static_cast<std::size_t>(node)];
                }
            }
            return;
        }
        const auto container = std::find_if(scene.placedObjects.begin(), scene.placedObjects.end(),
                                            [&](const ts2::scene::PlacedObject& c) { return c.object == static_cast<std::uint32_t>(holder); });
        const auto slot = static_cast<std::size_t>(sim.data[kSlotField]);
        // In a slot of an object drawn by its clips (a car's seat), it hangs at the slot's bone as the
        // object is drawn, as a carried Sim does.
        const auto drawn = std::find_if(scene.objectActors.begin(), scene.objectActors.end(),
                                        [&](const ts2::scene::Actor& a) { return a.object == static_cast<std::uint32_t>(holder); });
        if (drawn != scene.objectActors.end() && slot < drawn->slotNodes.size() && drawn->slotNodes[slot] >= 0) {
            actor.carrier = drawn->object;
            actor.carrierSlot = slot;
            if (pose != nullptr) {
                if (const auto last = pose->carried.find(actor.object); last != pose->carried.end()) {
                    actor.place = last->second;
                    return;
                }
            }
        }
        if (container != scene.placedObjects.end() && container->instance < scene.instances.size()) {
            if (const auto at = slotAt(*container, slot)) {
                actor.place = *at;
                return;
            }
        }
        if (container == scene.placedObjects.end()) {
            if (const auto at = partSlot(holder, slot)) {
                actor.place = *at;
                return;
            }
        }
    }
    const float x = frame.y;  // the lot's x and y are the position's y and x
    const float y = frame.x;
    actor.place = actor.lotFrame * ts2::scene::Transform::at(x, y, standHeight(actor, sim, sim.level, x, y), ts2::sim::drawnHeading(frame.heading));
}

// A Sim and what it holds are on the storey the simulation has the Sim on (1 = ground), so a view
// of a lower storey hides them as it hides objects up there.
void setSimStorey(ts2::scene::Scene& scene, const ts2::scene::Actor& actor, int level) {
    const int storey = scene.groundLevel + level - 1;
    for (const auto& part : actor.skinned) {
        if (part.instance < scene.instances.size()) {
            scene.instances[part.instance].level = storey;
        }
    }
    for (const auto& held : actor.held) {
        if (held.instance < scene.instances.size()) {
            scene.instances[held.instance].level = storey;
        }
    }
}

} // namespace

void LotLife::OwnAnimations::noteStarts(const ts2::sim::SimObject& object, const ts2::scene::Transform& place, bool walks,
                                        const std::optional<ts2::scene::Transform>& hung, std::pair<std::int16_t, std::int16_t> holderSlot) {
    const auto drawn = [&](const Started& s) {
        const auto is = [&](const std::optional<ts2::sim::PlayingAnimation>& a) { return a && a->startMs == s.startMs && a->name == s.name; };
        return is(current) || is(previous) || is(held) ||
               std::any_of(object.overlays.begin(), object.overlays.end(),
                           [&](const ts2::sim::PlayingOverlay& o) { return o.startMs == s.startMs && o.name == s.name; });
    };
    std::erase_if(started, [&](const auto& s) { return !drawn(s); });
    const auto note = [&](const std::string& name, double startMs) {
        if (std::none_of(started.begin(), started.end(), [&](const auto& s) { return s.startMs == startMs && s.name == name; })) {
            Started& s = started.emplace_back(Started{name, startMs, place});
            if (hung) {
                s.fromCarrier = hung->rigidInverse() * place;
                s.carrierSlot = holderSlot;
            }
        }
    };
    if (current && !walks) {
        note(current->name, current->startMs);
    }
    for (const auto& overlay : object.overlays) {
        note(overlay.name, overlay.startMs);
    }
}

std::unique_ptr<ts2::sim::LotSimulation> loadSimulation(const std::filesystem::path& install, const std::filesystem::path& lot,
                                                        std::optional<std::uint32_t> seed) {
    try {
        return std::make_unique<ts2::sim::LotSimulation>(install, lot, nullptr, seed);
    } catch (const std::exception& ex) {
        ts2::diag::error(std::string("the lot's Sims won't come to life: ") + ex.what());
        return nullptr;
    }
}

LotLife::LotLife(std::unique_ptr<ts2::sim::LotSimulation> sim, ts2::scene::Assets& assets,
                 std::shared_ptr<const RouteModelTable> routeModels)
    : assets_(assets), sim_(std::move(sim)), animations_(assets), outfits_(assets), objectMaterials_(assets) {
    sim_->setRouteSources(routeSources(assets, std::move(routeModels)));
    // A birth then finds the baby outfits read and doesn't stop the game to read them.
    babyCatalog_ = std::async(std::launch::async, [packages = assets.skinPackages()] { return ts2::scene::readBabyCatalog(packages); }).share();
    outfitCatalog_ = std::async(std::launch::async, [packages = assets.skinCatalog()] { return ts2::scene::readOutfitCatalog(packages); }).share();
    try {
        sim_->routeWorld();
    } catch (const std::exception& ex) {
        ts2::diag::error(std::string("the lot's route grid could not be built: ") + ex.what());
    }
    sim_->world().nextAfter(0, [this](const ts2::sim::SimObject& o) {
        if (!o.isPerson()) {
            drawnTypes_[o.id] = o.definition.guid();
        }
        return false;
    });
    sim_->world().setAnimationLengths([this](const std::string& name) { return animations_.length(name); });
    sim_->world().setAnimationCarries([this](const std::string& name) { return animations_.hasTargetType(name, 2); });
    sim_->world().setLocoClips([this](const std::string& name) -> std::optional<ts2::sim::LocoClip> {
        const auto a = animations_.find(name);
        return a ? std::optional<ts2::sim::LocoClip>({name, a->duration, a->travel, a->loopPhase, a->match, a->speed, a->rootTurn, a->category}) : std::nullopt;
    });
    sim_->world().setAnimationEventTime([this](const std::string& name, int id) { return animations_.eventTime(name, id); });
    sim_->world().setAnimationEvents([this](const std::string& name) -> const std::vector<ts2::sim::AnimationEvent>& {
        if (const auto it = animationEvents_.find(name); it != animationEvents_.end()) {
            return it->second;
        }
        std::vector<ts2::sim::AnimationEvent> events;
        for (const auto& [ms, id] : animations_.simAnticsEvents(name)) {
            events.push_back({ms, id, {}});
        }
        for (const auto& [ms, sound] : animations_.soundEvents(name)) {
            events.push_back({ms, 0, sound});
        }
        std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.timeMs < b.timeMs; });
        return animationEvents_.emplace(name, std::move(events)).first->second;
    });
    sim_->world().setAnimationTurns([this](const std::string& name, float ms) { return animations_.rootTurn(name, ms); });
    sim_->world().setOutfitProperty([this](std::int16_t neighbor, std::uint16_t category, std::uint16_t property) -> std::optional<std::int16_t> {
        if (const auto file = sim_->characterFile(neighbor)) {
            return outfits_.get(*file, category, property);
        }
        const auto group = sim_->characterGroup(neighbor);
        return group ? std::optional<std::int16_t>(outfits_.get(*group, category, property)) : std::nullopt;
    });
    sim_->world().setOutfitBuild([this](const ts2::sim::SimObject& sim, const ts2::sim::World::OutfitBuildRequest& request) {
        return buildOutfit(sim, request);
    });
    sim_->world().setHasOutfit([this](const ts2::sim::SimObject& object, std::uint32_t category) -> std::optional<bool> {
        const std::int16_t neighbor = sim_->world().neighborIdOf(object);
        if (const auto file = neighbor != 0 ? sim_->characterFile(neighbor) : std::nullopt) {
            return outfits_.hasOutfit(*file, category);
        }
        return outfits_.hasOutfit(object.group, category);
    });
    sim_->world().setAgeDataBuild([this](const ts2::sim::SimObject& sim, std::uint32_t age) { return buildAgeData(sim, age); });
}

void LotLife::traceIn(ts2::scene::Scene& scene, const ts2::scene::ScenePose& pose, const std::vector<std::uint8_t>& shown) {
    rays_ = std::make_unique<ts2::scene::SceneRays>(scene);
    pose_ = &pose;
    sim_->world().setSimScale([&scene](std::int16_t object) {
        for (const auto& actor : scene.actors) {
            if (actor.object == static_cast<std::uint32_t>(object) && actor.rig) {
                return actor.rig->stretch;
            }
        }
        return 1.0f;
    });
    sim_->world().setRayQuery([this, &scene, &pose, &shown](const ts2::sim::RayRequest& request) -> std::optional<ts2::sim::RayAnswer> {
        addCreatedObjects(scene, false);
        placeObjects(scene);
        auto point = [](const ts2::sim::RayPoint& p) {
            return ts2::scene::SceneRays::Point{static_cast<std::uint32_t>(p.object), p.kind, p.slot};
        };
        std::optional<ts2::scene::SceneRays::Point> to;
        if (request.to) {
            to = point(*request.to);
        }
        const auto traced = rays_->trace(point(request.from), to, pose, shown);
        ts2::sim::RayAnswer answer;
        answer.slotMissing = traced.slotMissing;
        answer.objectMissing = traced.objectMissing;
        for (const auto& hit : traced.hits) {
            using Kind = ts2::scene::SceneRays::Hit::Kind;
            const auto kind = hit.kind == Kind::Wall ? ts2::sim::RayHit::Kind::Wall
                            : hit.kind == Kind::Floor ? ts2::sim::RayHit::Kind::Floor : ts2::sim::RayHit::Kind::Object;
            answer.hits.push_back({kind, static_cast<std::int16_t>(hit.object), hit.t});
        }
        return answer;
    });
    if (!scene.actors.empty()) {
        lotFrame_ = scene.actors.front().lotFrame;
    } else if (!scene.placedObjects.empty()) {
        lotFrame_ = scene.placedObjects.front().lotFrame;
    }
    sim_->world().setCameraPoint([this]() -> std::optional<std::array<float, 2>> {
        if (!cameraEye_) {
            return std::nullopt;
        }
        return routePoint(*cameraEye_);
    });
}

std::array<float, 2> LotLife::routePoint(const std::array<float, 3>& scenePoint) const {
    // Into the lot's frame (a turn and a move, so the turn's transpose undoes it).
    const auto& m = lotFrame_.m;
    const float d[3] = {scenePoint[0] - m[3], scenePoint[1] - m[7], scenePoint[2] - m[11]};
    const float lotX = m[0] * d[0] + m[4] * d[1] + m[8] * d[2];
    const float lotY = m[1] * d[0] + m[5] * d[1] + m[9] * d[2];
    return {lotY, lotX};  // the lot's x and y are the position's y and x
}

ts2::sim::Speed LotLife::speed() {
    const auto s = static_cast<ts2::sim::Speed>(sim_->globals()[ts2::sim::global::kSpeed]);
    return s == ts2::sim::Speed::High || s == ts2::sim::Speed::Ultra ? s : ts2::sim::Speed::Normal;
}

void LotLife::setSpeed(ts2::sim::Speed speed) {
    if (speed != this->speed()) {
        ts2::diag::step(std::string("speed: ") + (speed == ts2::sim::Speed::Ultra ? "ultra" : speed == ts2::sim::Speed::High ? "high" : "normal"));
    }
    sim_->globals()[ts2::sim::global::kSpeed] = static_cast<std::int16_t>(speed);
    pacer_.setSpeed(clockRate(speed));
    setPaused(false);
}

std::int16_t LotLife::selected() { return sim_->globals()[ts2::sim::global::kSelectedPerson]; }

void LotLife::select(std::int16_t sim) {
    if (sim != selected()) {
        ts2::diag::step(std::format("selected Sim {}", sim));
    }
    sim_->globals()[ts2::sim::global::kSelectedPerson] = sim;
}

bool LotLife::paused() { return sim_->globals()[ts2::sim::global::kPaused] != 0; }

void LotLife::setPaused(bool paused) {
    if (paused != this->paused()) {
        ts2::diag::step(paused ? "paused" : "unpaused");
    }
    sim_->globals()[ts2::sim::global::kPaused] = paused ? 1 : 0;
}

// The game takes a multi-tile part's lead as the target of a look. An object's targeting slot
// (kind 1) or routing slot (kind 2) is read from the lead's SLOT list, counting that kind's entries
// from 0: a slot at a bone is that bone of the model, any other its offset (x, -y, z) in the model.
LotLife::LookAim LotLife::lookAim(const ts2::sim::SimObject& sim, const ts2::scene::Scene& scene) {
    constexpr std::uint16_t kModelBones = 0x90;
    LookAim aim;
    const int following = sim.look.following();
    const auto target = std::find_if(sim.lookTargets.begin(), sim.lookTargets.end(),
                                      [&](const ts2::sim::LookTarget& t) { return t.request == following; });
    if (target == sim.lookTargets.end()) {
        return aim;
    }
    aim.request = following;
    aim.object = target->object;
    const auto& world = sim_->world();
    const ts2::sim::SimObject* object = target->object == -1 ? nullptr : world.find(target->object);
    if (object == nullptr) {
        return aim;
    }
    const std::int16_t lead = object->data[ts2::sim::kLeadTileField];
    if (lead != 0 && lead != object->id && world.find(lead) != nullptr) {
        aim.object = lead;
        object = world.find(lead);
    }
    if ((target->pointKind != 1 && target->pointKind != 2) || object->isPerson()) {
        return aim;
    }
    aim.objectSlot = true;
    auto& code = sim_->code();
    const auto* type = code.objectType(object->definition.guid());
    const auto* slots = type == nullptr ? nullptr : target->pointKind == 1 ? &type->targetSlots : &type->routingSlots;
    if (slots == nullptr || target->slot >= slots->size()) {
        return aim;
    }
    const auto& entry = (*slots)[target->slot];
    if (!ts2::sim::isBoneSlot(entry)) {
        aim.local = ts2::scene::Transform::at(entry.x, -entry.y, entry.z, 0);
        return aim;
    }
    // The bone is the first joint its "Model - Bones" entry names, aimed at as drawn (see drawnJoint).
    const auto names = entry.bone >= 0 ? code.string(type->group, kModelBones, static_cast<std::size_t>(entry.bone)) : std::nullopt;
    const auto joints = names ? ts2::sim::slotJointNames(*names) : std::vector<std::string>{};
    const auto placed = std::find_if(scene.placedObjects.begin(), scene.placedObjects.end(),
                                     [&](const ts2::scene::PlacedObject& p) { return p.object == static_cast<std::uint32_t>(aim.object); });
    if (joints.empty() || placed == scene.placedObjects.end() || placed->model.empty()) {
        return aim;
    }
    const auto rig = objectMaterials_.objectRig(placed->model, placed->savedJoints);
    const int node = rig ? rig->find(joints.front()) : -1;
    if (node >= 0) {
        std::vector<ts2::scene::Transform> rest;
        ts2::scene::poseRig(*rig, nullptr, 0, rest);
        aim.local = rest[static_cast<std::size_t>(node)];
        aim.model = placed->model;
        aim.joint = joints.front();
        aim.rig = rig;
        aim.node = node;
    }
    return aim;
}

// Where a look's bone is as the object is drawn at `nowMs`, in its model's space: moved by the
// object's clip, else where it was last drawn, else at rest. The game aims at the bone's own scene
// node, which its clips move.
std::optional<ts2::scene::Transform> LotLife::drawnJoint(const LookAim& aim, double nowMs) {
    const auto* object = sim_->world().find(aim.object);
    if (object == nullptr || !aim.rig || aim.node < 0) {
        return aim.local;
    }
    const std::vector<ts2::scene::HeldJoint>* held = nullptr;
    if (pose_ != nullptr) {
        if (const auto found = pose_->held.find(static_cast<std::uint32_t>(aim.object)); found != pose_->held.end()) {
            held = &found->second;
        }
    }
    if (object->animation) {
        if (const auto posed = objectMaterials_.jointPose(*object, nowMs, animations_, aim.model, aim.joint, nullptr, held)) {
            return posed;
        }
    }
    if (held != nullptr && held->size() == aim.rig->nodes.size()) {
        std::vector<ts2::scene::Transform> world;
        ts2::scene::poseRig(*aim.rig, nullptr, 0, world, held);
        return world[static_cast<std::size_t>(aim.node)];
    }
    return aim.local;
}

// Turns each Sim's head as its looks have it (Look At). The look moves on as the Sim is posed, from
// where its neck is then toward where its target was last drawn, and the head IK follows it. The
// states it took are told at the next update.
void LotLife::lookHeads(ts2::scene::Scene& scene, double nowMs) {
    for (auto& [id, changes] : lookChanges_) {
        sim_->tellLookStates(id, changes);
    }
    lookChanges_.clear();
    for (auto& actor : scene.actors) {
        auto* sim = actor.object != 0 && actor.rig ? sim_->world().find(static_cast<std::int16_t>(actor.object)) : nullptr;
        if (sim == nullptr || !sim->isPerson() || !sim->look.busy() || pose_ == nullptr || !rays_) {
            actor.look = nullptr;
            continue;
        }
        const auto id = static_cast<std::int16_t>(actor.object);
        const ts2::scene::Transform fromLot = actor.place.rigidInverse();
        const LookAim aim = lookAim(*sim, scene);
        actor.look = [this, id, fromLot, nowMs, aim](const ts2::scene::HeadPlace& head) -> std::optional<ts2::scene::HeadLook> {
            auto* sim = sim_->world().find(id);
            if (sim == nullptr) {
                return std::nullopt;
            }
            ts2::sim::LookPose look;
            look.neck = head.neck;
            look.head = head.head;
            look.eye = head.eye;
            look.headTurn = head.headTurn;
            look.headRest = head.headRest;
            // Where the target is, from the rig's space.
            const int following = sim->look.following();
            const auto target = std::find_if(sim->lookTargets.begin(), sim->lookTargets.end(),
                                              [&](const ts2::sim::LookTarget& t) { return t.request == following; });
            std::optional<std::array<float, 3>> at;
            if (target != sim->lookTargets.end()) {
                if (target->object == -1) {
                    at = cameraEye_;
                } else if (aim.request == following && (!aim.objectSlot || aim.local) && sim_->world().find(aim.object) != nullptr) {
                    bool slotMissing = false, objectMissing = false;
                    const ts2::scene::SceneRays::Point point{static_cast<std::uint32_t>(aim.object), target->pointKind, target->slot,
                                                             drawnJoint(aim, nowMs)};
                    if (const auto place = rays_->locate(point, *pose_, slotMissing, objectMissing, true)) {
                        at = std::array<float, 3>{place->m[3], place->m[7], place->m[11]};
                    }
                }
            }
            if (at) {
                float p[3] = {(*at)[0], (*at)[1], (*at)[2]};
                fromLot.apply(p, true);
                look.target = {p[0], p[1], p[2]};
            } else {
                // A point not placed yet (another kind of slot, or none drawn) ends the look, which turns
                // back from where it last aimed.
                sim->look.lose(nowMs);
                look.target = sim->look.target();
            }
            auto& changes = lookChanges_[id];
            sim->look.update(nowMs, look, changes);
            const auto* current = sim->look.current();
            if (current == nullptr) {
                return std::nullopt;
            }
            return ts2::scene::HeadLook{current->weightAt(nowMs), sim->look.target(), sim->look.pole()};
        };
    }
}

void LotLife::update(double ms, ts2::scene::Scene& scene) {
    using ts2::sim::LotSimulation;
    using ts2::sim::Speed;
    moved_.clear();
    changedInstances_.clear();
    relit_.clear();
    shapeColored_.clear();
    if (paused()) {
        // The time already run towards the next tick is kept, so the Sims hold the moment they were
        // drawn at instead of stepping back to the last tick.
        sim_->pausedPass();
        wasPaused_ = true;
    } else {
        if (wasPaused_) {
            pacer_.resume();
            wasPaused_ = false;
        }
        // A script that changed the speed changed it as the speed buttons do.
        if (pacer_.clock().rate() != clockRate(speed())) {
            pacer_.setSpeed(clockRate(speed()));
        }
        // This frame's time less what the clock already took in while the last frame's ticks ran.
        watchMs_ += std::max(0.0, ms - frameTicksMs_);
        const auto start = std::chrono::steady_clock::now();
        const auto taken = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); };
        pacer_.frame(
            speed() == Speed::Ultra, [&] { sim_->tick(); },
            [&] { return static_cast<std::int64_t>(std::floor(watchMs_ + taken())); }, taken);
        frameTicksMs_ = taken();
        watchMs_ += frameTicksMs_;
    }
    // Drawn at the speed clock's time, as the game plays clips by that clock. Ticks often run several
    // behind it, so capping the lead at a tick would stall the drawing and make walks stutter. Never
    // behind the last tick, as clips here are timed from the tick that started them.
    leadMs_ = std::max(0.0, static_cast<double>(pacer_.owedMs()));
    if (leadMs_ > 0) {
        sim_->tellAnimationEvents(drawnMs());
    }
    buildNewAges(scene);
    if (!loadedSimsDressed_) {
        loadedSimsDressed_ = true;
        for (const auto& actor : scene.actors) {
            if (actor.object != 0 && actor.object <= static_cast<std::uint32_t>(INT16_MAX)) {
                dressFromSource(static_cast<std::int16_t>(actor.object));
            }
        }
    }
    wearChangedOutfits(scene);
    // Objects the simulation moved go to their new places first: a Sim riding in one (a carpool
    // passenger) is placed by its slot, as the car stands now.
    placeObjects(scene);
    for (auto& actor : scene.actors) {
        if (const auto* object = actor.object ? sim_->world().find(static_cast<std::int16_t>(actor.object)) : nullptr) {
            poseSim(actor, *object, scene);
        }
    }
    addCreatedObjects(scene);
    placeObjects(scene);
    hideModels(scene);
    heldInstances(scene, heldNow_);
    followClips(scene);
    animateObjects(scene);
    hangInAnimated(scene);
    shadeHeld(scene);
    shapeColors(scene);
    for (auto* list : {&changedInstances_, &relit_}) {
        std::sort(list->begin(), list->end());
        list->erase(std::unique(list->begin(), list->end()), list->end());
    }
    objectMaterials_.update(sim_->world(), scene);
    lookHeads(scene, drawnMs());
}


void LotLife::poseSim(ts2::scene::Actor& actor, const ts2::sim::SimObject& object, ts2::scene::Scene& scene) {
    auto& own = ownAnimations_[actor.object];
    const auto& now = object.animation;
    const double nowMs = drawnMs();
    const bool walking = object.walking && ts2::sim::alongPlan(*object.walking);
    const auto tailOver = [&own](double ms) {
        return own.tail && ms - own.tail->startMs >= ts2::sim::walkFadeOutMs(own.tail->plan) + ts2::sim::kWalkFadeMs;
    };
    // A stopped walk that settled: what the Sim held is gone.
    if (object.walkSettledMs && own.settledMs != object.walkSettledMs) {
        own.settledMs = object.walkSettledMs;
        own.settledAt = actor.place;
        own.current.reset();
        own.previous.reset();
        own.held.reset();
        own.tail.reset();
    }
    if (walking) {
        // The walk is drawn from its plan. What the Sim did before stays, to fade out under it.
        // A walk that has handed back plays out its own fade under the new one, in place of
        // its last pose.
        if (!own.walking || own.walkStartMs != object.walking->startMs) {
            // The walk drawn last handed back and this one started within the same update.
            // Its clips play out their fade under this one, as when it handed back on an update of its own.
            if (own.walking && object.walkTail && object.walkTail->startMs == own.walkStartMs) {
                own.tail = *object.walkTail;
                own.tailHeading = own.endedHeading;
                own.current.reset();
            }
            own.walking = true;
            own.walkStartMs = object.walking->startMs;
            if (tailOver(own.walkStartMs)) {
                own.tail.reset();
            }
            own.previous = own.current ? std::move(own.current) : own.tail ? std::nullopt : std::move(own.held);
            // An Animate Sim clip that has played out is gone.
            if (own.previous && own.previous->fades && own.previous->finished(object.walking->startMs)) {
                own.previous.reset();
            }
            own.held.reset();
        }
        if (tailOver(nowMs)) {
            own.tail.reset();
        }
        own.current = now;
    } else {
        if (own.walking) {
            // The walk's route has handed back. Its clips play out their fade, and the Sim
            // then keeps its last pose, which the simulation's animation holds when it has
            // played (a stop's last pose). A walk without a stop ends in a standing clip.
            own.walking = false;
            own.previous.reset();
            own.current.reset();
            own.held.reset();
            if (now && now->finished(sim_->world().clockMs())) {
                own.held = now;
            }
            own.tail.reset();
            if (object.walkTail && object.walkTail->startMs == own.walkStartMs) {
                own.tail = *object.walkTail;
                own.tailHeading = own.endedHeading;
            }
        }
        if (tailOver(nowMs)) {
            own.tail.reset();
        }
        const auto same = [&](const std::optional<ts2::sim::PlayingAnimation>& a) {
            return a && a->startMs == now->startMs && a->name == now->name;
        };
        if (now && !same(own.current) && !same(own.held)) {
            // A new animation fades in over the one before it, or over the last pose of one
            // that has played out, as the game's play clips follow one another. One starting
            // as a walk ends fades in over the walk's clips instead.
            own.previous = own.tail ? std::nullopt : own.current ? std::move(own.current) : std::move(own.held);
            // An Animate Sim clip is gone at its end, so one that has played out can't come back.
            if (own.previous && own.previous->fades && !stillPlaying(*own.previous, *now)) {
                own.previous.reset();
            }
            own.current = *now;
            own.held.reset();
        }
        if (own.previous && own.current) {
            if (nowMs > previousGoneMs(*own.previous, *own.current, animations_)) {
                own.previous.reset();
            }
        }
        // An Animate Sim clip that has played and that nothing followed fades out, then is gone
        // (locomotion clips aside). One that doesn't blend out (Animate Sim byte 2 flag 0x40)
        // holds its last pose until the next animation takes over, as the game's does.
        if (!now && own.current && !own.current->loops && !own.current->baseHeading && own.current->blendsOut &&
            (own.current->fades ? own.current->fadingOut(nowMs) : own.current->finished(nowMs))) {
            const double endMs = own.current->startMs + own.current->lengthMs;
            const bool fading = object.ending && object.ending->startMs == own.current->startMs && object.ending->name == own.current->name;
            // A clip fades within its own length, and one without fades is dropped at its end.
            if (!fading || nowMs >= endMs) {
                own.held = std::move(own.current);
                own.current.reset();
                own.previous.reset();
            }
        }
    }
    const SimFrame frame = simFrame(object, nowMs);
    // As the game places each clip, one playing as the Sim snaps into a carrier's or an object's slot
    // stays where it was in the world, and from then on hangs from the slot's bone with the Sim: its
    // start place is kept as seen from the bone, taken once per slot, and follows the bone (a
    // carpool passenger's seated clips drive off with the car).
    std::optional<ts2::scene::Transform> hung;
    const std::pair<std::int16_t, std::int16_t> holderSlot{object.data[kContainerField], object.data[kSlotField]};
    if (pose_ != nullptr && holderSlot.first > 0) {
        if (const auto found = pose_->carried.find(actor.object); found != pose_->carried.end()) {
            hung = found->second;
        }
    }
    if (!hung && holderSlot.first > 0 && holderSlot.second >= 0) {
        const auto container = std::find_if(scene.placedObjects.begin(), scene.placedObjects.end(), [&](const ts2::scene::PlacedObject& c) {
            return c.object == static_cast<std::uint32_t>(holderSlot.first);
        });
        if (container != scene.placedObjects.end() && container->instance < scene.instances.size()) {
            hung = slotPlace(*container, static_cast<std::size_t>(holderSlot.second), scene);
        }
    }
    for (auto& s : own.started) {
        if (!hung) {
            s.fromCarrier.reset();
            continue;
        }
        if (!s.fromCarrier || s.carrierSlot != holderSlot) {
            s.fromCarrier = hung->rigidInverse() * s.place;
            s.carrierSlot = holderSlot;
        }
        s.place = *hung * *s.fromCarrier;
    }
    const StartedAt startedAt = [&own](const std::string& name, double startMs) -> std::optional<ts2::scene::Transform> {
        for (const auto& s : own.started) {
            if (s.startMs == startMs && s.name == name) {
                return s.place;
            }
        }
        return std::nullopt;
    };
    // A walk that handed back plays on along its plan and path, its clips weighed by the walk's
    // fade, facing the way the path runs there (a step keeps the facing it walked with).
    std::optional<ts2::scene::Transform> tailPlace;
    if (own.tail) {
        const float sinceStart = static_cast<float>(nowMs - own.tail->startMs);
        const float walked = ts2::sim::walkedAt(own.tail->plan, sinceStart);
        ts2::sim::walkPoses(own.tail->plan, walked, tailPoses_, false);
        const float weight = ts2::sim::walkWeight(own.tail->plan, sinceStart);
        for (auto& pose : tailPoses_) {
            pose.weight *= weight;
        }
        if (actor.floorHeight) {
            const auto at = own.tail->path.at(walked);
            const float heading = at.heading && !own.tail->plan.keepsFacing ? *at.heading : own.tailHeading;
            tailPlace = actor.lotFrame * ts2::scene::Transform::at(at.y, at.x, standHeight(actor, object, own.tail->level, at.y, at.x), heading);
        }
    }
    const auto* tailPoses = own.tail ? &tailPoses_ : nullptr;
    const float drawn = ts2::sim::drawnHeading(frame.heading);
    if (walking) {
        // The walk's clips weigh by the walk's own fades, half as it starts (see walkWeight).
        // Drawn at the nearest eighth, they turn by the rest of the walk's heading.
        ts2::sim::walkPoses(object.walking->plan, frame.walked, walkPoses_, false);
        const float weight = ts2::sim::walkWeight(object.walking->plan, static_cast<float>(nowMs - object.walking->startMs));
        for (auto& pose : walkPoses_) {
            pose.weight *= weight;
            pose.turn += ts2::sim::wrapAngle(frame.heading - drawn);
        }
        own.endedHeading = frame.heading;
        const double fade = std::clamp((nowMs - object.walking->startMs) / ts2::sim::kWalkFadeMs, 0.0, 1.0);
        playClips(actor, object, std::nullopt, own.previous, drawn, animations_, &walkPoses_, static_cast<float>(fade), object.walking->startHeading,
                  tailPoses, own.tailHeading, startedAt, tailPlace);
    } else {
        playClips(actor, object, own.current, own.previous, drawn, animations_, nullptr, 1, 0, tailPoses, own.tailHeading,
                  startedAt, tailPlace);
    }
    if (own.settledMs && nowMs < *own.settledMs + ts2::sim::kSettleWalkPoseMs) {
        addSettleClips(actor, object, *own.settledMs, own.settledAt, animations_, sim_->world().locoClips());
    }
    placeSim(
        actor, object, frame, scene, [&](const ts2::scene::PlacedObject& p, std::size_t slot) { return slotPlace(p, slot, scene); },
        [&](std::int16_t part, std::size_t slot) { return partSlot(part, slot, scene); }, pose_);
    own.noteStarts(object, actor.place, walking, hung, holderSlot);
    setSimStorey(scene, actor, object.level);
    // The game picks a Sim's lights afresh as it moves.
    if (actor.place.m != own.litPlace || object.level != own.litLevel) {
        own.litPlace = actor.place.m;
        own.litLevel = object.level;
        const auto inLot = actor.lotFrame.rigidInverse() * actor.place;
        for (const auto& skinned : actor.skinned) {
            relight(scene, skinned.instance, inLot, object.level - 1);
        }
    }
}

} // namespace app
