#include "scene/rig.hpp"

#include "diag/timing.hpp"
#include "formats/hash.hpp"
#include "formats/scenegraph.hpp"
#include "scene/assets.hpp"
#include "scene/rig_layers.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_set>

namespace sg = ts2::scenegraph;

namespace ts2::scene {

namespace {

constexpr std::uint32_t kCres = 0xE519C933;

} // namespace

std::array<float, 4> eulerRotation(float x, float y, float z) {
    constexpr float kHalfRadians = 3.14159265358979f / 360.0f;
    const float cx = std::cos(x * kHalfRadians), sx = std::sin(x * kHalfRadians);
    const float cy = std::cos(y * kHalfRadians), sy = std::sin(y * kHalfRadians);
    const float cz = std::cos(z * kHalfRadians), sz = std::sin(z * kHalfRadians);
    return {sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz, cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz};
}

std::array<float, 4> turnProduct(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

std::array<float, 4> partialTurn(const std::array<float, 4>& q, float share) {
    // The game takes the angle from w only when it is strictly inside -1..1 (acos, inferred).
    const float half = q[3] > -1 && q[3] < 1 ? std::acos(q[3]) : 0.0f;
    const float s = std::sin(half);
    const float k = s != 0 ? std::sin(half * share) / s : 0.0f;
    return {k * q[0], k * q[1], k * q[2], std::cos(half * share)};
}

std::array<float, 4> channelTurn(const anim::Channel& channel, float ms) {
    const auto& k = channel.components;
    const auto at = [&](float t) { return eulerRotation(k[0].sample(t), k[1].sample(t), k[2].sample(t)); };
    const auto frameTime = [](float frame) { return std::trunc(frame * 33.333f + 0.1667f); };
    const float frame = std::trunc(std::max(ms, 0.0f) * 0.03f);
    const float ta = frameTime(frame), tb = frameTime(frame + 1);
    const float s = std::clamp((ms - ta) / (tb - ta), 0.0f, 1.0f);
    const auto a = at(ta);
    auto b = at(tb);
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dot < 0) {
        b = {-b[0], -b[1], -b[2], -b[3]};
        dot = -dot;
    }
    float wa = 1 - s, wb = s;
    if (dot < 0.95f) {
        const float angle = std::acos(dot), sine = std::sin(angle);
        wa = std::sin((1 - s) * angle) / sine;
        wb = std::sin(s * angle) / sine;
    }
    std::array<float, 4> q{wa * a[0] + wb * b[0], wa * a[1] + wb * b[1], wa * a[2] + wb * b[2], wa * a[3] + wb * b[3]};
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    return length > 1e-6f ? std::array<float, 4>{q[0] / length, q[1] / length, q[2] / length, q[3] / length} : a;
}

std::array<float, 4> animatedRotation(const Rig::Node& node, const anim::Channel& channel, float ms) {
    const auto b = channelTurn(channel, ms);
    return turnProduct(node.animationTurn, b);
}

std::array<float, 4> animatedRotation(const Rig::Node& node, float x, float y, float z) {
    const auto b = eulerRotation(x, y, z);
    return turnProduct(node.animationTurn, b);
}

void placeRig(const Rig& rig, const std::vector<Transform>& local, std::vector<Transform>& world) {
    for (std::size_t i = 0; i < rig.nodes.size(); ++i) {
        const int parent = rig.nodes[i].parent;
        world[i] = parent >= 0 ? world[static_cast<std::size_t>(parent)] * local[i] : local[i];
    }
}

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
        // A shape node is a joint too, as in held objects whose root_rot holds the mesh.
        if (node.kind == sg::SceneNode::Kind::Transform || node.kind == sg::SceneNode::Kind::ShapeRef) {
            n.name = lower(node.name);
        }
        n.parent = parent;
        n.translation = node.translation;
        n.rotation = node.rotation;
        for (const auto e : node.extensions) {
            if (e < tree.nodes.size() && tree.nodes[e].boneData) {
                n.animationTurn = tree.nodes[e].boneData->rotation;
                // The game indexes the turn's components by the axis unchecked. A broken file's axis out
                // of range is kept as -1, which leaves the joint's values unreflected.
                const std::uint32_t axis = tree.nodes[e].boneData->words[1];
                n.mirrorAxis = axis < 3 ? static_cast<int>(axis) : -1;
                // 3 is the game's value for none.
                const std::uint32_t turn = tree.nodes[e].boneData->words[0];
                n.turnAxis = turn < 3 ? static_cast<int>(turn) : -1;
                // A stretch axis above 4 would make the game scale a value outside the translation.
                // It is taken as none here, as is a weight that isn't a number.
                const std::uint32_t stretchAxis = tree.nodes[e].boneData->words[2];
                const float weight = tree.nodes[e].boneData->weight;
                n.stretchAxis = stretchAxis <= Rig::kStretchAll ? static_cast<int>(stretchAxis) : Rig::kStretchNone;
                n.stretchWeight = std::isfinite(weight) ? weight : 0;
            }
        }
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

std::array<float, 3> stretchedPlace(const Rig& rig, const Rig::Node& node, float stretch) {
    auto t = node.translation;
    const float w = node.stretchWeight;
    const float factor = ((stretch - 1) * w + 1) / ((rig.stretch - 1) * w + 1);
    if (node.name.empty() || !std::isfinite(factor)) {
        return t;
    }
    if (node.stretchAxis == Rig::kStretchAll) {
        for (auto& v : t) {
            v *= factor;
        }
    } else if (node.stretchAxis >= 0 && node.stretchAxis < 3) {
        t[static_cast<std::size_t>(node.stretchAxis)] *= factor;
    }
    return t;
}

std::vector<JointAdjust> jointAdjusts(const std::vector<scenegraph::DataValue>& dataLists) {
    using Type = scenegraph::DataValue::Type;
    const auto finite = [](const std::vector<float>& v) {
        return std::all_of(v.begin(), v.end(), [](float f) { return std::isfinite(f); });
    };
    std::vector<JointAdjust> out;
    for (const auto& list : dataLists) {
        if (list.name != "bone_adjust" || list.type != Type::List) {
            continue;
        }
        const auto* tag = list.find("bone_adjust_bone_tag");
        if (tag == nullptr || tag->type != Type::String || tag->text.empty()) {
            continue;
        }
        JointAdjust a;
        a.joint = tag->text;
        const auto* t = list.find("bone_adjust_translation");
        const auto* q = list.find("bone_adjust_rotation");
        if (t && t->type == Type::Vector && t->floats.size() == 3) {
            if (!finite(t->floats)) {
                continue;
            }
            a.translation = std::array<float, 3>{t->floats[0], t->floats[1], t->floats[2]};
        }
        if (q && q->type == Type::Rotation && q->floats.size() == 4) {
            if (!finite(q->floats)) {
                continue;
            }
            a.rotation = std::array<float, 4>{q->floats[0], q->floats[1], q->floats[2], q->floats[3]};
        }
        out.push_back(std::move(a));
    }
    return out;
}

void adjustJoints(Rig& rig, std::span<const JointAdjust> adjusts) {
    for (const auto& a : adjusts) {
        const auto it = std::find_if(rig.nodes.begin(), rig.nodes.end(), [&](const Rig::Node& n) { return n.name == a.joint; });
        if (it == rig.nodes.end()) {
            continue;
        }
        if (a.translation) {
            it->translation = *a.translation;
        }
        if (a.rotation) {
            it->rotation = *a.rotation;
        }
    }
}

void stretchTeenArms(Rig& rig) {
    for (const char* name : {"l_upperarm", "r_upperarm"}) {
        const int node = rig.find(name);
        if (node >= 0) {
            rig.nodes[static_cast<std::size_t>(node)].stretchAxis = 1;
        }
    }
}

Rig stretched(Rig rig, float stretch) {
    if (!(stretch > 0) || !std::isfinite(stretch)) {
        return rig;
    }
    for (auto& node : rig.nodes) {
        node.translation = stretchedPlace(rig, node, stretch);
    }
    rig.stretch = stretch;
    return rig;
}

std::array<float, 3> fittedPlace(const PlaceFit& fit, std::array<float, 3> t, float ms) {
    if (fit.axis < 0 || fit.axis > 2) {
        return t;
    }
    float factor = fit.factor;
    if (fit.weight != nullptr) {
        const float taken = std::clamp(fit.weight->sample(ms), 0.0f, 1.0f);
        factor = (factor - 1) * (1 - taken) + 1;
    }
    float& v = t[static_cast<std::size_t>(fit.axis)];
    v = fit.height * v > 0 ? v + (factor - 1) * fit.height : v * factor;
    return t;
}

PlaceFit placeFitAt(const RigAnimation& animation, std::size_t node) {
    return node < animation.placeFits.size() ? animation.placeFits[node] : PlaceFit{};
}

// The fit of a joint's translation channel when `clip` plays on `rig` (see PlaceFit).
PlaceFit placeFitOf(const Rig& rig, const Rig::Node& node, const anim::Animation& clip) {
    PlaceFit fit;
    if (node.stretchAxis < 0 || node.stretchAxis > 2) {
        return fit;
    }
    const float factor = rig.stretch / ((clip.bodyStretch - 1) * node.stretchWeight + 1);
    if (!std::isfinite(factor)) {
        return fit;
    }
    fit.axis = node.stretchAxis;
    fit.factor = factor;
    if (node.name == "root_trans") {
        fit.height = clip.rootHeight;
        for (const auto& channel : clip.channels) {
            if (channel.name == node.name && channel.kind() == anim::ChannelKind::Scalar && !channel.components.empty()) {
                fit.weight = &channel.components.front();
                break;
            }
        }
    }
    return fit;
}

std::string mirroredJointName(const std::string& name) {
    if (name.size() >= 2 && name[1] == '_' && (name[0] == 'l' || name[0] == 'r')) {
        return (name[0] == 'l' ? "r" : "l") + name.substr(1);
    }
    return name;
}

std::array<float, 4> mirroredTurn(std::array<float, 4> q, int axis) {
    if (axis < 0 || axis > 2) {
        return q;
    }
    for (int i = 0; i < 3; ++i) {
        if (i != axis) {
            q[static_cast<std::size_t>(i)] = -q[static_cast<std::size_t>(i)];
        }
    }
    return q;
}

std::array<float, 3> mirroredPlace(std::array<float, 3> t, int axis) {
    if (axis >= 0 && axis < 3) {
        t[static_cast<std::size_t>(axis)] = -t[static_cast<std::size_t>(axis)];
    }
    return t;
}

std::array<float, 4> turnedTurn(const std::array<float, 4>& q, int axis, float angle) {
    if (axis < 0 || axis > 2 || angle == 0) {
        return q;
    }
    std::array<float, 4> r{0, 0, 0, std::cos(angle * 0.5f)};
    r[static_cast<std::size_t>(axis)] = std::sin(angle * 0.5f);
    return turnProduct(r, q);
}

std::array<float, 3> turnedPlace(const std::array<float, 3>& t, int axis, float angle) {
    if (axis < 0 || axis > 2 || angle == 0) {
        return t;
    }
    const auto i = static_cast<std::size_t>((axis + 1) % 3), j = static_cast<std::size_t>((axis + 2) % 3);
    const float c = std::cos(angle), s = std::sin(angle);
    auto r = t;
    r[i] = c * t[i] - s * t[j];
    r[j] = s * t[i] + c * t[j];
    return r;
}

RigAnimation bindAnimation(const Rig& rig, std::shared_ptr<const anim::Animation> animation, bool mirrored) {
    RigAnimation r;
    r.rotation.assign(rig.nodes.size(), -1);
    r.translation.assign(rig.nodes.size(), -1);
    r.placeFits.assign(rig.nodes.size(), PlaceFit{});
    if (animation) {
        for (std::size_t c = 0; c < animation->channels.size(); ++c) {
            const auto& channel = animation->channels[c];
            if (channel.components.size() != 3) {
                continue;
            }
            const int node = rig.find(lower(mirrored ? mirroredJointName(channel.name) : channel.name));
            if (node < 0) {
                continue;
            }
            if (channel.kind() == anim::ChannelKind::Rotation) {
                r.rotation[static_cast<std::size_t>(node)] = static_cast<int>(c);
            } else if (channel.kind() == anim::ChannelKind::Translation) {
                r.translation[static_cast<std::size_t>(node)] = static_cast<int>(c);
                r.placeFits[static_cast<std::size_t>(node)] = placeFitOf(rig, rig.nodes[static_cast<std::size_t>(node)], *animation);
            }
        }
    }
    if (animation) {
        r.limbs = bindLimbs(rig, *animation, mirrored);
        for (const auto& chain : animation->chains) {
            int weight = -1;
            for (std::size_t c = 0; c < animation->channels.size() && chain.extra != 0; ++c) {
                const auto& channel = animation->channels[c];
                if (channel.kind() == anim::ChannelKind::Scalar && channel.components.size() == 1 && hash::crc32(lower(channel.name)) == chain.extra) {
                    weight = static_cast<int>(c);
                    break;
                }
            }
            r.chainWeights.push_back(weight);
        }
    }
    r.animation = std::move(animation);
    r.mirrored = mirrored;
    return r;
}

float channelShare(const RigAnimation& animation, int channel, float ms) {
    if (!animation.animation || channel < 0 || static_cast<std::size_t>(channel) >= animation.animation->channels.size()) {
        return 1;
    }
    const int chain = animation.animation->channels[static_cast<std::size_t>(channel)].chain();
    // A tag past the animation's chains names none, and the channel counts in full.
    if (chain < 0 || static_cast<std::size_t>(chain) >= animation.chainWeights.size()) {
        return 1;
    }
    const int weight = animation.chainWeights[static_cast<std::size_t>(chain)];
    return weight < 0 ? -1.0f : 1 - limbWeightAt(*animation.animation, weight, ms);
}

void poseRig(const Rig& rig, const RigAnimation* animation, float ms, std::vector<Transform>& world,
             const std::vector<HeldJoint>* held) {
    world.resize(rig.nodes.size());
    std::vector<Transform> local(rig.nodes.size());
    const auto* channels = animation && animation->animation ? &animation->animation->channels : nullptr;
    std::vector<float> turns(rig.nodes.size(), 0.0f);
    const bool holding = held != nullptr && held->size() == rig.nodes.size();
    for (std::size_t i = 0; i < rig.nodes.size(); ++i) {
        const auto& node = rig.nodes[i];
        auto t = holding ? (*held)[i].translation : node.translation;
        auto q = holding ? (*held)[i].rotation : node.rotation;
        if (channels) {
            if (const int c = animation->translation[i]; c >= 0 && channelShare(*animation, c, ms) >= 0) {
                const auto& k = (*channels)[static_cast<std::size_t>(c)].components;
                t = fittedPlace(placeFitAt(*animation, i), {k[0].sample(ms), k[1].sample(ms), k[2].sample(ms)}, ms);
                if (animation->mirrored) {
                    t = mirroredPlace(t, node.mirrorAxis);
                }
            }
            if (const int c = animation->rotation[i]; c >= 0) {
                if (const float share = channelShare(*animation, c, ms); share >= 0) {
                    q = animatedRotation(node, (*channels)[static_cast<std::size_t>(c)], ms);
                    if (animation->mirrored) {
                        q = mirroredTurn(q, node.mirrorAxis);
                    }
                    turns[i] = share;
                }
            }
        }
        local[i] = Transform::of(t, q);
    }
    placeRig(rig, local, world);
    if (channels && !animation->limbs.empty()) {
        // The clip alone on its layer: each limb joint weighed as the game weighs its layers.
        std::vector<float> shares(rig.nodes.size(), 1.0f);
        for (const auto& limb : animation->limbs) {
            if (isAdditive(*animation->animation, limb)) {
                continue;
            }
            for (std::size_t k = 0; k < limb.joints.size(); ++k) {
                const auto j = static_cast<std::size_t>(limb.joints[k]);
                const LimbFloor floor = limbFloor(*animation->animation, limb, k, ms);
                const LimbJointLayer layer{turns[j], floor.targets, floor.pole};
                float scale = 0;
                const float ik = weighLimbJoint(std::span(&layer, 1), std::span(&scale, 1));
                shares[j] = solvedShare(ik, scale * turns[j]);
            }
        }
        solveLimbs(rig, *animation->animation, animation->limbs, ms, local, world, &shares);
    }
}

bool isSkinned(const Primitive& p) {
    const std::size_t vertices = p.positions.size() / 3;
    return p.joints.size() == vertices * 4 && p.weights.size() == vertices * 4;
}

void posedPositions(const Primitive& p, const std::vector<Transform>& joints, std::vector<float>& out) {
    constexpr std::size_t kNoJoint = 255;
    const std::size_t vertices = p.positions.size() / 3;
    out.assign(p.positions.size(), 0.0f);
    if (!isSkinned(p)) {
        return;
    }
    for (std::size_t v = 0; v < vertices; ++v) {
        const std::array<float, 3> rest{p.positions[v * 3], p.positions[v * 3 + 1], p.positions[v * 3 + 2]};
        for (std::size_t k = 0; k < 4; ++k) {
            const std::size_t j = p.joints[v * 4 + k];
            const float w = p.weights[v * 4 + k];
            if (j == kNoJoint || j >= joints.size() || !(w > 0)) {
                continue;
            }
            auto moved = rest;
            joints[j].apply(moved.data(), true);
            for (std::size_t n = 0; n < 3; ++n) {
                out[v * 3 + n] += w * moved[n];
            }
        }
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
    return std::any_of(animation->limbs.begin(), animation->limbs.end(), [&](const LimbChain& l) {
        return std::find(l.joints.begin(), l.joints.end(), node) != l.joints.end();
    });
}

} // namespace

void skinMatrices(const Mesh& mesh, const std::vector<int>& nodes, const Rig& rig, const RigAnimation* animation,
                  const std::vector<Transform>& world, std::vector<Transform>& out) {
    const RigAnimation* const one[] = {animation};
    skinMatrices(mesh, nodes, rig, std::span<const RigAnimation* const>(one, animation ? 1 : 0), world, out);
}

void skinMatrices(const Mesh& mesh, const std::vector<int>& nodes, const Rig& rig, std::span<const RigAnimation* const> animations,
                  const std::vector<Transform>& world, std::vector<Transform>& out, bool ownRig) {
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
        const bool moved = ownRig || std::any_of(animations.begin(), animations.end(), [&](const RigAnimation* a) { return animated(a, n); });
        if (s.rest && parent >= 0 && static_cast<std::size_t>(parent) < world.size() && !moved) {
            out[i] = s.placement * world[static_cast<std::size_t>(parent)] * *s.rest * s.inverseBind;
        } else {
            out[i] = s.placement * world[static_cast<std::size_t>(n)] * s.inverseBind;
        }
    }
}

namespace {

// Where a bone (by its name's hash) of the actor showing a lot object is in the world, as last posed.
using BoneFinder = std::function<std::optional<Transform>(std::uint32_t object, std::uint32_t bone)>;

// Milliseconds into a clip `length` ms long, `elapsedMs` after it started at `speed` (1: as authored).
// A negative speed plays it from its end back to its start, as the game's clip track does.
float clipTime(double length, double elapsedMs, float speed, bool repeats) {
    if (!(length > 0)) {
        return 0.0f;
    }
    const double played = elapsedMs * std::abs(speed);
    const double at = repeats ? std::fmod(played, length) : std::min(played, length);
    return static_cast<float>(speed < 0 ? length - at : at);
}

// The actor's clips as they play at `seconds`.
void clipsAt(const Actor& actor, double seconds, std::vector<LayeredClip>& clips, std::vector<const RigAnimation*>& playing,
             const BoneFinder& boneOf, const std::vector<HeldJoint>* frozen = nullptr) {
    clips.clear();
    playing.clear();
    const auto faded = [&](const Actor::Clip& clip) {
        double weight = clip.weight;
        if (clip.fadeIn && clip.fadeInSeconds > 0) {
            weight *= std::clamp((seconds - clip.start) / clip.fadeInSeconds, 0.0, 1.0);
        }
        if (clip.fadeOut && clip.end > clip.start) {
            weight *= clip.fadeOutSeconds > 0 ? std::clamp((clip.end - seconds) / clip.fadeOutSeconds, 0.0, 1.0) : (seconds < clip.end ? 1.0 : 0.0);
        }
        return static_cast<float>(weight);
    };
    for (const auto& clip : actor.clips) {
        if (clip.frozen && frozen != nullptr && seconds >= clip.start) {
            LayeredClip& held = clips.emplace_back();
            held.weight = faded(clip);
            held.layer = clip.layer;
            held.start = clip.start;
            held.pose = frozen;
            if (clip.playedAt) {
                held.placeMove = actor.place.rigidInverse() * *clip.playedAt;
            }
            continue;
        }
        if (!clip.animation || !clip.animation->animation || seconds < clip.start) {
            continue;
        }
        const double length = clip.animation->animation->duration;
        float ms = clipTime(length, std::max(0.0, seconds - clip.start) * 1000.0, clip.speed, clip.repeats);
        if (clip.atMs) {
            ms = std::clamp(*clip.atMs, 0.0f, static_cast<float>(std::max(0.0, length)));
        }
        const float weight = faded(clip);
        // The clip plays at its own frame turn too (mirrored the other way).
        float turn = clip.turn;
        if (clip.animation->animation) {
            const float own = clip.animation->animation->frameTurn;
            turn += clip.animation->mirrored ? -own : own;
        }
        clips.push_back({clip.animation.get(), ms, weight, clip.layer, clip.start, turn, clip.bodyOnly,
                         clip.noRootMove, clip.objectPlace ? std::optional<Transform>(actor.place.rigidInverse() * *clip.objectPlace) : std::nullopt});
        if (clip.playedAt) {
            clips.back().placeMove = actor.place.rigidInverse() * *clip.playedAt;
        }
        if (clip.ikActor != 0) {
            clips.back().objectBone = [&actor, &boneOf, object = clip.ikActor](std::uint32_t bone) -> std::optional<Transform> {
                const auto at = boneOf(object, bone);
                return at ? std::optional<Transform>(actor.place.rigidInverse() * *at) : std::nullopt;
            };
        }
        playing.push_back(clip.animation.get());
    }
}

} // namespace

void poseActors(const Scene& scene, double seconds, ScenePose& out) {
    diag::Timer timer("pose actors");
    out.places.clear();
    std::size_t posed = 0;    // actors in out.nodes, their vectors are kept between calls
    std::size_t skinned = 0;  // meshes in out.joints, kept the same way
    // An entry of out.joints for a skinned mesh, reusing the storage of the last call's.
    const auto jointsFor = [&](std::size_t instance) -> std::vector<Transform>& {
        if (skinned == out.joints.size()) {
            out.joints.emplace_back();
        }
        out.joints[skinned].first = instance;
        return out.joints[skinned++].second;
    };
    // The working lists are kept between calls, as every actor is posed each frame.
    thread_local std::vector<Transform> world;
    thread_local std::vector<LayeredClip> clips;
    thread_local std::vector<const RigAnimation*> playing;
    // Each posed instance's morph weights as its clips give them now, from what they last were. An
    // instance no longer posed gives up its weights.
    std::unordered_map<std::size_t, std::vector<float>> morphs;
    std::unordered_map<std::size_t, std::array<float, 4>> eyes;
    const auto weighMorphs = [&](std::size_t instance, const Mesh& mesh) {
        if (std::any_of(mesh.primitives.begin(), mesh.primitives.end(), [](const Primitive& p) { return !p.eyeUvs.empty(); })) {
            auto& values = eyes.try_emplace(instance).first->second;
            if (const auto was = out.eyes.find(instance); was != out.eyes.end()) {
                values = was->second;
            }
            mixEyes(clips, values);
        }
        if (mesh.morphs.empty()) {
            return;
        }
        auto& weights = morphs[instance];
        if (const auto was = out.morphs.find(instance); was != out.morphs.end()) {
            weights = std::move(was->second);
        }
        mixMorphWeights(mesh, clips, weights, &out.morphBindings, scene.instances[instance].mesh);
    };
    // A Sim's bones as last posed: this call's pose for a Sim posed before the one asking, else the
    // last call's, each at the place it was posed at. A lot object's bones as this call posed them, as
    // objects are posed before Sims (a fridge's door handle), or as its clip placed them (a dining
    // chair's seat grabs, see PlacedObject::bones). One a Sim holds is where it last hung in the hand.
    // Each rig's node names are hashed once per call, on its first lookup.
    std::unordered_map<const Rig*, std::vector<std::uint32_t>> nameHashes;
    thread_local std::vector<std::pair<const Actor*, std::vector<Transform>>> objectNodes;
    std::size_t objectsPosed = 0;  // entries of objectNodes posed this call
    // Where this call placed each carried Sim (see Actor::carrier). Any other stands at its own place.
    std::unordered_map<const Actor*, Transform> carriedNow;
    const auto placeOf = [&](const Actor& actor) {
        const auto found = carriedNow.find(&actor);
        return found != carriedNow.end() ? found->second : actor.place;
    };
    std::unordered_map<std::uint32_t, Transform> drawnAt;
    thread_local std::vector<const Actor*> posedNow;
    thread_local std::vector<std::size_t> inHand;  // sorted
    posedNow.clear();
    inHand.clear();
    // With what hangs in a lot object's slots, which is placed from that object's bones below.
    for (const auto* list : {&scene.actors, &scene.objectActors}) {
        for (const auto& actor : *list) {
            for (const auto& h : actor.held) {
                inHand.push_back(h.instance);
            }
        }
    }
    std::sort(inHand.begin(), inHand.end());
    const auto isInHand = [&](std::size_t instance) { return std::binary_search(inHand.begin(), inHand.end(), instance); };
    const auto lastDrawn = [&](const Actor& actor) -> std::optional<Transform> {
        const auto found = out.drawnAt.find(actor.object);
        return found != out.drawnAt.end() ? std::optional<Transform>(found->second) : std::nullopt;
    };
    const auto boneIn = [&](const Actor& actor, const Transform& place, const std::vector<Transform>& nodes, std::uint32_t bone) -> std::optional<Transform> {
        auto [hashes, first] = nameHashes.try_emplace(actor.rig.get());
        if (first) {
            for (const auto& node : actor.rig->nodes) {
                // 0 for a node with no name, which no bone matches.
                hashes->second.push_back(node.name.empty() ? 0 : hash::crc32(node.name));
            }
        }
        for (std::size_t i = 0; i < hashes->second.size() && i < nodes.size(); ++i) {
            if (bone != 0 && hashes->second[i] == bone) {
                return place * nodes[i];
            }
        }
        return std::nullopt;
    };
    const BoneFinder boneOf = [&](std::uint32_t object, std::uint32_t bone) -> std::optional<Transform> {
        const auto actor = std::find_if(scene.actors.begin(), scene.actors.end(), [&](const Actor& a) { return a.object == object; });
        if (actor == scene.actors.end() || !actor->rig) {
            const auto objectsEnd = objectNodes.begin() + static_cast<std::ptrdiff_t>(objectsPosed);
            const auto posed = std::find_if(objectNodes.begin(), objectsEnd, [&](const auto& n) { return n.first->object == object; });
            if (posed != objectsEnd) {
                const Actor& a = *posed->first;
                const bool held = std::any_of(a.skinned.begin(), a.skinned.end(), [&](const Actor::Skinned& s) { return isInHand(s.instance); });
                const auto hung = held ? lastDrawn(a) : std::nullopt;
                return boneIn(a, hung.value_or(a.place), posed->second, bone);
            }
            const auto placed = std::find_if(scene.placedObjects.begin(), scene.placedObjects.end(),
                                             [&](const PlacedObject& p) { return p.object == object && !p.bones.empty(); });
            if (placed == scene.placedObjects.end() || bone == 0) {
                return std::nullopt;
            }
            const auto found = std::find_if(placed->bones.begin(), placed->bones.end(), [&](const auto& b) { return b.first == bone; });
            return found != placed->bones.end() ? std::optional<Transform>(found->second) : std::nullopt;
        }
        const auto index = static_cast<std::size_t>(actor - scene.actors.begin());
        const auto nodes = std::find_if(out.nodes.begin(), out.nodes.end(), [&](const auto& n) { return n.first == index; });
        if (nodes == out.nodes.end()) {
            return std::nullopt;
        }
        const bool posedHere = std::find(posedNow.begin(), posedNow.end(), &*actor) != posedNow.end();
        const auto drawn = posedHere ? std::nullopt : lastDrawn(*actor);
        return boneIn(*actor, drawn.value_or(placeOf(*actor)), nodes->second, bone);
    };
    // Only the actors still in the scene keep what they hold.
    std::unordered_map<std::uint32_t, std::vector<HeldJoint>> held;
    std::unordered_map<std::uint32_t, std::pair<double, std::vector<HeldJoint>>> frozen;
    std::unordered_map<std::uint32_t, Transform> carried;
    // A pose kept from another rig (a Sim that aged onto a new skeleton) doesn't fit this one.
    std::unordered_map<std::uint32_t, const Rig*> rigs;
    for (const auto* list : {&scene.objectActors, &scene.actors}) {
        for (const auto& actor : *list) {
            if (const auto was = out.heldRig.find(actor.object); was != out.heldRig.end() && was->second != actor.rig.get()) {
                out.held.erase(actor.object);
                out.frozen.erase(actor.object);
            }
            rigs[actor.object] = actor.rig.get();
        }
    }
    out.heldRig = std::move(rigs);
    // Lot objects first: a Sim's hand places an object it holds over where the object would stand.
    for (const auto& actor : scene.objectActors) {
        if (!actor.rig) {
            continue;
        }
        clipsAt(actor, seconds, clips, playing, boneOf);
        auto& last = held[actor.object];
        if (const auto was = out.held.find(actor.object); was != out.held.end()) {
            last = std::move(was->second);
        }
        poseRigLayered(*actor.rig, clips, world, &last);
        if (objectsPosed == objectNodes.size()) {
            objectNodes.emplace_back();
        }
        objectNodes[objectsPosed].first = &actor;
        objectNodes[objectsPosed++].second.assign(world.begin(), world.end());
        for (const auto& s : actor.skinned) {
            const int mesh = s.instance < scene.instances.size() ? scene.instances[s.instance].mesh : -1;
            if (mesh < 0 || static_cast<std::size_t>(mesh) >= scene.meshes.size()) {
                continue;
            }
            auto& joints = jointsFor(s.instance);
            skinMatrices(scene.meshes[static_cast<std::size_t>(mesh)], s.nodes, *actor.rig, playing, world, joints, true);
            weighMorphs(s.instance, scene.meshes[static_cast<std::size_t>(mesh)]);
            // One a Sim holds is placed at the Sim's hand below.
            if (!isInHand(s.instance)) {
                out.places.emplace_back(s.instance, actor.place);
            }
        }
    }
    // A carried Sim is posed after its carrier and hangs at the bone of the carrier's slot it is in,
    // as that bone is posed now. The carrier is a Sim or a lot object (a car's seat). A carrier missing
    // from the scene leaves it at its own place.
    const auto carrierOf = [&](const Actor& actor) -> const Actor* {
        if (actor.carrier == 0) {
            return nullptr;
        }
        const auto found = std::find_if(scene.actors.begin(), scene.actors.end(), [&](const Actor& a) { return a.object == actor.carrier; });
        if (found != scene.actors.end()) {
            return &*found != &actor ? &*found : nullptr;
        }
        const auto object = std::find_if(scene.objectActors.begin(), scene.objectActors.end(),
                                         [&](const Actor& a) { return a.object == actor.carrier; });
        return object != scene.objectActors.end() ? &*object : nullptr;
    };
    // The carrier's bones as this call posed them, none when it isn't posed yet.
    const auto posedBones = [&](const Actor& carrier) -> const std::vector<Transform>* {
        const auto objects = objectNodes.begin() + static_cast<std::ptrdiff_t>(objectsPosed);
        if (const auto o = std::find_if(objectNodes.begin(), objects, [&](const auto& n) { return n.first == &carrier; }); o != objects) {
            return &o->second;
        }
        const auto carrierIndex = static_cast<std::size_t>(&carrier - scene.actors.data());
        const auto sims = out.nodes.begin() + static_cast<std::ptrdiff_t>(posed);
        const auto s = std::find_if(out.nodes.begin(), sims, [&](const auto& n) { return n.first == carrierIndex; });
        return s != sims ? &s->second : nullptr;
    };
    // Carries nest only so deep, so a loop of carriers in a broken save still ends.
    constexpr int kMaxCarryDepth = 4;
    thread_local std::vector<std::pair<int, std::size_t>> order;  // carry depth, actor
    order.clear();
    for (std::size_t i = 0; i < scene.actors.size(); ++i) {
        int depth = 0;
        for (const Actor* c = carrierOf(scene.actors[i]); c != nullptr && depth < kMaxCarryDepth; c = carrierOf(*c)) {
            ++depth;
        }
        order.emplace_back(depth, i);
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [depth, actorIndex] : order) {
        const Actor& actor = scene.actors[actorIndex];
        if (!actor.rig) {
            continue;
        }
        if (const Actor* carrier = carrierOf(actor); carrier != nullptr && actor.carrierSlot < carrier->slotNodes.size()) {
            const int node = carrier->slotNodes[actor.carrierSlot];
            const auto* nodes = posedBones(*carrier);
            if (node >= 0 && nodes != nullptr && static_cast<std::size_t>(node) < nodes->size()) {
                carriedNow[&actor] = placeOf(*carrier) * (*nodes)[static_cast<std::size_t>(node)];
                carried[actor.object] = carriedNow[&actor];
            }
        }
        const Transform place = placeOf(actor);
        if (!actor.clips.empty()) {
            // A frozen clip holds the pose the actor was last posed in before it started.
            const std::vector<HeldJoint>* still = nullptr;
            const auto freeze = std::find_if(actor.clips.begin(), actor.clips.end(), [](const Actor::Clip& c) { return c.frozen; });
            if (actor.object != 0 && freeze != actor.clips.end() && seconds >= freeze->start) {
                auto& kept = frozen[actor.object];
                if (const auto was = out.frozen.find(actor.object); was != out.frozen.end() && was->second.first == freeze->start) {
                    kept = std::move(was->second);
                } else if (const auto last = out.held.find(actor.object); last != out.held.end()) {
                    kept = {freeze->start, last->second};
                }
                still = kept.second.empty() ? nullptr : &kept.second;
            }
            clipsAt(actor, seconds, clips, playing, boneOf, still);
            if (actor.object != 0) {
                auto& joints = held[actor.object];
                if (const auto was = out.held.find(actor.object); was != out.held.end()) {
                    joints = std::move(was->second);
                }
                poseRigLayered(*actor.rig, clips, world, &joints, actor.look ? &actor.look : nullptr);
            } else {
                poseRigLayered(*actor.rig, clips, world);
            }
        } else {
            // No clip weighs its morphs or eyes. The list still holds the last actor's clips, which
            // may belong to a lot no longer shown.
            clips.clear();
            playing.assign(1, actor.animation.get());
        }
        float ms = 0;
        if (actor.animation && actor.animation->animation && actor.animation->animation->duration > 0) {
            ms = clipTime(actor.animation->animation->duration, std::max(0.0, seconds - actor.start) * 1000.0, actor.speed, actor.loop);
        }
        if (actor.clips.empty()) {
            poseRig(*actor.rig, actor.animation.get(), ms, world);
        }
        for (const auto& s : actor.skinned) {
            if (s.instance >= scene.instances.size()) {
                continue;
            }
            const int mesh = scene.instances[s.instance].mesh;
            if (mesh < 0 || static_cast<std::size_t>(mesh) >= scene.meshes.size()) {
                continue;
            }
            auto& joints = jointsFor(s.instance);
            skinMatrices(scene.meshes[static_cast<std::size_t>(mesh)], s.nodes, *actor.rig, playing, world, joints);
            weighMorphs(s.instance, scene.meshes[static_cast<std::size_t>(mesh)]);
            // The body goes where the actor stands now (it walks).
            out.places.emplace_back(s.instance, place);
        }
        if (posed == out.nodes.size()) {
            out.nodes.emplace_back();
        }
        out.nodes[posed].first = static_cast<std::size_t>(&actor - scene.actors.data());
        out.nodes[posed++].second.assign(world.begin(), world.end());
        posedNow.push_back(&actor);
        drawnAt[actor.object] = place;
        for (const auto& h : actor.held) {
            const bool known = h.node >= 0 && static_cast<std::size_t>(h.node) < world.size();
            const Transform at = (known ? place * world[static_cast<std::size_t>(h.node)] : place) * h.offset;
            out.places.emplace_back(h.instance, at);
            for (const auto& o : scene.objectActors) {
                if (std::any_of(o.skinned.begin(), o.skinned.end(), [&](const Actor::Skinned& s) { return s.instance == h.instance; })) {
                    drawnAt[o.object] = at;
                }
            }
        }
    }
    // What hangs in a lot object's slots goes to the slot's bone as the object's clips posed it, at
    // the place the object is drawn now: in a Sim's hand, or in a slot of another such object.
    // Objects nest a few deep at most, and the cap stops a broken lot's containers that hold each other.
    constexpr int kMaxNesting = 8;
    thread_local std::vector<std::pair<std::size_t, Transform>> hung;
    hung.clear();
    for (int depth = 0; depth < kMaxNesting; ++depth) {
        bool changed = false;
        const auto objectsEnd = objectNodes.begin() + static_cast<std::ptrdiff_t>(objectsPosed);
        for (auto o = objectNodes.begin(); o != objectsEnd; ++o) {
            const Actor& actor = *o->first;
            if (actor.held.empty()) {
                continue;
            }
            const auto drawn = drawnAt.find(actor.object);
            const Transform& base = drawn != drawnAt.end() ? drawn->second : actor.place;
            for (const auto& h : actor.held) {
                if (h.node < 0 || static_cast<std::size_t>(h.node) >= o->second.size()) {
                    continue;
                }
                const Transform at = base * o->second[static_cast<std::size_t>(h.node)] * h.offset;
                auto was = std::find_if(hung.begin(), hung.end(), [&](const auto& p) { return p.first == h.instance; });
                if (was == hung.end()) {
                    hung.emplace_back(h.instance, at);
                } else if (was->second.m != at.m) {
                    was->second = at;
                } else {
                    continue;
                }
                changed = true;
                for (const auto& inner : scene.objectActors) {
                    if (std::any_of(inner.skinned.begin(), inner.skinned.end(), [&](const Actor::Skinned& s) { return s.instance == h.instance; })) {
                        drawnAt[inner.object] = at;
                    }
                }
            }
        }
        if (!changed) {
            break;
        }
    }
    out.places.insert(out.places.end(), hung.begin(), hung.end());
    out.nodes.resize(posed);
    out.joints.resize(skinned);
    out.held = std::move(held);
    out.frozen = std::move(frozen);
    out.morphs = std::move(morphs);
    out.eyes = std::move(eyes);
    out.carried = std::move(carried);
    out.drawnAt = std::move(drawnAt);
}

} // namespace ts2::scene
