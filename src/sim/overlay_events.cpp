#include "sim/overlay_events.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ts2::sim {

namespace {
// Clips mark a handful of events. More than this in one clip is a broken file.
constexpr std::size_t kMaxEventsPerClip = 64;
const std::vector<AnimationEvent> kNoEvents;

// Counts as told the marks a clip restored from a save had played past before it was loaded.
void skipRestoredEvents(std::optional<double>& restoredAtMs, std::size_t& told, const std::vector<AnimationEvent>& marks,
                        double startMs, double speed) {
    if (!restoredAtMs) {
        return;
    }
    while (told < marks.size() && startMs + marks[told].timeMs / speed <= *restoredAtMs) {
        ++told;
    }
    restoredAtMs.reset();
}
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
    // Events can start or drop overlays, so each overlay playing now is numbered and looked up again by
    // its number before anything is done with it. Overlays started by an event are numbered 0.
    // Kept between calls so it reuses its storage. Event trees don't tell events themselves.
    thread_local std::vector<std::uint32_t> playing;
    playing.clear();
    for (auto& o : object->overlays) {
        o.serial = static_cast<std::uint32_t>(playing.size() + 1);
        playing.push_back(o.serial);
    }
    auto find = [&](std::uint32_t key) -> PlayingOverlay* {
        object = world.find(id);
        if (object == nullptr) {
            return nullptr;
        }
        const auto it = std::find_if(object->overlays.begin(), object->overlays.end(), [&](const PlayingOverlay& p) { return p.serial == key; });
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
        const double blendOut = found->blendOutMs.value_or(kOverlayBlendMs);
        // The clip's own SimAntics events and the blending-out event, in the order they come, the clip's
        // own first where two come at once. An overlay played more than once tells only its fades, as
        // whether its clip's events come again each play is not known yet.
        const auto& marks = found->plays == 1 && world.animationEvents() ? world.animationEvents()(found->name) : kNoEvents;
        const double speed = std::abs(found->speed) > 0 ? std::abs(found->speed) : 1.0;
        const double start = found->startMs;
        skipRestoredEvents(found->restoredAtMs, found->eventsTold, marks, start, speed);
        for (std::size_t told = 0; found != nullptr && told < kMaxEventsPerClip; ++told) {
            constexpr double kNever = std::numeric_limits<double>::infinity();
            const double markAt = found->eventsTold < marks.size() ? start + marks[found->eventsTold].timeMs / speed : kNever;
            const double blendOutAt = found->blendingOut ? kNever : start + length - blendOut;
            if (std::min(markAt, blendOutAt) > now) {
                break;
            }
            if (markAt <= blendOutAt) {
                const auto& mark = marks[found->eventsTold];
                const auto event = static_cast<std::int16_t>(mark.id);
                ++found->eventsTold;
                if (!mark.sound.empty()) {
                    if (const auto& play = world.animationSound()) {
                        play(id, mark.sound);
                    }
                } else {
                    tell(PlayingOverlay(*found), event, percent, markAt);
                }
            } else {
                found->blendingOut = true;
                tell(PlayingOverlay(*found), kBlendingOutEvent, percent, blendOutAt);
            }
            found = find(key);
        }
        if (found == nullptr) {
            continue;
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
        std::int16_t event = 0;
        std::string sound;
        double at = 0;
        for (auto* candidate : {object->ending ? &*object->ending : nullptr, object->animation ? &*object->animation : nullptr}) {
            if (candidate == nullptr) {
                continue;
            }
            // Its next event, the clip's own marks first where two come at once.
            const auto& list = world.animationEvents()(candidate->name);
            const double speed = std::abs(candidate->speed) > 0 ? std::abs(candidate->speed) : 1.0;
            const double end = candidate->startMs + candidate->lengthMs;
            skipRestoredEvents(candidate->restoredAtMs, candidate->eventsTold, list, candidate->startMs, speed);
            std::int16_t next = 0;
            std::string nextSound;
            double when = std::numeric_limits<double>::infinity();
            if (candidate->eventsTold < list.size()) {
                next = static_cast<std::int16_t>(list[candidate->eventsTold].id);
                nextSound = list[candidate->eventsTold].sound;
                when = candidate->startMs + list[candidate->eventsTold].timeMs / speed;
            }
            if (candidate->tellsEnd && candidate->endEventsTold == 0 && end - candidate->fadeOutMs < when) {
                next = kBlendingOutEvent;
                nextSound.clear();
                when = end - candidate->fadeOutMs;
            } else if (candidate->tellsEnd && candidate->endEventsTold == 1 && end < when) {
                next = kBlendedOutEvent;
                nextSound.clear();
                when = end;
            }
            if (when <= now) {
                clip = candidate;
                event = next;
                sound = std::move(nextSound);
                at = when;
                break;
            }
        }
        if (clip == nullptr) {
            return;
        }
        PlayingAnimation& a = *clip;
        if (event == kBlendingOutEvent) {
            a.endEventsTold = 1;
        } else if (event == kBlendedOutEvent) {
            a.endEventsTold = 2;
        } else {
            ++a.eventsTold;
        }
        if (!sound.empty()) {
            if (const auto& play = world.animationSound()) {
                play(id, sound);
            }
            continue;
        }
        const double played = now - a.startMs;
        const auto percent = static_cast<std::int16_t>(a.lengthMs > 0 ? std::clamp(100.0 * played / a.lengthMs, 0.0, 100.0) : 100.0);
        const std::uint32_t owner = a.eventCodeOwner;
        const std::uint16_t tree = a.eventTree;
        const std::int16_t stackObject = a.eventStackObject;
        if (tree == 0) {
            continue;
        }
        // The end comes as two events, one after the other.
        for (const std::int16_t sent : {event, event == kBlendedOutEvent ? kEndEvent : std::int16_t{0}}) {
            object = world.find(id);
            if (sent == 0 || object == nullptr) {
                break;
            }
            const std::array<std::int16_t, 5> args{sent, stackObject, percent, 1, 0};
            world.setEventClock(std::min(at, now));
            const RunResult r = vm.runEvent(*object, owner, tree, stackObject, args, true);
            world.setEventClock(std::nullopt);
            if (r.outcome != Outcome::True && r.outcome != Outcome::False) {
                onStop(r);
            }
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
