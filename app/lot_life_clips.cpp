#include "lot_life_clips.hpp"

#include "sim/turn.hpp"

#include <algorithm>
#include <cmath>

namespace app {

namespace {

// The layers overlays play on by priority, as the game ranks them: an object's own animation (layer 0)
// below every overlay but the underlay (priority 4).
constexpr int kOverlayLayers[] = {1, 3, 5, 7, 0};
// A Sim starts in its standing pose. After that, a joint no clip moves keeps where it last was.
constexpr int kBasePoseLayer = -1;

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

// Whether `previous` is an Animate Sim clip still fading out as `latest` starts.
bool stillFading(const ts2::sim::PlayingAnimation& previous, const ts2::sim::PlayingAnimation& latest) {
    return previous.fades && previous.fadeOutMs > 0 && previous.startMs + previous.lengthMs > latest.startMs;
}

// An Animate Sim clip's weight `ms` on the world's clock, by its fades.
float animateSimWeight(const ts2::sim::PlayingAnimation& a, double ms) {
    const double t = ms - a.startMs;
    const double in = a.fadeInMs > 0 ? std::clamp(t / a.fadeInMs, 0.0, 1.0) : 1.0;
    const double out = a.fadeOutMs > 0 ? std::clamp((a.lengthMs - t) / a.fadeOutMs, 0.0, 1.0) : 1.0;
    return static_cast<float>(std::min(in, out));
}

} // namespace

bool stillPlaying(const ts2::sim::PlayingAnimation& previous, const ts2::sim::PlayingAnimation& latest) {
    return previous.fades && previous.startMs + previous.lengthMs > latest.startMs;
}

bool crossfades(const ts2::sim::PlayingAnimation& previous, const ts2::sim::PlayingAnimation& latest, ts2::scene::AnimationLibrary& animations) {
    constexpr std::uint16_t kBodyTarget = 1;
    return stillFading(previous, latest) && latest.fades && previous.blendsOut && latest.blendsIn &&
           previous.startMs + previous.lengthMs - latest.startMs <= ts2::sim::kAnimateFadeMs &&
           animations.hasTargetType(previous.name, kBodyTarget) && animations.hasTargetType(latest.name, kBodyTarget);
}

double previousGoneMs(const ts2::sim::PlayingAnimation& previous, const ts2::sim::PlayingAnimation& latest,
                      ts2::scene::AnimationLibrary& animations) {
    if (crossfades(previous, latest, animations)) {
        return latest.startMs + ts2::sim::kAnimateFadeMs;
    }
    if (stillPlaying(previous, latest)) {
        return previous.startMs + previous.lengthMs;
    }
    return latest.startMs;
}

void playClips(ts2::scene::Actor& actor, const ts2::sim::SimObject& object, const std::optional<ts2::sim::PlayingAnimation>& own,
               const std::optional<ts2::sim::PlayingAnimation>& previous, float frameHeading, ts2::scene::AnimationLibrary& animations,
               const std::vector<ts2::sim::WalkPose>* walk, float walkFade, float walkStartHeading,
               const std::vector<ts2::sim::WalkPose>* endedWalk, float endedHeading, const StartedAt& startedAt,
               const std::optional<ts2::scene::Transform>& endedPlace) {
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
        const auto clip = animations.find(a.name);
        const float heading = ik.heading + (clip ? clip->rootTurn : 0.0f);
        return actor.lotFrame * ts2::scene::Transform::at(ik.y, ik.x, actor.floorHeight(ik.level, ik.y, ik.x) + ik.height, heading);
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
        // A clip before the walk with fades of its own plays them out under it and is gone at its end.
        // The walk fades nothing else: the game's walk cuts no clip but a running route's.
        if (previous && previous->fades) {
            if (auto* clip = add(previous->name, previous->startMs, previous->lengthMs, previous->speed, previous->flipped, previous->baseHeading)) {
                clip->layer = previous->layer;
                fadeAsAnimateSim(*clip, *previous);
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
    // An Animate Sim clip still fading out as the latest starts plays out its fades under it, or the
    // latest's crossfade (see crossfades), its root blended with the latest's. Any other ends, its joints
    // held until the latest moves them. A turn plays on facing its base heading, any other where it
    // started, so a stair step's clip stays a step down as the Sim snaps on, and getting into bed keeps the elbows still.
    const bool crossfade = previous && own && crossfades(*previous, *own, animations);
    if (!walk && previous && own && (crossfade || stillPlaying(*previous, *own))) {
        if (auto* clip = add(previous->name, previous->startMs, previous->lengthMs, previous->speed, previous->flipped, previous->baseHeading)) {
            clip->layer = previous->layer;
            clip->objectPlace = objectPlace(*previous);
            clip->ikActor = static_cast<std::uint32_t>(std::max<std::int16_t>(previous->ikTarget, 0));
            if (crossfade) {
                clip->weight = animateSimWeight(*previous, own->startMs);
                clip->end = (own->startMs + ts2::sim::kAnimateFadeMs) / 1000.0;
                clip->fadeIn = false;
                clip->fadeOut = true;
                clip->fadeOutSeconds = ts2::sim::kAnimateFadeMs / 1000.0f;
            } else {
                // A clip with fades of its own plays them out under the latest.
                fadeAsAnimateSim(*clip, *previous);
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
    // A clip without fades of its own (the pose a walk leaves) shows at full once it starts. It waits
    // while a walk that has handed back plays out, as the game keeps no such clip and the walk's
    // clips play out alone.
    if (!walk && own && (own->fades || endedWalk == nullptr)) {
        if (auto* clip = add(own->name, own->startMs, own->lengthMs, own->speed, own->flipped, own->baseHeading)) {
            clip->layer = own->layer;
            clip->objectPlace = objectPlace(*own);
            clip->ikActor = static_cast<std::uint32_t>(std::max<std::int16_t>(own->ikTarget, 0));
            if (own->fades) {
                fadeAsAnimateSim(*clip, *own);
            } else {
                clip->fadeIn = clip->fadeOut = false;
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

void addSettleClips(ts2::scene::Actor& actor, const ts2::sim::SimObject& object, double settledMs, const std::optional<ts2::scene::Transform>& at,
                    ts2::scene::AnimationLibrary& animations, const ts2::sim::LocoClipLookup& clips) {
    const double start = settledMs / 1000.0;
    auto& still = actor.clips.emplace_back();
    still.frozen = true;
    still.start = start;
    still.end = start + ts2::sim::kSettlePoseMs / 1000.0;
    still.fadeIn = false;
    still.fadeOutSeconds = ts2::sim::kSettlePoseMs / 1000.0f;
    still.playedAt = at;
    const std::string name = ts2::sim::settlePose(object.walkSets.empty() ? nullptr : &object.walkSets.back()[0],
                                                  object.personData[ts2::sim::kLifeStagePersonData], object.personData[ts2::sim::kGenderPersonData], clips);
    if (auto bound = animations.bound(actor.rig, name, false)) {
        auto& pose = actor.clips.emplace_back();
        pose.animation = std::move(bound);
        pose.start = start;
        pose.end = start + ts2::sim::kSettleWalkPoseMs / 1000.0;
        pose.fadeInSeconds = pose.fadeOutSeconds = ts2::sim::settleWalkPoseFadeMs() / 1000.0f;
    }
}

} // namespace app
