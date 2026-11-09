#include "scene/instance_merge.hpp"

#include "scene/object_shadow.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace ts2::scene {

namespace {

// A joined primitive starts again past this many vertices, so its indices stay far from overflowing.
constexpr std::size_t kMostVertices = std::size_t{1} << 22;
// The most vertices of a mesh placed more than once that still joins (fence pieces, posts, trims).
constexpr std::size_t kMostRepeatedVertices = 256;

// Which per-vertex streams a primitive brings, one bit each. Only primitives with the same streams join.
unsigned streams(const Primitive& p) {
    const std::size_t n = p.positions.size() / 3;
    return (p.normals.size() == n * 3 ? 1u : 0u) | (p.uvs.size() == n * 2 ? 2u : 0u) | (p.alphas.size() == n ? 4u : 0u) |
           (p.colors.size() == n * 3 ? 8u : 0u) | (p.lightUvs.size() == n * 2 ? 16u : 0u) | (p.tangents.size() == n * 3 ? 32u : 0u);
}

bool joinable(const Primitive& p, const std::vector<Material>& materials) {
    const std::size_t n = p.positions.size() / 3;
    if (n == 0 || p.positions.size() != n * 3 || p.indices.empty() || !p.joints.empty() || !p.weights.empty() || !p.morphs.empty() ||
        !p.eyeUvs.empty() || objectshadow::isShadowGroup(p.group)) {
        return false;
    }
    if (p.material < 0 || static_cast<std::size_t>(p.material) >= materials.size()) {
        return false;
    }
    const Material& m = materials[static_cast<std::size_t>(p.material)];
    if ((m.alpha != AlphaMode::Opaque && m.alpha != AlphaMode::Mask) || m.poolSurface) {
        return false;
    }
    return std::all_of(p.indices.begin(), p.indices.end(), [&](std::uint32_t i) { return i < n; });
}

// Instances placed farther out than this, or not at a number, come from broken files and stay apart,
// so their place always fits the cell numbers below.
constexpr float kFarthestJoined = 1.0e6f;

bool joinable(const Instance& i, const std::vector<Mesh>& meshes) {
    const bool placed = std::all_of(i.transform.m.begin(), i.transform.m.end(), [](float v) { return std::isfinite(v); }) &&
                        std::abs(i.transform.m[3]) < kFarthestJoined && std::abs(i.transform.m[7]) < kFarthestJoined;
    return placed && i.mesh >= 0 && static_cast<std::size_t>(i.mesh) < meshes.size() && !i.hidden && i.walls.empty() && !i.cut &&
           i.part != Part::Object && !i.poolWater && i.shadowMesh < 0 && meshes[static_cast<std::size_t>(i.mesh)].skin.empty();
}

// Everything about an instance that changes how its primitives are drawn, besides its mesh and place.
// The light set is the one a primitive takes (see Instance::partLightSets).
auto drawnAlike(const Instance& i, int lightSet, float cell) {
    const auto cx = static_cast<long>(std::floor(i.transform.m[3] / cell)), cy = static_cast<long>(std::floor(i.transform.m[7] / cell));
    return std::make_tuple(i.part, i.level, i.ground, i.lightMap, i.incidenceMap, i.lightMapGain, i.lightMapOnly, i.lightMapMultiply,
                           i.skirtAmbient, i.skirtRoad, i.skirtScale, i.skirtSand, i.seaReflected, i.seaNight, i.poolNight, i.poolMirrorKeep,
                           i.sunTest, i.litOutdoors, i.underside, i.floorHeight, i.lightGroup, lightSet, i.shapeColor, i.neighborHouse, cx, cy);
}
using DrawnAlike = decltype(drawnAlike(Instance{}, 0, 1.0f));

void append(Primitive& to, const Primitive& p, const Transform& t) {
    const std::size_t n = p.positions.size() / 3;
    const auto base = static_cast<std::uint32_t>(to.positions.size() / 3);
    const unsigned has = streams(p);
    for (std::size_t v = 0; v < n; ++v) {
        float x[3] = {p.positions[v * 3], p.positions[v * 3 + 1], p.positions[v * 3 + 2]};
        t.apply(x, true);
        to.positions.insert(to.positions.end(), x, x + 3);
        // As the vertex shader turns them, without making them unit length.
        for (const auto& [bit, from, into] : {std::tuple{1u, &p.normals, &to.normals}, std::tuple{32u, &p.tangents, &to.tangents}}) {
            if (has & bit) {
                float d[3] = {(*from)[v * 3], (*from)[v * 3 + 1], (*from)[v * 3 + 2]};
                t.apply(d, false);
                into->insert(into->end(), d, d + 3);
            }
        }
    }
    if (has & 2u) {
        to.uvs.insert(to.uvs.end(), p.uvs.begin(), p.uvs.end());
    }
    if (has & 4u) {
        to.alphas.insert(to.alphas.end(), p.alphas.begin(), p.alphas.end());
    }
    if (has & 8u) {
        to.colors.insert(to.colors.end(), p.colors.begin(), p.colors.end());
    }
    if (has & 16u) {
        to.lightUvs.insert(to.lightUvs.end(), p.lightUvs.begin(), p.lightUvs.end());
    }
    for (const std::uint32_t i : p.indices) {
        to.indices.push_back(base + i);
    }
}

struct Joined {
    Instance instance;
    Mesh mesh;
    // The primitive still taking more of each material, share of the sun and streams.
    std::map<std::tuple<int, float, unsigned>, std::size_t> open;
};

} // namespace

std::size_t mergeAlike(Scene& scene, float cell) {
    if (!scene.actors.empty() || !scene.placedObjects.empty() || !(cell > 0)) {
        return 0;
    }
    // A mesh placed more than once is copied for each place, so of those only small ones join.
    std::vector<int> uses(scene.meshes.size(), 0);
    for (const Instance& i : scene.instances) {
        if (i.mesh >= 0 && static_cast<std::size_t>(i.mesh) < uses.size()) {
            ++uses[static_cast<std::size_t>(i.mesh)];
        }
    }
    auto copiedCheaply = [&](int mesh) {
        if (uses[static_cast<std::size_t>(mesh)] == 1) {
            return true;
        }
        std::size_t vertices = 0;
        for (const Primitive& p : scene.meshes[static_cast<std::size_t>(mesh)].primitives) {
            vertices += p.positions.size() / 3;
        }
        return vertices <= kMostRepeatedVertices;
    };
    std::map<DrawnAlike, Joined> joined;
    std::vector<Instance> kept;
    std::size_t merged = 0;
    for (Instance& instance : scene.instances) {
        if (!joinable(instance, scene.meshes) || !copiedCheaply(instance.mesh)) {
            kept.push_back(std::move(instance));
            continue;
        }
        const Mesh& mesh = scene.meshes[static_cast<std::size_t>(instance.mesh)];
        std::vector<std::size_t> left;
        for (std::size_t k = 0; k < mesh.primitives.size(); ++k) {
            const Primitive& p = mesh.primitives[k];
            if (!joinable(p, scene.materials)) {
                left.push_back(k);
                continue;
            }
            const int lightSet = k < instance.partLightSets.size() ? instance.partLightSets[k] : instance.lightSet;
            const float sun = k < instance.sun.size() ? instance.sun[k] : 1.0f;
            auto [it, made] = joined.try_emplace(drawnAlike(instance, lightSet, cell));
            Joined& j = it->second;
            if (made) {
                j.instance = instance;
                j.instance.name = "joined";
                j.instance.object = 0;
                j.instance.transform = Transform{};
                j.instance.lightSet = lightSet;
                j.instance.partLightSets.clear();
                j.instance.sun.clear();
                j.mesh.name = "joined";
            }
            const auto key = std::make_tuple(p.material, sun, streams(p));
            auto open = j.open.find(key);
            if (open == j.open.end() || j.mesh.primitives[open->second].positions.size() / 3 + p.positions.size() / 3 > kMostVertices) {
                Primitive fresh;
                fresh.material = p.material;
                j.mesh.primitives.push_back(std::move(fresh));
                j.instance.sun.push_back(sun);
                open = j.open.insert_or_assign(key, j.mesh.primitives.size() - 1).first;
            }
            append(j.mesh.primitives[open->second], p, instance.transform);
        }
        if (left.size() == mesh.primitives.size()) {
            kept.push_back(std::move(instance));
            continue;
        }
        ++merged;
        if (left.empty()) {
            continue;
        }
        // What can't join stays with the instance, in a mesh of its own, with its per-primitive lists to match.
        Mesh rest;
        rest.name = mesh.name;
        std::vector<float> sun;
        std::vector<int> partLightSets;
        for (const std::size_t k : left) {
            rest.primitives.push_back(mesh.primitives[k]);
            if (k < instance.sun.size()) {
                sun.push_back(instance.sun[k]);
            }
            if (k < instance.partLightSets.size()) {
                partLightSets.push_back(instance.partLightSets[k]);
            }
        }
        instance.sun = sun.size() == left.size() ? std::move(sun) : std::vector<float>{};
        instance.partLightSets = partLightSets.size() == left.size() ? std::move(partLightSets) : std::vector<int>{};
        instance.mesh = static_cast<int>(scene.meshes.size());
        scene.meshes.push_back(std::move(rest));
        kept.push_back(std::move(instance));
    }
    for (auto& [key, j] : joined) {
        j.instance.mesh = static_cast<int>(scene.meshes.size());
        scene.meshes.push_back(std::move(j.mesh));
        kept.push_back(std::move(j.instance));
    }
    scene.instances = std::move(kept);
    // Only the meshes still placed are kept.
    std::vector<int> moved(scene.meshes.size(), -1);
    std::vector<Mesh> used;
    for (Instance& i : scene.instances) {
        for (int* m : {&i.mesh, &i.shadowMesh}) {
            if (*m < 0 || static_cast<std::size_t>(*m) >= moved.size()) {
                continue;
            }
            if (moved[static_cast<std::size_t>(*m)] < 0) {
                moved[static_cast<std::size_t>(*m)] = static_cast<int>(used.size());
                used.push_back(std::move(scene.meshes[static_cast<std::size_t>(*m)]));
            }
            *m = moved[static_cast<std::size_t>(*m)];
        }
    }
    scene.meshes = std::move(used);
    return merged;
}

} // namespace ts2::scene
