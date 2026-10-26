#include "rig_layers.hpp"

#include "scene/limb_ik.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace ts2::scene {

namespace {

// A joint's local place as kept between frames: its translation and its rotation as a quaternion.
HeldJoint heldOf(const Transform& t) { return {{t.m[3], t.m[7], t.m[11]}, turnOf(t)}; }

// `joint`, or the node's rest pose when any part of it isn't finite. A held joint carries into
// the next frame, so one broken limb solve or channel value would otherwise stay for good.
HeldJoint finiteOr(const HeldJoint& joint, const Rig::Node& node) {
    const auto finite = [](float v) { return std::isfinite(v); };
    if (std::all_of(joint.translation.begin(), joint.translation.end(), finite) &&
        std::all_of(joint.rotation.begin(), joint.rotation.end(), finite)) {
        return joint;
    }
    return {node.translation, node.rotation};
}

// Contributions this light are left out, as the game does.
constexpr float kLeastWeight = 0.00075f;

struct Contribution {
    int layer = 0;
    double start = 0;
    float weight = 0;
    float steady = 0;  // its weight under a newer clip on its layer
    std::array<float, 4> value{};
};

float steadyOf(const LayeredClip& clip) {
    return clip.steady < 0 ? clip.weight : clip.steady;
}

// Higher layers first, and the newest clip first within a layer.
bool coversEarlier(int layer, double start, int otherLayer, double otherStart) {
    return layer != otherLayer ? layer > otherLayer : start > otherStart;
}

// The weighted average of the contributions, from the top layer and the newest clip down, each
// getting what those above it leave. False when none counts.
template <std::size_t N>
bool mix(std::vector<Contribution>& parts, std::array<float, N>& out) {
    std::sort(parts.begin(), parts.end(),
              [](const Contribution& a, const Contribution& b) { return coversEarlier(a.layer, a.start, b.layer, b.start); });
    std::array<float, N> sum{};
    float total = 0;
    float left = 1;
    for (std::size_t i = 0; i < parts.size() && left > 0;) {
        const int layer = parts[i].layer;
        float layerWeight = 0;
        const bool covered = i > 0 && parts[i - 1].layer == layer;
        // Overlay layers take one clip at a time. Layer 0 takes all its clips together.
        for (; i < parts.size() && parts[i].layer == layer; ++i) {
            const float own = layer > 0 && covered ? parts[i].steady : parts[i].weight;
            const float w = left * own;
            layerWeight += own;
            if (w > kLeastWeight) {
                // A rotation and its negation are the same turn: take each the short way from the
                // first counted, as the game's slerping mixer does.
                float sign = 1;
                if constexpr (N == 4) {
                    float d = 0;
                    for (std::size_t k = 0; k < N; ++k) {
                        d += sum[k] * parts[i].value[k];
                    }
                    sign = d < 0 ? -1.0f : 1.0f;
                }
                for (std::size_t k = 0; k < N; ++k) {
                    sum[k] += sign * w * parts[i].value[k];
                }
                total += w;
            }
            if (layer > 0) {
                ++i;
                break;
            }
        }
        left *= 1 - std::min(layerWeight, 1.0f);
    }
    if (total <= 0) {
        return false;
    }
    for (std::size_t k = 0; k < N; ++k) {
        out[k] = sum[k] / total;
    }
    return true;
}

// Additive translations: each weighted value added to the mixed one.
void addOffsets(const std::vector<Contribution>& parts, std::array<float, 3>& out) {
    for (const auto& p : parts) {
        if (p.weight > kLeastWeight) {
            for (std::size_t k = 0; k < 3; ++k) {
                out[k] += p.weight * p.value[k];
            }
        }
    }
}

// Additive rotations: each partly by its weight, chained in order, and the chain applied after the
// mixed rotation, in the joint's own frame.
void addTurns(const std::vector<Contribution>& parts, std::array<float, 4>& out) {
    std::array<float, 4> chain{0, 0, 0, 1};
    bool any = false;
    for (const auto& p : parts) {
        if (p.weight <= kLeastWeight) {
            continue;
        }
        chain = turnProduct(chain, partialTurn(p.value, p.weight));
        any = true;
    }
    if (any) {
        out = turnProduct(out, chain);
    }
}

} // namespace

void poseRigLayered(const Rig& rig, std::span<const LayeredClip> clips, std::vector<Transform>& world, std::vector<HeldJoint>* held) {
    world.resize(rig.nodes.size());
    const bool holding = held != nullptr && held->size() == rig.nodes.size();
    std::vector<Transform> local(rig.nodes.size());
    std::vector<Contribution> rotations, translations, addedRotations, addedTranslations;
    const int rootTrans = rig.find("root_trans"), rootRot = rig.find("root_rot");
    for (std::size_t i = 0; i < rig.nodes.size(); ++i) {
        const bool root = static_cast<int>(i) == rootTrans || static_cast<int>(i) == rootRot;
        rotations.clear();
        translations.clear();
        addedRotations.clear();
        addedTranslations.clear();
        for (const auto& clip : clips) {
            if (clip.animation == nullptr || !clip.animation->animation || clip.weight <= 0 || (holding && clip.layer < 0) ||
                (root && clip.bodyOnly)) {
                continue;
            }
            const auto& channels = clip.animation->animation->channels;
            if (i < clip.animation->translation.size() && !(clip.noRootMove && static_cast<int>(i) == rootTrans)) {
                if (const int c = clip.animation->translation[i]; c >= 0) {
                    const auto& channel = channels[static_cast<std::size_t>(c)];
                    const auto& k = channel.components;
                    std::array<float, 3> t{k[0].sample(clip.ms), k[1].sample(clip.ms), k[2].sample(clip.ms)};
                    if (clip.animation->mirrored) {
                        t = mirroredPlace(t, rig.nodes[i].mirrorAxis);
                    }
                    t = turnedPlace(t, rig.nodes[i].turnAxis, clip.turn);
                    (channel.additive() ? addedTranslations : translations)
                        .push_back({clip.layer, clip.start, clip.weight, steadyOf(clip), {t[0], t[1], t[2], 0}});
                }
            }
            if (i < clip.animation->rotation.size()) {
                if (const int c = clip.animation->rotation[i]; c >= 0) {
                    const auto& channel = channels[static_cast<std::size_t>(c)];
                    // An added turn is the channel's own value: the joint's animation turn and the
                    // turn to the clip's heading only go into averaged turns.
                    if (channel.additive()) {
                        auto q = channelTurn(channel, clip.ms);
                        if (clip.animation->mirrored) {
                            q = mirroredTurn(q, rig.nodes[i].mirrorAxis);
                        }
                        addedRotations.push_back({clip.layer, clip.start, clip.weight, steadyOf(clip), q});
                        continue;
                    }
                    auto q = animatedRotation(rig.nodes[i], channel, clip.ms);
                    if (clip.animation->mirrored) {
                        q = mirroredTurn(q, rig.nodes[i].mirrorAxis);
                    }
                    q = turnedTurn(q, rig.nodes[i].turnAxis, clip.turn);
                    rotations.push_back({clip.layer, clip.start, clip.weight, steadyOf(clip), q});
                }
            }
        }
        const auto& node = rig.nodes[i];
        std::array<float, 3> t = holding ? (*held)[i].translation : node.translation;
        std::array<float, 4> q = holding ? (*held)[i].rotation : node.rotation;
        // Additive channels only count where some other clip moves the joint.
        if (mix(translations, t)) {
            addOffsets(addedTranslations, t);
        }
        if (mix(rotations, q)) {
            addTurns(addedRotations, q);
            const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            q = length > 1e-6f ? std::array<float, 4>{q[0] / length, q[1] / length, q[2] / length, q[3] / length} : node.rotation;
        }
        local[i] = Transform::of(t, q);
        if (held != nullptr) {
            held->resize(rig.nodes.size());
            (*held)[i] = finiteOr({t, q}, node);
        }
    }
    placeRig(rig, local, world);
    // For a clip whose root move is left out: the move from where the clip has its root to where the
    // root is drawn. With bodyOnly that is its turn too.
    const auto rootMove = [&](const LayeredClip& clip) -> Transform {
        const int last = clip.bodyOnly ? rootRot : rootTrans;
        if (!clip.noRootMove || rootTrans < 0 || last < 0 || rig.nodes[static_cast<std::size_t>(rootTrans)].parent < 0) {
            return {};
        }
        const auto& moves = clip.animation->translation;
        if (!clip.bodyOnly && (static_cast<std::size_t>(rootTrans) >= moves.size() || moves[static_cast<std::size_t>(rootTrans)] < 0)) {
            return {};
        }
        const auto& channels = clip.animation->animation->channels;
        // Each root joint as the clip alone has it, up from the skeleton.
        const auto own = [&](int i) {
            const auto& node = rig.nodes[static_cast<std::size_t>(i)];
            std::array<float, 3> t = node.translation;
            std::array<float, 4> q = node.rotation;
            // The clip's own (not additive) channel of the joint in `list`, -1 for none.
            const auto channelOf = [&](const std::vector<int>& list) {
                const auto at = static_cast<std::size_t>(i);
                const int c = at < list.size() ? list[at] : -1;
                return c >= 0 && !channels[static_cast<std::size_t>(c)].additive() ? c : -1;
            };
            if (const int c = channelOf(clip.animation->translation); c >= 0) {
                const auto& k = channels[static_cast<std::size_t>(c)].components;
                t = {k[0].sample(clip.ms), k[1].sample(clip.ms), k[2].sample(clip.ms)};
                if (clip.animation->mirrored) {
                    t = mirroredPlace(t, node.mirrorAxis);
                }
                t = turnedPlace(t, node.turnAxis, clip.turn);
            }
            if (const int c = channelOf(clip.animation->rotation); c >= 0) {
                q = animatedRotation(node, channels[static_cast<std::size_t>(c)], clip.ms);
                if (clip.animation->mirrored) {
                    q = mirroredTurn(q, node.mirrorAxis);
                }
                q = turnedTurn(q, node.turnAxis, clip.turn);
            }
            return Transform::of(t, q);
        };
        // Walked up to the skeleton, and in a broken rig no more steps than the rig has nodes.
        const int skeleton = rig.nodes[static_cast<std::size_t>(rootTrans)].parent;
        Transform below;
        std::size_t steps = 0;
        for (int i = last; i >= 0 && i != skeleton && steps < rig.nodes.size(); i = rig.nodes[static_cast<std::size_t>(i)].parent, ++steps) {
            below = own(i) * below;
        }
        const Transform ownWorld = world[static_cast<std::size_t>(skeleton)] * below;
        const auto& drawn = world[static_cast<std::size_t>(last)];
        // Without bodyOnly the root keeps the drawn turn, so only the place counts.
        if (!clip.bodyOnly) {
            return Transform::of({drawn.m[3] - ownWorld.m[3], drawn.m[7] - ownWorld.m[7], drawn.m[11] - ownWorld.m[11]}, {0, 0, 0, 1});
        }
        return drawn * ownWorld.rigidInverse();
    };
    // The clips with limbs, each weighted as the mixing above weighs a joint they all move.
    // Clips whose limbs only add offsets take no share of the layers. They come in by their own weight.
    // A clip fading out by its body only still reaches with its limbs, so the arms of a Sim getting
    // into bed don't drop to rest as one clip hands over to the next.
    std::vector<const LayeredClip*> withLimbs;
    std::vector<LimbSource> sources;
    for (const auto& clip : clips) {
        if (!clip.animation || !clip.animation->animation || clip.animation->limbs.empty() || (holding && clip.layer < 0)) {
            continue;
        }
        const auto& a = *clip.animation->animation;
        const auto& limbs = clip.animation->limbs;
        if (std::all_of(limbs.begin(), limbs.end(), [&](const LimbChain& l) { return isAdditive(a, l); })) {
            sources.push_back({&a, &limbs, clip.ms, 0, clip.weight, clip.turn, {}, clip.objectFrame, rootMove(clip), clip.objectBone});
        } else {
            withLimbs.push_back(&clip);
        }
    }
    std::sort(withLimbs.begin(), withLimbs.end(),
              [](const LayeredClip* a, const LayeredClip* b) { return coversEarlier(a->layer, a->start, b->layer, b->start); });
    // Each limb is weighed on its own, as the mixing weighs its joints: the clips that move any of its
    // joints cover one another by layer, whether they reach with it or only turn its joints (an
    // arm-only overlay leaves the legs to the walk under it, and a walk's swinging arms cover the
    // standing pose's reach below).
    std::vector<std::vector<float>> weights(withLimbs.size());
    for (std::size_t c = 0; c < withLimbs.size(); ++c) {
        weights[c].assign(withLimbs[c]->animation->limbs.size(), 0.0f);
    }
    std::vector<const LayeredClip*> ordered;
    for (const auto& clip : clips) {
        if (clip.animation && clip.animation->animation && clip.weight > 0 && !(holding && clip.layer < 0)) {
            ordered.push_back(&clip);
        }
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const LayeredClip* a, const LayeredClip* b) { return coversEarlier(a->layer, a->start, b->layer, b->start); });
    const auto limbOf = [](const LayeredClip& clip, const std::vector<int>& joints) -> int {
        const auto& limbs = clip.animation->limbs;
        for (std::size_t k = 0; k < limbs.size(); ++k) {
            if (limbs[k].joints == joints && !isAdditive(*clip.animation->animation, limbs[k])) {
                return static_cast<int>(k);
            }
        }
        return -1;
    };
    const auto turnsJoints = [](const LayeredClip& clip, const std::vector<int>& joints) {
        const auto& channels = clip.animation->animation->channels;
        const auto moves = [&](const std::vector<int>& by, int j) {
            const int c = static_cast<std::size_t>(j) < by.size() ? by[static_cast<std::size_t>(j)] : -1;
            return c >= 0 && !channels[static_cast<std::size_t>(c)].additive();
        };
        return std::any_of(joints.begin(), joints.end(), [&](int j) { return moves(clip.animation->rotation, j) || moves(clip.animation->translation, j); });
    };
    std::vector<const std::vector<int>*> seen;
    for (const auto* first : withLimbs) {
        for (const auto& l : first->animation->limbs) {
            if (std::any_of(seen.begin(), seen.end(), [&](const std::vector<int>* s) { return *s == l.joints; })) {
                continue;
            }
            seen.push_back(&l.joints);
            std::vector<const LayeredClip*> moving;
            for (const auto* clip : ordered) {
                if (limbOf(*clip, l.joints) >= 0 || turnsJoints(*clip, l.joints)) {
                    moving.push_back(clip);
                }
            }
            float left = 1;
            for (std::size_t i = 0; i < moving.size() && left > 0;) {
                const int layer = moving[i]->layer;
                float layerWeight = 0;
                const bool covered = i > 0 && moving[i - 1]->layer == layer;
                for (; i < moving.size() && moving[i]->layer == layer; ++i) {
                    const LayeredClip& clip = *moving[i];
                    const float own = layer > 0 && covered ? steadyOf(clip) : clip.weight;
                    if (const int k = limbOf(clip, l.joints); k >= 0) {
                        const auto c = static_cast<std::size_t>(std::find(withLimbs.begin(), withLimbs.end(), &clip) - withLimbs.begin());
                        if (c < withLimbs.size()) {
                            weights[c][static_cast<std::size_t>(k)] = left * own;
                        }
                    }
                    layerWeight += own;
                    if (layer > 0) {
                        ++i;
                        break;
                    }
                }
                left *= 1 - std::min(layerWeight, 1.0f);
            }
        }
    }
    for (std::size_t c = 0; c < withLimbs.size(); ++c) {
        if (std::any_of(weights[c].begin(), weights[c].end(), [](float w) { return w > kLeastWeight; })) {
            const LayeredClip& clip = *withLimbs[c];
            sources.push_back({clip.animation->animation.get(), &clip.animation->limbs, clip.ms, 0, clip.weight, clip.turn, std::move(weights[c]), clip.objectFrame, rootMove(clip), clip.objectBone});
        }
    }
    if (!sources.empty()) {
        solveLimbs(rig, sources, local, world);
        // A joint keeps the pose it was drawn in, its limb's reach included.
        if (held != nullptr && held->size() == rig.nodes.size()) {
            for (const auto& s : sources) {
                for (const auto& limb : *s.limbs) {
                    for (const int j : limb.joints) {
                        (*held)[static_cast<std::size_t>(j)] = finiteOr(heldOf(local[static_cast<std::size_t>(j)]), rig.nodes[static_cast<std::size_t>(j)]);
                    }
                }
            }
        }
    }
}

} // namespace ts2::scene
