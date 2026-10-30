#include "lot_life.hpp"

#include "content/neighborhood.hpp"
#include "diag/log.hpp"
#include "formats/image.hpp"
#include "scene/look_at.hpp"
#include "scene/newborn_face.hpp"
#include "scene/object_lights.hpp"
#include "sim/motives.hpp"
#include "sim/object_model.hpp"
#include "sim/ground_click.hpp"
#include "sim/pie_menu.hpp"
#include "sim/route_goals.hpp"
#include "sim/snap.hpp"
#include "sim/turn.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <limits>
#include <functional>
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
// An object's new own animation takes over from the one before it over this long where the game's
// rule is not known yet: after a pose that has played out, and between clips other than Animate Sim's.
constexpr double kOwnCrossfadeSeconds = 0.25;
// How far ahead a walk's way is read where the Sim hasn't moved yet (route slivers shorter than this don't turn it).
constexpr float kRouteAhead = 0.01f;

// An Animate Sim clip's weight as the game gives it: in over its fade in from its start, out over
// its fade out to its end, each clip on its own whatever plays after it. One without a fade out
// plays at full weight to its end and is gone there, the joints it drove keeping their last pose
// until another clip moves them.
void fadeAsAnimateSim(ts2::scene::Actor::Clip& clip, const ts2::sim::PlayingAnimation& a) {
    clip.end = (a.startMs + std::max(0.0f, a.lengthMs)) / 1000.0;
    clip.fadeIn = a.fadeInMs > 0;
    clip.fadeInSeconds = a.fadeInMs / 1000.0f;
    clip.fadeOut = true;
    clip.fadeOutSeconds = a.fadeOutMs / 1000.0f;
}

// Whether `previous` is an Animate Sim clip still playing as `latest` starts. One that has played
// out is gone, as the game drops each Animate Sim clip at its end.
bool stillPlaying(const ts2::sim::PlayingAnimation& previous, const ts2::sim::PlayingAnimation& latest) {
    return previous.fades && previous.startMs + previous.lengthMs > latest.startMs;
}

// What the game reads from an object to choose its shadow (see objectshadow), as the lot scene reads
// it for the lot's own objects.
ts2::scene::objectshadow::Facts shadowFacts(const ts2::sim::SimObject& o, const std::string& model, int mesh,
                                            const ts2::scene::Scene& scene) {
    ts2::scene::objectshadow::Facts f;
    f.type = o.definition.field(ts2::objd::field::kType);
    f.data8 = o.data[8];
    f.data13 = o.data[0x0D];
    f.data68 = o.data[0x44];
    if (mesh >= 0 && static_cast<std::size_t>(mesh) < scene.meshes.size()) {
        const auto& primitives = scene.meshes[static_cast<std::size_t>(mesh)].primitives;
        f.drawable = !primitives.empty() && ts2::scene::lower(model) != "nullmodel";
        f.hasShadowGroups = std::ranges::any_of(primitives, [](const ts2::scene::Primitive& p) {
            return ts2::scene::objectshadow::isShadowGroup(p.group);
        });
    }
    return f;
}

// The scene instances of the objects Sims hold.
std::set<std::size_t> heldInstances(const ts2::scene::Scene& scene) {
    std::set<std::size_t> held;
    for (const auto& actor : scene.actors) {
        for (const auto& h : actor.held) {
            held.insert(h.instance);
        }
    }
    return held;
}

// Whether `previous` is an Animate Sim clip still fading out as `latest` starts.
bool stillFading(const ts2::sim::PlayingAnimation& previous, const ts2::sim::PlayingAnimation& latest) {
    return previous.fades && previous.fadeOutMs > 0 && previous.startMs + previous.lengthMs > latest.startMs;
}

// Whether `latest` crossfades `previous`, still fading, as the game does between two Animate Sim
// clips that move the body (both have a body target): the latest fades in, and the previous one may
// fade out and has a fade's length or less left. The previous one then keeps the weight it has as
// the latest starts and falls from there to none over a fade, whatever its own fade had left.
bool crossfades(const ts2::sim::PlayingAnimation& previous, const ts2::sim::PlayingAnimation& latest, ts2::scene::AnimationLibrary& animations) {
    constexpr std::uint16_t kBodyTarget = 1;
    return stillFading(previous, latest) && latest.fades && previous.blendsOut && latest.blendsIn &&
           previous.startMs + previous.lengthMs - latest.startMs <= ts2::sim::kAnimateFadeMs &&
           animations.hasTargetType(previous.name, kBodyTarget) && animations.hasTargetType(latest.name, kBodyTarget);
}

// An Animate Sim clip's weight `ms` on the world's clock, by its fades.
float animateSimWeight(const ts2::sim::PlayingAnimation& a, double ms) {
    const double t = ms - a.startMs;
    const double in = a.fadeInMs > 0 ? std::clamp(t / a.fadeInMs, 0.0, 1.0) : 1.0;
    const double out = a.fadeOutMs > 0 ? std::clamp((a.lengthMs - t) / a.fadeOutMs, 0.0, 1.0) : 1.0;
    return static_cast<float>(std::min(in, out));
}

// When the animation before `latest` is gone: one still fading at its end, or a fade after the
// latest starts when the latest crossfades it. Others are gone once the latest has faded in.
double previousGoneMs(const ts2::sim::PlayingAnimation& previous, const ts2::sim::PlayingAnimation& latest,
                      ts2::scene::AnimationLibrary& animations) {
    if (crossfades(previous, latest, animations)) {
        return latest.startMs + ts2::sim::kAnimateFadeMs;
    }
    if (stillPlaying(previous, latest)) {
        return previous.startMs + previous.lengthMs;
    }
    return latest.startMs + kOwnCrossfadeSeconds * 1000.0;
}

// Where the Sim stood as a clip started, by the clip's name and start, if known.
using StartedAt = std::function<std::optional<ts2::scene::Transform>(const std::string& name, double startMs)>;

// What an object plays, as the actor's clips: its own animation and its overlays, or while a Sim
// walks, the walk's clips at the moments the distance walked gives them and at their weights (`walk`),
// over `previous`. `walkFade` rises from 0 to 1 over a fade from the walk's start. A locomotion
// clip plays turned from the heading the Sim is placed at (`frameHeading`) to its own base heading
// (see PlayingAnimation::baseHeading). Any other clip
// with a root joint plays on where the Sim stood as it started (`startedAt`), as the game places
// each clip: snapping the Sim to a slot or walking it off leaves such a clip where it was.
void playClips(ts2::scene::Actor& actor, const ts2::sim::SimObject& object, const std::optional<ts2::sim::PlayingAnimation>& own,
               const std::optional<ts2::sim::PlayingAnimation>& previous, float frameHeading, ts2::scene::AnimationLibrary& animations,
               const std::vector<ts2::sim::WalkPose>* walk = nullptr, float walkFade = 1, float walkStartHeading = 0, bool ownEnding = false,
               const std::vector<ts2::sim::WalkPose>* endedWalk = nullptr, float endedHeading = 0, const StartedAt& startedAt = {},
               const std::optional<ts2::scene::Transform>& endedPlace = std::nullopt) {
    actor.clips.clear();
    const int rootJoint = actor.rig ? actor.rig->find("root_trans") : -1;
    const auto placeFrom = [&](ts2::scene::Actor::Clip& clip, const std::string& name, double startMs) {
        const auto has = [&](const std::vector<int>& channels) {
            return rootJoint >= 0 && static_cast<std::size_t>(rootJoint) < channels.size() && channels[static_cast<std::size_t>(rootJoint)] >= 0;
        };
        if (startedAt && (has(clip.animation->translation) || has(clip.animation->rotation))) {
            clip.playedAt = startedAt(name, startMs);
        }
    };
    // A turn's route holds where the turn started, facing its base heading, so its clip plays from
    // there while it fades out. Without a start place it fades out without its root's place
    // (`leaveOut`).
    const auto placeTurn = [&](ts2::scene::Actor::Clip& clip, const ts2::sim::PlayingAnimation& a, bool& leaveOut) {
        const auto at = startedAt ? startedAt(a.name, a.startMs) : std::nullopt;
        if (!at || !a.baseHeading) {
            leaveOut = true;
            return;
        }
        const auto inLot = actor.lotFrame.rigidInverse() * *at;
        clip.turn = ts2::sim::wrapAngle(*a.baseHeading - std::atan2(inLot.m[4], inLot.m[0]));
        clip.playedAt = at;
    };
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
        // An Animate Sim clip before the walk plays out its own fades under it and is gone at its end.
        // Any other fades out linearly as the walk starts, as how the game hands those over is not
        // known yet.
        const bool ownFades = previous && previous->fades;
        if (previous && (ownFades || walkFade < 1)) {
            if (auto* clip = add(previous->name, previous->startMs, previous->lengthMs, previous->speed, previous->flipped, previous->baseHeading)) {
                clip->layer = previous->layer;
                if (ownFades) {
                    fadeAsAnimateSim(*clip, *previous);
                } else {
                    clip->weight = 1 - walkFade;
                    clip->fadeIn = clip->fadeOut = false;
                }
                clip->repeats = previous->loops;
                if (previous->baseHeading) {
                    placeTurn(*clip, *previous, clip->noRootMove);
                } else {
                    placeFrom(*clip, previous->name, previous->startMs);
                }
            }
        }
        for (const auto& pose : *walk) {
            if (auto* clip = add(pose.clip->name, 0, pose.clip->lengthMs, 1, pose.mirrored)) {
                clip->weight = pose.weight;
                clip->atMs = pose.phase * pose.clip->lengthMs;
                clip->turn = pose.turn;
                clip->fadeIn = clip->fadeOut = false;
            }
        }
    }
    // An Animate Sim clip still fading out as the latest starts plays out its own fades under it, or
    // the latest's crossfade (see crossfades), its root blended with the latest's. Otherwise the
    // animation before fades out as the latest fades in, both holding their last pose once played.
    // Between two animations only the upper one's weight changes, as the game's mixer gives the
    // layer under the top 1 less the top's weight: the latest fades in over one below it or on its
    // own layer, and one above it fades out over it. A turn plays on from where it started, facing
    // its base heading, and a stop the walk left held fades out by its body only. Any other plays
    // on where it started, so a stair step's clip stays a step down as the Sim snaps a step on, and
    // getting into bed keeps its elbows still as lying there places the Sim 0.92 m on.
    const bool crossfade = previous && own;
    if (!walk && previous && own) {
        if (auto* clip = add(previous->name, previous->startMs, previous->lengthMs, previous->speed, previous->flipped, previous->baseHeading)) {
            clip->layer = previous->layer;
            clip->objectPlace = objectPlace(*previous);
            clip->ikActor = static_cast<std::uint32_t>(std::max<std::int16_t>(previous->ikTarget, 0));
            if (crossfades(*previous, *own, animations)) {
                clip->weight = animateSimWeight(*previous, own->startMs);
                clip->end = (own->startMs + ts2::sim::kAnimateFadeMs) / 1000.0;
                clip->fadeIn = false;
                clip->fadeOut = true;
                clip->fadeOutSeconds = ts2::sim::kAnimateFadeMs / 1000.0f;
            } else if (stillPlaying(*previous, *own)) {
                // An Animate Sim clip plays out its own fades under the latest.
                fadeAsAnimateSim(*clip, *previous);
            } else {
                clip->end = own->startMs / 1000.0 + kOwnCrossfadeSeconds;
                clip->fadeIn = false;
                clip->fadeOut = previous->layer >= own->layer;
            }
            if (previous->baseHeading) {
                placeTurn(*clip, *previous, clip->bodyOnly);
            } else {
                placeFrom(*clip, previous->name, previous->startMs);
            }
        }
    }
    // A walk that has handed back plays on under what follows, a new walk too, on its own layer, until
    // its fade is over. Its clips play where the walk has got to on its path (`endedPlace`), as the
    // game's route goes on to the path's end and holds there. Without that place they keep only the
    // body's joints, their limb controls turned to the heading the walk ended on.
    if (endedWalk) {
        for (const auto& pose : *endedWalk) {
            if (auto* clip = add(pose.clip->name, 0, pose.clip->lengthMs, 1, pose.mirrored)) {
                clip->weight = pose.weight;
                clip->atMs = pose.phase * pose.clip->lengthMs;
                clip->fadeIn = clip->fadeOut = false;
                if (endedPlace) {
                    clip->turn = pose.turn;
                    clip->playedAt = endedPlace;
                } else {
                    clip->turn = pose.turn + ts2::sim::wrapAngle(endedHeading - frameHeading);
                    clip->bodyOnly = true;
                }
            }
        }
    }
    if (!walk && own) {
        if (auto* clip = add(own->name, own->startMs, own->lengthMs, own->speed, own->flipped, own->baseHeading)) {
            clip->layer = own->layer;
            clip->objectPlace = objectPlace(*own);
            clip->ikActor = static_cast<std::uint32_t>(std::max<std::int16_t>(own->ikTarget, 0));
            if (own->fades) {
                fadeAsAnimateSim(*clip, *own);
            } else {
                clip->fadeIn = (crossfade && own->layer >= previous->layer) || endedWalk != nullptr;
                clip->fadeOut = ownEnding;
                if (ownEnding) {
                    clip->end += kOwnCrossfadeSeconds;
                }
            }
            clip->repeats = own->loops;
            if (!own->baseHeading) {
                placeFrom(*clip, own->name, own->startMs);
            }
        }
    }
    for (const auto& overlay : object.overlays) {
        auto* clip = add(overlay.name, overlay.startMs, overlay.plays < 0 ? 0.0 : overlay.lengthMs, overlay.speed, overlay.flipped);
        if (clip) {
            clip->weight = overlay.weight;
            clip->layer = kOverlayLayers[std::min<std::size_t>(overlay.priority, 4)];
            clip->ikActor = static_cast<std::uint32_t>(std::max<std::int16_t>(overlay.ikTarget, 0));
            clip->repeats = overlay.plays != 1;
            clip->fadeIn = overlay.blendsIn;
            clip->fadeOut = overlay.blendsOut;
            clip->fadeInSeconds = overlay.blendInMs.value_or(ts2::sim::kOverlayBlendMs) / 1000.0f;
            clip->fadeOutSeconds = overlay.blendOutMs.value_or(ts2::sim::kOverlayBlendMs) / 1000.0f;
            if (overlay.stoppedAt) {
                // Stopped: from the weight it had then down to none over the stop's blend.
                clip->weight *= *overlay.stoppedAt;
                clip->fadeIn = false;
                clip->fadeOut = true;
            }
            placeFrom(*clip, overlay.name, overlay.startMs);
        }
    }
}

// A stopped walk that settled at `settledMs` (see SimObject::walkSettledMs): the pose the Sim
// was in fades out, held where it stood then (`at`), while the walk pose fades in and out.
void addSettleClips(ts2::scene::Actor& actor, const ts2::sim::SimObject& object, double settledMs, const std::optional<ts2::scene::Transform>& at,
                    ts2::scene::AnimationLibrary& animations) {
    const double start = settledMs / 1000.0;
    auto& still = actor.clips.emplace_back();
    still.frozen = true;
    still.start = start;
    still.end = start + ts2::sim::kSettlePoseMs / 1000.0;
    still.fadeIn = false;
    still.fadeOutSeconds = ts2::sim::kSettlePoseMs / 1000.0f;
    still.playedAt = at;
    const std::string name = ts2::sim::walkPoseClip(object.personData[ts2::sim::kLifeStagePersonData]);
    if (auto bound = animations.bound(actor.rig, name, false)) {
        auto& pose = actor.clips.emplace_back();
        pose.animation = std::move(bound);
        pose.start = start;
        pose.end = start + ts2::sim::kSettleWalkPoseMs / 1000.0;
        pose.fadeInSeconds = pose.fadeOutSeconds = ts2::sim::settleWalkPoseFadeMs() / 1000.0f;
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
// `slotAt` gives where an object's slot is (see LotLife::slotPlace). A slot of a multi-tile part
// drawn by its lead (the far side of a double bed) is placed by `partSlot`. A Sim in another Sim's
// slot is carried: posing hangs it at that Sim's slot bone (see Actor::carrier), and until then it
// stands where it was last posed (`pose`), else at its carrier's place.
void placeSim(ts2::scene::Actor& actor, const ts2::sim::SimObject& sim, const SimFrame& frame, const ts2::scene::Scene& scene,
              const std::function<std::optional<ts2::scene::Transform>(const ts2::scene::PlacedObject&, std::size_t slot)>& slotAt,
              const std::function<std::optional<ts2::scene::Transform>(std::int16_t part, std::size_t slot)>& partSlot,
              const ts2::scene::ScenePose* pose) {
    constexpr std::size_t kContainerField = 2, kSlotField = 3;
    actor.carrier = 0;
    if (!actor.floorHeight || sim.x == ts2::sim::kOffLot || sim.y == ts2::sim::kOffLot) {
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
    actor.place = actor.lotFrame * ts2::scene::Transform::at(x, y, actor.floorHeight(sim.level, x, y), frame.heading);
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

// The model a created object is drawn with (see createdModelName).
std::string modelName(ts2::scene::Assets& assets, const ts2::sim::SimObject& object) {
    constexpr std::uint32_t kModelNames = 0x85;
    return ts2::sim::createdModelName(object, [&](std::size_t row) { return assets.objectString(object.group, kModelNames, row); });
}

} // namespace

void LotLife::OwnAnimations::noteStarts(const ts2::sim::SimObject& object, const ts2::scene::Transform& place, bool walks) {
    const auto drawn = [&](const Started& s) {
        const auto is = [&](const std::optional<ts2::sim::PlayingAnimation>& a) { return a && a->startMs == s.startMs && a->name == s.name; };
        return is(current) || is(previous) || is(held) ||
               std::any_of(object.overlays.begin(), object.overlays.end(),
                           [&](const ts2::sim::PlayingOverlay& o) { return o.startMs == s.startMs && o.name == s.name; });
    };
    std::erase_if(started, [&](const auto& s) { return !drawn(s); });
    const auto note = [&](const std::string& name, double startMs) {
        if (std::none_of(started.begin(), started.end(), [&](const auto& s) { return s.startMs == startMs && s.name == name; })) {
            started.push_back({name, startMs, place});
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
    sim_->world().setHasOutfit([this](const ts2::sim::SimObject& object, std::uint32_t category) -> std::optional<bool> {
        const std::int16_t neighbor = sim_->world().neighborIdOf(object);
        if (const auto file = neighbor != 0 ? sim_->characterFile(neighbor) : std::nullopt) {
            return outfits_.hasOutfit(*file, category);
        }
        return outfits_.hasOutfit(object.group, category);
    });
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

ts2::sim::SimObject* LotLife::lotObject(std::uint32_t id) {
    return id > 0 && id <= static_cast<std::uint32_t>(INT16_MAX) ? sim_->world().find(static_cast<std::int16_t>(id)) : nullptr;
}

bool LotLife::hoverable(std::uint32_t object) {
    auto* found = lotObject(object);
    auto* actor = sim_->world().find(selected());
    if (found == nullptr || actor == nullptr) {
        return false;
    }
    return !ts2::sim::pieMenu(sim_->vm(), sim_->code(), *actor, *found, {.firstOnly = true}).choices.empty();
}

bool LotLife::isSim(std::uint32_t object) {
    const auto* found = lotObject(object);
    return found != nullptr && found->isPerson();
}

std::uint32_t LotLife::partAt(std::uint32_t object, const std::array<float, 3>& scenePoint, const ts2::scene::Scene& scene) {
    const auto* found = lotObject(object);
    const auto multiTile = [](const ts2::sim::SimObject& o) { return o.definition.field(ts2::objd::field::kMultiTileMasterId) != 0; };
    if (found == nullptr || !multiTile(*found) || !scene.floorHeight) {
        return object;
    }
    const std::int16_t lead = found->data[ts2::sim::kLeadTileField];
    std::uint32_t best = object;
    float nearest = std::numeric_limits<float>::max();
    sim_->world().nextAfter(0, [&](const ts2::sim::SimObject& o) {
        if (o.data[ts2::sim::kLeadTileField] != lead || !multiTile(o) || o.removed || o.x == ts2::sim::kOffLot || o.y == ts2::sim::kOffLot) {
            return false;
        }
        const float x = static_cast<float>(o.y) / 65536.0f;  // the lot's x and y are the position's y and x
        const float y = static_cast<float>(o.x) / 65536.0f;
        float spot[3] = {x, y, scene.floorHeight(o.level, x, y)};
        lotFrame_.apply(spot, true);
        const float dx = spot[0] - scenePoint[0], dy = spot[1] - scenePoint[1], dz = spot[2] - scenePoint[2];
        const float d = dx * dx + dy * dy + dz * dz;
        if (d < nearest) {
            nearest = d;
            best = static_cast<std::uint32_t>(o.id);
        }
        return false;
    });
    return best;
}

std::vector<ts2::sim::PieChoice> LotLife::pieMenu(std::uint32_t object) {
    auto* found = lotObject(object);
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
    auto* found = lotObject(object);
    auto* actor = sim_->world().find(selected());
    if (found == nullptr || actor == nullptr) {
        return;
    }
    if (ts2::sim::runsAtOnce(sim_->code(), *found, choice)) {
        ts2::diag::step(std::format("Sim {} does \"{}\" on {} {} at once", actor->id, choice.name, found->definition.name, found->id));
        const auto r = ts2::sim::runAtOnce(sim_->vm(), sim_->code(), sim_->world(), *actor, *found, choice);
        if (r.outcome != ts2::sim::Outcome::True && r.outcome != ts2::sim::Outcome::False) {
            ts2::diag::warning(std::format("simulation: \"{}\" stopped: {} {}", choice.name, r.reason, r.detail));
        }
        return;
    }
    if (const auto queued = ts2::sim::queuedChoice(sim_->code(), sim_->world(), *found, choice)) {
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
    if (person->running) {
        add(*person->running, !person->running->nested, person->actionIconObject, person->actionIconGuid, person->actionIconIndex, person->actionString);
    }
    if (person->runningSub) {
        add(*person->runningSub, person->runningSub->nested, person->runningSub->icon(), person->runningSub->iconGuid, person->runningSub->iconIndex,
            person->runningSub->name);
    }
    for (const auto& q : person->queue) {
        add(q, false, q.icon(), q.iconGuid, q.iconIndex, q.name);
    }
    return out;
}

bool LotLife::cancelInteraction(std::int16_t sim, std::uint32_t id) {
    auto* person = sim_->world().find(sim);
    if (person == nullptr || !ts2::sim::cancelInteraction(sim_->world(), *person, id)) {
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
    if (auto given = world.neighborFirstName(id)) {
        name.first = std::move(*given);
    }
    if (auto taken = world.neighborSurname(id)) {
        name.last = std::move(*taken);
    }
    return name;
}

LotLife::SimName LotLife::simName(std::int16_t sim) {
    constexpr std::size_t kTitle = 0xE6;
    auto& world = sim_->world();
    const auto* object = world.find(sim);
    if (object == nullptr) {
        return {};
    }
    SimName name;
    const std::int16_t neighbor = world.neighborIdOf(*object);
    if (auto known = neighborName(neighbor)) {
        name.first = std::move(known->first);
        name.last = std::move(known->last);
    } else if (const auto* type = sim_->code().objectType(object->definition.guid())) {
        const auto set = static_cast<std::uint16_t>(type->definition.field(ts2::objd::field::kCatalogStringsId));
        name.first = sim_->code().catalogString(type->group, set, 0).value_or("");
    }
    name.title = kTitle < object->personData.size() ? object->personData[kTitle] : world.neighborPersonValue(neighbor, kTitle);
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

std::vector<std::int16_t> LotLife::inventoryValues(std::int16_t sim, std::uint32_t guid) {
    auto& world = sim_->world();
    const auto* object = world.find(sim);
    const std::int16_t self = object ? world.neighborIdOf(*object) : 0;
    const auto* inventory = self ? world.inventory(ts2::sim::InventoryKind::Neighbor, self) : nullptr;
    if (inventory == nullptr) {
        return {};
    }
    for (const auto* list : {&inventory->singular, &inventory->counted}) {
        for (const auto& token : *list) {
            if (token->guid == guid) {
                return token->properties;
            }
        }
    }
    return {};
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
    // The bone is the first joint its "Model - Bones" entry names, in the model at rest.
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
    }
    return aim;
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
                    const ts2::scene::SceneRays::Point point{static_cast<std::uint32_t>(aim.object), target->pointKind, target->slot, aim.local};
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
    if (paused()) {
        // The time already run towards the next tick is kept, so the Sims hold the moment they were
        // drawn at instead of stepping back to the last tick.
        sim_->pausedPass();
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
            const auto tailOver = [&own](double ms) {
                return own.tail && ms - own.tail->startMs >= ts2::sim::walkFadeOutMs(own.tail->plan) + ts2::sim::kWalkFadeMs;
            };
            // A stopped walk that settled: what the Sim held is gone.
            if (object->walkSettledMs && own.settledMs != object->walkSettledMs) {
                own.settledMs = object->walkSettledMs;
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
                if (!own.walking || own.walkStartMs != object->walking->startMs) {
                    // The walk drawn last handed back and this one started within the same update.
                    // Its clips play out their fade under this one, as when it handed back on an update of its own.
                    if (own.walking && object->walkTail && object->walkTail->startMs == own.walkStartMs) {
                        own.tail = object->walkTail;
                        own.tailHeading = own.endedHeading;
                        own.current.reset();
                    }
                    own.walking = true;
                    own.walkStartMs = object->walking->startMs;
                    if (tailOver(own.walkStartMs)) {
                        own.tail.reset();
                    }
                    own.previous = own.current ? std::move(own.current) : own.tail ? std::nullopt : std::move(own.held);
                    // An Animate Sim clip that has played out is gone.
                    if (own.previous && own.previous->fades && own.previous->finished(object->walking->startMs)) {
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
                    if (object->walkTail && object->walkTail->startMs == own.walkStartMs) {
                        own.tail = object->walkTail;
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
                    const bool fading = object->ending && object->ending->startMs == own.current->startMs && object->ending->name == own.current->name;
                    // An Animate Sim clip fades within its own length, others over a while after it.
                    const double goneMs = own.current->fades ? endMs : endMs + kOwnCrossfadeSeconds * 1000.0;
                    if (!fading || nowMs >= goneMs) {
                        own.held = std::move(own.current);
                        own.current.reset();
                        own.previous.reset();
                    } else {
                        ownEnding = true;
                    }
                }
            }
            const SimFrame frame = simFrame(*object, nowMs);
            // As the game places each clip, one playing as the Sim snaps into a carrier's slot stays where it
            // was in the world, and from then on hangs from the slot's bone with the Sim: its start place is
            // kept as seen from the bone, taken once, and follows the bone.
            constexpr std::size_t kContainerField = 2;
            const ts2::scene::Transform* hung = nullptr;
            if (pose_ != nullptr && object->data[kContainerField] > 0) {
                if (const auto found = pose_->carried.find(actor.object); found != pose_->carried.end()) {
                    hung = &found->second;
                }
            }
            for (auto& s : own.started) {
                if (hung == nullptr) {
                    s.fromCarrier.reset();
                    continue;
                }
                if (!s.fromCarrier) {
                    s.fromCarrier = hung->rigidInverse() * s.place;
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
                    tailPlace = actor.lotFrame * ts2::scene::Transform::at(at.y, at.x, actor.floorHeight(own.tail->level, at.y, at.x), heading);
                }
            }
            const auto* tailPoses = own.tail ? &tailPoses_ : nullptr;
            if (walking) {
                // The walk's clips weigh by the walk's own fades, half as it starts (see walkWeight).
                ts2::sim::walkPoses(object->walking->plan, frame.walked, walkPoses_, false);
                const float weight = ts2::sim::walkWeight(object->walking->plan, static_cast<float>(nowMs - object->walking->startMs));
                for (auto& pose : walkPoses_) {
                    pose.weight *= weight;
                }
                own.endedHeading = frame.heading;
                const double fade = std::clamp((nowMs - object->walking->startMs) / ts2::sim::kWalkFadeMs, 0.0, 1.0);
                playClips(actor, *object, std::nullopt, own.previous, frame.heading, animations_, &walkPoses_, static_cast<float>(fade), object->walking->startHeading,
                          false, tailPoses, own.tailHeading, startedAt, tailPlace);
            } else {
                playClips(actor, *object, own.current, own.previous, frame.heading, animations_, nullptr, 1, 0, ownEnding, tailPoses, own.tailHeading,
                          startedAt, tailPlace);
            }
            if (own.settledMs && nowMs < *own.settledMs + ts2::sim::kSettleWalkPoseMs) {
                addSettleClips(actor, *object, *own.settledMs, own.settledAt, animations_);
            }
            placeSim(
                actor, *object, frame, scene, [&](const ts2::scene::PlacedObject& p, std::size_t slot) { return slotPlace(p, slot, scene); },
                [&](std::int16_t part, std::size_t slot) { return partSlot(part, slot, scene); }, pose_);
            own.noteStarts(*object, actor.place, walking);
            setSimStorey(scene, actor, object->level);
            // The game picks a Sim's lights afresh as it moves.
            if (actor.place.m != own.litPlace || object->level != own.litLevel) {
                own.litPlace = actor.place.m;
                own.litLevel = object->level;
                const auto inLot = actor.lotFrame.rigidInverse() * actor.place;
                for (const auto& skinned : actor.skinned) {
                    relight(scene, skinned.instance, inLot, object->level - 1);
                }
            }
        }
    }
    addCreatedObjects(scene);
    placeObjects(scene);
    heldNow_ = heldInstances(scene);
    followClips(scene);
    animateObjects(scene);
    shadeHeld(scene);
    for (auto* list : {&changedInstances_, &relit_}) {
        std::sort(list->begin(), list->end());
        list->erase(std::unique(list->begin(), list->end()), list->end());
    }
    objectMaterials_.update(sim_->world(), scene);
    lookHeads(scene, sim_->world().clockMs() + leadMs_);
}

// An object a script moved goes where the simulation has it: into a Sim's hand (where the Sim's pose
// carries it), onto another object's slot, or onto its storey's floor, facing its direction. One a
// script removed is no longer drawn. Objects that haven't moved keep the place the lot gave them.
void LotLife::placeObjects(ts2::scene::Scene& scene) {
    for (const auto& placed : scene.placedObjects) {
        if (placed.object > static_cast<std::uint32_t>(INT16_MAX) || placed.instance >= scene.instances.size()) {
            continue;
        }
        // An object the lot keeps until it unloads is still drawn.
        const auto* object = sim_->world().findIncludingRemoved(static_cast<std::int16_t>(placed.object));
        ObjectPlace now;
        if (object == nullptr || (object->removed && !object->keptUntilUnload)) {
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
            place = slotPlace(*container, static_cast<std::size_t>(now.slot), scene).value_or(standingPlace(*container, scene));
        } else if (now.x == ts2::sim::kOffLot || now.y == ts2::sim::kOffLot || !scene.floorHeight) {
            continue;
        } else {
            const float x = static_cast<float>(now.y) / 65536.0f;  // the lot's x and y are the position's y and x
            const float y = static_cast<float>(now.x) / 65536.0f;
            place = placed.lotFrame * ts2::scene::Transform::at(x, y, scene.floorHeight(now.level, x, y), ts2::sim::compassHeading(now.direction));
        }
        if (!now.gone) {
            // An object in a slot is on its container's storey, as the simulation keeps it.
            scene.instances[placed.instance].level = scene.groundLevel + now.level - 1;
        }
        scene.instances[placed.instance].transform = place;
        clipBases_.erase(placed.instance);
        moved_.emplace_back(placed.instance, place);
        relight(scene, placed.instance, placed.lotFrame.rigidInverse() * place, now.level - 1);
        if (!now.gone && unshaded_.erase(placed.instance) > 0) {
            shadeCreated(scene, placed.instance, now.direction);
        }
    }
}

// A created object's shadow, chosen where it is first placed out of a Sim's hands, as a lot object's
// is where the lot has it: its shadow groups indoors or its sun picture outdoors, as it asks.
void LotLife::shadeCreated(ts2::scene::Scene& scene, std::size_t index, std::int16_t direction) {
    using namespace ts2::scene;
    auto& instance = scene.instances[index];
    const auto asked = objectshadow::requested(instance.shadowFacts);
    const int storey = objectlights::lightStorey(scene.floorHeight, instance.transform, instance.level - scene.groundLevel);
    const bool outdoors = scene.roomAt && scene.roomAt(storey, instance.transform.m[3], instance.transform.m[7]) == 0;
    instance.shadow = objectshadow::resolved(asked, outdoors, instance.shadowFacts);
    if (asked == objectshadow::Kind::Sun) {
        instance.shadowMesh = instance.mesh;
        instance.shadowFacing = static_cast<std::uint8_t>((2 - direction) & 7);
    }
    // A shadow it had before a Sim took it gives way to this one.
    heldShadows_.erase(index);
}

// Where container slot `slot` of a multi-tile part without a model of its own holds what is in it:
// at the bone of the part's slot in its lead's model, as the lead draws the whole object. None for
// other objects, and when the lead's model has no such bone.
std::optional<ts2::scene::Transform> LotLife::partSlot(std::int16_t part, std::size_t slot, const ts2::scene::Scene& scene) {
    const auto* object = sim_->world().find(part);
    if (object == nullptr || object->data[ts2::sim::kLeadTileField] == part) {
        return std::nullopt;
    }
    const auto lead = std::find_if(scene.placedObjects.begin(), scene.placedObjects.end(), [&](const ts2::scene::PlacedObject& p) {
        return p.object == static_cast<std::uint32_t>(object->data[ts2::sim::kLeadTileField]);
    });
    if (lead == scene.placedObjects.end() || lead->model.empty() || lead->instance >= scene.instances.size()) {
        return std::nullopt;
    }
    const auto key = std::pair<std::int16_t, std::size_t>(part, slot);
    auto [it, first] = partSlots_.try_emplace(key);
    if (first) {
        const std::string bone = assets_.slotBone(object->definition.guid(), slot);
        const auto rig = bone.empty() ? nullptr : objectMaterials_.objectRig(lead->model, lead->savedJoints);
        const int node = rig ? rig->find(ts2::scene::lower(bone)) : -1;
        if (node >= 0) {
            std::vector<ts2::scene::Transform> rest;
            ts2::scene::poseRig(*rig, nullptr, 0, rest);
            it->second = rest[static_cast<std::size_t>(node)];
        }
    }
    if (!it->second) {
        return std::nullopt;
    }
    return standingPlace(*lead, scene) * *it->second;
}

// Where a lot object's container slot `slot` is in the world, none when its model has no such bone.
// A slot hangs under bones of the object's own, which its clip poses: a dining chair's getting-in
// clip holds the seat still as the chair slides out and back, and one that leaves the chair out
// takes the seat out with it.
std::optional<ts2::scene::Transform> LotLife::slotPlace(const ts2::scene::PlacedObject& placed, std::size_t slot, const ts2::scene::Scene& scene) {
    if (slot >= placed.slots.size() || !placed.slots[slot]) {
        return std::nullopt;
    }
    const auto& base = standingPlace(placed, scene);
    const auto* object = !placed.followJoint.empty() && placed.object <= static_cast<std::uint32_t>(INT16_MAX)
                             ? sim_->world().find(static_cast<std::int16_t>(placed.object))
                             : nullptr;
    if (object != nullptr && !object->removed && object->animation) {
        const std::string bone = ts2::scene::lower(assets_.slotBone(object->definition.guid(), slot));
        const double nowMs = sim_->world().clockMs() + leadMs_;
        if (const auto posed = bone.empty() ? std::nullopt : objectMaterials_.jointPose(*object, nowMs, animations_, placed.model, bone)) {
            return base * *posed;
        }
    }
    return base * *placed.slots[slot];
}

// Where a lot object stands without its clip, which moves its model from there (see followClips).
const ts2::scene::Transform& LotLife::standingPlace(const ts2::scene::PlacedObject& placed, const ts2::scene::Scene& scene) const {
    const auto base = clipBases_.find(placed.instance);
    return base != clipBases_.end() ? base->second : scene.instances[placed.instance].transform;
}

// An object playing a clip (Animate Object) whose whole drawn model hangs under one joint moves
// with that joint, from where it stands: a dining chair sliding in under its table or out again.
// It keeps the clip's last pose once the clip has played.
void LotLife::followClips(ts2::scene::Scene& scene) {
    const double nowMs = sim_->world().clockMs() + leadMs_;
    for (auto& placed : scene.placedObjects) {
        if (placed.followJoint.empty() || placed.object > static_cast<std::uint32_t>(INT16_MAX) ||
            placed.instance >= scene.instances.size() || heldNow_.contains(placed.instance)) {
            continue;
        }
        const auto* object = sim_->world().find(static_cast<std::int16_t>(placed.object));
        if (object == nullptr || object->removed || !object->animation) {
            continue;
        }
        const auto joint = objectMaterials_.jointPose(*object, nowMs, animations_, placed.model, placed.followJoint, &placed.bones);
        if (!joint) {
            continue;
        }
        auto& instance = scene.instances[placed.instance];
        const auto base = clipBases_.try_emplace(placed.instance, instance.transform).first;
        for (auto& bone : placed.bones) {
            bone.second = base->second * bone.second;
        }
        const ts2::scene::Transform place = base->second * *joint * placed.followRest.rigidInverse();
        if (place.m != instance.transform.m) {
            instance.transform = place;
            moved_.emplace_back(placed.instance, place);
        }
    }
}

// Any other object whose clips move its model is drawn skinned to the model's joints from its first
// clip on, as a Sim is: its own clip (Animate Object) and the overlays laid on it. A bed's covers
// fold back as a Sim gets in and a fridge's doors swing open. One a Sim holds hangs from the Sim's
// hand (see poseActors) while its clip moves its joints: a snack bag tips as the Sim eats.
void LotLife::animateObjects(ts2::scene::Scene& scene) {
    const double nowMs = sim_->world().clockMs() + leadMs_;
    for (const auto& placed : scene.placedObjects) {
        if ((!placed.followJoint.empty() && !heldNow_.contains(placed.instance)) || placed.model.empty() ||
            placed.object > static_cast<std::uint32_t>(INT16_MAX) || placed.instance >= scene.instances.size()) {
            continue;
        }
        const auto* object = sim_->world().find(static_cast<std::int16_t>(placed.object));
        auto actor = std::find_if(scene.objectActors.begin(), scene.objectActors.end(),
                                  [&](const ts2::scene::Actor& a) { return a.object == placed.object; });
        // One a script removed is no longer drawn (placeObjects gave its instance no place).
        if (object == nullptr || object->removed || object->isPerson()) {
            if (actor != scene.objectActors.end()) {
                actor->place = scene.instances[placed.instance].transform;
                actor->clips.clear();
            }
            continue;
        }
        if (actor == scene.objectActors.end()) {
            if ((!object->animation && object->overlays.empty()) || unskinned_.contains(placed.object)) {
                continue;
            }
            const int mesh = objectMaterials_.skinnedModel(placed.model, placed.overrides, scene);
            auto rig = mesh >= 0 ? objectMaterials_.objectRig(placed.model, placed.savedJoints) : nullptr;
            if (!rig || rig->nodes.empty()) {
                unskinned_.insert(placed.object);
                continue;
            }
            ts2::scene::Actor added;
            added.object = placed.object;
            added.skinned.push_back({placed.instance, ts2::scene::skinNodes(scene.meshes[static_cast<std::size_t>(mesh)], *rig)});
            added.rig = std::move(rig);
            scene.instances[placed.instance].mesh = mesh;
            changedInstances_.push_back(placed.instance);
            objectMaterials_.showAgain(object->id);
            scene.objectActors.push_back(std::move(added));
            actor = scene.objectActors.end() - 1;
        }
        actor->place = standingPlace(placed, scene);
        // The clip its latest replaced plays on under it while the game keeps it: until the latest
        // has crossfaded it, else to its own end.
        const auto& own = object->animation;
        const auto& replaced = object->ending;
        const bool under = own && replaced && (crossfades(*replaced, *own, animations_) || stillPlaying(*replaced, *own));
        playClips(*actor, *object, own, under ? replaced : std::nullopt, 0, animations_);
        // A stopped clip holds the pose it was stopped at.
        if (own && nowMs - own->startMs >= own->lengthMs) {
            const std::string ownName = ts2::scene::lower(own->name);
            for (auto& clip : actor->clips) {
                if (clip.layer == own->layer && clip.animation && clip.animation->animation &&
                    ts2::scene::lower(clip.animation->animation->name) == ownName) {
                    clip.atMs = clipMs(*own, clip.animation->animation->duration, nowMs);
                }
            }
        }
    }
}

// An object a Sim holds that asks for a sun picture draws no shadow indoors, as the game's do. Outdoors
// the game may give it another kind of shadow, not known yet, so none is drawn there either. Put
// down, it has its own again.
void LotLife::shadeHeld(ts2::scene::Scene& scene) {
    using ts2::scene::objectshadow::Kind;
    for (const auto& placed : scene.placedObjects) {
        if (placed.instance >= scene.instances.size()) {
            continue;
        }
        auto& instance = scene.instances[placed.instance];
        const auto was = heldShadows_.find(placed.instance);
        if (!heldNow_.contains(placed.instance)) {
            if (was != heldShadows_.end()) {
                instance.shadow = was->second;
                heldShadows_.erase(was);
            }
        } else if (was == heldShadows_.end() && ts2::scene::objectshadow::requested(instance.shadowFacts) == Kind::Sun) {
            heldShadows_.emplace(placed.instance, instance.shadow);
            instance.shadow = Kind::None;
        }
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
    for (const auto& actor : scene.actors) {
        const auto inLot = actor.lotFrame.rigidInverse() * actor.place;
        for (const auto& skinned : actor.skinned) {
            if (skinned.instance < scene.instances.size()) {
                relight(scene, skinned.instance, inLot, scene.instances[skinned.instance].level - scene.groundLevel);
            }
        }
    }
    std::sort(relit_.begin(), relit_.end());
    relit_.erase(std::unique(relit_.begin(), relit_.end()), relit_.end());
}

// An object takes the sun that reaches it under the buildings' shadow where it stands now, and a
// thing or a Sim the lights of the room it is in (doors and windows keep theirs).
void LotLife::relight(ts2::scene::Scene& scene, std::size_t index, const ts2::scene::Transform& inLot, int storey) {
    // Light sets only ever grow. Past this a thing keeps the light it has.
    constexpr std::size_t kMaxLightSets = 4096;
    if (index >= scene.instances.size()) {
        return;
    }
    auto& instance = scene.instances[index];
    if (scene.thingSun && instance.part == ts2::scene::Part::Object) {
        ts2::scene::Instance placed = instance;
        placed.transform = inLot;
        if (auto sun = scene.thingSun(scene, placed); sun != instance.sun) {
            instance.sun = std::move(sun);
            relit_.push_back(index);
        }
    }
    if (!scene.thingLights || instance.lightGroup == ts2::scene::LightGroup::Portals || instance.mesh < 0 ||
        static_cast<std::size_t>(instance.mesh) >= scene.meshes.size()) {
        return;
    }
    std::vector<int> parts;
    for (auto& set : scene.thingLights(scene.meshes[static_cast<std::size_t>(instance.mesh)], inLot,
                                       ts2::scene::objectlights::lightStorey(scene.floorHeight, inLot, storey), instance.lightGroup)) {
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

// A Sim created during play (a visitor) is drawn in what its character file has it wear,
// standing idle as the lot's Sims start. It is placed with the others on the next update. A newborn
// has no character file yet: it wears what the game picks for its genes, and has no slots.
bool LotLife::addCreatedSim(ts2::scene::Scene& scene, const ts2::sim::SimObject& sim) {
    constexpr std::int16_t kBabyLifeStage = 1;
    constexpr std::size_t kGenderPersonData = 0x41;  // 1 female
    const std::int16_t neighbor = sim_->world().neighborIdOf(sim);
    const auto file = neighbor != 0 ? sim_->characterFile(neighbor) : std::nullopt;
    if (!scene.floorHeight) {
        return false;
    }
    std::optional<ts2::scene::Assets::CharacterInUse> inUse;
    ts2::scene::SimLook look;
    if (file) {
        inUse.emplace(assets_, *file);
        look = ts2::scene::simLook(assets_, *file);
    } else if (const auto* made = neighbor != 0 ? sim_->world().neighbor(neighbor) : nullptr;
               made != nullptr && sim.personData[ts2::sim::kLifeStagePersonData] == kBabyLifeStage) {
        // A newborn has no character file yet: it wears what the game picks for its skin gene.
        const auto skin = made->genes.find(ts2::sim::kSkinGene);
        if (skin == made->genes.end() || skin->second.dominant.empty()) {
            return false;
        }
        ts2::scene::Newborn baby;
        baby.gender = sim.personData[kGenderPersonData] == 1 ? 1 : 2;
        baby.skinTone = skin->second.dominant;
        const auto gene = [&](std::uint32_t number) {
            const auto it = made->genes.find(number);
            return it != made->genes.end() ? it->second.dominant : std::string();
        };
        baby.hairTone = gene(ts2::sim::kHairGene);
        baby.eyeColour = gene(ts2::sim::kEyeGene);
        baby.browDraw = made->browDraw;
        const auto shape = [&](std::int16_t parent) -> std::optional<ts2::faceshape::FaceShape> {
            if (const auto parentFile = parent != 0 ? sim_->characterFile(parent) : std::nullopt) {
                return ts2::scene::faceShapeOf(*parentFile);
            }
            // A Sim with no character file (an NPC such as the alien) keeps its face shape with its object.
            const auto* saved = sim_->world().neighbor(parent);
            const auto* type = saved != nullptr ? sim_->code().objectType(saved->guid) : nullptr;
            const auto bytes = type != nullptr ? sim_->code().characterResource(ts2::faceshape::kType, type->group) : std::nullopt;
            try {
                return bytes ? std::optional(ts2::faceshape::parse(*bytes)) : std::nullopt;
            } catch (const std::exception& ex) {
                ts2::diag::warnOnce(std::string("newborn face: a parent's face shape can't be read: ") + ex.what());
                return std::nullopt;
            }
        };
        // The blend depends only on the parents and the picks, so babies alike share one face mesh
        // and a neighbor id given again later can't bring back an earlier baby's face.
        std::string key = std::format("newborn face {} {}", made->faceParentA, made->faceParentB);
        for (const auto& [region, fromA] : made->facePicks) {
            key += std::format(" {}={}", region, fromA ? 'a' : 'b');
        }
        baby.face = std::make_shared<const ts2::scene::MeshEdit>(ts2::scene::newbornFaceEdit(
            assets_, newbornFaces_, std::move(key), shape(made->faceParentA), shape(made->faceParentB), made->facePicks));
        look = ts2::scene::newbornLook(assets_, babyCatalog_.get(), baby);
    }
    for (const auto& problem : look.problems) {
        ts2::diag::warnOnce(std::format("Sim {}: {}", sim.id, problem));
    }
    if (look.parts.empty()) {
        return false;
    }
    ts2::scene::Actor actor = objectMaterials_.sim(look, std::to_string(sim.id), lotFrame_, scene.groundLevel + sim.level - 1,
                                                   ts2::scene::standingIdle(assets_, look.age), scene, freeSimInstances_);
    actor.object = static_cast<std::uint32_t>(sim.id);
    actor.basePose = ts2::scene::standingPose(assets_, look.age);
    actor.lotFrame = lotFrame_;
    actor.floorHeight = scene.floorHeight;
    for (const auto& bone : file ? ts2::scene::simSlotBones(assets_, *file) : std::vector<std::string>{}) {
        actor.slotNodes.push_back(actor.rig && !bone.empty() ? actor.rig->find(bone) : -1);
    }
    for (const auto& bone : file ? ts2::scene::simSlotBones(assets_, *file, true) : std::vector<std::string>{}) {
        actor.targetNodes.push_back(actor.rig && !bone.empty() ? actor.rig->find(bone) : -1);
    }
    auto& instances = createdSims_[sim.id];
    for (const auto& s : actor.skinned) {
        instances.push_back(s.instance);
        changedInstances_.push_back(s.instance);
    }
    scene.actors.push_back(std::move(actor));
    return true;
}

// Objects scripts create after the lot was loaded are drawn as the lot draws its objects: their model
// (see modelName) placed by placeObjects, which finds them moved from nowhere. One without a model gets
// an instance drawing nothing, so that RayTrace can still start or end on it: the game's point on an
// object is the middle of its model's bounds, which always hold its origin, so on a model with nothing
// in it the point is where the object stands. A removed one's instance draws nothing again and goes
// to the next object created. New objects take the lowest free id, so they are looked for among all
// ids, and an id now held by an object of another type than the one drawn there is drawn anew.
// Removed Sims are taken out only with `removeSims`: the frame's pose finds Sims by their place in the
// actor list and their instances by number, so a ray query between frames leaves both as they are.
void LotLife::addCreatedObjects(ts2::scene::Scene& scene, bool removeSims) {
    // A lot that keeps creating objects can't grow the scene past this.
    constexpr std::size_t kMaxCreated = 4096;
    constexpr std::size_t kMaxSlots = 16;  // as the lot scene reads them
    auto& world = sim_->world();
    // What is drawn for `id` is taken out: a created object's instance goes to the next one created,
    // a lot object's draws nothing from now on.
    const auto undraw = [&](std::int16_t id) {
        const auto created = created_.find(id);
        std::vector<std::size_t> indices;
        for (const auto& p : scene.placedObjects) {
            if (p.object == static_cast<std::uint32_t>(id)) {
                indices.push_back(p.instance);
            }
        }
        for (const std::size_t index : indices) {
            if (created != created_.end()) {
                scene.instances[index] = ts2::scene::Instance{};
                freeInstances_.push_back(index);
            } else {
                scene.instances[index].transform.m.fill(0.0f);
            }
            for (auto& actor : scene.actors) {
                std::erase_if(actor.held, [&](const ts2::scene::Actor::Held& h) { return h.instance == index; });
            }
            std::erase_if(moved_, [&](const auto& m) { return m.first == index; });
            heldShadows_.erase(index);
            unshaded_.erase(index);
            changedInstances_.push_back(index);
        }
        std::erase_if(scene.objectActors, [&](const ts2::scene::Actor& a) { return a.object == static_cast<std::uint32_t>(id); });
        std::erase_if(scene.placedObjects, [&](const ts2::scene::PlacedObject& p) { return p.object == static_cast<std::uint32_t>(id); });
        objectPlaces_.erase(id);
        drawnTypes_.erase(id);
        // What was known of the old object's model goes with it, so the next one at this id starts afresh.
        unskinned_.erase(static_cast<std::uint32_t>(id));
        objectMaterials_.showAgain(id);
        if (created != created_.end()) {
            created_.erase(created);
        }
    };
    std::vector<std::int16_t> gone;
    for (const auto& [id, index] : created_) {
        const auto* object = world.find(id);
        if (object == nullptr || object->removed) {
            gone.push_back(id);
        }
    }
    for (const std::int16_t id : gone) {
        undraw(id);
    }
    for (auto it = createdSims_.begin(); it != createdSims_.end();) {
        const auto* sim = world.find(it->first);
        if (!removeSims || (sim != nullptr && !sim->removed)) {
            ++it;
            continue;
        }
        // Its instances draw nothing from now on and go to the next Sim drawn.
        for (const std::size_t index : it->second) {
            scene.instances[index] = ts2::scene::Instance{};
            changedInstances_.push_back(index);
            freeSimInstances_.push_back(index);
        }
        std::erase_if(scene.actors, [&](const ts2::scene::Actor& a) { return a.object == static_cast<std::uint32_t>(it->first); });
        triedSims_.erase(it->first);
        it = createdSims_.erase(it);
    }
    // A Sim the lot was loaded with goes the same way once removed, so a new Sim given its id is drawn
    // as itself.
    std::erase_if(scene.actors, [&](const ts2::scene::Actor& a) {
        if (!removeSims || a.object == 0 || a.object > static_cast<std::uint32_t>(INT16_MAX)) {
            return false;
        }
        const auto* sim = world.findIncludingRemoved(static_cast<std::int16_t>(a.object));
        if (sim != nullptr && (!sim->removed || sim->keptUntilUnload)) {
            return false;
        }
        for (const auto& s : a.skinned) {
            if (s.instance < scene.instances.size()) {
                scene.instances[s.instance] = ts2::scene::Instance{};
                changedInstances_.push_back(s.instance);
                freeSimInstances_.push_back(s.instance);
            }
        }
        triedSims_.erase(static_cast<std::int16_t>(a.object));
        return true;
    });
    // New objects take the lowest free id, so Sims are looked for among all of them. Each is tried once.
    world.nextAfter(0, [&](const ts2::sim::SimObject& o) {
        if (!o.isPerson() || o.removed || !triedSims_.insert(o.id).second ||
            std::any_of(scene.actors.begin(), scene.actors.end(), [&](const ts2::scene::Actor& a) { return a.object == static_cast<std::uint32_t>(o.id); })) {
            return false;
        }
        try {
            if (!addCreatedSim(scene, o)) {
                ts2::diag::warning(std::format("Sim {} is not drawn: it has no look yet", o.id));
            }
        } catch (const std::exception& ex) {
            ts2::diag::warning(std::format("Sim {} is not drawn: its look can't be read: {}", o.id, ex.what()));
        }
        return false;
    });
    world.nextAfter(0, [&](const ts2::sim::SimObject& o) {
        if (o.isPerson() || o.removed) {
            return false;
        }
        if (const auto drawn = drawnTypes_.find(o.id); drawn != drawnTypes_.end()) {
            if (drawn->second == o.definition.guid()) {
                return false;
            }
            undraw(o.id);
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
        // Its shadow is what it asks for until it is first placed (see shadeCreated).
        instance.shadowFacts = shadowFacts(o, model, instance.mesh, scene);
        instance.shadow = ts2::scene::objectshadow::requested(instance.shadowFacts);
        std::size_t index = scene.instances.size();
        if (!freeInstances_.empty()) {
            index = freeInstances_.back();
            freeInstances_.pop_back();
            scene.instances[index] = std::move(instance);
        } else {
            scene.instances.push_back(std::move(instance));
        }
        ts2::scene::PlacedObject placed{static_cast<std::uint32_t>(o.id), index, lotFrame_, {}};
        placed.model = model;
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
        unshaded_.insert(index);
        drawnTypes_[o.id] = o.definition.guid();
        // Never placed yet, so placeObjects places it.
        objectPlaces_[o.id] = ObjectPlace{.gone = true};
        changedInstances_.push_back(index);
        return false;
    });
}

} // namespace app
