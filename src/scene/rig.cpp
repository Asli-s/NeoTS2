#include "scene/rig.hpp"

#include "formats/hash.hpp"
#include "formats/scenegraph.hpp"
#include "scene/assets.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace sg = ts2::scenegraph;

namespace ts2::scene {

namespace {

constexpr std::uint32_t kCres = 0xE519C933;

// Euler angles in degrees, applied x first, then y, then z: q = qz * qy * qx.
std::array<float, 4> eulerRotation(float x, float y, float z) {
    constexpr float kHalfRadians = 3.14159265358979f / 360.0f;
    const float cx = std::cos(x * kHalfRadians), sx = std::sin(x * kHalfRadians);
    const float cy = std::cos(y * kHalfRadians), sy = std::sin(y * kHalfRadians);
    const float cz = std::cos(z * kHalfRadians), sz = std::sin(z * kHalfRadians);
    return {sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz, cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz};
}

using Vec = std::array<float, 3>;

Vec sub(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec add(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec scale(const Vec& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec cross(const Vec& a, const Vec& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float length(const Vec& a) { return std::sqrt(dot(a, a)); }
Vec unit(const Vec& a) {
    const float l = length(a);
    return l > 1e-8f ? scale(a, 1 / l) : Vec{0, 0, 0};
}
Vec position(const Transform& t) { return {t.m[3], t.m[7], t.m[11]}; }
Vec axis(const Transform& t, int i) { return {t.m[i], t.m[4 + i], t.m[8 + i]}; }

// A rotation from its axes (the columns), with a translation.
Transform frame(const Vec& x, const Vec& y, const Vec& z, const Vec& at) {
    Transform r;
    r.m = {x[0], y[0], z[0], at[0], x[1], y[1], z[1], at[1], x[2], y[2], z[2], at[2]};
    return r;
}

// The rotation of `t` undone (its transpose), without translation.
Transform unrotate(const Transform& t) {
    return frame({t.m[0], t.m[1], t.m[2]}, {t.m[4], t.m[5], t.m[6]}, {t.m[8], t.m[9], t.m[10]}, {0, 0, 0});
}

// `local` with its rotation replaced by that of `rotation`.
Transform rotated(const Transform& local, const Transform& rotation) {
    Transform r = rotation;
    r.m[3] = local.m[3];
    r.m[7] = local.m[7];
    r.m[11] = local.m[11];
    return r;
}

// Solves one limb on the posed rig, changing its three joints' local rotations: the knee (middle)
// bends about its own x axis by pi - acos((a^2 + b^2 - d^2) / 2ab), a and b being the bones'
// lengths along their y axes and d the distance from the hip to the target; the hip turns so that
// the knee lies in the plane of the hip, the target and the pole, on the pole's side; the foot
// (end) takes the target's orientation.
void solveLimb(const Rig& rig, const RigAnimation::Limb& limb, std::vector<Transform>& local, const std::vector<Transform>& world) {
    const auto at = [&](int node, int frameNode) {
        return frameNode >= 0 ? world[static_cast<std::size_t>(frameNode)] * local[static_cast<std::size_t>(node)]
                              : world[static_cast<std::size_t>(node)];
    };
    const std::size_t b0 = static_cast<std::size_t>(limb.begin), m = static_cast<std::size_t>(limb.middle);
    const std::size_t e = static_cast<std::size_t>(limb.end);
    const Transform target = at(limb.target, limb.targetFrame);
    const Vec hip = position(world[b0]), goal = position(target);
    const float a = std::fabs(rig.nodes[m].translation[1]), b = std::fabs(rig.nodes[e].translation[1]);
    const Vec toGoal = sub(goal, hip);
    if (a <= 0 || b <= 0 || length(toGoal) < 1e-6f) {
        return;
    }
    const float d = std::clamp(length(toGoal), std::fabs(a - b) + 1e-5f, a + b);
    const float knee = 3.14159265358979f - std::acos(std::clamp((a * a + b * b - d * d) / (2 * a * b), -1.0f, 1.0f));
    const float hipAngle = std::acos(std::clamp((a * a + d * d - b * b) / (2 * a * d), -1.0f, 1.0f));
    const Vec u = unit(toGoal);
    // Toward the pole, across the line from the hip to the target.
    Vec v = limb.pole >= 0 ? sub(position(at(limb.pole, limb.poleFrame)), hip) : axis(world[b0], 2);
    v = unit(sub(v, scale(u, dot(v, u))));
    if (length(v) == 0) {
        v = unit(cross(u, axis(world[b0], 0)));
    }
    const Vec kneeAt = add(hip, scale(add(scale(u, std::cos(hipAngle)), scale(v, std::sin(hipAngle))), a));
    // The thigh points at the knee; the calf bends from it toward the thigh's +z, away from the pole.
    const Vec y = unit(sub(kneeAt, hip));
    const Vec down = sub(goal, kneeAt);
    Vec z = sub(down, scale(y, dot(down, y)));
    z = length(z) > 1e-6f ? unit(z) : unit(sub(scale(v, -1), scale(y, -dot(v, y))));
    const Vec x = cross(y, z);
    const Transform hipWorld = frame(x, y, z, hip);
    const int parent = rig.nodes[b0].parent;
    const Transform parentWorld = parent >= 0 ? world[static_cast<std::size_t>(parent)] : Transform{};
    local[b0] = rotated(local[b0], unrotate(parentWorld) * hipWorld);
    const float c = std::cos(knee), s = std::sin(knee);
    local[m] = rotated(local[m], frame({1, 0, 0}, {0, c, s}, {0, -s, c}, {0, 0, 0}));
    const Transform kneeWorld = hipWorld * local[m];
    const Transform orientation = limb.targetRotation >= 0 ? world[static_cast<std::size_t>(limb.targetRotation)] : target;
    local[e] = rotated(local[e], unrotate(kneeWorld) * rotated(Transform{}, orientation));
}

// The places of all nodes from their local places, parents first.
void placeAll(const Rig& rig, const std::vector<Transform>& local, std::vector<Transform>& world) {
    for (std::size_t i = 0; i < rig.nodes.size(); ++i) {
        const int parent = rig.nodes[i].parent;
        world[i] = parent >= 0 ? world[static_cast<std::size_t>(parent)] * local[i] : local[i];
    }
}

} // namespace

int Rig::find(const std::string& name) const {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

Rig loadRig(Assets& assets, const std::string& skeleton) {
    Rig rig;
    const auto cres = assets.byName(kCres, skeleton + "_cres");
    if (!cres) {
        return rig;
    }
    sg::ResourceNode tree;
    try {
        tree = sg::parseResourceNode(*cres);
    } catch (const std::exception&) {
        return rig;
    }
    std::vector<bool> seen(tree.nodes.size(), false);
    auto walk = [&](auto&& self, std::uint32_t index, int parent, int depth) -> void {
        if (index >= tree.nodes.size() || seen[index] || depth > 64) {
            return;
        }
        seen[index] = true;
        const auto& node = tree.nodes[index];
        Rig::Node n;
        if (node.kind == sg::SceneNode::Kind::Transform) {
            n.name = lower(node.name);
        }
        n.parent = parent;
        n.translation = node.translation;
        n.rotation = node.rotation;
        rig.nodes.push_back(std::move(n));
        const int me = static_cast<int>(rig.nodes.size() - 1);
        for (const auto c : node.children) {
            self(self, c, me, depth + 1);
        }
    };
    if (!tree.nodes.empty()) {
        walk(walk, 0, -1, 0);
    }
    return rig;
}

RigAnimation bindAnimation(const Rig& rig, std::shared_ptr<const anim::Animation> animation) {
    RigAnimation r;
    r.rotation.assign(rig.nodes.size(), -1);
    r.translation.assign(rig.nodes.size(), -1);
    if (animation) {
        for (std::size_t c = 0; c < animation->channels.size(); ++c) {
            const auto& channel = animation->channels[c];
            if (channel.components.size() != 3) {
                continue;
            }
            const int node = rig.find(lower(channel.name));
            if (node < 0) {
                continue;
            }
            if (channel.kind() == anim::ChannelKind::Rotation) {
                r.rotation[static_cast<std::size_t>(node)] = static_cast<int>(c);
            } else if (channel.kind() == anim::ChannelKind::Translation) {
                r.translation[static_cast<std::size_t>(node)] = static_cast<int>(c);
            }
        }
    }
    if (animation) {
        std::map<std::uint32_t, int> byHash;
        for (std::size_t i = 0; i < rig.nodes.size(); ++i) {
            if (!rig.nodes[i].name.empty()) {
                byHash.try_emplace(hash::crc32(rig.nodes[i].name), static_cast<int>(i));
            }
        }
        const auto node = [&](std::uint32_t h) {
            const auto it = byHash.find(h);
            return h != 0 && it != byHash.end() ? it->second : -1;
        };
        for (const auto& chain : animation->chains) {
            // Limbs of three joints, reaching for a target that is always fully on.
            if (chain.strategy != anim::kLimbStrategy || chain.targets.empty() || chain.targets[0].weight != 0 ||
                chain.targets[0].kind > 1) {
                continue;
            }
            const auto& t = chain.targets[0];
            RigAnimation::Limb limb;
            limb.begin = node(chain.begin);
            limb.end = node(chain.end);
            limb.middle = limb.end >= 0 ? rig.nodes[static_cast<std::size_t>(limb.end)].parent : -1;
            limb.target = node(t.node);
            limb.targetFrame = t.kind == 1 ? node(t.frame) : -1;
            limb.targetRotation = node(t.rotationNode);
            limb.pole = node(chain.pole);
            limb.poleFrame = node(chain.poleFrame);
            if (limb.begin >= 0 && limb.middle >= 0 && limb.target >= 0 &&
                rig.nodes[static_cast<std::size_t>(limb.middle)].parent == limb.begin) {
                r.limbs.push_back(limb);
            }
        }
    }
    r.animation = std::move(animation);
    return r;
}

void poseRig(const Rig& rig, const RigAnimation* animation, float ms, std::vector<Transform>& world) {
    world.resize(rig.nodes.size());
    std::vector<Transform> local(rig.nodes.size());
    const auto* channels = animation && animation->animation ? &animation->animation->channels : nullptr;
    for (std::size_t i = 0; i < rig.nodes.size(); ++i) {
        const auto& node = rig.nodes[i];
        auto t = node.translation;
        auto q = node.rotation;
        if (channels) {
            if (const int c = animation->translation[i]; c >= 0) {
                const auto& k = (*channels)[static_cast<std::size_t>(c)].components;
                t = {k[0].sample(ms), k[1].sample(ms), k[2].sample(ms)};
            }
            if (const int c = animation->rotation[i]; c >= 0) {
                const auto& k = (*channels)[static_cast<std::size_t>(c)].components;
                q = eulerRotation(k[0].sample(ms), k[1].sample(ms), k[2].sample(ms));
            }
        }
        local[i] = Transform::of(t, q);
    }
    placeAll(rig, local, world);
    if (animation && !animation->limbs.empty()) {
        for (const auto& limb : animation->limbs) {
            solveLimb(rig, limb, local, world);
        }
        placeAll(rig, local, world);
    }
}

std::vector<int> skinNodes(const Mesh& mesh, const Rig& rig) {
    std::vector<int> nodes;
    nodes.reserve(mesh.skin.size());
    for (const auto& s : mesh.skin) {
        nodes.push_back(s.joint.empty() ? -1 : rig.find(s.joint));
    }
    return nodes;
}

namespace {

// Whether the animation moves a rig node: by a channel, or as part of a limb it bends.
bool animated(const RigAnimation* animation, int node) {
    if (animation == nullptr) {
        return false;
    }
    const auto n = static_cast<std::size_t>(node);
    if ((n < animation->rotation.size() && animation->rotation[n] >= 0) ||
        (n < animation->translation.size() && animation->translation[n] >= 0)) {
        return true;
    }
    return std::any_of(animation->limbs.begin(), animation->limbs.end(), [&](const RigAnimation::Limb& l) {
        return l.begin == node || l.middle == node || l.end == node;
    });
}

} // namespace

void skinMatrices(const Mesh& mesh, const std::vector<int>& nodes, const Rig& rig, const RigAnimation* animation,
                  const std::vector<Transform>& world, std::vector<Transform>& out) {
    out.resize(mesh.skin.size());
    for (std::size_t i = 0; i < mesh.skin.size(); ++i) {
        const auto& s = mesh.skin[i];
        const int n = i < nodes.size() ? nodes[i] : -1;
        if (n < 0 || static_cast<std::size_t>(n) >= world.size() || static_cast<std::size_t>(n) >= rig.nodes.size()) {
            // A joint the rig lacks stays where the bind pose has it.
            out[i] = s.placement;
            continue;
        }
        const int parent = rig.nodes[static_cast<std::size_t>(n)].parent;
        if (s.rest && parent >= 0 && static_cast<std::size_t>(parent) < world.size() && !animated(animation, n)) {
            out[i] = s.placement * world[static_cast<std::size_t>(parent)] * *s.rest * s.inverseBind;
        } else {
            out[i] = s.placement * world[static_cast<std::size_t>(n)] * s.inverseBind;
        }
    }
}

void poseActors(const Scene& scene, double seconds, ScenePose& out) {
    out.joints.clear();
    out.places.clear();
    std::vector<Transform> world;
    for (const auto& actor : scene.actors) {
        if (!actor.rig) {
            continue;
        }
        float ms = 0;
        if (actor.animation && actor.animation->animation && actor.animation->animation->duration > 0) {
            const double length = actor.animation->animation->duration;
            const double played = std::max(0.0, seconds - actor.start) * 1000.0 * actor.speed;
            ms = static_cast<float>(actor.loop ? std::fmod(played, length) : std::min(played, length));
        }
        poseRig(*actor.rig, actor.animation.get(), ms, world);
        for (const auto& s : actor.skinned) {
            if (s.instance >= scene.instances.size()) {
                continue;
            }
            const int mesh = scene.instances[s.instance].mesh;
            if (mesh < 0 || static_cast<std::size_t>(mesh) >= scene.meshes.size()) {
                continue;
            }
            auto& joints = out.joints.emplace_back(s.instance, std::vector<Transform>{}).second;
            skinMatrices(scene.meshes[static_cast<std::size_t>(mesh)], s.nodes, *actor.rig, actor.animation.get(), world, joints);
        }
        for (const auto& h : actor.held) {
            const bool known = h.node >= 0 && static_cast<std::size_t>(h.node) < world.size();
            out.places.emplace_back(h.instance, known ? actor.place * world[static_cast<std::size_t>(h.node)] : actor.place);
        }
    }
}

} // namespace ts2::scene
