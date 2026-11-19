#include "scene/overlay_fit.hpp"

#include "formats/propset.hpp"
#include "formats/reference_list.hpp"
#include "scene/assets.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>

namespace ts2::scene {

namespace {

using Point = std::array<float, 3>;
using Nearest = std::vector<std::pair<float, std::size_t>>;  // squared distance, vertex

Point at(std::span<const float> v, std::size_t i) {
    return {v[i * 3], v[i * 3 + 1], v[i * 3 + 2]};
}

float squared(const Point& a, const Point& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

// Puts `vertex` into the list before the first entry strictly farther. A list of `cap` entries keeps
// its size, as does one shorter that takes a vertex in its middle: the last entry drops out.
void addNearest(Nearest& list, float distance, std::size_t vertex, std::size_t cap) {
    std::size_t k = 0;
    while (k < list.size() && !(list[k].first > distance)) {
        ++k;
    }
    if (k >= cap) {
        return;
    }
    if (k == list.size()) {
        list.emplace_back(distance, vertex);
        return;
    }
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(k), {distance, vertex});
    list.pop_back();
}

// The average of `rest` moved by each listed move, summed in floats and divided as the game does.
template <class Move>
Point averaged(const Point& rest, const Nearest& list, Move move) {
    Point sum{};
    for (const auto& [distance, vertex] : list) {
        const Point m = move(vertex);
        for (int k = 0; k < 3; ++k) {
            sum[k] = (m[k] + rest[k]) + sum[k];
        }
    }
    const double inverse = 1.0 / static_cast<double>(list.size());
    return {static_cast<float>(sum[0] * inverse), static_cast<float>(sum[1] * inverse), static_cast<float>(sum[2] * inverse)};
}

} // namespace

std::vector<float> fitMeshOverlay(std::span<const float> rest, std::span<const float> maskPositions, std::span<const std::uint8_t> maskColours,
                                  std::span<const float> baseFace, std::span<const float> face) {
    const std::size_t n = rest.size() / 3, masks = maskPositions.size() / 3, faces = baseFace.size() / 3;
    if (rest.size() % 3 != 0 || maskPositions.size() % 3 != 0 || baseFace.size() % 3 != 0 || face.size() != baseFace.size() ||
        (!maskColours.empty() && maskColours.size() != masks * 4) || n > kMaxFitVertices || masks > kMaxFitVertices ||
        faces > kMaxFitVertices) {
        return {};
    }
    constexpr float kHalf = 0.5f, kSamePlace = 1e-4f, kOnFaceVertex = 1e-6f;
    constexpr std::size_t kFaceNeighbours = 3, kTiedNeighbours = 5;
    std::vector<Point> mask;
    for (std::size_t m = 0; m < masks && !maskColours.empty(); ++m) {
        if (static_cast<float>(maskColours[m * 4 + 3]) / 255.0f > kHalf) {
            mask.push_back(at(maskPositions, m));
        }
    }
    // With no mask points (no mask, a mask without colours, or none over half) every vertex is tied.
    std::vector<bool> tied(n, mask.empty());
    for (std::size_t i = 0; i < n && !mask.empty(); ++i) {
        const Point p = at(rest, i);
        for (const Point& m : mask) {
            if (std::abs(m[0] - p[0]) < kSamePlace && std::abs(m[1] - p[1]) < kSamePlace && std::abs(m[2] - p[2]) < kSamePlace) {
                tied[i] = true;
                break;
            }
        }
    }
    std::vector<Point> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = at(rest, i);
        if (!tied[i]) {
            continue;
        }
        const Point p = out[i];
        Nearest list;
        bool onVertex = false;
        for (std::size_t v = 0; v < faces; ++v) {
            const float d = squared(p, at(baseFace, v));
            if (d < kOnFaceVertex) {
                out[i] = at(face, v);
                onVertex = true;
                break;
            }
            addNearest(list, d, v, kFaceNeighbours);
        }
        if (!onVertex && !list.empty()) {
            out[i] = averaged(p, list, [&](std::size_t v) {
                const Point a = at(face, v), b = at(baseFace, v);
                return Point{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
            });
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (tied[i]) {
            continue;
        }
        const Point p = at(rest, i);
        Nearest list;
        for (std::size_t j = 0; j < n; ++j) {
            if (tied[j]) {
                addNearest(list, squared(p, at(rest, j)), j, kTiedNeighbours);
            }
        }
        if (!list.empty()) {
            out[i] = averaged(p, list, [&](std::size_t j) {
                const Point r = at(rest, j);
                return Point{out[j][0] - r[0], out[j][1] - r[1], out[j][2] - r[2]};
            });
        }
    }
    std::vector<float> flat;
    flat.reserve(n * 3);
    for (const Point& p : out) {
        flat.insert(flat.end(), p.begin(), p.end());
    }
    return flat;
}

std::vector<float> meshPositions(std::span<const std::uint8_t> mesh) {
    std::vector<float> out;
    if (mesh.empty()) {
        return out;
    }
    for (const auto& e : scenegraph::parseMesh(mesh).elements) {
        if (e.identity == scenegraph::kPositions && e.stream == 0 && e.count > 0) {
            const auto f = scenegraph::floats(e);
            out.insert(out.end(), f.begin(), f.end());
        }
    }
    return out;
}

std::vector<std::uint8_t> meshColours(std::span<const std::uint8_t> mesh) {
    std::vector<std::uint8_t> out;
    if (mesh.empty()) {
        return out;
    }
    for (const auto& e : scenegraph::parseMesh(mesh).elements) {
        if (e.identity == scenegraph::kColors && e.stream == 0 && e.count > 0) {
            out.insert(out.end(), e.data.begin(), e.data.end());
        }
    }
    return out;
}

std::optional<MeshOverlayCatalog> meshOverlayCatalog(Assets& assets, const scenegraph::Link& part, std::string& problem) {
    constexpr std::uint32_t kNodeType = 0x7BA3838C, kMeshType = 0xAC4F8687, kMaterialType = 0x49596978;
    const auto fail = [&](std::string what) -> std::optional<MeshOverlayCatalog> {
        problem = "a mesh overlay's " + what + " isn't in the catalog";
        return std::nullopt;
    };
    const auto bytes = assets.skinResource(part);
    if (!bytes) {
        return fail("part");
    }
    const auto p = propset::parse(*bytes);
    const scenegraph::Link refsKey{reflist::kType, part.group, part.instance, part.instanceHigh};
    auto refsBytes = assets.skinResource(refsKey);
    if (!refsBytes) {
        refsBytes = assets.byKey(refsKey);
    }
    if (!refsBytes) {
        return fail("reference list");
    }
    const auto refs = reflist::parse(*refsBytes);
    const auto field = [&](const std::string& name) -> std::optional<std::size_t> {
        const auto it = p.find(name);
        if (it == p.end()) {
            return std::nullopt;
        }
        char* end = nullptr;
        const unsigned long v = std::strtoul(it->second.c_str(), &end, 0);
        if (end == it->second.c_str() || v >= refs.size()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(v);
    };
    // A shape's first geometry node and the mesh that node draws.
    const auto shapeMesh = [&](const char* name, std::vector<std::uint8_t>* node,
                               std::vector<std::uint8_t>* shapeBytes) -> std::optional<std::vector<std::uint8_t>> {
        const auto index = field(name);
        const auto shape = index ? assets.byKey(refs[*index]) : std::nullopt;
        if (!shape) {
            return std::nullopt;
        }
        if (shapeBytes != nullptr) {
            *shapeBytes = *shape;
        }
        const auto parsed = scenegraph::parseShape(*shape);
        std::optional<std::vector<std::uint8_t>> nodeBytes;
        if (!parsed.geometryNodes.empty() && !parsed.geometryNodes[0].empty()) {
            nodeBytes = assets.byName(kNodeType, parsed.geometryNodes[0]);
        } else if (!parsed.geometryLinks.empty() && parsed.geometryLinks[0] && *parsed.geometryLinks[0] < parsed.container.links.size()) {
            nodeBytes = assets.byKey(parsed.container.links[*parsed.geometryLinks[0]]);
        }
        if (!nodeBytes) {
            return std::nullopt;
        }
        for (const auto& link : scenegraph::parseContainer(*nodeBytes).links) {
            if (link.type == kMeshType) {
                if (auto mesh = assets.byKey(link)) {
                    if (node != nullptr) {
                        *node = std::move(*nodeBytes);
                    }
                    return mesh;
                }
            }
        }
        return std::nullopt;
    };
    MeshOverlayCatalog out;
    auto mesh = shapeMesh("shapekeyidx", &out.node, &out.shape);
    if (!mesh) {
        return fail("shape");
    }
    out.mesh = std::move(*mesh);
    // Some parts have no mask, which leaves maskMesh empty.
    if (auto mask = shapeMesh("maskshapekeyidx", nullptr, nullptr)) {
        out.maskMesh = std::move(*mask);
    }
    // The overrides, capped as a part has a few.
    constexpr std::size_t kMaxOverrides = 16;
    const auto count = p.contains("numoverrides") ? std::strtoul(p.at("numoverrides").c_str(), nullptr, 0) : 0ul;
    for (std::size_t i = 0; i < count && i < kMaxOverrides; ++i) {
        const std::string stem = "override" + std::to_string(i);
        const auto subset = p.find(stem + "subset");
        const auto index = field(stem + "resourcekeyidx");
        auto material = index ? assets.byKey(refs[*index]) : std::nullopt;
        if (subset == p.end() || !material) {
            return fail("material");
        }
        out.subsets.push_back({subset->second, std::move(*material)});
    }
    // The shape's other mesh groups keep the shape's own material.
    for (const auto& s : scenegraph::parseShape(out.shape).parts) {
        const bool overridden = std::any_of(out.subsets.begin(), out.subsets.end(), [&](const auto& o) { return o.subset == s.group; });
        if (overridden || out.subsets.size() >= kMaxOverrides) {
            continue;
        }
        auto material = assets.byName(kMaterialType, s.material + "_txmt");
        if (!material) {
            return fail("material");
        }
        out.subsets.push_back({s.group, std::move(*material)});
    }
    return out;
}

} // namespace ts2::scene
