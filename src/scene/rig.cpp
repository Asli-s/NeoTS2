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

Rig stretched(Rig rig, float stretch) {
    for (auto& node : rig.nodes) {
        if (!node.name.empty()) {
            for (auto& t : node.translation) {
                t *= stretch;
            }
        }
    }
    rig.stretch *= stretch;
    return rig;
}

void fitToStretch(anim::Animation& clip, const Rig& rig) {
    const float factor = rig.stretch / clip.bodyStretch;
    if (!std::isfinite(factor) || std::abs(factor - 1) < 1e-4f || !(rig.stretch > 0)) {
        return;
    }
    for (auto& channel : clip.channels) {
        if (channel.kind() != anim::ChannelKind::Translation) {
            continue;
        }
        // A joint's move keeps its size: only the joint's rest place changes with the body.
        const int node = rig.find(lower(channel.name));
        if (node >= 0 && !channel.additive() && channel.components.size() == 3) {
            const auto& rest = rig.nodes[static_cast<std::size_t>(node)].translation;
            for (std::size_t k = 0; k < 3; ++k) {
                const float shift = rest[k] / rig.stretch * (rig.stretch - clip.bodyStretch);
                for (auto& key : channel.components[k].keys) {
                    key.value += shift;
                }
            }
            continue;
        }
        for (auto& component : channel.components) {
            for (auto& key : component.keys) {
                key.value *= factor;
                key.in *= factor;
                key.out *= factor;
            }
        }
    }
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
    if (animation && std::abs(rig.stretch / animation->bodyStretch - 1) >= 1e-4f) {
        auto fitted = std::make_shared<anim::Animation>(*animation);
        fitToStretch(*fitted, rig);
        animation = std::move(fitted);
    }
    RigAnimation r;
    r.rotation.assign(rig.nodes.size(), -1);
    r.translation.assign(rig.nodes.size(), -1);
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

void poseRig(const Rig& rig, const RigAnimation* animation, float ms, std::vector<Transform>& world) {
    world.resize(rig.nodes.size());
    std::vector<Transform> local(rig.nodes.size());
    const auto* channels = animation && animation->animation ? &animation->animation->channels : nullptr;
    std::vector<float> turns(rig.nodes.size(), 0.0f);
    for (std::size_t i = 0; i < rig.nodes.size(); ++i) {
        const auto& node = rig.nodes[i];
        auto t = node.translation;
        auto q = node.rotation;
        if (channels) {
            if (const int c = animation->translation[i]; c >= 0 && channelShare(*animation, c, ms) >= 0) {
                const auto& k = (*channels)[static_cast<std::size_t>(c)].components;
                t = {k[0].sample(ms), k[1].sample(ms), k[2].sample(ms)};
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

namespace {

// When the Sim's placement turns by `turn` radians while no clip moves its root joints, the root
// joints it holds turn back by as much, so the body stays where it was drawn. The game turns its node
// in eighth turns and its root joint the other way in the same frame.
void turnHeldRoot(const Rig& rig, std::span<const LayeredClip> clips, std::vector<HeldJoint>& joints, float turn) {
    if (std::abs(turn) < 1e-5f || joints.size() != rig.nodes.size()) {
        return;
    }
    for (const bool translation : {true, false}) {
        const int j = rig.find(translation ? "root_trans" : "root_rot");
        if (j < 0) {
            continue;
        }
        const auto at = static_cast<std::size_t>(j);
        const bool driven = std::any_of(clips.begin(), clips.end(), [&](const LayeredClip& c) {
            if (!c.animation || c.weight <= 0 || c.bodyOnly || c.layer < 0) {
                return false;
            }
            const auto& by = translation ? c.animation->translation : c.animation->rotation;
            return at < by.size() && by[at] >= 0;
        });
        if (driven) {
            continue;
        }
        auto& h = joints[at];
        const int axis = rig.nodes[at].turnAxis;
        // root_rot hangs under root_trans: the place turns with the one, the facing with the other.
        if (translation) {
            h.translation = turnedPlace(h.translation, axis, turn);
        } else {
            h.rotation = turnedTurn(h.rotation, axis, turn);
        }
    }
}

} // namespace

void poseActors(const Scene& scene, double seconds, ScenePose& out) {
    diag::Timer timer("pose actors");
    out.joints.clear();
    out.places.clear();
    std::size_t posed = 0;  // actors in out.nodes, their vectors are kept between calls
    std::vector<Transform> world;
    std::vector<LayeredClip> clips;
    std::vector<const RigAnimation*> playing;
    // A Sim's bones as last posed: this call's pose for a Sim posed before the one asking, else the
    // last call's, each at the place it was posed at. A lot object's bones as this call posed them, as
    // objects are posed before Sims (a fridge's door handle), or as its clip placed them (a dining
    // chair's seat grabs, see PlacedObject::bones). One a Sim holds is where it last hung in the hand.
    // Each rig's node names are hashed once per call, on its first lookup.
    std::unordered_map<const Rig*, std::vector<std::uint32_t>> nameHashes;
    std::vector<std::pair<const Actor*, std::vector<Transform>>> objectNodes;
    // Where this call placed each carried Sim (see Actor::carrier). Any other stands at its own place.
    std::unordered_map<const Actor*, Transform> carriedNow;
    const auto placeOf = [&](const Actor& actor) {
        const auto found = carriedNow.find(&actor);
        return found != carriedNow.end() ? found->second : actor.place;
    };
    std::unordered_map<std::uint32_t, Transform> drawnAt;
    std::unordered_set<const Actor*> posedNow;
    std::unordered_set<std::size_t> inHand;
    for (const auto& actor : scene.actors) {
        for (const auto& h : actor.held) {
            inHand.insert(h.instance);
        }
    }
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
            const auto posed = std::find_if(objectNodes.begin(), objectNodes.end(), [&](const auto& n) { return n.first->object == object; });
            if (posed != objectNodes.end()) {
                const Actor& a = *posed->first;
                const bool held = std::any_of(a.skinned.begin(), a.skinned.end(), [&](const Actor::Skinned& s) { return inHand.contains(s.instance); });
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
        const auto drawn = posedNow.contains(&*actor) ? std::nullopt : lastDrawn(*actor);
        return boneIn(*actor, drawn.value_or(placeOf(*actor)), nodes->second, bone);
    };
    // Only the actors still in the scene keep what they hold.
    std::unordered_map<std::uint32_t, std::vector<HeldJoint>> held;
    std::unordered_map<std::uint32_t, std::pair<double, std::vector<HeldJoint>>> frozen;
    std::unordered_map<std::uint32_t, float> headings;
    std::unordered_map<std::uint32_t, Transform> carried;
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
        objectNodes.emplace_back(&actor, world);
        for (const auto& s : actor.skinned) {
            const int mesh = s.instance < scene.instances.size() ? scene.instances[s.instance].mesh : -1;
            if (mesh < 0 || static_cast<std::size_t>(mesh) >= scene.meshes.size()) {
                continue;
            }
            auto& joints = out.joints.emplace_back(s.instance, std::vector<Transform>{}).second;
            skinMatrices(scene.meshes[static_cast<std::size_t>(mesh)], s.nodes, *actor.rig, playing, world, joints, true);
            // One a Sim holds is placed at the Sim's hand below.
            if (!inHand.contains(s.instance)) {
                out.places.emplace_back(s.instance, actor.place);
            }
        }
    }
    // A carried Sim is posed after its carrier and hangs at the bone of the carrier's slot it is in,
    // as that bone is posed now. A carrier missing from the scene leaves it at its own place.
    const auto carrierOf = [&](const Actor& actor) -> const Actor* {
        if (actor.carrier == 0) {
            return nullptr;
        }
        const auto found = std::find_if(scene.actors.begin(), scene.actors.end(), [&](const Actor& a) { return a.object == actor.carrier; });
        return found != scene.actors.end() && &*found != &actor ? &*found : nullptr;
    };
    // Carries nest only so deep, so a loop of carriers in a broken save still ends.
    constexpr int kMaxCarryDepth = 4;
    std::vector<std::pair<int, std::size_t>> order;  // carry depth, actor
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
            const auto carrierIndex = static_cast<std::size_t>(carrier - scene.actors.data());
            const int node = carrier->slotNodes[actor.carrierSlot];
            const auto nodes = std::find_if(out.nodes.begin(), out.nodes.begin() + static_cast<std::ptrdiff_t>(posed),
                                            [&](const auto& n) { return n.first == carrierIndex; });
            if (node >= 0 && nodes != out.nodes.begin() + static_cast<std::ptrdiff_t>(posed) &&
                static_cast<std::size_t>(node) < nodes->second.size()) {
                carriedNow[&actor] = placeOf(*carrier) * nodes->second[static_cast<std::size_t>(node)];
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
                const float heading = std::atan2(place.m[4], place.m[0]);
                headings[actor.object] = heading;
                if (const auto was = out.heldHeading.find(actor.object); was != out.heldHeading.end()) {
                    turnHeldRoot(*actor.rig, clips, joints, was->second - heading);
                }
                poseRigLayered(*actor.rig, clips, world, &joints, actor.look ? &actor.look : nullptr);
            } else {
                poseRigLayered(*actor.rig, clips, world);
            }
        } else {
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
            auto& joints = out.joints.emplace_back(s.instance, std::vector<Transform>{}).second;
            skinMatrices(scene.meshes[static_cast<std::size_t>(mesh)], s.nodes, *actor.rig, playing, world, joints);
            // The body goes where the actor stands now (it walks).
            out.places.emplace_back(s.instance, place);
        }
        if (posed == out.nodes.size()) {
            out.nodes.emplace_back();
        }
        out.nodes[posed].first = static_cast<std::size_t>(&actor - scene.actors.data());
        out.nodes[posed++].second.assign(world.begin(), world.end());
        posedNow.insert(&actor);
        drawnAt[actor.object] = place;
        for (const auto& h : actor.held) {
            const bool known = h.node >= 0 && static_cast<std::size_t>(h.node) < world.size();
            const Transform at = known ? place * world[static_cast<std::size_t>(h.node)] : place;
            out.places.emplace_back(h.instance, at);
            for (const auto& o : scene.objectActors) {
                if (std::any_of(o.skinned.begin(), o.skinned.end(), [&](const Actor::Skinned& s) { return s.instance == h.instance; })) {
                    drawnAt[o.object] = at;
                }
            }
        }
    }
    out.nodes.resize(posed);
    out.held = std::move(held);
    out.heldHeading = std::move(headings);
    out.frozen = std::move(frozen);
    out.carried = std::move(carried);
    out.drawnAt = std::move(drawnAt);
}

} // namespace ts2::scene
