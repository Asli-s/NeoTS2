// The visual effects clips start, and where an effect on an object or a Sim sits.
#include "lot_life.hpp"

#include "formats/hash.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <string_view>

namespace app {

namespace {

// A mirrored clip's effect goes on the other side: a leading l_ and r_ swap, in either case.
std::string otherSide(std::string name) {
    if (name.size() >= 2 && name[1] == '_') {
        if (name[0] == 'l' || name[0] == 'L') {
            name.replace(0, 2, "r_");
        } else if (name[0] == 'r' || name[0] == 'R') {
            name.replace(0, 2, "l_");
        }
    }
    return name;
}

} // namespace

// Each object's clip and overlays, from the time each was last seen. Clips already playing as the lot opens have told
// theirs. Only a clip's first play tells, as whether its events come again when it repeats is not known yet.
void LotLife::clipEffects(const ts2::scene::Scene& scene, const std::function<void(const ts2::sim::EffectCommand&)>& send,
                          const std::function<void(std::int16_t object, const std::string& sound)>& sound) {
    const double now = sim_->world().eventClockMs();
    std::map<std::tuple<std::int16_t, double, std::string>, double> seen;
    const auto play = [&](std::int16_t id, const std::string& name, double startMs, double speed, bool flipped) {
        auto key = std::make_tuple(id, startMs, name);
        const auto told = clipEffectsTold_.find(key);
        const bool isNew = told == clipEffectsTold_.end();
        // A new clip's first moment counts too.
        const double from = !isNew ? told->second : clipEffectsStarted_ ? startMs - 1 : now;
        seen.emplace(std::move(key), now);
        auto effects = clipEffects_.find(name);
        if (effects == clipEffects_.end()) {
            std::vector<ClipEffect> list;
            std::vector<ClipSound> sounds;
            if (const auto clip = animations_.find(name)) {
                for (const auto& event : clip->events) {
                    if (auto effect = ts2::anim::effectEvent(event)) {
                        list.push_back({static_cast<float>(event.timeMs), std::move(*effect)});
                    } else if (auto named = ts2::anim::soundEventName(event)) {
                        sounds.push_back({static_cast<float>(event.timeMs), std::move(*named)});
                    }
                }
                std::stable_sort(list.begin(), list.end(), [](const ClipEffect& a, const ClipEffect& b) { return a.ms < b.ms; });
            }
            effects = clipEffects_.emplace(name, std::move(list)).first;
            clipSounds_.insert_or_assign(name, std::move(sounds));
        }
        if (sound) {
            if (const auto sounds = clipSounds_.find(name); sounds != clipSounds_.end()) {
                for (const ClipSound& s : sounds->second) {
                    const double at = startMs + s.ms / speed;
                    if (at > from && at <= now) {
                        sound(id, s.name);
                    }
                }
            }
        }
        for (const ClipEffect& e : effects->second) {
            const double at = startMs + e.ms / speed;
            if (at <= from || at > now) {
                continue;
            }
            ts2::sim::EffectCommand c;
            using Do = ts2::sim::EffectCommand::Do;
            const std::string& op = e.event.op;
            const auto is = [&](std::string_view word) {
                return op.size() == word.size() && std::equal(op.begin(), op.end(), word.begin(), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                });
            };
            if (is("softStart") || is("hardStart")) {
                c.what = Do::Start;
                c.hard = is("hardStart");
            } else if (is("softStop") || is("hardStop")) {
                c.what = Do::Stop;
                c.hard = is("hardStop");
            } else {
                continue;
            }
            c.object = id;
            c.name = flipped ? otherSide(e.event.effect) : e.event.effect;
            c.bone = flipped ? otherSide(e.event.bone) : e.event.bone;
            c.kind = 0;
            send(c);
        }
    };
    const auto clipsOf = [&](std::int16_t id) {
        const auto* object = sim_->world().find(id);
        if (object == nullptr) {
            return;
        }
        if (object->animation) {
            play(id, object->animation->name, object->animation->startMs, std::max(0.001f, std::abs(object->animation->speed)), object->animation->flipped);
        }
        for (const auto& overlay : object->overlays) {
            play(id, overlay.name, overlay.startMs, std::max(0.001f, std::abs(overlay.speed)), overlay.flipped);
        }
    };
    for (const auto& actor : scene.actors) {
        clipsOf(static_cast<std::int16_t>(actor.object));
    }
    for (const auto& placed : scene.placedObjects) {
        if (placed.object <= static_cast<std::uint32_t>(INT16_MAX)) {
            clipsOf(static_cast<std::int16_t>(placed.object));
        }
    }
    clipEffectsTold_ = std::move(seen);
    clipEffectsStarted_ = true;
}

ts2::scene::ObjectEffects::Where LotLife::effectPlace(const ts2::scene::Scene& scene, const ts2::scene::ScenePose& pose, std::int16_t id, const std::string& bone, int highestLevelShown) {
    using Where = ts2::scene::ObjectEffects::Where;
    constexpr std::size_t kRoomField = 0x1D;  // object data 0x1D, its room
    Where where;
    const auto* object = sim_->world().find(id);
    if (object == nullptr || object->removed) {
        where.state = Where::State::Gone;
        return where;
    }
    // An object in no room is out of the world, like one in an inventory. Effects also hide on the
    // storeys above the view, and those below it keep playing.
    where.hidden = object->data[kRoomField] == -1 || object->level > highestLevelShown;
    const std::string joint = ts2::scene::lower(bone);
    const bool onNode = joint.empty() || joint == "locator";
    const auto drawn = pose.drawnAt.find(static_cast<std::uint32_t>(id));
    std::optional<ts2::scene::Transform> found;
    if (object->isPerson()) {
        const auto actor = std::find_if(scene.actors.begin(), scene.actors.end(), [&](const ts2::scene::Actor& a) { return a.object == static_cast<std::uint32_t>(id) && a.rig; });
        if (actor == scene.actors.end()) {
            return where;
        }
        const ts2::scene::Transform place = drawn != pose.drawnAt.end() ? drawn->second : actor->place;
        if (onNode) {
            found = place;
        } else {
            const int node = actor->rig->find(joint);
            const auto index = static_cast<std::size_t>(actor - scene.actors.begin());
            const auto nodes = std::find_if(pose.nodes.begin(), pose.nodes.end(), [&](const auto& n) { return n.first == index; });
            if (nodes == pose.nodes.end()) {
                return where;
            }
            if (node >= 0 && static_cast<std::size_t>(node) < nodes->second.size()) {
                found = place * nodes->second[static_cast<std::size_t>(node)];
            }
        }
    } else {
        const auto placed = std::find_if(scene.placedObjects.begin(), scene.placedObjects.end(), [&](const ts2::scene::PlacedObject& p) { return p.object == static_cast<std::uint32_t>(id); });
        if (placed == scene.placedObjects.end() || placed->instance >= scene.instances.size()) {
            return where;
        }
        const ts2::scene::Transform place = drawn != pose.drawnAt.end() ? drawn->second : scene.instances[placed->instance].transform;
        if (onNode) {
            found = place;
        } else if (const auto moved = std::find_if(placed->bones.begin(), placed->bones.end(), [&](const auto& b) { return b.first == ts2::hash::crc32(joint); });
                   moved != placed->bones.end()) {
            found = moved->second;
        } else if (const auto rig = objectMaterials_.objectRig(placed->model, placed->savedJoints)) {
            // The bone at rest, through its parents.
            const int node = rig->find(joint);
            if (node >= 0) {
                ts2::scene::Transform rest;
                for (int at = node; at >= 0; at = rig->nodes[static_cast<std::size_t>(at)].parent) {
                    const auto& n = rig->nodes[static_cast<std::size_t>(at)];
                    rest = ts2::scene::Transform::of(n.translation, n.rotation) * rest;
                }
                found = place * rest;
            }
        }
    }
    if (!found) {
        where.state = Where::State::NoBone;
        return where;
    }
    where.state = Where::State::Here;
    // Effects take a node's place and turn, and never its size.
    const auto& m = found->m;
    for (std::size_t column = 0; column < 3; ++column) {
        const float length = std::sqrt(m[column] * m[column] + m[4 + column] * m[4 + column] + m[8 + column] * m[8 + column]);
        const float k = length > 0 ? 1 / length : 0;
        where.place.rotation[column] = m[column] * k;
        where.place.rotation[3 + column] = m[4 + column] * k;
        where.place.rotation[6 + column] = m[8 + column] * k;
    }
    where.place.position = {m[3], m[7], m[11]};
    return where;
}

} // namespace app
