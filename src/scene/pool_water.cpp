#include "scene/pool_water.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace ts2::scene::poolwater {

namespace {

constexpr float kLayerStep = 0.75f;
constexpr std::array<float, 3> kLayerOpacity{0.2f, 0.2f, 0.5f};
// How strongly a layer shows at a point: inside the pool, beside its wall, on the wall.
constexpr float kInside = 0.27f, kBesideWall = 0.09f, kOnWall = 0.0f;

using Point = std::pair<int, int>;  // in half tiles

// A quarter's outer edge, from A to B as the game walks it, and the tile's centre.
std::array<Point, 3> cornersOf(const Quarter& q) {
    const int x = 2 * q.x, y = 2 * q.y;
    const Point centre{x + 1, y + 1};
    switch (q.quarter) {
    case 0: return {Point{x, y + 2}, Point{x, y}, centre};
    case 1: return {Point{x + 2, y + 2}, Point{x, y + 2}, centre};
    case 2: return {Point{x + 2, y}, Point{x + 2, y + 2}, centre};
    default: return {Point{x, y}, Point{x + 2, y}, centre};
    }
}

// The quarter across the outer edge, in the next tile.
Quarter across(const Quarter& q) {
    switch (q.quarter) {
    case 0: return {q.x - 1, q.y, 2};
    case 1: return {q.x, q.y + 1, 3};
    case 2: return {q.x + 1, q.y, 0};
    default: return {q.x, q.y - 1, 1};
    }
}

} // namespace

std::vector<Quarter> waterQuarters(const content::Lot& lot) {
    const auto surface = lot.grids.find(content::kSurfaceGridInstance);
    const auto floors = lot.grids.find(content::kFloorGridInstance);
    const int ground = content::groundGridLevel(lot);
    if (surface == lot.grids.end() || floors == lot.grids.end() || surface->second.elementSize != 4 || floors->second.elementSize != 8 ||
        ground < 1) {
        return {};
    }
    std::vector<Quarter> out;
    for (std::uint32_t x = 0; x < surface->second.width; ++x) {
        for (std::uint32_t y = 0; y < surface->second.height; ++y) {
            const auto tx = static_cast<std::int32_t>(x), ty = static_cast<std::int32_t>(y);
            const auto open = surface->second.at(ground, tx, ty);
            if (open.size() != 4 || !floors->second.contains(ground - 1, tx, ty)) {
                continue;
            }
            for (std::size_t q = 0; q < 4; ++q) {
                if (open[q] == 0 && floors->second.i16(ground - 1, tx, ty, q) != 0) {
                    out.push_back({tx, ty, static_cast<int>(q)});
                }
            }
        }
    }
    return out;
}

std::array<Layer, 3> depthLayers(const std::vector<Quarter>& quarters, const std::function<float(float, float)>& height) {
    std::set<std::tuple<int, int, int>> water;
    for (const auto& q : quarters) {
        water.insert({q.x, q.y, q.quarter});
    }
    auto isWater = [&](const Quarter& q) { return water.contains({q.x, q.y, q.quarter}); };

    // Where the pool's wall runs: an edge with no water on its other side. Its points are clear, and
    // the centre of a tile beside it faint.
    std::map<Point, float> strength;
    auto lower = [&](const Point& p, float s) {
        const auto [it, added] = strength.emplace(p, s);
        if (!added) {
            it->second = std::min(it->second, s);
        }
    };
    for (const auto& q : quarters) {
        const auto c = cornersOf(q);
        const bool outerWall = !isWater(across(q));
        const bool nextWall = !isWater({q.x, q.y, (q.quarter + 1) % 4});
        const bool previousWall = !isWater({q.x, q.y, (q.quarter + 3) % 4});
        lower(c[0], outerWall || nextWall ? kOnWall : kInside);
        lower(c[1], outerWall || previousWall ? kOnWall : kInside);
        lower(c[2], nextWall || previousWall ? kOnWall : outerWall ? kBesideWall : kInside);
    }

    std::array<Layer, 3> layers;
    for (std::size_t n = 0; n < layers.size(); ++n) {
        Layer& layer = layers[n];
        layer.opacity = kLayerOpacity[n];
        const float depth = kLayerStep * static_cast<float>(n + 1);
        std::map<Point, std::uint32_t> index;
        auto vertex = [&](const Point& p) {
            const auto [it, added] = index.emplace(p, static_cast<std::uint32_t>(layer.alphas.size()));
            if (added) {
                const float x = static_cast<float>(p.first) / 2, y = static_cast<float>(p.second) / 2;
                layer.positions.insert(layer.positions.end(), {x, y, height(x, y) - depth});
                layer.alphas.push_back(strength.at(p));
            }
            return it->second;
        };
        for (const auto& q : quarters) {
            const auto c = cornersOf(q);
            layer.indices.insert(layer.indices.end(), {vertex(c[0]), vertex(c[1]), vertex(c[2])});
        }
    }
    return layers;
}

float surfaceHeight(float levelBelow) {
    return levelBelow + 2.75f;
}

Surface surface(const std::vector<Quarter>& quarters, const std::function<float(float, float)>& height) {
    using V = std::array<float, 3>;
    Surface out;
    // Points shared by quarters are one vertex, found by x and y alone as the game finds them.
    std::map<std::pair<float, float>, std::uint32_t> index;
    auto vertex = [&](const V& v) {
        const auto [it, added] = index.emplace(std::pair{v[0], v[1]}, static_cast<std::uint32_t>(out.positions.size() / 3));
        if (added) {
            out.positions.insert(out.positions.end(), v.begin(), v.end());
        }
        return it->second;
    };
    auto point = [&](const Point& p) {
        const float x = static_cast<float>(p.first) / 2, y = static_cast<float>(p.second) / 2;
        return V{x, y, height(x, y)};
    };
    // An edge's middle takes its first point's height, and every quarter point is halfway between a
    // corner and the outer edge's middle, as the game builds them.
    auto middle = [](const V& a, const V& b) { return V{(a[0] + b[0]) / 2, (a[1] + b[1]) / 2, a[2]}; };
    auto half = [](const V& a, const V& b) { return V{(a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2}; };
    static constexpr std::array<std::uint32_t, 24> kTriangles{0, 2, 7, 2, 1, 7, 7, 1, 8, 7, 8, 6, 1, 4, 8, 1, 5, 4, 4, 5, 3, 4, 6, 8};
    for (const auto& q : quarters) {
        const auto c = cornersOf(q);
        const V a = point(c[0]), b = point(c[1]), centre = point(c[2]);
        const V ab = middle(a, b);
        const std::array<V, 9> v{a, ab, half(a, ab), b, middle(b, centre), half(b, ab), centre, middle(centre, a), half(centre, ab)};
        std::array<std::uint32_t, 9> at{};
        for (std::size_t k = 0; k < v.size(); ++k) {
            at[k] = vertex(v[k]);
        }
        for (const auto k : kTriangles) {
            out.indices.push_back(at[k]);
        }
    }
    return out;
}

std::vector<std::size_t> lightPoolWater(Scene& scene, bool day) {
    const float night = day ? 0.75f : 0.2f;
    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        auto& instance = scene.instances[i];
        if (instance.poolWater && instance.poolNight != night) {
            instance.poolNight = night;
            changed.push_back(i);
        }
    }
    return changed;
}

} // namespace ts2::scene::poolwater
