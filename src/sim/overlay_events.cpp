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
        SimObject* who = world.find(o.caller != 0 ? o.caller : id);
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
        if (object == nullptr || !world.animationEvents()) {
            return;
        }
        // The clip that ended its wait plays on as it fades out, and its last events still come.
        PlayingAnimation* clip = nullptr;
        std::vector<AnimationEvent> events;
        double at = 0;
        for (auto* candidate : {object->ending ? &*object->ending : nullptr, object->animation ? &*object->animation : nullptr}) {
            if (candidate == nullptr || candidate->eventTree == 0) {
                continue;
            }
            auto list = world.animationEvents()(candidate->name);
            if (candidate->eventsTold >= list.size()) {
                continue;
            }
            const double speed = std::abs(candidate->speed) > 0 ? std::abs(candidate->speed) : 1.0;
            const double when = candidate->startMs + list[candidate->eventsTold].timeMs / speed;
            if (when <= now) {
                clip = candidate;
                events = std::move(list);
                at = when;
                break;
            }
        }
        if (clip == nullptr) {
            return;
        }
        PlayingAnimation& a = *clip;
        const AnimationEvent& e = events[a.eventsTold];
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

void tellLookStates(World& world, Vm& vm, std::int16_t id, std::span<const LookController::Change> changes,
                    const std::function<void(const RunResult&)>& onStop) {
    for (const auto& change : changes) {
        SimObject* sim = world.find(id);
        if (sim == nullptr) {
            return;
        }
        const auto t = std::find_if(sim->lookTargets.begin(), sim->lookTargets.end(),
                                    [&](const LookTarget& l) { return l.request == change.request; });
        if (t == sim->lookTargets.end() || t->eventTree == 0) {
            continue;
        }
        const LookTarget target = *t;
        const auto flag = static_cast<std::int16_t>(change.state == kLookEndedEarly || change.state == kLookCancelled ? 1 : 0);
        for (const int event : {change.state, change.state >= kLookDone ? kEndEvent : 0}) {
            SimObject* who = world.find(id);
            if (event == 0 || who == nullptr) {
                break;
            }
            const std::array<std::int16_t, 5> args{static_cast<std::int16_t>(event), target.stackObject,
                                                   static_cast<std::int16_t>(change.percent), static_cast<std::int16_t>(std::clamp(change.holdMs, -1, 32767)), flag};
            const RunResult r = vm.runEvent(*who, target.codeOwner, target.eventTree, target.stackObject, args);
            if (r.outcome != Outcome::True && r.outcome != Outcome::False) {
                onStop(r);
            }
        }
    }
    if (SimObject* sim = world.find(id)) {
        // A look started by an event tree is followed next, but the look the head is on still
        // needs its target until it ends.
        const LookRequest* current = sim->look.current();
        const int following = sim->look.following();
        const int keep = current ? std::min(current->id, following) : following;
        std::erase_if(sim->lookTargets, [&](const LookTarget& l) { return l.request < keep; });
    }
}

void noteLook(SimObject& sim) {
    const LookRequest* r = sim.look.current();
    const auto t = r ? std::find_if(sim.lookTargets.begin(), sim.lookTargets.end(), [&](const LookTarget& l) { return l.request == r->id; })
                     : sim.lookTargets.end();
    if (t == sim.lookTargets.end()) {
        sim.personData[kLookTargetPersonData] = 0;
        sim.personData[kLookPointPersonData] = 0;
        sim.personData[kLookStatePersonData] = static_cast<std::int16_t>(kLookNone);
        sim.personData[kLookHoldPersonData] = 0;
        return;
    }
    sim.personData[kLookTargetPersonData] = t->object;
    sim.personData[kLookPointPersonData] = static_cast<std::int16_t>(t->slot | t->pointKind << 12);
    sim.personData[kLookStatePersonData] = static_cast<std::int16_t>(r->state);
    sim.personData[kLookHoldPersonData] = r->untilStopped ? std::int16_t{-1} : static_cast<std::int16_t>(std::clamp(r->remainingMs, 0.0f, 32767.0f));
}

} // namespace ts2::sim
