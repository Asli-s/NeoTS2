#include "sim/overlay_events.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace ts2::sim {

namespace {
// Clips mark a handful of events. More than this in one clip is a broken file.
constexpr std::size_t kMaxEventsPerClip = 64;
} // namespace

void tellOverlayEvents(World& world, Vm& vm, std::int16_t id, const std::function<void(const RunResult&)>& onStop, double now) {
    SimObject* object = world.find(id);
    if (object == nullptr) {
        return;
    }
    // The tree runs at the moment of the event, which a tick only notices after it.
    auto tell = [&](const PlayingOverlay& o, std::int16_t event, std::int16_t percent, double atMs) {
        if (o.eventTree == 0) {
            return;
        }
        SimObject* who = world.find(id);
        if (who == nullptr) {
            return;
        }
        const std::array<std::int16_t, 5> args{event, o.stackObject, percent, 1, 0};
        world.setEventClock(std::min(atMs, now));
        const RunResult r = vm.runEvent(*who, o.codeOwner, o.eventTree, o.stackObject, args);
        world.setEventClock(std::nullopt);
        if (r.outcome != Outcome::True && r.outcome != Outcome::False) {
            onStop(r);
        }
    };
    // Events can start or drop overlays, so each overlay playing now is looked up again by its start
    // and name before anything is done with it.
    std::vector<std::pair<double, std::string>> playing;
    for (const auto& o : object->overlays) {
        playing.emplace_back(o.startMs, o.name);
    }
    auto find = [&](const std::pair<double, std::string>& key) -> PlayingOverlay* {
        object = world.find(id);
        if (object == nullptr) {
            return nullptr;
        }
        const auto it = std::find_if(object->overlays.begin(), object->overlays.end(),
                                     [&](const PlayingOverlay& p) { return p.startMs == key.first && p.name == key.second; });
        return it != object->overlays.end() ? &*it : nullptr;
    };
    for (const auto& key : playing) {
        PlayingOverlay* found = find(key);
        if (found == nullptr) {
            continue;
        }
        const double played = now - found->startMs;
        if (found->stopped) {
            if (played >= found->lengthMs) {
                object->overlays.erase(object->overlays.begin() + (found - object->overlays.data()));
            }
            continue;
        }
        if (found->plays < 0) {
            continue;
        }
        const double length = found->lengthMs;
        const auto percent = static_cast<std::int16_t>(length > 0 ? std::clamp(100.0 * played / length, 0.0, 100.0) : 100.0);
        if (!found->blendingOut && played >= length - kOverlayBlendMs) {
            found->blendingOut = true;
            tell(PlayingOverlay(*found), kBlendingOutEvent, percent, found->startMs + length - kOverlayBlendMs);
            found = find(key);
            if (found == nullptr) {
                continue;
            }
        }
        if (played >= length) {
            const PlayingOverlay ended = *found;
            object->overlays.erase(object->overlays.begin() + (found - object->overlays.data()));
            tell(ended, kBlendedOutEvent, 100, ended.startMs + length);
            tell(ended, kEndEvent, 100, ended.startMs + length);
        }
    }
}

void tellAnimateSimEvents(World& world, Vm& vm, std::int16_t id, const std::function<void(const RunResult&)>& onStop, double now) {
    // An event tree can start another animation or remove the object, so each event looks again.
    for (std::size_t told = 0; told < kMaxEventsPerClip; ++told) {
        SimObject* object = world.find(id);
        if (object == nullptr || !object->animation || object->animation->eventTree == 0 || !world.animationEvents()) {
            return;
        }
        PlayingAnimation& a = *object->animation;
        const std::vector<AnimationEvent> events = world.animationEvents()(a.name);
        if (a.eventsTold >= events.size()) {
            return;
        }
        const AnimationEvent& e = events[a.eventsTold];
        const double speed = std::abs(a.speed) > 0 ? std::abs(a.speed) : 1.0;
        const double at = a.startMs + e.timeMs / speed;
        if (at > now) {
            return;
        }
        ++a.eventsTold;
        const double played = now - a.startMs;
        const auto percent = static_cast<std::int16_t>(a.lengthMs > 0 ? std::clamp(100.0 * played / a.lengthMs, 0.0, 100.0) : 100.0);
        const std::array<std::int16_t, 5> args{static_cast<std::int16_t>(e.id), a.eventStackObject, percent, 1, 0};
        const std::uint32_t owner = a.eventCodeOwner;
        const std::uint16_t tree = a.eventTree;
        const std::int16_t stackObject = a.eventStackObject;
        world.setEventClock(std::min(at, now));
        const RunResult r = vm.runEvent(*object, owner, tree, stackObject, args, true);
        world.setEventClock(std::nullopt);
        if (r.outcome != Outcome::True && r.outcome != Outcome::False) {
            onStop(r);
        }
    }
}

} // namespace ts2::sim
