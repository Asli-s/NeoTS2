#include "scene/look_at.hpp"

#include "scene/limb_ik.hpp"
#include "scene/rig.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::scene {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDegree = kPi / 180;

using Vec = std::array<float, 3>;

Vec sub(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec scaled(const Vec& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec cross(const Vec& a, const Vec& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }

// `v` in the space of a world rotation `m` (its transpose applied).
Vec intoRotation(const Transform& m, const Vec& v) {
    return {m.m[0] * v[0] + m.m[4] * v[1] + m.m[8] * v[2], m.m[1] * v[0] + m.m[5] * v[1] + m.m[9] * v[2],
            m.m[2] * v[0] + m.m[6] * v[1] + m.m[10] * v[2]};
}

} // namespace

HeadChain headChain(const Rig& rig) {
    const int head = rig.find("head"), grip = rig.find("head_grip");
    if (head < 0 || grip < 0 || rig.nodes[static_cast<std::size_t>(grip)].parent != head) {
        return {};
    }
    return {head, grip};
}

bool aimHead(const Rig& rig, const HeadChain& chain, const Vec& target, const Vec& pole, std::vector<Transform>& local,
             const std::vector<Transform>& world) {
    if (chain.head < 0 || chain.grip < 0 || local.size() != rig.nodes.size() || world.size() != rig.nodes.size() ||
        static_cast<std::size_t>(std::max(chain.head, chain.grip)) >= rig.nodes.size()) {
        return false;
    }
    const auto head = static_cast<std::size_t>(chain.head);
    const int parent = rig.nodes[head].parent;
    // The target and pole in the head's parent's frame, from the head.
    const Vec origin{world[head].m[3], world[head].m[7], world[head].m[11]};
    const Transform frame = parent >= 0 ? world[static_cast<std::size_t>(parent)] : Transform{};
    const Vec t = intoRotation(frame, sub(target, origin));
    const Vec p = intoRotation(frame, sub(pole, origin));
    const float d2 = dot(t, t), d = std::sqrt(d2);
    // Only the eye's offset along the head's Y counts.
    const float eye = local[static_cast<std::size_t>(chain.grip)].m[7];
    if (!(d2 - eye * eye > 0)) {
        return false;
    }
    Vec n = cross(p, t);
    const float nl = std::sqrt(dot(n, n));
    if (!(nl > 0)) {
        return false;
    }
    n = scaled(n, 1 / nl);
    Vec u = cross(t, n);
    u = scaled(u, 1 / std::sqrt(dot(u, u)));
    const Vec forward = scaled(t, 1 / d);
    // The head's Y makes the angle with the target whose cosine is eye / d, toward the pole, and its
    // Z then runs from the eye through the target.
    const float c = eye / d;
    const float angle = c > 0 ? std::acos(std::min(c, 1.0f)) : kPi / 2;
    const float s = std::sin(angle), k = std::cos(angle);
    const Vec y{u[0] * s + forward[0] * k, u[1] * s + forward[1] * k, u[2] * s + forward[2] * k};
    const Vec z{forward[0] * s - u[0] * k, forward[1] * s - u[1] * k, forward[2] * s - u[2] * k};
    auto& m = local[head].m;
    m = {n[0], y[0], z[0], m[3], n[1], y[1], z[1], m[7], n[2], y[2], z[2], m[11]};
    return true;
}

std::optional<HeadPlace> headPlace(const Rig& rig, const HeadChain& chain, const std::vector<Transform>& world) {
    if (chain.head < 0 || chain.grip < 0 || static_cast<std::size_t>(chain.head) >= world.size()) {
        return std::nullopt;
    }
    const auto head = static_cast<std::size_t>(chain.head);
    const int neck = rig.nodes[head].parent;
    if (neck < 0) {
        return std::nullopt;
    }
    const Transform& n = world[static_cast<std::size_t>(neck)];
    HeadPlace p;
    p.neck = turnOf(n);
    p.head = {world[head].m[3], world[head].m[7], world[head].m[11]};
    p.eye = rig.nodes[static_cast<std::size_t>(chain.grip)].translation;
    p.headTurn = turnOf(n.rigidInverse() * world[head]);
    p.headRest = rig.nodes[head].rotation;
    return p;
}

} // namespace ts2::scene
