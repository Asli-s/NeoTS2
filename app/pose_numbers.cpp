#include "pose_numbers.hpp"

#include "lot_life.hpp"
#include "scene/rig.hpp"
#include "scene/scene.hpp"
#include "sim/turn.hpp"

#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <string>

namespace app {
namespace {

using ts2::scene::Transform;

// A transform's translation, to the millimetre. Minus zero prints as zero, so it never differs
// from a plus zero.
std::string at(const Transform& t) {
    std::string text;
    for (const std::size_t i : {3u, 7u, 11u}) {
        const double v = std::round(t.m[i] * 1000.0) / 1000.0;
        text += std::format(" {:.3f}", v == 0 ? 0.0 : v);
    }
    return text;
}

// The direction a transform faces, in whole degrees.
int facing(const Transform& t) {
    const int degrees = static_cast<int>(std::lround(std::atan2(t.m[4], t.m[0]) * 57.29578));
    return degrees == -180 ? 180 : degrees;
}

void writeSample(std::ofstream& out, LotLife& life, const ts2::scene::Scene& scene, const ts2::scene::ScenePose& pose, double seconds,
                 const std::map<std::size_t, Transform>& moved) {
    out << std::format("time {:.1f}\n", seconds);
    // Each Sim as its scripts have it: place, facing (degrees) and animation.
    life.simulation().world().nextAfter(0, [&](const ts2::sim::SimObject& o) {
        if (o.isPerson()) {
            const float heading = o.heading ? *o.heading : ts2::sim::compassHeading(o.data[1]);
            out << std::format("script sim {} at {:.3f} {:.3f} level {} facing {} {}\n", o.id, o.x / 65536.0, o.y / 65536.0, o.level,
                               std::lround(heading * 57.29578f), o.animation ? o.animation->name : "-");
        }
        return false;
    });
    for (const auto& [index, nodes] : pose.nodes) {
        if (index >= scene.actors.size()) {
            continue;
        }
        const auto& actor = scene.actors[index];
        out << std::format("sim {} at{} facing {}\n", actor.object, at(actor.place), facing(actor.place));
        for (const auto& clip : actor.clips) {
            const std::string name = clip.frozen                                     ? "(frozen)"
                                     : clip.animation && clip.animation->animation ? clip.animation->animation->name
                                                                                     : "-";
            out << std::format("  clip {} start {:.3f} end {:.3f} layer {} weight {:.3f}{}\n", name, clip.start, clip.end, clip.layer,
                               clip.weight, clip.repeats ? " repeats" : "");
        }
        for (std::size_t i = 0; i < nodes.size() && actor.rig && i < actor.rig->nodes.size(); ++i) {
            if (!actor.rig->nodes[i].name.empty()) {
                out << std::format("  bone {}{}\n", actor.rig->nodes[i].name, at(actor.place * nodes[i]));
            }
        }
    }
    // Objects: where each animated or moved one is drawn, and its joints.
    std::map<std::size_t, Transform> places = moved;
    for (const auto& [instance, place] : pose.places) {
        places[instance] = place;
    }
    std::map<std::size_t, const std::vector<Transform>*> joints;
    for (const auto& [instance, j] : pose.joints) {
        if (instance < scene.instances.size() && scene.instances[instance].object != 0) {
            joints[instance] = &j;
        }
    }
    for (const auto& [instance, place] : places) {
        const std::uint32_t object = instance < scene.instances.size() ? scene.instances[instance].object : 0;
        out << std::format("object {} instance {} at{}\n", object, instance, at(place));
        if (const auto j = joints.find(instance); j != joints.end()) {
            for (std::size_t i = 0; i < j->second->size(); ++i) {
                out << std::format("  joint {}{}\n", i, at((*j->second)[i]));
            }
        }
    }
}

} // namespace

bool writePoseNumbers(LotLife& life, ts2::scene::Scene& scene, double seconds, const std::filesystem::path& file) {
    std::ofstream out(file);
    if (!out) {
        return false;
    }
    ts2::scene::ScenePose pose;
    const std::vector<std::uint8_t> allShown;
    life.traceIn(scene, pose, allShown);
    life.setPaused(false);
    // The last place each moved object instance was given.
    std::map<std::size_t, Transform> moved;
    const int steps = static_cast<int>(std::round(seconds * 10));
    for (int step = 1; step <= steps; ++step) {
        life.update(100, scene);
        for (const auto& [instance, place] : life.movedObjects()) {
            moved[instance] = place;
        }
        if (step % 10 == 0) {
            ts2::scene::poseActors(scene, life.seconds(), pose);
            writeSample(out, life, scene, pose, step / 10.0, moved);
        }
    }
    return static_cast<bool>(out);
}

} // namespace app
