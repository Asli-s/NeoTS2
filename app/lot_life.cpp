#include "lot_life.hpp"

#include "content/neighborhood.hpp"
#include "diag/log.hpp"
#include "formats/image.hpp"
#include "scene/object_lights.hpp"
#include "sim/motives.hpp"
#include "sim/ground_click.hpp"
#include "sim/pie_menu.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <exception>
#include <format>
#include <set>
#include <string>

namespace app {

namespace {

// After a long pause (a slow frame, a window drag) the simulation catches up at most this many ticks.
constexpr int kMaxTicksPerUpdate = 10;
// At ultra speed the simulation runs as many ticks as fit in this much time each update.
constexpr double kUltraBudgetMs = 8.0;

// The layers overlays play on by priority, as the game ranks them: an object's own animation (layer 0)
// below every overlay but the underlay (priority 4).
constexpr int kOverlayLayers[] = {1, 3, 5, 7, 0};
// A Sim starts in its standing pose. After that, a joint no clip moves keeps where it last was.
constexpr int kBasePoseLayer = -1;
// An object's new own animation takes over from the one before it over this long (as clips fade in).
constexpr double kOwnCrossfadeSeconds = 0.25;
// A walk fades in over what the Sim did before it over this long, as the game fades its walks.
constexpr double kWalkFadeMs = 233.33;
// How far ahead a walk's way is read where the Sim hasn't moved yet (route slivers shorter than this don't turn it).
constexpr float kRouteAhead = 0.01f;

// What an object plays, as the actor's clips: its own animation and its overlays, or while a Sim
// walks, the walk's clips at the moments the distance walked gives them (`walk`, fading in by
// `walkFade` over `previous`, which keeps the facing the Sim had as the walk started, as the game's
// body does, but not its root's place). A locomotion clip plays turned from the heading the Sim is
// placed at (`frameHeading`) to its own base heading (see PlayingAnimation::baseHeading).
void playClips(ts2::scene::Actor& actor, const ts2::sim::SimObject& object, const std::optional<ts2::sim::PlayingAnimation>& own,
               const std::optional<ts2::sim::PlayingAnimation>& previous, float frameHeading, ts2::scene::AnimationLibrary& animations,
               const std::vector<ts2::sim::WalkPose>* walk = nullptr, float walkFade = 1, float walkStartHeading = 0, bool ownEnding = false,
               const std::vector<ts2::sim::WalkPose>* endedWalk = nullptr) {
    actor.clips.clear();
    // Where an animation's IK object stands in the world (the lot's x and y are the position's y and x).
    const auto objectPlace = [&](const ts2::sim::PlayingAnimation& a) -> std::optional<ts2::scene::Transform> {
        if (!a.ikObject || !actor.floorHeight) {
            return std::nullopt;
        }
        const auto& ik = *a.ikObject;
        return actor.lotFrame * ts2::scene::Transform::at(ik.y, ik.x, actor.floorHeight(ik.level, ik.y, ik.x) + ik.height, ik.heading);
    };
    const auto add = [&](const std::string& name, double startMs, double lengthMs, float speed, bool flipped,
                         std::optional<float> baseHeading = std::nullopt) -> ts2::scene::Actor::Clip* {
        auto bound = animations.bound(actor.rig, name, flipped);
        if (!bound) {
            return nullptr;
        }
        auto& clip = actor.clips.emplace_back();
        clip.animation = std::move(bound);
        clip.start = startMs / 1000.0;
        clip.end = (startMs + std::max(0.0, lengthMs)) / 1000.0;
        clip.speed = speed;
        clip.turn = baseHeading ? ts2::sim::wrapAngle(*baseHeading - frameHeading) : 0.0f;
        return &clip;
    };
    if (!actor.basePose.empty()) {
        // While a walk fades in, the standing pose keeps the facing the Sim had, as the body does.
        const std::optional<float> base = walk && walkFade < 1 ? std::optional<float>(walkStartHeading) : std::nullopt;
        if (auto* clip = add(actor.basePose, 0, 0, 1, false, base)) {
            clip->layer = kBasePoseLayer;
            clip->fadeIn = false;
            clip->fadeOut = false;
        }
    }
    if (walk) {
        if (previous && walkFade < 1) {
            const float base = previous->baseHeading ? *previous->baseHeading : walkStartHeading;
            if (auto* clip = add(previous->name, previous->startMs, previous->lengthMs, previous->speed, previous->flipped, base)) {
                clip->layer = previous->layer;
                clip->weight = 1 - walkFade;
                clip->fadeIn = clip->fadeOut = false;
                clip->repeats = previous->loops;
                clip->noRootMove = true;
            }
        }
        for (const auto& pose : *walk) {
            if (auto* clip = add(pose.clip->name, 0, pose.clip->lengthMs, 1, pose.mirrored)) {
                clip->weight = pose.weight * walkFade;
                clip->atMs = pose.phase * pose.clip->lengthMs;
                clip->turn = pose.turn;
                clip->fadeIn = clip->fadeOut = false;
            }
        }
    }
    // The animation before fades out as the latest fades in, both holding their last pose once played.
    // Between two animations only the upper one's weight changes, as the game's mixer gives the
    // layer under the top 1 less the top's weight: the latest fades in over one below it or on its
    // own layer, and one above it fades out over it. A turn or stop clip (one placed by its base
    // heading) fades out by its body only, as the root already follows the latest. So does any
    // animation before one that moves the root: the game's root takes the latest at once (a stair
    // step's clip starts a step on from where the last one ended, as the Sim snaps a step on).
    const bool crossfade = previous && own;
    std::optional<std::size_t> previousClip;
    if (!walk && previous && own) {
        if (auto* clip = add(previous->name, previous->startMs, previous->lengthMs, previous->speed, previous->flipped, previous->baseHeading)) {
            clip->layer = previous->layer;
            clip->objectPlace = objectPlace(*previous);
            clip->end = own->startMs / 1000.0 + kOwnCrossfadeSeconds;
            clip->fadeIn = false;
            clip->fadeOut = previous->layer >= own->layer;
            clip->bodyOnly = previous->baseHeading.has_value();
            previousClip = actor.clips.size() - 1;
        }
    }
    // An animation starting as a walk ends fades in over the walk's last pose. The game's root
    // already follows the animation then, so the walk keeps only the body's joints.
    if (!walk && own && endedWalk) {
        for (const auto& pose : *endedWalk) {
            if (auto* clip = add(pose.clip->name, 0, pose.clip->lengthMs, 1, pose.mirrored)) {
                clip->weight = pose.weight;
                clip->atMs = pose.phase * pose.clip->lengthMs;
                clip->turn = pose.turn;
                clip->fadeIn = clip->fadeOut = false;
                clip->bodyOnly = true;
            }
        }
    }
    if (!walk && own) {
        if (auto* clip = add(own->name, own->startMs, own->lengthMs, own->speed, own->flipped, own->baseHeading)) {
            clip->layer = own->layer;
            clip->objectPlace = objectPlace(*own);
            clip->fadeIn = (crossfade && own->layer >= previous->layer) || endedWalk != nullptr;
            clip->fadeOut = ownEnding;
            if (ownEnding) {
                clip->end += kOwnCrossfadeSeconds;
            }
            clip->repeats = own->loops;
            const int root = actor.rig ? actor.rig->find("root_trans") : -1;
            const auto& moves = clip->animation->translation;
            if (previousClip && root >= 0 && static_cast<std::size_t>(root) < moves.size() && moves[static_cast<std::size_t>(root)] >= 0) {
                actor.clips[*previousClip].bodyOnly = true;
            }
        }
    }
    for (const auto& overlay : object.overlays) {
        auto* clip = add(overlay.name, overlay.startMs, overlay.plays < 0 ? 0.0 : overlay.lengthMs, overlay.speed, overlay.flipped);
        if (clip) {
            clip->weight = overlay.weight;
            clip->layer = kOverlayLayers[std::min<std::size_t>(overlay.priority, 4)];
            clip->repeats = overlay.plays != 1;
            clip->fadeIn = overlay.blendsIn;
            clip->fadeOut = overlay.blendsOut;
            clip->fadeInSeconds = overlay.blendInMs.value_or(ts2::sim::kOverlayBlendMs) / 1000.0f;
            clip->fadeOutSeconds = overlay.blendOutMs.value_or(ts2::sim::kOverlayBlendMs) / 1000.0f;
        }
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

// A Sim in an object's slot (a seat, a stair step) is drawn where the slot's bone is, turned as it
// is, as the game draws it: a stair's slots stand a step higher each and face up or down the stair.
void placeSim(ts2::scene::Actor& actor, const ts2::sim::SimObject& sim, const SimFrame& frame, const ts2::scene::Scene& scene) {
    constexpr std::size_t kContainerField = 2, kSlotField = 3;
    if (!actor.floorHeight || sim.x == ts2::sim::kOffLot || sim.y == ts2::sim::kOffLot) {
        return;
    }
    if (const std::int16_t holder = sim.data[kContainerField]; holder > 0 && sim.data[kSlotField] >= 0) {
        const auto container = std::find_if(scene.placedObjects.begin(), scene.placedObjects.end(),
                                            [&](const ts2::scene::PlacedObject& c) { return c.object == static_cast<std::uint32_t>(holder); });
        const auto slot = static_cast<std::size_t>(sim.data[kSlotField]);
        if (container != scene.placedObjects.end() && container->instance < scene.instances.size() && slot < container->slots.size() &&
            container->slots[slot]) {
            actor.place = scene.instances[container->instance].transform * *container->slots[slot];
            return;
        }
    }
    const float x = frame.y;  // the lot's x and y are the position's y and x
    const float y = frame.x;
    actor.place = actor.lotFrame * ts2::scene::Transform::at(x, y, actor.floorHeight(sim.level, x, y), frame.heading);
}

// The model a created object is drawn with, as the game picks it: the row of its model names
// (string table 0x85) that its object data 0 gives, or its definition's field 0x48 when that is 0.
// Row 0 means no model. A name may end in "_cres", which the builder adds.
std::string modelName(ts2::scene::Assets& assets, const ts2::sim::SimObject& object) {
    constexpr std::uint32_t kModelNames = 0x85;
    const auto own = static_cast<std::uint16_t>(object.data[0]);
    const std::uint16_t row = own != 0 ? own : object.definition.field(ts2::objd::field::kDefaultModel);
    const auto name = row != 0 ? assets.objectString(object.group, kModelNames, row) : std::nullopt;
    if (!name) {
        return {};
    }
    std::string model = *name;
    const std::string low = ts2::scene::lower(model);
    if (low.size() >= 5 && low.ends_with("_cres")) {
        model.resize(model.size() - 5);
    }
    const std::string bare = ts2::scene::lower(model);
    return bare.empty() || bare == "nullmodel" || bare == "nomodel" ? std::string{} : model;
}

} // namespace

std::unique_ptr<ts2::sim::LotSimulation> loadSimulation(const std::filesystem::path& install, const std::filesystem::path& lot) {
    try {
        return std::make_unique<ts2::sim::LotSimulation>(install, lot);
    } catch (const std::exception& ex) {
        ts2::diag::error(std::string("the lot's Sims won't come to life: ") + ex.what());
        return nullptr;
    }
}

LotLife::LotLife(std::unique_ptr<ts2::sim::LotSimulation> sim, ts2::scene::Assets& assets,
                 std::shared_ptr<const RouteModelTable> routeModels)
    : assets_(assets), sim_(std::move(sim)), animations_(assets), outfits_(assets), objectMaterials_(assets) {
    sim_->setRouteSources(routeSources(assets, std::move(routeModels)));
    try {
        sim_->routeWorld();
    } catch (const std::exception& ex) {
        ts2::diag::error(std::string("the lot's route grid could not be built: ") + ex.what());
    }
    sim_->world().nextAfter(0, [this](const ts2::sim::SimObject& o) {
        lastSeenId_ = o.id;
        return false;
    });
    sim_->world().setAnimationLengths([this](const std::string& name) { return animations_.length(name); });
    sim_->world().setAnimationCarries([this](const std::string& name) { return animations_.hasTargetType(name, 2); });
    sim_->world().setLocoClips([this](const std::string& name) -> std::optional<ts2::sim::LocoClip> {
        const auto a = animations_.find(name);
        return a ? std::optional<ts2::sim::LocoClip>({name, a->duration, a->travel, a->loopPhase, a->match, a->speed, a->rootTurn}) : std::nullopt;
    });
    sim_->world().setAnimationEventTime([this](const std::string& name, int id) { return animations_.eventTime(name, id); });
    sim_->world().setAnimationEvents([this](const std::string& name) {
        std::vector<ts2::sim::AnimationEvent> events;
        for (const auto& [ms, id] : animations_.simAnticsEvents(name)) {
            events.push_back({ms, id});
        }
        return events;
    });
    sim_->world().setAnimationTurns([this](const std::string& name, float ms) { return animations_.rootTurn(name, ms); });
    sim_->world().setOutfitProperty([this](std::int16_t neighbor, std::uint16_t category, std::uint16_t property) -> std::optional<std::int16_t> {
        if (const auto file = sim_->characterFile(neighbor)) {
            return outfits_.get(*file, category, property);
        }
        const auto group = sim_->characterGroup(neighbor);
        return group ? std::optional<std::int16_t>(outfits_.get(*group, category, property)) : std::nullopt;
    });
}

void LotLife::traceIn(ts2::scene::Scene& scene, const ts2::scene::ScenePose& pose, const std::vector<std::uint8_t>& shown) {
    rays_ = std::make_unique<ts2::scene::SceneRays>(scene);
    sim_->world().setSimScale([&scene](std::int16_t object) {
        for (const auto& actor : scene.actors) {
            if (actor.object == static_cast<std::uint32_t>(object) && actor.rig) {
                return actor.rig->stretch;
            }
        }
        return 1.0f;
    });
    sim_->world().setRayQuery([this, &scene, &pose, &shown](const ts2::sim::RayRequest& request) -> std::optional<ts2::sim::RayAnswer> {
        addCreatedObjects(scene);
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
        // Into the lot's frame (a turn and a move, so the turn's transpose undoes it).
        const auto& m = lotFrame_.m;
        const float d[3] = {(*cameraEye_)[0] - m[3], (*cameraEye_)[1] - m[7], (*cameraEye_)[2] - m[11]};
        const float lotX = m[0] * d[0] + m[4] * d[1] + m[8] * d[2];
        const float lotY = m[1] * d[0] + m[5] * d[1] + m[9] * d[2];
        return std::array<float, 2>{lotY, lotX};  // the lot's x and y are the position's y and x
    });
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
    setPaused(false);
}

std::int16_t LotLife::aspiration() {
    constexpr std::size_t kAspiration = 46;
    const auto* sim = sim_->world().find(sim_->globals()[ts2::sim::global::kSelectedPerson]);
    return sim ? sim->personData[kAspiration] : 0;
}

std::int16_t LotLife::selected() { return sim_->globals()[ts2::sim::global::kSelectedPerson]; }

void LotLife::select(std::int16_t sim) {
    if (sim != selected()) {
        ts2::diag::step(std::format("selected Sim {}", sim));
    }
    sim_->globals()[ts2::sim::global::kSelectedPerson] = sim;
}

namespace {
constexpr std::size_t kLifeStage = 0x3A, kAspirationScore = 0xA0, kAgeDaysLeft = 0x5B, kAgeDuration = 0x9C;
} // namespace

bool LotLife::hoverable(std::uint32_t object) {
    if (object == 0 || object > static_cast<std::uint32_t>(INT16_MAX)) {
        return false;
    }
    auto* found = sim_->world().find(static_cast<std::int16_t>(object));
    auto* actor = sim_->world().find(selected());
    if (found == nullptr || found->isPerson() || actor == nullptr) {
        return false;
    }
    return !ts2::sim::pieMenu(sim_->vm(), sim_->code(), *actor, *found, {.firstOnly = true}).choices.empty();
}

std::vector<ts2::sim::PieChoice> LotLife::pieMenu(std::uint32_t object) {
    auto* found = object > 0 && object <= static_cast<std::uint32_t>(INT16_MAX) ? sim_->world().find(static_cast<std::int16_t>(object)) : nullptr;
    auto* actor = sim_->world().find(selected());
    if (found == nullptr || actor == nullptr) {
        return {};
    }
    return ts2::sim::pieMenu(sim_->vm(), sim_->code(), *actor, *found).choices;
}

LotLife::GroundMenu LotLife::groundPieMenu(const std::array<float, 3>& scenePoint, int storey) {
    const auto* actor = sim_->world().find(selected());
    if (actor == nullptr) {
        return {};
    }
    // Into the lot's frame, where the lot's x and y are the scene's y and x.
    const auto& m = lotFrame_.m;
    const float d[3] = {scenePoint[0] - m[3], scenePoint[1] - m[7], scenePoint[2] - m[11]};
    const float sceneX = m[0] * d[0] + m[4] * d[1] + m[8] * d[2];
    const float sceneY = m[1] * d[0] + m[5] * d[1] + m[9] * d[2];
    auto* made = ts2::sim::groundDestination(sim_->vm(), sim_->world(), actor, {sceneY, sceneX, storey});
    if (made == nullptr) {
        return {};
    }
    ts2::diag::step(std::format("ground clicked at {:.2f} {:.2f} level {}: destination {}", made->x / 65536.0, made->y / 65536.0, made->level,
                                made->id));
    return {static_cast<std::uint32_t>(made->id), ts2::sim::pieMenu(sim_->vm(), sim_->code(), *sim_->world().find(selected()), *made).choices};
}

void LotLife::choose(std::uint32_t object, const ts2::sim::PieChoice& choice) {
    auto* found = object > 0 && object <= static_cast<std::uint32_t>(INT16_MAX) ? sim_->world().find(static_cast<std::int16_t>(object)) : nullptr;
    auto* actor = sim_->world().find(selected());
    if (found == nullptr || actor == nullptr) {
        return;
    }
    if (const auto queued = ts2::sim::queuedChoice(sim_->code(), *found, choice)) {
        ts2::diag::step(std::format("Sim {} is told to \"{}\" on {} {}", actor->id, choice.name, found->definition.name, found->id));
        ts2::sim::queueInteraction(*actor, *queued);
    }
}

std::vector<LotLife::QueuedAction> LotLife::queue(std::int16_t sim) {
    // A part of a multi-tile object shows its lead (object data 0x13). With no picture named, an
    // object whose OBJD field 0x37 is 0 shows the picture its object data 0 names.
    constexpr std::size_t kLeadField = 0x13, kGraphicField = 0x00, kGraphicPictureField = 0x37;
    std::vector<QueuedAction> out;
    const auto* person = sim_->world().find(sim);
    if (person == nullptr) {
        return out;
    }
    auto add = [&](const ts2::sim::QueuedInteraction& q, bool running, std::int16_t object, std::uint32_t guid, std::uint32_t index,
                   const std::string& name) {
        QueuedAction a{q.id, running, q.cancelled, q.nested, 0, index, 0, name};
        if (a.name.empty()) {
            const auto* target = sim_->world().find(q.object);
            a.name = target ? ts2::sim::interactionName(sim_->code(), *target, q.index) : std::string();
        }
        if (guid != 0 && guid != UINT32_MAX) {
            a.guid = guid;
        } else if (const auto* icon = sim_->world().find(object)) {
            if (icon->isPerson()) {
                a.neighbor = sim_->world().neighborIdOf(*icon);
            } else {
                const auto* lead = icon->data[kLeadField] != 0 ? sim_->world().find(icon->data[kLeadField]) : nullptr;
                const auto& shown = lead ? *lead : *icon;
                a.guid = shown.definition.guid();
                if (a.index == 0 && shown.definition.field(kGraphicPictureField) == 0) {
                    a.index = static_cast<std::uint16_t>(shown.data[kGraphicField]);
                }
            }
        } else {
            return;  // no icon: the queue leaves it out
        }
        out.push_back(a);
    };
    // The running interaction's icon is lit when it is a top-level one, its sub-interaction's when
    // it is nested.
    auto iconOf = [](const ts2::sim::QueuedInteraction& q) { return q.iconObject != 0 ? q.iconObject : q.partner != 0 ? q.partner : q.object; };
    if (person->running) {
        add(*person->running, !person->running->nested, person->actionIconObject, person->actionIconGuid, person->actionIconIndex, person->actionString);
    }
    if (person->runningSub) {
        add(*person->runningSub, person->runningSub->nested, iconOf(*person->runningSub), person->runningSub->iconGuid, person->runningSub->iconIndex,
            person->runningSub->name);
    }
    for (const auto& q : person->queue) {
        add(q, false, iconOf(q), q.iconGuid, q.iconIndex, q.name);
    }
    return out;
}

bool LotLife::cancelInteraction(std::int16_t sim, std::uint32_t id) {
    auto* person = sim_->world().find(sim);
    if (person == nullptr || !ts2::sim::cancelInteraction(*person, id)) {
        return false;
    }
    ts2::diag::step(std::format("Sim {} is told to stop interaction {}", sim, id));
    return true;
}

ts2::content::AgeProgress LotLife::ageProgress(std::int16_t sim) {
    const auto* object = sim_->world().find(sim);
    return object ? ts2::content::ageProgress(object->personData[kAgeDuration], object->personData[kAgeDaysLeft]) : ts2::content::AgeProgress{};
}

std::vector<ts2::swaf::Want> LotLife::wants(std::int16_t sim) {
    const auto* object = sim_->world().find(sim);
    const std::int16_t id = object ? sim_->world().neighborIdOf(*object) : 0;
    return id ? sim_->world().neighborWants(id) : std::vector<ts2::swaf::Want>{};
}

std::vector<ts2::swaf::Want> LotLife::fears(std::int16_t sim) {
    const auto* object = sim_->world().find(sim);
    const std::int16_t id = object ? sim_->world().neighborIdOf(*object) : 0;
    return id ? sim_->world().neighborFears(id) : std::vector<ts2::swaf::Want>{};
}

ts2::swaf::Slots LotLife::wantSlots(std::int16_t sim) {
    const auto* object = sim_->world().find(sim);
    const std::int16_t id = object ? sim_->world().neighborIdOf(*object) : 0;
    return id ? sim_->world().wantSlots(id) : ts2::swaf::Slots{};
}

std::optional<LotLife::NeighborName> LotLife::neighborName(std::int16_t id) {
    constexpr std::size_t kGender = 0x41;
    auto& world = sim_->world();
    const auto* neighbor = world.neighbor(id);
    if (neighbor == nullptr) {
        return std::nullopt;
    }
    auto& code = sim_->code();
    NeighborName name;
    name.female = world.neighborPersonValue(id, kGender) == 1;
    if (const auto* type = code.objectType(neighbor->guid)) {
        const auto set = static_cast<std::uint16_t>(type->definition.field(ts2::objd::field::kCatalogStringsId));
        name.first = code.catalogString(type->group, set, 0).value_or("");
        name.last = code.catalogString(type->group, set, 2).value_or("");
    }
    return name;
}

std::string LotLife::objectName(std::uint32_t guid) {
    auto& code = sim_->code();
    const auto* type = code.objectType(guid);
    if (type == nullptr) {
        return {};
    }
    const auto set = static_cast<std::uint16_t>(type->definition.field(ts2::objd::field::kCatalogStringsId));
    return code.catalogString(type->group, set, 0).value_or("");
}

void LotLife::toggleHold(std::int16_t sim, std::size_t index, bool fear) {
    constexpr std::uint8_t kHeld = 0x01;
    const auto* object = sim_->world().find(sim);
    const std::int16_t id = object ? sim_->world().neighborIdOf(*object) : 0;
    if (id == 0) {
        return;
    }
    auto& world = sim_->world();
    auto& wants = world.neighborWants(id);
    auto& fears = world.neighborFears(id);
    auto& list = fear ? fears : wants;
    if (index >= list.size()) {
        return;
    }
    auto [order, fresh] = holds_.try_emplace(id);
    if (fresh) {
        for (const bool f : {false, true}) {
            for (const auto& w : f ? fears : wants) {
                if (w.flags & kHeld) {
                    order->second.push_back({f, w.guid});
                }
            }
        }
    }
    auto& held = order->second;
    auto& want = list[index];
    want.flags = static_cast<std::uint8_t>(want.flags ^ kHeld);
    const Hold hold{fear, want.guid};
    // Holds on wants fulfilled or gone since no longer count.
    std::erase_if(held, [&](const Hold& h) {
        const auto& from = h.fear ? fears : wants;
        return h == hold || std::none_of(from.begin(), from.end(), [&](const auto& w) { return w.guid == h.guid && (w.flags & kHeld); });
    });
    if (want.flags & kHeld) {
        held.push_back(hold);
        while (held.size() > world.wantSlots(id).holds) {
            auto& from = held.front().fear ? fears : wants;
            for (auto& w : from) {
                if (w.guid == held.front().guid) {
                    w.flags = static_cast<std::uint8_t>(w.flags & ~kHeld);
                }
            }
            held.pop_front();
            ts2::diag::step("released the oldest hold");
        }
    }
    ts2::diag::step(std::string(want.flags & kHeld ? "held " : "released ") + (fear ? "a fear" : "a want"));
}

std::int16_t LotLife::aspirationScore(std::int16_t sim) {
    const auto* object = sim_->world().find(sim);
    return object ? object->personData[kAspirationScore] : 0;
}

std::span<const std::int16_t> LotLife::personData(std::int16_t sim) {
    const auto* object = sim_->world().find(sim);
    return object ? std::span<const std::int16_t>(object->personData) : std::span<const std::int16_t>{};
}

std::vector<LotLife::Relationship> LotLife::relationships(std::int16_t sim) {
    constexpr std::size_t kFamily = 0x3D, kSpecies = 0xBA, kGender = 0x41, kHidden = 0x44;
    auto& world = sim_->world();
    const auto* object = world.find(sim);
    const std::int16_t self = object ? world.neighborIdOf(*object) : 0;
    if (self == 0 || object->personData[kFamily] == 0) {
        return {};
    }
    std::set<std::int16_t> household, onLot;
    for (const std::int16_t member : sim_->household()) {
        if (const auto* m = world.find(member)) {
            household.insert(world.neighborIdOf(*m));
        }
    }
    for (const auto* p = world.nextAfter(0, [](const auto& o) { return o.isPerson(); }); p != nullptr;
         p = world.nextAfter(p->id, [](const auto& o) { return o.isPerson(); })) {
        onLot.insert(world.neighborIdOf(*p));
    }
    std::vector<Relationship> out;
    for (const std::int16_t other : world.neighborRelationshipTargets(self)) {
        if (other == self || world.neighbor(other) == nullptr) {
            continue;
        }
        auto value = [&](std::size_t i) { return world.neighborRelationshipValue(self, other, i); };
        auto person = [&](std::size_t i) { return world.neighborPersonValue(other, i); };
        Relationship r;
        r.neighbor = other;
        r.daily = value(0);
        r.flags = value(1);
        r.lifetime = value(2);
        r.familyTie = value(3) != 0;
        r.coins = (value(9) & 1) != 0;
        r.species = person(kSpecies);
        r.lifeStage = person(kLifeStage);
        r.female = person(kGender) == 1;
        r.skip = (person(kHidden) & 1) != 0;
        r.household = household.contains(other);
        r.onLot = onLot.contains(other);
        out.push_back(r);
    }
    return out;
}

std::vector<LotLife::InventoryObject> LotLife::inventoryObjects(std::int16_t sim) {
    auto& world = sim_->world();
    const auto* object = world.find(sim);
    const std::int16_t self = object ? world.neighborIdOf(*object) : 0;
    const auto* inventory = self ? world.inventory(ts2::sim::InventoryKind::Neighbor, self) : nullptr;
    std::vector<InventoryObject> out;
    if (inventory == nullptr) {
        return out;
    }
    for (const auto& token : inventory->singular) {
        if ((token->flags & 0x7) != 0x1) {
            continue;
        }
        const auto same = std::find_if(out.begin(), out.end(), [&](const InventoryObject& o) { return o.guid == token->guid; });
        if (same != out.end()) {
            ++same->count;
        } else {
            out.push_back({token->guid, 1});
        }
    }
    return out;
}

std::optional<std::int16_t> LotLife::inventoryValue(std::int16_t sim, std::uint32_t guid) {
    auto& world = sim_->world();
    const auto* object = world.find(sim);
    const std::int16_t self = object ? world.neighborIdOf(*object) : 0;
    const auto* inventory = self ? world.inventory(ts2::sim::InventoryKind::Neighbor, self) : nullptr;
    if (inventory == nullptr) {
        return std::nullopt;
    }
    for (const auto* list : {&inventory->singular, &inventory->counted}) {
        for (const auto& token : *list) {
            if (token->guid == guid && !token->properties.empty()) {
                return token->properties[0];
            }
        }
    }
    return std::nullopt;
}

bool LotLife::hasCountedToken(std::int16_t sim, std::uint32_t guid) {
    auto& world = sim_->world();
    const auto* object = world.find(sim);
    const std::int16_t self = object ? world.neighborIdOf(*object) : 0;
    const auto* inventory = self ? world.inventory(ts2::sim::InventoryKind::Neighbor, self) : nullptr;
    return inventory != nullptr &&
           std::any_of(inventory->counted.begin(), inventory->counted.end(), [&](const auto& token) { return token->guid == guid; });
}

std::vector<LotLife::Memory> LotLife::memories(std::int16_t sim) {
    // A memory token: flags 0x1 (visible) and 0x2 (memory), at least 12 values. Value 0 is the kind
    // (4 to 6 bad), values 6:5 the subject's GUID. The memory object must be of type 0x0F.
    constexpr std::uint16_t kMemoryFlags = 0x3;
    constexpr std::size_t kMemoryValues = 12;
    constexpr std::uint16_t kMemoryType = 0x0F;
    constexpr std::size_t kGender = 0x41;
    auto& world = sim_->world();
    auto& code = sim_->code();
    const auto* object = world.find(sim);
    const std::int16_t self = object ? world.neighborIdOf(*object) : 0;
    const auto* inventory = self ? world.inventory(ts2::sim::InventoryKind::Neighbor, self) : nullptr;
    if (inventory == nullptr) {
        return {};
    }
    // Line `line` of an object's catalog strings (0 its name, 1 its description, and for a Sim 2 its
    // last name), the women's set (the next string table) when asked for and not empty.
    auto catalog = [&](const ts2::sim::ObjectType& type, std::size_t line, bool female) {
        const auto set = static_cast<std::uint16_t>(type.definition.field(ts2::objd::field::kCatalogStringsId));
        std::string text = female ? code.catalogString(type.group, static_cast<std::uint16_t>(set + 1), line).value_or("") : "";
        return text.empty() ? code.catalogString(type.group, set, line).value_or("") : text;
    };
    auto putSubject = [](std::string text, const std::string& subject) {
        for (std::size_t at = text.find("$Subject"); at != std::string::npos; at = text.find("$Subject", at + subject.size())) {
            text.replace(at, 8, subject);
        }
        return text;
    };
    std::vector<Memory> out;
    for (auto it = inventory->singular.rbegin(); it != inventory->singular.rend(); ++it) {
        const auto& token = **it;
        if ((token.flags & kMemoryFlags) != kMemoryFlags || token.properties.size() < kMemoryValues) {
            continue;
        }
        const auto* type = code.objectType(token.guid);
        if (type == nullptr || type->definition.field(ts2::objd::field::kType) != kMemoryType) {
            continue;
        }
        Memory m;
        m.guid = token.guid;
        m.subject = static_cast<std::uint32_t>(static_cast<std::uint16_t>(token.properties[6])) << 16 | static_cast<std::uint16_t>(token.properties[5]);
        m.good = token.properties[0] < 4 || token.properties[0] > 6;
        m.pictureGroup = type->group;
        // The text follows the subject's gender. "$Subject" becomes the subject's name.
        const auto* subject = m.subject ? code.objectType(m.subject) : nullptr;
        bool female = false;
        if (subject != nullptr && subject->definition.field(ts2::objd::field::kType) == ts2::sim::kPersonType) {
            m.subjectNeighbor = world.neighborIdWithGuid(m.subject);
            female = m.subjectNeighbor != 0 && world.neighborPersonValue(m.subjectNeighbor, kGender) == 1;
            m.subjectFullName = catalog(*subject, 0, false) + " " + catalog(*subject, 2, false);
        }
        const std::string subjectName = subject ? catalog(*subject, 0, false) : "";
        m.name = putSubject(catalog(*type, 0, female), subjectName);
        m.description = putSubject(catalog(*type, 1, female), subjectName);
        out.push_back(std::move(m));
    }
    return out;
}

std::int16_t LotLife::lifeStage(std::int16_t sim) {
    const auto* object = sim_->world().find(sim);
    return object ? object->personData[kLifeStage] : 0;
}

std::array<int, 16> LotLife::motives(std::int16_t sim) {
    std::array<int, 16> out{};
    if (const auto* object = sim_->world().find(sim)) {
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = ts2::sim::wholeMotive(*object, i);
        }
    }
    return out;
}

int LotLife::mood(std::int16_t sim) {
    const auto* object = sim_->world().find(sim);
    return object ? ts2::sim::wholeMotive(*object, ts2::sim::motive::kMood) : 0;
}

std::optional<ts2::tga::Pixels> LotLife::portrait(std::int16_t sim) {
    const auto* object = sim_->world().find(sim);
    const auto image = object ? ts2::content::portraitImage(object->personData[kLifeStage]) : std::nullopt;
    const auto data = image ? sim_->simImage(sim, *image) : std::vector<std::uint8_t>{};
    return data.empty() ? std::nullopt : ts2::image::decode(data);
}

std::int16_t LotLife::neighborId(std::int16_t sim) {
    const auto* object = sim_->world().find(sim);
    return object ? sim_->world().neighborIdOf(*object) : 0;
}

std::optional<LotLife::PortraitFile> LotLife::neighborPortrait(std::int16_t neighbor) {
    const auto* n = sim_->world().neighbor(neighbor);
    if (n == nullptr || n->personData.size() <= kLifeStage) {
        return std::nullopt;
    }
    const auto image = ts2::content::portraitImage(n->personData[kLifeStage]);
    auto file = image ? sim_->characterFile(neighbor) : std::nullopt;
    return file ? std::optional<PortraitFile>(PortraitFile{std::move(*file), *image}) : std::nullopt;
}

bool LotLife::paused() { return sim_->globals()[ts2::sim::global::kPaused] != 0; }

void LotLife::setPaused(bool paused) {
    if (paused != this->paused()) {
        ts2::diag::step(paused ? "paused" : "unpaused");
    }
    sim_->globals()[ts2::sim::global::kPaused] = paused ? 1 : 0;
}

void LotLife::update(double ms, ts2::scene::Scene& scene) {
    using ts2::sim::LotSimulation;
    using ts2::sim::Speed;
    moved_.clear();
    changedInstances_.clear();
    relit_.clear();
    if (paused()) {
        pendingMs_ = 0;
    } else if (speed() == Speed::Ultra) {
        const auto start = std::chrono::steady_clock::now();
        do {
            sim_->tick();
        } while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() < kUltraBudgetMs);
        pendingMs_ = 0;
    } else {
        const double tickMs = speed() == Speed::High ? LotSimulation::kHighTickMs : LotSimulation::kNormalTickMs;
        pendingMs_ = std::min(pendingMs_ + ms, kMaxTicksPerUpdate * tickMs);
        while (pendingMs_ >= tickMs) {
            sim_->tick();
            pendingMs_ -= tickMs;
        }
    }
    const double tickMs = speed() == Speed::High ? LotSimulation::kHighTickMs : LotSimulation::kNormalTickMs;
    leadMs_ = pendingMs_ * LotSimulation::kNormalTickMs / tickMs;
    if (leadMs_ > 0) {
        sim_->tellAnimationEvents(sim_->world().clockMs() + leadMs_);
    }
    for (auto& actor : scene.actors) {
        if (const auto* object = actor.object ? sim_->world().find(static_cast<std::int16_t>(actor.object)) : nullptr) {
            auto& own = ownAnimations_[actor.object];
            const auto& now = object->animation;
            const double nowMs = sim_->world().clockMs() + leadMs_;
            const bool walking = object->walking && ts2::sim::alongPlan(*object->walking);
            bool ownEnding = false;
            if (walking) {
                // The walk is drawn from its plan. What the Sim did before stays, to fade out under it.
                if (!own.walking || own.walkStartMs != object->walking->startMs) {
                    own.walking = true;
                    own.walkStartMs = object->walking->startMs;
                    own.previous = own.current ? std::move(own.current) : std::move(own.held);
                    own.held.reset();
                }
                own.current = now;
            } else {
                if (own.walking) {
                    // The walk hands over to the stop's last pose, which it already shows.
                    own.walking = false;
                    own.walkEndMs = nowMs;
                    own.previous.reset();
                    own.current = now;
                }
                if (now && (!own.current || own.current->startMs != now->startMs || own.current->name != now->name)) {
                    // A new animation fades in over the one before it, or over the last pose of one
                    // that has played out, as the game's play clips follow one another.
                    own.previous = own.current ? std::move(own.current) : std::move(own.held);
                    own.current = *now;
                    own.held.reset();
                }
                if (own.previous && own.current && seconds() > own.current->startMs / 1000.0 + kOwnCrossfadeSeconds) {
                    own.previous.reset();
                }
                // An Animate Sim clip that has played and that nothing followed fades out, then is gone
                // (locomotion clips aside). One that doesn't blend out (Animate Sim byte 2 flag 0x40)
                // holds its last pose until the next animation takes over, as the game's does.
                if (!now && own.current && !own.current->loops && !own.current->baseHeading && own.current->blendsOut &&
                    own.current->finished(nowMs)) {
                    const double endMs = own.current->startMs + own.current->lengthMs;
                    const bool fading = object->ending && object->ending->startMs == own.current->startMs && object->ending->name == own.current->name;
                    if (!fading || nowMs >= endMs + kOwnCrossfadeSeconds * 1000.0) {
                        own.held = std::move(own.current);
                        own.current.reset();
                        own.previous.reset();
                    } else {
                        ownEnding = true;
                    }
                }
            }
            const SimFrame frame = simFrame(*object, nowMs);
            if (walking) {
                ts2::sim::walkPoses(object->walking->plan, frame.walked, walkPoses_);
                own.endedClips.clear();
                for (const auto& pose : walkPoses_) {
                    own.endedClips.push_back(*pose.clip);
                }
                own.endedPoses = walkPoses_;
                for (std::size_t i = 0; i < own.endedPoses.size(); ++i) {
                    own.endedPoses[i].clip = &own.endedClips[i];
                }
                const double fade = std::clamp((nowMs - object->walking->startMs) / kWalkFadeMs, 0.0, 1.0);
                playClips(actor, *object, std::nullopt, own.previous, frame.heading, animations_, &walkPoses_, static_cast<float>(fade), object->walking->startHeading);
            } else {
                const bool overWalk = own.walkEndMs >= 0 && own.current && std::abs(own.current->startMs - own.walkEndMs) <= 100 &&
                                      nowMs < own.current->startMs + kOwnCrossfadeSeconds * 1000.0;
                if (!overWalk && own.walkEndMs >= 0 && nowMs >= own.walkEndMs + kOwnCrossfadeSeconds * 1000.0) {
                    own.walkEndMs = -1;
                }
                playClips(actor, *object, own.current, own.previous, frame.heading, animations_, nullptr, 1, 0, ownEnding,
                          overWalk && !own.endedPoses.empty() ? &own.endedPoses : nullptr);
            }
            placeSim(actor, *object, frame, scene);
        }
    }
    addCreatedObjects(scene);
    placeObjects(scene);
    for (auto* list : {&changedInstances_, &relit_}) {
        std::sort(list->begin(), list->end());
        list->erase(std::unique(list->begin(), list->end()), list->end());
    }
    objectMaterials_.update(sim_->world(), scene);
    const double nowMs = sim_->world().clockMs() + leadMs_;
    for (auto& actor : scene.actors) {
        for (auto& h : actor.held) {
            const std::uint32_t id = h.instance < scene.instances.size() ? scene.instances[h.instance].object : 0;
            const auto* object = id > 0 && id <= static_cast<std::uint32_t>(INT16_MAX) ? sim_->world().find(static_cast<std::int16_t>(id)) : nullptr;
            const auto pose = object ? objectMaterials_.pose(*object, nowMs, animations_) : std::nullopt;
            h.pose = pose.value_or(ts2::scene::Transform{});
        }
    }
}

// An object a script moved goes where the simulation has it: into a Sim's hand (where the Sim's pose
// carries it), onto another object's slot, or onto its storey's floor, facing its direction. One a
// script removed is no longer drawn. Objects that haven't moved keep the place the lot gave them.
void LotLife::placeObjects(ts2::scene::Scene& scene) {
    for (const auto& placed : scene.placedObjects) {
        if (placed.object > static_cast<std::uint32_t>(INT16_MAX) || placed.instance >= scene.instances.size()) {
            continue;
        }
        const auto* object = sim_->world().find(static_cast<std::int16_t>(placed.object));
        ObjectPlace now;
        if (object == nullptr || object->removed) {
            now.gone = true;
        } else {
            now = {object->x, object->y, object->level, object->data[1], object->data[2], object->data[3], false};
        }
        const auto [it, first] = objectPlaces_.try_emplace(placed.object, now);
        if (first || it->second == now) {
            continue;
        }
        it->second = now;
        for (auto& actor : scene.actors) {
            std::erase_if(actor.held, [&](const ts2::scene::Actor::Held& h) { return h.instance == placed.instance; });
        }
        ts2::scene::Transform place;
        if (now.gone) {
            place.m.fill(0.0f);
        } else if (now.container > 0) {
            // Through holders with no model of their own (the head slots holder the plumbob sits in), an
            // object held by a Sim goes to the bone of the Sim's slot the outermost holder is in, as
            // the lot scene places it. Their own slot offsets aren't known.
            constexpr int kMaxDepth = 8;
            std::int16_t holderId = now.container, holderSlot = now.slot;
            auto holder = scene.actors.end();
            for (int depth = 0; depth <= kMaxDepth; ++depth) {
                holder = std::find_if(scene.actors.begin(), scene.actors.end(),
                                      [&](const ts2::scene::Actor& a) { return a.object == static_cast<std::uint32_t>(holderId); });
                const auto* outer = sim_->world().find(holderId);
                if (holder != scene.actors.end() || outer == nullptr || outer->removed || outer->isPerson() || outer->data[2] <= 0) {
                    break;
                }
                holderSlot = outer->data[3];
                holderId = outer->data[2];
            }
            if (holder != scene.actors.end()) {
                const auto at = static_cast<std::size_t>(holderSlot);
                holder->held.push_back({placed.instance, at < holder->slotNodes.size() ? holder->slotNodes[at] : -1});
                continue;
            }
            const auto container = std::find_if(scene.placedObjects.begin(), scene.placedObjects.end(),
                                                [&](const ts2::scene::PlacedObject& c) { return c.object == static_cast<std::uint32_t>(now.container); });
            if (container == scene.placedObjects.end() || container->instance >= scene.instances.size()) {
                continue;
            }
            const auto& base = scene.instances[container->instance].transform;
            const auto slot = static_cast<std::size_t>(now.slot);
            place = slot < container->slots.size() && container->slots[slot] ? base * *container->slots[slot] : base;
        } else if (now.x == ts2::sim::kOffLot || now.y == ts2::sim::kOffLot || !scene.floorHeight) {
            continue;
        } else {
            const float x = static_cast<float>(now.y) / 65536.0f;  // the lot's x and y are the position's y and x
            const float y = static_cast<float>(now.x) / 65536.0f;
            place = placed.lotFrame * ts2::scene::Transform::at(x, y, scene.floorHeight(now.level, x, y), ts2::sim::compassHeading(now.direction));
            scene.instances[placed.instance].level = scene.groundLevel + now.level - 1;
        }
        scene.instances[placed.instance].transform = place;
        moved_.emplace_back(placed.instance, place);
        relight(scene, placed.instance, placed.lotFrame.rigidInverse() * place, now.level - 1);
    }
}

void LotLife::relightAll(ts2::scene::Scene& scene) {
    for (const auto& placed : scene.placedObjects) {
        if (placed.instance >= scene.instances.size()) {
            continue;
        }
        const auto& instance = scene.instances[placed.instance];
        if (std::all_of(instance.transform.m.begin(), instance.transform.m.end(), [](float v) { return v == 0; })) {
            continue;  // removed by a script
        }
        relight(scene, placed.instance, placed.lotFrame.rigidInverse() * instance.transform, instance.level - scene.groundLevel);
    }
    std::sort(relit_.begin(), relit_.end());
    relit_.erase(std::unique(relit_.begin(), relit_.end()), relit_.end());
}

// An object takes the sun that reaches it under the buildings' shadow where it stands now, and a
// thing the lights of the room it is in (doors and windows keep theirs).
void LotLife::relight(ts2::scene::Scene& scene, std::size_t index, const ts2::scene::Transform& inLot, int storey) {
    // Light sets only ever grow. Past this a thing keeps the light it has.
    constexpr std::size_t kMaxLightSets = 4096;
    auto& instance = scene.instances[index];
    if (scene.thingSun && instance.part == ts2::scene::Part::Object) {
        ts2::scene::Instance placed = instance;
        placed.transform = inLot;
        if (auto sun = scene.thingSun(scene, placed); sun != instance.sun) {
            instance.sun = std::move(sun);
            relit_.push_back(index);
        }
    }
    if (!scene.thingLights || instance.lightGroup != ts2::scene::LightGroup::Props || instance.mesh < 0 ||
        static_cast<std::size_t>(instance.mesh) >= scene.meshes.size()) {
        return;
    }
    std::vector<int> parts;
    for (auto& set : scene.thingLights(scene.meshes[static_cast<std::size_t>(instance.mesh)], inLot,
                                       ts2::scene::objectlights::lightStorey(scene.floorHeight, inLot, storey))) {
        const auto same = std::find(scene.lightSets.begin(), scene.lightSets.end(), set);
        if (same == scene.lightSets.end() && scene.lightSets.size() >= kMaxLightSets) {
            return;
        }
        parts.push_back(static_cast<int>(same - scene.lightSets.begin()));
        if (same == scene.lightSets.end()) {
            scene.lightSets.push_back(std::move(set));
        }
    }
    int whole = -1;
    if (!parts.empty() && std::all_of(parts.begin(), parts.end(), [&](int set) { return set == parts[0]; })) {
        whole = parts[0];
        parts.clear();
    }
    if (whole != instance.lightSet || parts != instance.partLightSets) {
        instance.lightSet = whole;
        instance.partLightSets = std::move(parts);
        relit_.push_back(index);
    }
}

// Objects scripts create after the lot was loaded are drawn as the lot draws its objects: their model
// (see modelName) placed by placeObjects, which finds them moved from nowhere. One without a model gets
// an instance drawing nothing, so that RayTrace can still start or end on it: the game's point on an
// object is the middle of its model's bounds, which always hold its origin, so on a model with nothing
// in it the point is where the object stands. A removed one's instance draws nothing again and goes
// to the next object created.
void LotLife::addCreatedObjects(ts2::scene::Scene& scene) {
    // A lot that keeps creating objects can't grow the scene past this.
    constexpr std::size_t kMaxCreated = 4096;
    constexpr std::size_t kMaxSlots = 16;  // as the lot scene reads them
    auto& world = sim_->world();
    for (auto it = created_.begin(); it != created_.end();) {
        const auto* object = world.find(it->first);
        if (object != nullptr && !object->removed) {
            ++it;
            continue;
        }
        const std::size_t index = it->second;
        scene.instances[index] = ts2::scene::Instance{};
        std::erase_if(scene.placedObjects, [&](const ts2::scene::PlacedObject& p) { return p.instance == index; });
        for (auto& actor : scene.actors) {
            std::erase_if(actor.held, [&](const ts2::scene::Actor::Held& h) { return h.instance == index; });
        }
        objectPlaces_.erase(it->first);
        std::erase_if(moved_, [&](const auto& m) { return m.first == index; });
        freeInstances_.push_back(index);
        changedInstances_.push_back(index);
        it = created_.erase(it);
    }
    world.nextAfter(lastSeenId_, [&](const ts2::sim::SimObject& o) {
        lastSeenId_ = o.id;
        if (o.isPerson()) {
            return false;
        }
        if (created_.size() >= kMaxCreated) {
            ts2::diag::warning(std::format("object {} is not drawn: {} created objects already are", o.id, kMaxCreated));
            return false;
        }
        const std::string model = modelName(assets_, o);
        ts2::scene::Instance instance;
        instance.mesh = model.empty() ? -1 : objectMaterials_.model(model, scene);
        instance.name = std::format("{} {}", o.id, model);
        instance.part = ts2::scene::Part::Object;
        instance.object = static_cast<std::uint32_t>(o.id);
        instance.transform.m.fill(0.0f);
        std::size_t index = scene.instances.size();
        if (!freeInstances_.empty()) {
            index = freeInstances_.back();
            freeInstances_.pop_back();
            scene.instances[index] = std::move(instance);
        } else {
            scene.instances.push_back(std::move(instance));
        }
        ts2::scene::PlacedObject placed{static_cast<std::uint32_t>(o.id), index, lotFrame_, {}};
        for (std::size_t slot = 0; slot < kMaxSlots && !model.empty(); ++slot) {
            const std::string bone = assets_.slotBone(o.definition.guid(), slot);
            const ts2::scene::Transform* at = bone.empty() ? nullptr : objectMaterials_.bone(model, bone);
            placed.slots.push_back(at ? std::optional<ts2::scene::Transform>(*at) : std::nullopt);
        }
        while (!placed.slots.empty() && !placed.slots.back()) {
            placed.slots.pop_back();
        }
        scene.placedObjects.push_back(std::move(placed));
        created_[o.id] = index;
        // Never placed yet, so placeObjects places it.
        objectPlaces_[o.id] = ObjectPlace{.gone = true};
        changedInstances_.push_back(index);
        return false;
    });
}

} // namespace app
