#include "scene/limb_ik.hpp"

#include "formats/hash.hpp"
#include "scene/assets.hpp"
#include "scene/rig.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace ts2::scene {

namespace {

constexpr float kPi = 3.14159265358979f;
// Of the upper arm's turn about its own length, the share the bicep takes. Of the hand's, the wrist's.
constexpr float kUpperTwistShare = 0.65f;
constexpr float kEndTwistShare = 0.75f;
// Weights at or below this count as off.
constexpr float kWeightEpsilon = 0.00075f;

using Vec = std::array<float, 3>;
// A rotation matrix, row by row (column vectors: v' = R v).
using Mat = std::array<float, 9>;

Vec sub(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec add(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec scale(const Vec& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec cross(const Vec& a, const Vec& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float length(const Vec& a) { return std::sqrt(dot(a, a)); }
Vec unit(const Vec& a) {
    const float l = length(a);
    return l > 0 ? scale(a, 1 / l) : Vec{0, 0, 0};
}

Vec position(const Transform& t) { return {t.m[3], t.m[7], t.m[11]}; }
Mat rotation(const Transform& t) { return {t.m[0], t.m[1], t.m[2], t.m[4], t.m[5], t.m[6], t.m[8], t.m[9], t.m[10]}; }

Mat mul(const Mat& a, const Mat& b) {
    Mat r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r[3 * i + j] = a[3 * i] * b[j] + a[3 * i + 1] * b[3 + j] + a[3 * i + 2] * b[6 + j];
        }
    }
    return r;
}
Mat transposed(const Mat& a) { return {a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8]}; }
Vec times(const Mat& a, const Vec& v) {
    return {a[0] * v[0] + a[1] * v[1] + a[2] * v[2], a[3] * v[0] + a[4] * v[1] + a[5] * v[2], a[6] * v[0] + a[7] * v[1] + a[8] * v[2]};
}
Mat aboutX(float a) { return {1, 0, 0, 0, std::cos(a), -std::sin(a), 0, std::sin(a), std::cos(a)}; }
Mat aboutY(float a) { return {std::cos(a), 0, std::sin(a), 0, 1, 0, -std::sin(a), 0, std::cos(a)}; }
// A turn by `angle` about the unit axis `u`.
Mat aboutAxis(const Vec& u, float angle) {
    const float c = std::cos(angle), s = std::sin(angle), k = 1 - c;
    return {c + k * u[0] * u[0],        k * u[0] * u[1] - s * u[2], k * u[0] * u[2] + s * u[1],
            k * u[1] * u[0] + s * u[2], c + k * u[1] * u[1],        k * u[1] * u[2] - s * u[0],
            k * u[2] * u[0] - s * u[1], k * u[2] * u[1] + s * u[0], c + k * u[2] * u[2]};
}

using Quat = std::array<float, 4>;  // x, y, z, w

Quat quatOf(const Mat& m) {
    const float trace = m[0] + m[4] + m[8];
    Quat q;
    if (trace > 0) {
        const float s = 2 * std::sqrt(trace + 1);
        q = {(m[7] - m[5]) / s, (m[2] - m[6]) / s, (m[3] - m[1]) / s, s / 4};
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const float s = 2 * std::sqrt(1 + m[0] - m[4] - m[8]);
        q = {s / 4, (m[1] + m[3]) / s, (m[2] + m[6]) / s, (m[7] - m[5]) / s};
    } else if (m[4] > m[8]) {
        const float s = 2 * std::sqrt(1 + m[4] - m[0] - m[8]);
        q = {(m[1] + m[3]) / s, s / 4, (m[5] + m[7]) / s, (m[2] - m[6]) / s};
    } else {
        const float s = 2 * std::sqrt(1 + m[8] - m[0] - m[4]);
        q = {(m[2] + m[6]) / s, (m[5] + m[7]) / s, s / 4, (m[3] - m[1]) / s};
    }
    return q;
}
Mat matOf(const Quat& q) { return rotation(Transform::of({0, 0, 0}, q)); }

// From `a` toward `b` by `t`, the short way round.
Quat slerp(const Quat& a, Quat b, float t) {
    float c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (c < 0) {
        b = {-b[0], -b[1], -b[2], -b[3]};
        c = -c;
    }
    float wa = 1 - t, wb = t;
    if (c < 0.9995f) {
        const float angle = std::acos(c), s = std::sin(angle);
        wa = std::sin((1 - t) * angle) / s;
        wb = std::sin(t * angle) / s;
    }
    Quat r{wa * a[0] + wb * b[0], wa * a[1] + wb * b[1], wa * a[2] + wb * b[2], wa * a[3] + wb * b[3]};
    const float l = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
    return l > 0 ? Quat{r[0] / l, r[1] / l, r[2] / l, r[3] / l} : a;
}

float weightAt(const anim::Animation& a, int channel, float ms) {
    if (channel < 0) {
        return 1.0f;
    }
    const float w = a.channels[static_cast<std::size_t>(channel)].components[0].sample(ms);
    return std::isfinite(w) ? std::clamp(w, 0.0f, 1.0f) : 0.0f;
}

// `local` with its rotation replaced.
void setRotation(Transform& local, const Mat& r) {
    local.m = {r[0], r[1], r[2], local.m[3], r[3], r[4], r[5], local.m[7], r[6], r[7], r[8], local.m[11]};
}

// A channel's value at `ms`, reflected across `axis` in a mirrored clip (-1: as it is), then turned
// `turn` radians about `turnAxis` (see LimbSource::turn).
Vec sampleVec(const anim::Animation& a, int channel, float ms, int axis, int turnAxis, float turn) {
    const auto& k = a.channels[static_cast<std::size_t>(channel)].components;
    return turnedPlace(mirroredPlace({k[0].sample(ms), k[1].sample(ms), k[2].sample(ms)}, axis), turnAxis, turn);
}
Mat sampleRotation(const anim::Animation& a, int channel, float ms, int axis, int turnAxis, float turn) {
    const auto q = mirroredTurn(channelTurn(a.channels[static_cast<std::size_t>(channel)], ms), axis);
    return rotation(Transform::of({0, 0, 0}, turnedTurn(q, turnAxis, turn)));
}

} // namespace

std::array<float, 4> turnToward(const std::array<float, 4>& a, const std::array<float, 4>& b, float t) { return slerp(a, b, t); }

// A limb whose target positions are additive channels: its whole chain only adds to the others.
bool isAdditive(const anim::Animation& a, const LimbChain& limb) {
    return !limb.targets.empty() && std::all_of(limb.targets.begin(), limb.targets.end(), [&](const LimbTarget& t) {
        return t.position >= 0 && a.channels[static_cast<std::size_t>(t.position)].additive();
    });
}

float limbWeightAt(const anim::Animation& a, int channel, float ms) { return weightAt(a, channel, ms); }

LimbFloor limbFloor(const anim::Animation& a, const LimbChain& limb, std::size_t joint, float ms) {
    const float weight = weightAt(a, limb.weight, ms);
    const bool placing = joint < (limb.joints.size() == 3 ? 2u : 3u);
    LimbFloor floor;
    for (const auto& t : limb.targets) {
        if (placing || t.rotation >= 0) {
            floor.targets += weight * weightAt(a, t.weight, ms);
        }
    }
    floor.pole = placing ? weight : 0.0f;
    return floor;
}

float weighLimbJoint(std::span<const LimbJointLayer> layers, std::span<float> scales) {
    std::fill(scales.begin(), scales.end(), 0.0f);
    float left = 1, total = 0, ik = 0;
    for (std::size_t k = 0; k < layers.size() && k < scales.size(); ++k) {
        const float floor = std::max(layers[k].targets, layers[k].pole);
        const float sum = layers[k].turns + floor;
        if (!(sum > 0)) {
            continue;
        }
        // A layer that takes all that is left shares it by its own weights and closes the mix.
        if (sum >= 1) {
            total += 1;
            scales[k] = left / sum;
            ik += left * floor / sum;
            break;
        }
        total += sum;
        scales[k] = left;
        ik += left * floor;
        left *= 1 - sum;
    }
    // Layers that weigh less than 1 in all are scaled up to 1, but the IK weight stays as it is.
    if (total > 0 && total < 1) {
        for (auto& s : scales) {
            s /= total;
        }
    }
    return ik;
}

float solvedShare(float ikWeight, float turns) {
    if (!(ikWeight > kWeightEpsilon)) {
        return 0;
    }
    return turns > 0 ? ikWeight / (turns + ikWeight) : 1.0f;
}

namespace {

// A point given in a frame node's space (the skeleton's root when -1), in the rig's space.
Vec inFrame(const std::vector<Transform>& world, int frame, const Vec& p) {
    if (frame < 0) {
        return p;
    }
    Vec v = p;
    world[static_cast<std::size_t>(frame)].apply(v.data(), true);
    return v;
}

// An additive limb's share: its target and pole offsets (turned into the rig's space) times its clip's
// weight, and its target turns, each by that weight's share of its angle, chained onto `turn`.
void addOffsets(const std::vector<Transform>& world, const anim::Animation& a, const LimbChain& limb, float ms, float weight,
                float clipTurn, Vec& target, Vec& pole, Quat& turn) {
    if (weight <= kWeightEpsilon) {
        return;
    }
    const auto offset = [&](int frame, int channel, int axis, int turnAxis) {
        return scale(sub(inFrame(world, frame, sampleVec(a, channel, ms, axis, turnAxis, clipTurn)), inFrame(world, frame, {0, 0, 0})), weight);
    };
    if (limb.pole >= 0 && a.channels[static_cast<std::size_t>(limb.pole)].additive()) {
        pole = add(pole, offset(limb.poleFrame, limb.pole, limb.poleAxis, limb.poleTurnAxis));
    }
    for (const auto& t : limb.targets) {
        target = add(target, offset(t.frame, t.position, t.positionAxis, t.positionTurnAxis));
        if (t.rotation < 0) {
            continue;
        }
        Mat m = sampleRotation(a, t.rotation, ms, t.rotationAxis, t.rotationTurnAxis, clipTurn);
        if (t.extraRotation >= 0) {
            m = mul(m, sampleRotation(a, t.extraRotation, ms, t.extraRotationAxis, -1, 0));
        }
        turn = turnProduct(turn, partialTurn(quatOf(m), weight));
    }
}

// Places the limb's start, middle twist and bend so that its end lands on `target`, bending away from
// `pole`. False when the solve fails (target on the line to the pole).
bool solvePosition(const LimbChain& limb, std::size_t split, const Vec& target, const Vec& pole, std::vector<Transform>& local,
                   const std::vector<Transform>& world, const Rig& rig) {
    const auto& j = limb.joints;
    float a = 0, b = 0;
    for (std::size_t i = 1; i < j.size(); ++i) {
        (i < split ? a : b) += local[static_cast<std::size_t>(j[i])].m[7];
    }
    if (a <= 0 || b <= 0) {
        return false;
    }
    const auto begin = static_cast<std::size_t>(j[0]);
    const int parent = rig.nodes[begin].parent;
    const Mat parentRotation = parent >= 0 ? rotation(world[static_cast<std::size_t>(parent)]) : Mat{1, 0, 0, 0, 1, 0, 0, 0, 1};
    const Mat toParent = transposed(parentRotation);
    const Vec origin = position(world[begin]);
    const Vec t = times(toParent, sub(target, origin));
    const Vec p = times(toParent, sub(pole, origin));
    const float d2 = dot(t, t), d = std::sqrt(d2);
    const Vec n = unit(cross(t, p));
    if (length(n) == 0 || d == 0) {
        return false;
    }
    const Vec u = unit(cross(n, t));
    const float c1 = (d2 + a * a - b * b) / (2 * d * a);
    const Vec dir = c1 > 0 ? unit(add(t, scale(u, d * std::tan(std::acos(std::min(c1, 1.0f)))))) : u;
    // Swing the bone's length (+y) onto that direction, then turn it about itself so that its x axis
    // is the bend's axis.
    const float theta = std::acos(std::clamp(dir[1], -1.0f, 1.0f));
    Vec axis = unit({a * dir[2], 0, -a * dir[0]});
    if (length(axis) == 0) {
        axis = {0, 0, 1};
    }
    const Mat m = aboutAxis(axis, theta);
    float phi = std::acos(-std::clamp(m[0] * n[0] + m[3] * n[1] + m[6] * n[2], -1.0f, 1.0f));
    if (m[2] * n[0] + m[5] * n[1] + m[8] * n[2] < 0) {
        phi = -phi;
    }
    Mat start = mul(m, aboutY(phi));
    if (j.size() == 5) {
        // The bicep takes part of the upper arm's twist (its Euler y).
        const float twist = kUpperTwistShare * std::atan2(-start[6], std::sqrt(start[0] * start[0] + start[3] * start[3]));
        start = mul(start, aboutY(-twist));
        setRotation(local[static_cast<std::size_t>(j[1])], aboutY(twist));
    }
    setRotation(local[begin], start);
    const float bend = kPi - std::acos(std::clamp((b * b + a * a - d2) / (2 * b * a), -1.0f, 1.0f));
    setRotation(local[static_cast<std::size_t>(j[split - 1])], aboutX(bend));
    return true;
}

// Turns the limb's end (foot, or wrist and hand) to the orientation `target` (in the rig's space).
void solveOrientation(const LimbChain& limb, std::size_t split, const Mat& target, std::vector<Transform>& local,
                      const std::vector<Transform>& world, const Rig& rig) {
    const auto& j = limb.joints;
    const auto end = static_cast<std::size_t>(j[split]);
    const int parent = rig.nodes[end].parent;
    const Mat parentRotation = parent >= 0 ? rotation(world[static_cast<std::size_t>(parent)]) : Mat{1, 0, 0, 0, 1, 0, 0, 0, 1};
    const Mat wanted = mul(transposed(parentRotation), target);
    if (j.size() == 3) {
        setRotation(local[end], wanted);
        return;
    }
    // The wrist takes part of the turn (the z angle of a y-z-x decomposition), the hand the rest.
    const float twist = kEndTwistShare * -std::atan2(-wanted[3], std::sqrt(wanted[6] * wanted[6] + wanted[0] * wanted[0]));
    setRotation(local[end], aboutY(twist));
    setRotation(local[static_cast<std::size_t>(j[split + 1])], mul(aboutY(-twist), wanted));
}

} // namespace

std::vector<LimbChain> bindLimbs(const Rig& rig, const anim::Animation& animation, bool mirrored) {
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
    std::map<std::uint32_t, int> rotations, translations, scalars;
    for (std::size_t c = 0; c < animation.channels.size(); ++c) {
        const auto& ch = animation.channels[c];
        const std::uint32_t h = hash::crc32(lower(ch.name));
        if (ch.kind() == anim::ChannelKind::Rotation && ch.components.size() == 3) {
            rotations[h] = static_cast<int>(c);
        } else if (ch.kind() == anim::ChannelKind::Translation && ch.components.size() == 3) {
            translations[h] = static_cast<int>(c);
        } else if (ch.kind() == anim::ChannelKind::Scalar && ch.components.size() == 1) {
            scalars[h] = static_cast<int>(c);
        }
    }
    const auto channel = [](const std::map<std::uint32_t, int>& m, std::uint32_t h) {
        const auto it = m.find(h);
        return h != 0 && it != m.end() ? it->second : -1;
    };
    // Mirrored, a pole channel is reflected across the axis of the joint it is expressed in, or with
    // none, of the joint on the other side its name gives (unbound when that is missing).
    const auto mirrorAxis = [&](int& c, int frame) {
        if (!mirrored || c < 0) {
            return -1;
        }
        if (frame >= 0) {
            return rig.nodes[static_cast<std::size_t>(frame)].mirrorAxis;
        }
        const int joint = rig.find(lower(mirroredJointName(animation.channels[static_cast<std::size_t>(c)].name)));
        if (joint < 0) {
            c = -1;
            return -1;
        }
        return rig.nodes[static_cast<std::size_t>(joint)].mirrorAxis;
    };
    // A clip's own heading turns a target or pole channel about the turn axis of the joint it is
    // expressed in, or with none, of its own joint (the one on the other side when mirrored).
    const auto turnAxis = [&](int c, int frame) {
        if (c < 0) {
            return -1;
        }
        if (frame >= 0) {
            return rig.nodes[static_cast<std::size_t>(frame)].turnAxis;
        }
        const auto& name = animation.channels[static_cast<std::size_t>(c)].name;
        const int joint = rig.find(lower(mirrored ? mirroredJointName(name) : name));
        // A control the skeleton has no joint for (a child has no hand controls) turns with the root,
        // as every limb control does, so its target stays where an adult's does.
        const int about = joint >= 0 ? joint : rig.find("root_trans");
        return about >= 0 ? rig.nodes[static_cast<std::size_t>(about)].turnAxis : -1;
    };
    // Targets in their skeleton's own space are relative to its root node (the root's parent).
    const int rootTrans = rig.find("root_trans");
    const int root = rootTrans >= 0 ? rig.nodes[static_cast<std::size_t>(rootTrans)].parent : -1;

    std::vector<LimbChain> limbs;
    for (const auto& chain : animation.chains) {
        if (chain.strategy != anim::kLimbStrategy) {
            continue;
        }
        LimbChain limb;
        const int begin = node(mirrored ? chain.beginMirror : chain.begin);
        for (int n = node(mirrored ? chain.endMirror : chain.end); n >= 0 && limb.joints.size() < 6; n = rig.nodes[static_cast<std::size_t>(n)].parent) {
            limb.joints.push_back(n);
            if (n == begin) {
                break;
            }
        }
        if (begin < 0 || limb.joints.empty() || limb.joints.back() != begin || (limb.joints.size() != 3 && limb.joints.size() != 5)) {
            continue;
        }
        std::reverse(limb.joints.begin(), limb.joints.end());
        // A target on the animation's IK object (kind 2) whose frame is 0 or the special name below is
        // in the space of that object's own node: the clip's IK object where it has one (a reach
        // clip's), else the Sim itself. Any other frame names a bone of the IK object (a hug's hands on
        // the other Sim's back). A target with a frame isn't turned with the clip.
        constexpr std::uint32_t kIkObjectNode = 0x6E80A11A;
        for (const auto& t : chain.targets) {
            const std::uint32_t objectFrame = mirrored ? t.frameMirror : t.frame;
            const int kind = t.kind == 2 ? 0 : t.kind;
            LimbTarget target;
            target.position = channel(translations, t.position);
            target.rotation = channel(rotations, t.node);
            target.extraRotation = channel(rotations, t.rotationNode);
            target.weight = channel(scalars, t.weight);
            const int frame = kind == 1 ? node(mirrored ? t.frameMirror : t.frame) : -1;
            // Mirrored, a target channel keeps its name (it need not be a joint) and is reflected across
            // the axis of the joint it is expressed in, or x with none, as the game's IK binder does.
            const auto targetAxis = [&](int c) { return mirrored && c >= 0 ? (frame >= 0 ? rig.nodes[static_cast<std::size_t>(frame)].mirrorAxis : 0) : -1; };
            target.positionAxis = targetAxis(target.position);
            target.rotationAxis = targetAxis(target.rotation);
            target.extraRotationAxis = targetAxis(target.extraRotation);
            target.onObject = t.kind == 2;
            target.objectBone = target.onObject && objectFrame != kIkObjectNode ? objectFrame : 0;
            target.positionTurnAxis = target.onObject ? -1 : turnAxis(target.position, frame);
            target.rotationTurnAxis = target.onObject ? -1 : turnAxis(target.rotation, frame);
            target.frame = kind == 1 ? frame : root;
            if (kind <= 1 && target.position >= 0 && (kind == 0 || target.frame >= 0)) {
                limb.targets.push_back(target);
            }
        }
        limb.pole = channel(translations, chain.pole);
        const std::uint32_t poleFrame = mirrored ? chain.poleFrameMirror : chain.poleFrame;
        limb.poleAxis = mirrorAxis(limb.pole, poleFrame != 0 ? node(poleFrame) : -1);
        limb.poleTurnAxis = turnAxis(limb.pole, poleFrame != 0 ? node(poleFrame) : -1);
        limb.poleFrame = poleFrame != 0 ? node(poleFrame) : root;
        limb.weight = channel(scalars, chain.extra);
        if (!limb.targets.empty() && limb.pole >= 0) {
            limbs.push_back(std::move(limb));
        }
    }
    return limbs;
}

void solveLimbs(const Rig& rig, std::span<const LimbSource> sources, std::vector<Transform>& local, std::vector<Transform>& world,
                const std::vector<float>* shares) {
    // The skeleton's own space: the frame of targets that name none.
    const int rootTrans = rig.find("root_trans");
    const int skeleton = rootTrans >= 0 ? rig.nodes[static_cast<std::size_t>(rootTrans)].parent : -1;
    const auto inSkeleton = [&](int frame) { return frame < 0 || frame == skeleton; };
    // Each limb once, in the order the animations name them.
    std::vector<const LimbChain*> distinct;
    for (const auto& s : sources) {
        for (const auto& limb : *s.limbs) {
            if (std::none_of(distinct.begin(), distinct.end(), [&](const LimbChain* d) { return d->joints == limb.joints; })) {
                distinct.push_back(&limb);
            }
        }
    }
    for (const LimbChain* chain : distinct) {
        // All the animations' targets and poles for this limb: positions averaged, orientations
        // slerped in one at a time, each weighted by its animation's weight, the limb's weight in that
        // animation and its own.
        // Additive channels (see anim::Channel::additive) are added on top instead, by their clip's weight.
        Vec target{0, 0, 0}, pole{0, 0, 0}, addedTarget{0, 0, 0}, addedPole{0, 0, 0};
        float shown = 0, following = 0, positionWeight = 0, rotationWeight = 0;
        Quat orientation{0, 0, 0, 1}, addedTurn{0, 0, 0, 1};
        for (const auto& s : sources) {
            const auto& a = *s.animation;
            for (std::size_t k = 0; k < s.limbs->size(); ++k) {
                const auto& limb = (*s.limbs)[k];
                if (limb.joints != chain->joints) {
                    continue;
                }
                if (isAdditive(a, limb)) {
                    addOffsets(world, a, limb, s.ms, s.clipWeight, s.turn, addedTarget, addedPole, addedTurn);
                    continue;
                }
                const float weight = k < s.limbWeights.size() ? s.limbWeights[k] : s.weight;
                if (weight <= kWeightEpsilon) {
                    continue;
                }
                const float follow = weight * weightAt(a, limb.weight, s.ms);
                shown += weight;
                following += follow;
                if (follow <= kWeightEpsilon) {
                    continue;
                }
                Vec poleAt = inFrame(world, limb.poleFrame, sampleVec(a, limb.pole, s.ms, limb.poleAxis, limb.poleTurnAxis, s.turn));
                if (inSkeleton(limb.poleFrame)) {
                    s.rootMove.apply(poleAt.data(), true);
                }
                pole = add(pole, scale(poleAt, follow));
                for (const auto& t : limb.targets) {
                    const float w = follow * weightAt(a, t.weight, s.ms);
                    if (w <= kWeightEpsilon) {
                        continue;
                    }
                    const std::optional<Transform> objectFrame =
                        t.objectBone == 0 ? s.objectFrame : s.objectBone ? s.objectBone(t.objectBone) : std::nullopt;
                    if (t.objectBone != 0 && !objectFrame) {
                        continue;
                    }
                    const bool onObject = t.onObject && objectFrame;
                    Vec at = sampleVec(a, t.position, s.ms, t.positionAxis, t.positionTurnAxis, s.turn);
                    if (onObject) {
                        objectFrame->apply(at.data(), true);
                    } else {
                        at = inFrame(world, t.frame, at);
                        if (inSkeleton(t.frame)) {
                            s.rootMove.apply(at.data(), true);
                        }
                    }
                    target = add(target, scale(at, w));
                    positionWeight += w;
                    if (t.rotation < 0) {
                        continue;
                    }
                    Mat turn = sampleRotation(a, t.rotation, s.ms, t.rotationAxis, t.rotationTurnAxis, s.turn);
                    if (t.extraRotation >= 0) {
                        // The clip's turn turns the target's first rotation only, as the game's does.
                        turn = mul(turn, sampleRotation(a, t.extraRotation, s.ms, t.extraRotationAxis, -1, 0));
                    }
                    if (onObject) {
                        turn = mul(rotation(*objectFrame), turn);
                    } else if (t.frame >= 0) {
                        turn = mul(rotation(world[static_cast<std::size_t>(t.frame)]), turn);
                    }
                    if (!onObject && inSkeleton(t.frame)) {
                        turn = mul(rotation(s.rootMove), turn);
                    }
                    rotationWeight += w;
                    orientation = rotationWeight == w ? quatOf(turn) : slerp(orientation, quatOf(turn), w / rotationWeight);
                }
            }
        }
        if (positionWeight <= 0 || following <= 0 || shown <= 0) {
            continue;
        }
        target = add(scale(target, 1 / positionWeight), addedTarget);
        pole = add(scale(pole, 1 / following), addedPole);
        // Added target turns are in the target's own frame, so they come after its turn, as a joint's do.
        orientation = turnProduct(orientation, addedTurn);
        std::vector<Transform> own;
        for (const int j : chain->joints) {
            own.push_back(local[static_cast<std::size_t>(j)]);
        }
        const std::size_t split = chain->joints.size() == 3 ? 2 : 3;
        if (!solvePosition(*chain, split, target, pole, local, world, rig)) {
            continue;
        }
        placeRig(rig, local, world);
        if (rotationWeight > 0) {
            solveOrientation(*chain, split, matOf(orientation), local, world, rig);
            placeRig(rig, local, world);
        }
        // Each joint between the clips' own turn and the solved one, by the solved turn's share.
        if (shares != nullptr) {
            bool partly = false;
            for (std::size_t i = 0; i < chain->joints.size(); ++i) {
                const auto j = static_cast<std::size_t>(chain->joints[i]);
                const float share = j < shares->size() ? (*shares)[j] : 1.0f;
                if (share >= 1) {
                    continue;
                }
                auto& joint = local[j];
                setRotation(joint, matOf(slerp(quatOf(rotation(own[i])), quatOf(rotation(joint)), std::max(share, 0.0f))));
                partly = true;
            }
            if (partly) {
                placeRig(rig, local, world);
            }
        }
    }
}

std::array<float, 4> turnOf(const Transform& t) { return quatOf(rotation(t)); }

} // namespace ts2::scene
