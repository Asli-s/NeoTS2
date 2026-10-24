#include "scene/neighborhood_decals.hpp"

#include "diag/log.hpp"
#include "formats/scenegraph.hpp"
#include "scene/ground_decal.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <optional>

namespace ts2::scene {

namespace {

constexpr std::uint32_t kResourceNodeType = 0xE519C933;
constexpr float kTwoPi = 6.28318530717958647692f;
// Effects name other effects (type 0 components). The depth and the components played for one
// decoration are capped against files whose effects name each other in a loop or many times over.
constexpr int kMaxDepth = 8;
constexpr std::size_t kMaxComponents = 4096;
// The largest random variation taken, far above the game's. A larger one would make the random
// range overflow.
constexpr float kMaxVary = 100.0f;

bool finite(const effects::Curve& c) {
    return std::all_of(c.values.begin(), c.values.end(), [](float v) { return std::isfinite(v); });
}

// Whether a curve keeps one value over its life.
bool constant(const effects::Curve& c) {
    return finite(c) && std::all_of(c.values.begin(), c.values.end(), [&](float v) { return v == c.values.front(); });
}

bool identity(const std::array<float, 9>& m) {
    for (std::size_t i = 0; i < 9; ++i) {
        if (std::abs(m[i] - (i % 4 == 0 ? 1.0f : 0.0f)) > 1e-4f) {
            return false;
        }
    }
    return true;
}

bool usableVary(float vary) {
    return std::isfinite(vary) && vary <= kMaxVary;
}

// Whether the decal is one this draws: a texture laid down once with the plain decal material, its
// curves and colour the same over its life and its square not stretched.
bool drawable(const effects::DecalBlock& d) {
    using effects::DecalBlock;
    return d.draw == DecalBlock::Draw::Decal && (d.flags & ~DecalBlock::kStatic) == 0 && !d.texture.empty() &&
           constant(d.rotation) && constant(d.size) && constant(d.alpha) && constant(d.aspect) && d.aspect.at(0) == 1 &&
           d.colours.size() <= 1 && usableVary(d.alphaVary) && usableVary(d.sizeVary) && usableVary(d.rotationVary) &&
           std::all_of(d.colours.begin(), d.colours.end(), [](const auto& c) {
               return std::isfinite(c[0]) && std::isfinite(c[1]) && std::isfinite(c[2]);
           });
}

// Where a component plays: its place, its turn about z (radians) and its scale.
struct Placed {
    std::array<float, 3> at{};
    float turn = 0;
    float scale = 1;
};

// Where a component plays inside its parent: at its offset, turned and scaled with the parent, and
// scaled by its own scale. Components turned within their parent aren't played yet.
std::optional<Placed> componentPlace(const effects::Component& c, const Placed& parent) {
    if (!identity(c.orientation) || !(c.scale > 0) || !std::isfinite(c.scale) ||
        !std::all_of(c.offset.begin(), c.offset.end(), [](float v) { return std::isfinite(v); })) {
        return std::nullopt;
    }
    const float cs = std::cos(parent.turn), sn = std::sin(parent.turn);
    const auto& o = c.offset;
    return Placed{{parent.at[0] + parent.scale * (cs * o[0] - sn * o[1]), parent.at[1] + parent.scale * (sn * o[0] + cs * o[1]),
                   parent.at[2] + parent.scale * o[2]},
                  parent.turn, parent.scale * c.scale};
}

// The weighted pick of a group of components (a run of the same group id, 0 for none): one roll
// below 0x10000 per group, and the first component whose running weight reaches it plays.
class GroupPick {
public:
    explicit GroupPick(std::mt19937& random) : random_(random) {}

    bool plays(const effects::Component& c) {
        if (c.group != group_) {
            group_ = c.group;
            roll_ = std::uniform_int_distribution<std::uint32_t>(0, 0xFFFF)(random_);
            total_ = 0;
            picked_ = false;
        }
        if (group_ == 0) {
            return true;
        }
        total_ += c.weight;
        if (picked_ || total_ < roll_) {
            return false;
        }
        picked_ = true;
        return true;
    }

private:
    std::mt19937& random_;
    std::uint16_t group_ = 0;
    std::uint32_t roll_ = 0, total_ = 0;
    bool picked_ = false;
};

// Plays one effect's ground decals into a scene, and the effects its components name.
class DecalPlayer {
public:
    DecalPlayer(SceneBuilder& builder, const effects::File& file, std::uint32_t state, std::mt19937& random)
        : builder_(builder), file_(file), state_(state), random_(random) {}

    std::size_t play(const effects::VisualEffect& effect, const Placed& place, int depth) {
        std::size_t added = 0;
        GroupPick pick(random_);
        for (const auto& c : effect.components) {
            if (budget_ == 0) {
                diag::warnOnce("neighborhood decals: an effect playing more than " + std::to_string(kMaxComponents) +
                               " components is cut short");
                break;
            }
            --budget_;
            if (!pick.plays(c) || (state_ & c.stateMask) != c.state) {
                continue;
            }
            const auto child = componentPlace(c, place);
            if (!child) {
                diag::warnOnce("neighborhood decals: a turned, mirrored or broken effect component is left out (" + c.name + ")");
                continue;
            }
            if (c.type == 0) {
                added += playNamed(c.name, c.index, *child, depth);
            } else if (c.type == static_cast<std::uint8_t>(effects::BlockType::Decal) && c.index >= 0 &&
                       static_cast<std::size_t>(c.index) < file_.decals.size()) {
                added += addDecal(file_.decals[static_cast<std::size_t>(c.index)], *child) ? 1 : 0;
            } else {
                diag::warnOnce("neighborhood decals: an effect block of a kind not played yet is left out (" + c.name + ")");
            }
        }
        return added;
    }

private:
    // A type 0 component plays the effect its name names (its block index must still be valid).
    std::size_t playNamed(const std::string& name, std::int32_t index, const Placed& place, int depth) {
        const effects::VisualEffect* named = index >= 0 ? file_.find(name) : nullptr;
        if (named == nullptr) {
            diag::warnOnce("neighborhood decals: effect " + name + " is missing");
            return 0;
        }
        if (depth >= kMaxDepth) {
            diag::warnOnce("neighborhood decals: effects naming effects deeper than " + std::to_string(kMaxDepth) +
                           " are left out (" + name + ")");
            return 0;
        }
        return play(*named, place, depth + 1);
    }

    // A random factor in [1 - vary, 1 + vary], as the game varies a decal's size and alpha. The
    // game's own random numbers for it are not known, so it differs from the game's picks.
    float varied(float vary) {
        return vary > 0 ? std::uniform_real_distribution<float>(1 - vary, 1 + vary)(random_) : 1.0f;
    }

    bool addDecal(const effects::DecalBlock& d, const Placed& place) {
        if (!drawable(d)) {
            diag::warnOnce("neighborhood decals: a decal of a kind not drawn yet is left out (" + d.texture + ")");
            return false;
        }
        if (!builder_.scene().terrain) {
            return false;
        }
        // The size and alpha vary by their factors, the turn by its curve times a random number in
        // [-vary, vary] (1 without one), added to the place's turn.
        const float size = d.size.at(0) * place.scale * varied(d.sizeVary);
        const float spin = d.rotationVary > 0 ? std::uniform_real_distribution<float>(-d.rotationVary, d.rotationVary)(random_) : 1.0f;
        const float turns = d.rotation.at(0) * spin + place.turn / kTwoPi;
        // Rounded to the 256 steps a colour channel draws with, so varied decals share materials.
        const float alpha = std::round(std::clamp(d.alpha.at(0) * varied(d.alphaVary), 0.0f, 1.0f) * 255) / 255;
        if (!(size >= kMinGroundDecalWidth) || size > kMaxGroundDecalWidth || !std::isfinite(turns)) {
            diag::warnOnce("neighborhood decals: a decal of a size that can't be drawn is left out (" + d.texture + ")");
            return false;
        }
        const auto& colour = d.colours.empty() ? std::array<float, 3>{1, 1, 1} : d.colours.front();
        // The overlay material: unlit, texture times colour, blended after the ground on layer 2.
        const int material = builder_.particleMaterial(d.texture, {colour[0], colour[1], colour[2], alpha}, false, 2);
        if (material < 0) {
            diag::warnOnce("neighborhood decals: " + d.texture + " is missing");
            return false;
        }
        Primitive p = groundDecal(*builder_.scene().terrain, {place.at[0], place.at[1]}, size, turns, material);
        if (p.indices.empty()) {
            return false;
        }
        Surfaces s;
        s.emplace(material, std::move(p));
        return builder_.place(builder_.addMesh("decal " + d.texture, std::move(s)), "decal", Transform{}) != nullptr;
    }

    SceneBuilder& builder_;
    const effects::File& file_;
    std::uint32_t state_;
    std::mt19937& random_;
    std::size_t budget_ = kMaxComponents;
};

// A node's place in its model: its translation through its parents' transforms. The parent walk is
// capped at the node count against files whose nodes are their own ancestors.
std::array<float, 3> placeInModel(const scenegraph::ResourceNode& tree, std::size_t node) {
    std::vector<std::size_t> parents(tree.nodes.size(), tree.nodes.size());
    for (std::size_t i = 0; i < tree.nodes.size(); ++i) {
        for (const auto child : tree.nodes[i].children) {
            if (child < parents.size()) {
                parents[child] = i;
            }
        }
    }
    const auto& t = tree.nodes[node].translation;
    float p[3] = {t[0], t[1], t[2]};
    for (std::size_t step = 0; step < tree.nodes.size() && parents[node] < tree.nodes.size(); ++step) {
        node = parents[node];
        Transform::of(tree.nodes[node].translation, tree.nodes[node].rotation).apply(p, true);
    }
    return {p[0], p[1], p[2]};
}

} // namespace

std::vector<EffectSlot> effectSlots(Assets& assets, const std::string& model) {
    std::vector<EffectSlot> out;
    const auto cres = assets.byName(kResourceNodeType, model + "_cres");
    if (!cres) {
        return out;
    }
    try {
        const auto tree = scenegraph::parseResourceNode(*cres);
        for (const auto& list : scenegraph::parseDataLists(*cres)) {
            if (list.name != "EffectsList") {
                continue;
            }
            for (const auto& item : list.items) {
                if (item.type != scenegraph::DataValue::Type::String) {
                    continue;
                }
                const auto slot = std::find_if(tree.nodes.begin(), tree.nodes.end(), [&](const scenegraph::SceneNode& n) {
                    return n.kind == scenegraph::SceneNode::Kind::Transform && n.name == item.name;
                });
                if (slot == tree.nodes.end()) {
                    diag::warnOnce("neighborhood decals: " + model + " has no slot " + item.name);
                    continue;
                }
                out.push_back({item.text, placeInModel(tree, static_cast<std::size_t>(slot - tree.nodes.begin()))});
            }
        }
    } catch (const std::exception& ex) {
        diag::warnOnce("neighborhood decals: " + model + " can't be read: " + ex.what());
    }
    return out;
}

std::size_t addGroundDecals(SceneBuilder& builder, const effects::File& file, const std::string& effect, const std::array<float, 3>& at,
                            float turn, std::uint32_t state, std::mt19937& random) {
    const effects::VisualEffect* top = file.find(effect);
    if (top == nullptr || !std::isfinite(turn)) {
        return 0;
    }
    return DecalPlayer(builder, file, state, random).play(*top, {at, turn, 1}, 0);
}

} // namespace ts2::scene
