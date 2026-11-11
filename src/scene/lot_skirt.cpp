#include "scene/lot_skirt.hpp"

#include "content/lot.hpp"
#include "scene/lot_light_maps.hpp"
#include "scene/lot_parts.hpp"
#include "scene/pool_water.hpp"
#include "scene/terrain_water.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <functional>
#include <limits>
#include <optional>
#include <utility>

namespace ts2::scene {

namespace {

// The game draws the lot skirt, its fading edge and its roads on one layer before objects
// ("Shaders for lot skirt": lotSkirtLayer -6, times 8).
constexpr int kSkirtLayer = -48;

// Not in the original game: a pool-style sea's surface over neighborhood tile (x, y), at the water level,
// clear but for the view's reflection, split into `steps` by `steps` quads. It fades out over the last
// `shoreFade` units of depth to the waterline, so it meets the sand unseen. Each point keeps its depth
// over that fade past 0 and 1 (the shader clamps it), so the fade follows the waterline across a quad
// instead of spreading over the whole of it. `groundAt` is the ground's height at a point in
// neighborhood coordinates and `toLocal` turns those into the mesh's own.
void addPoolStyleSurface(Primitive& p, const PaintedTerrain& terrain, int x, int y, const std::function<float(float, float)>& groundAt,
                         const std::function<void(float (&)[3])>& toLocal, float shoreFade, int steps) {
    const float tile = terrain.tileSize, z = terrain.waterLevel;
    const auto base = static_cast<std::uint32_t>(p.positions.size() / 3);
    const auto row = static_cast<std::uint32_t>(steps + 1);
    for (int j = 0; j <= steps; ++j) {
        for (int i = 0; i <= steps; ++i) {
            const float px = (static_cast<float>(x) + static_cast<float>(i) / static_cast<float>(steps)) * tile;
            const float py = (static_cast<float>(y) + static_cast<float>(j) / static_cast<float>(steps)) * tile;
            float at[3] = {px, py, z};
            toLocal(at);
            p.positions.insert(p.positions.end(), {at[0], at[1], at[2]});
            p.normals.insert(p.normals.end(), {0, 0, 1});
            p.uvs.insert(p.uvs.end(), {0, 0});
            // Kept within a few fades either way, so a cliff beside the water doesn't pull the fade far
            // across a quad.
            constexpr float kFurthest = 4;
            p.alphas.push_back(std::clamp((z - groundAt(px, py)) / shoreFade, -kFurthest, kFurthest));
        }
    }
    for (std::uint32_t j = 0; j < static_cast<std::uint32_t>(steps); ++j) {
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(steps); ++i) {
            const std::uint32_t a = base + j * row + i;
            p.indices.insert(p.indices.end(), {a, a + 1, a + row + 1, a, a + row + 1, a + row});
        }
    }
}

// The sea around a lot: a flat sheet at the neighborhood's water level over each skirt tile with a corner
// under water, and, as low as the skirt's ground, over each tile whose corners all lie on a beach lot
// within 20 of its tiles of its beach side. Built in the lot's coordinates, which the waves run on. A
// beach lot's uses lotSkirtSea, its first texture coordinate 0.04 per unit of water depth (up to 15).
// Others use lotSkirtSeaNoReflection: the sky cube along the reflected view times (0.4, 0.6, 0.9), 0.4
// opaque, unlit, on layer 1 (nhoodWaterLayer). A pool-style sea (not in the original game) uses
// lotSkirtSea on every lot. `ground` is the skirt's height at a grid point and `surfaceAt` the ground's
// drawn height at a point, both in neighborhood coordinates. Returns the tiles covered.
std::vector<std::array<int, 2>> addSkirtSea(SceneBuilder& builder, const PaintedTerrain& terrain, const LotFootprint& lot, const SkirtArea& area,
                                            const content::WorldBounds* bounds, const std::function<float(int, int)>& ground,
                                            const std::function<float(float, float)>& surfaceAt) {
    const bool beachLot = bounds && lot.beachSide;
    const bool poolStyle = builder.poolStyleSea();
    constexpr std::array<float, 3> kSky{0.4f, 0.6f, 0.9f};
    constexpr float kOpacity = 0.4f;
    // The game draws lotSkirtSea one layer over the pools' water (poolWaterLayer 5).
    const int material = beachLot || poolStyle ? builder.skirtSeaMaterial("lotSkirtSea", "poolShape-body-bump", "neighborhood-sky2-envcube", kSky, kOpacity, 6,
                                                             poolStyle ? "causticsTiled" : "")
                                  : builder.cubeReflectionMaterial("lotSkirtSeaNoReflection", "neighborhood-sky2-envcube", kSky, kOpacity, 1);
    std::vector<std::array<int, 2>> cells;
    if (material < 0) {
        return cells;
    }
    const float tile = terrain.tileSize, z = terrain.waterLevel;
    auto wet = [&](int x, int y) {
        return terrain.heights[static_cast<std::size_t>(y) * terrain.columns + static_cast<std::size_t>(x)] < z;
    };
    const Transform toLot = lot.place.rigidInverse();
    auto beach = [&](int x, int y) {
        float p[3] = {static_cast<float>(x) * tile, static_cast<float>(y) * tile, 0};
        toLot.apply(p, true);
        constexpr int kAllBands = 7;
        return content::isBeachLocation(*bounds, lot.beachSide, p[0], p[1], kAllBands);
    };
    Surfaces sea;
    Primitive& p = sea[material];
    const std::function<void(float (&)[3])> toLocal = [&](float (&at)[3]) { toLot.apply(at, true); };
    for (int y = area.y0; y < area.y1; ++y) {
        for (int x = area.x0; x < area.x1; ++x) {
            const bool water = wet(x, y) || wet(x + 1, y) || wet(x + 1, y + 1) || wet(x, y + 1);
            if (!water && !(beachLot && beach(x, y) && beach(x + 1, y) && beach(x + 1, y + 1) && beach(x, y + 1))) {
                continue;
            }
            cells.push_back({x, y});
            if (poolStyle) {
                // Seen close up in lot view, it fades in over the first 1.5 units of depth, so the
                // bank goes under the water softly. Tiles by the shore are split
                // finer, so its edge follows the drawn ground's own waterline instead of running
                // straight from corner to corner.
                constexpr float kLotShoreFade = 1.5f, kShallow = 2;
                constexpr int kShoreSteps = 8;
                bool shore = false;
                for (const auto& [cx, cy] : {std::pair{x, y}, std::pair{x + 1, y}, std::pair{x + 1, y + 1}, std::pair{x, y + 1}}) {
                    shore = shore || z - ground(cx, cy) < kShallow;
                }
                addPoolStyleSurface(p, terrain, x, y, surfaceAt, toLocal, kLotShoreFade, shore ? kShoreSteps : 1);
                continue;
            }
            const auto base = static_cast<std::uint32_t>(p.positions.size() / 3);
            for (const auto& [cx, cy] : {std::pair{x, y}, std::pair{x + 1, y}, std::pair{x + 1, y + 1}, std::pair{x, y + 1}}) {
                float at[3] = {static_cast<float>(cx) * tile, static_cast<float>(cy) * tile, z};
                toLot.apply(at, true);
                p.positions.insert(p.positions.end(), {at[0], at[1], at[2]});
                p.normals.insert(p.normals.end(), {0, 0, 1});
                constexpr float kDeepest = 15, kPerUnit = 0.04f;
                const float depth = std::clamp(z - ground(cx, cy), 0.0f, kDeepest) * kPerUnit;
                p.uvs.insert(p.uvs.end(), {depth, depth});
            }
            p.indices.insert(p.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
    }
    if (p.indices.empty()) {
        return cells;
    }
    // A sea drawn with the pools' caustics (not in the original game) lights them as a pool does,
    // over its box from the lowest ground under it to its surface, in the lot's coordinates.
    std::array<float, 6> box{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    for (const auto& [x, y] : cells) {
        for (const auto& [cx, cy] : {std::pair{x, y}, std::pair{x + 1, y}, std::pair{x + 1, y + 1}, std::pair{x, y + 1}}) {
            for (const float h : {ground(cx, cy), z}) {
                float at[3] = {static_cast<float>(cx) * tile, static_cast<float>(cy) * tile, h};
                toLot.apply(at, true);
                for (std::size_t k = 0; k < 3; ++k) {
                    box[k] = std::min(box[k], at[k]);
                    box[k + 3] = std::max(box[k + 3], at[k]);
                }
            }
        }
    }
    if (auto* i = builder.place(builder.addMesh("lot skirt sea", std::move(sea)), "lot skirt sea",
                                Transform::at(0, 0, -kGroundDrop, 0) * lot.place, Part::Other);
        i != nullptr && poolStyle) {
        i->poolWater = true;
        i->poolBox = box;
    }
    return cells;
}

// Not in the original game: the pools' caustics on the ground under a pool-style sea. They fade in over
// 1.5 from the waterline, by the smaller of the real depth and its average over 6 around, so a sudden
// drop shades in gradually, and over the lot depth counts half (see depthAt). The haze over the ground
// is drawn with the ground itself (scene.frag's poolSeaHaze and terrain.frag), which dims them deeper.
// `groundAt` takes neighborhood coordinates. Built in the lot's coordinates as the sea is. Without a
// lot it is the neighborhood view's sea, built in the neighborhood's coordinates, a quad to a tile on
// the terrain's own points.
void addPoolStyleSea(SceneBuilder& builder, const PaintedTerrain& terrain, const LotFootprint* lot, const std::vector<std::array<int, 2>>& cells,
                     const std::function<float(float, float)>& groundAt) {
    // Quads a neighborhood tile is split into each way: over and beside the lot four to a lot tile,
    // on the points the lot's terrain is drawn through (its ground can drop steeply within a tile),
    // 2 apart elsewhere.
    constexpr int kLotSteps = 40, kSteps = 5;
    // The caustics reach full strength this deep.
    constexpr float kCausticsFade = 1.5f;
    // The caustics lie this far off the ground, out along its normal, so they stay in front of it on
    // steep faces too.
    constexpr float kLift = 0.03f;
    const float tile = terrain.tileSize, sea = terrain.waterLevel;
    const Transform toLot = lot ? lot->place.rigidInverse() : Transform{};
    Surfaces surfaces;
    // As bright on steep faces as on the bottom (caustics 4, see scene::Material::caustics).
    const int causticsMaterial = builder.causticsOverlayMaterial("sea caustics");
    if (causticsMaterial >= 0) {
        builder.scene().materials[static_cast<std::size_t>(causticsMaterial)].caustics = 4;
        builder.scene().materials[static_cast<std::size_t>(causticsMaterial)].seaDepthOverlay = true;
    }
    using Corners = std::array<std::array<float, 3>, 4>;
    // Corners in order around the quad, and how far under the water each is (negative above it). Over
    // the lot it is split as the lot's terrain splits its quarter tiles, from their low x and y corner
    // to their high one in the lot's coordinates, elsewhere as the skirt splits its own, the same way in
    // the neighborhood's. Each triangle is cut at the waterline, so nothing of it lies on dry ground.
    struct WetPoint {
        std::array<float, 3> at, normal;
        float alpha, wet;
    };
    auto addQuad = [&](int material, const Corners& corners, const std::array<float, 4>& alphas, const Corners& normals,
                       const std::array<float, 4>& wet, bool onLot) {
        Primitive& p = surfaces[material];
        std::array<WetPoint, 4> local{};
        for (std::size_t c = 0; c < 4; ++c) {
            float at[3] = {corners[c][0], corners[c][1], corners[c][2]};
            toLot.apply(at, true);
            float n[3] = {normals[c][0], normals[c][1], normals[c][2]};
            toLot.apply(n, false);
            local[c] = {{at[0], at[1], at[2]}, {n[0], n[1], n[2]}, alphas[c], wet[c]};
        }
        const auto& l = local;
        const bool across = onLot && std::abs((l[0].at[0] - l[0].at[1]) - (l[2].at[0] - l[2].at[1])) > 0.1f;
        const std::array<std::array<std::size_t, 3>, 2> triangles = across ? std::array<std::array<std::size_t, 3>, 2>{{{0, 1, 3}, {1, 2, 3}}}
                                                                           : std::array<std::array<std::size_t, 3>, 2>{{{0, 1, 2}, {0, 2, 3}}};
        for (const auto& triangle : triangles) {
            // The part under the water: at most four points, a new one where an edge meets the waterline.
            std::array<WetPoint, 4> kept{};
            std::size_t count = 0;
            for (std::size_t e = 0; e < 3; ++e) {
                const WetPoint& a = l[triangle[e]];
                const WetPoint& b = l[triangle[(e + 1) % 3]];
                if (a.wet >= 0) {
                    kept[count++] = a;
                }
                if ((a.wet >= 0) != (b.wet >= 0)) {
                    const float s = a.wet / (a.wet - b.wet);
                    WetPoint cut{};
                    for (std::size_t k = 0; k < 3; ++k) {
                        cut.at[k] = a.at[k] + (b.at[k] - a.at[k]) * s;
                        cut.normal[k] = a.normal[k] + (b.normal[k] - a.normal[k]) * s;
                    }
                    const float length = std::sqrt(cut.normal[0] * cut.normal[0] + cut.normal[1] * cut.normal[1] + cut.normal[2] * cut.normal[2]);
                    for (float& v : cut.normal) {
                        v = length > 0 ? v / length : v;
                    }
                    kept[count++] = cut;
                }
            }
            if (count < 3) {
                continue;
            }
            const auto base = static_cast<std::uint32_t>(p.positions.size() / 3);
            for (std::size_t k = 0; k < count; ++k) {
                const WetPoint& point = kept[k];
                p.positions.insert(p.positions.end(), point.at.begin(), point.at.end());
                p.normals.insert(p.normals.end(), point.normal.begin(), point.normal.end());
                p.uvs.insert(p.uvs.end(), {0, 0});
                p.alphas.push_back(point.alpha);
            }
            for (std::uint32_t k = 1; k + 1 < count; ++k) {
                p.indices.insert(p.indices.end(), {base, base + k, base + k + 1});
            }
        }
    };
    auto depthAt = [&](float x, float y, float ground) {
        constexpr float kAround = 6, kStep = 3;
        float sum = 0;
        int n = 0;
        for (float dx = -kAround; dx <= kAround; dx += kStep) {
            for (float dy = -kAround; dy <= kAround; dy += kStep) {
                sum += std::max(sea - groundAt(x + dx, y + dy), 0.0f);
                ++n;
            }
        }
        // The water over the lot counts half as deep, back to its own depth 6 out from the lot's
        // edge, so the lot's beach stays light where its ground drops away.
        constexpr float kLotDepth = 0.5f, kLotEase = 6;
        const float outside = lot ? std::max({lot->x0 - x, x - lot->x1, lot->y0 - y, y - lot->y1, 0.0f}) : kLotEase;
        const float scale = kLotDepth + (1 - kLotDepth) * std::min(outside / kLotEase, 1.0f);
        return std::min(sea - ground, sum / static_cast<float>(n)) * scale;
    };
    auto normalAt = [&](float x, float y) {
        constexpr float d = 0.5f;
        const float dx = groundAt(x + d, y) - groundAt(x - d, y), dy = groundAt(x, y + d) - groundAt(x, y - d);
        const float length = std::sqrt(dx * dx + dy * dy + 4 * d * d);
        return std::array<float, 3>{-dx / length, -dy / length, 2 * d / length};
    };
    for (const auto& [cx, cy] : cells) {
        const float x0 = static_cast<float>(cx) * tile, y0 = static_cast<float>(cy) * tile;
        const bool onLot = lot && x0 < lot->x1 && x0 + tile > lot->x0 && y0 < lot->y1 && y0 + tile > lot->y0;
        // Tiles beside the lot share its fine points along their common edge, so the two meet
        // without a crack.
        const bool byLot = lot && x0 <= lot->x1 && x0 + tile >= lot->x0 && y0 <= lot->y1 && y0 + tile >= lot->y0;
        const int steps = byLot ? kLotSteps : lot ? kSteps : 1;
        const float step = tile / static_cast<float>(steps);
        // The tile's grid points, each worked out once for the four quads that share it.
        struct Point {
            float x, y, ground, depth;
            std::array<float, 3> normal;
        };
        const auto side = static_cast<std::size_t>(steps) + 1;
        std::vector<Point> points(side * side);
        for (std::size_t i = 0; i < side; ++i) {
            for (std::size_t j = 0; j < side; ++j) {
                Point& point = points[i * side + j];
                point.x = x0 + static_cast<float>(i) * step;
                point.y = y0 + static_cast<float>(j) * step;
                point.ground = groundAt(point.x, point.y);
                point.depth = depthAt(point.x, point.y, point.ground);
                point.normal = normalAt(point.x, point.y);
            }
        }
        for (std::size_t i = 0; i + 1 < side; ++i) {
            for (std::size_t j = 0; j + 1 < side; ++j) {
                std::array<std::array<float, 2>, 4> xy{};
                std::array<float, 4> ground{}, depth{};
                std::array<std::array<float, 3>, 4> normals{};
                for (std::size_t c = 0; c < 4; ++c) {
                    const std::size_t di = c == 1 || c == 2 ? 1 : 0, dj = c >= 2 ? 1 : 0;
                    const Point& point = points[(i + di) * side + j + dj];
                    xy[c] = {point.x, point.y};
                    ground[c] = point.ground;
                    depth[c] = point.depth;
                    normals[c] = point.normal;
                }
                std::array<float, 4> wet{};
                for (std::size_t c = 0; c < 4; ++c) {
                    wet[c] = sea - ground[c];
                }
                if (causticsMaterial >= 0) {
                    // The depth under the water over kCausticsFade, negative above it and at most 1, so
                    // the shader's own dimming with depth (for murky ponds) stays at its first step.
                    std::array<float, 4> alphas{};
                    Corners corners{};
                    for (std::size_t c = 0; c < 4; ++c) {
                        alphas[c] = std::min(depth[c] / kCausticsFade, 1.0f);
                        corners[c] = {xy[c][0] + normals[c][0] * kLift, xy[c][1] + normals[c][1] * kLift, ground[c] + normals[c][2] * kLift};
                    }
                    if (*std::max_element(alphas.begin(), alphas.end()) > 0) {
                        addQuad(causticsMaterial, corners, alphas, normals, wet, onLot);
                    }
                }
            }
        }
    }
    if (!surfaces.empty()) {
        const std::string name = lot ? "lot skirt sea depth" : kNeighborhoodSeaDepthName;
        builder.place(builder.addMesh(name, std::move(surfaces)), name, lot ? Transform::at(0, 0, -kGroundDrop, 0) * lot->place : Transform{}, Part::Other);
    }
}

// The neighborhood's ground at (x, y), on its terrain's triangles.
float neighborhoodGround(const PaintedTerrain& t, float x, float y) {
    const float fx = std::clamp(x / t.tileSize, 0.0f, static_cast<float>(t.columns - 1)), fy = std::clamp(y / t.tileSize, 0.0f, static_cast<float>(t.rows - 1));
    const auto ix = std::min(static_cast<std::uint32_t>(fx), t.columns - 2), iy = std::min(static_cast<std::uint32_t>(fy), t.rows - 2);
    const float u = fx - static_cast<float>(ix), v = fy - static_cast<float>(iy);
    const std::size_t a = static_cast<std::size_t>(iy) * t.columns + ix;
    const float h00 = t.heights[a], h10 = t.heights[a + 1], h01 = t.heights[a + t.columns], h11 = t.heights[a + t.columns + 1];
    // Split from the tile's (x, y) corner to its (x + 1, y + 1) corner.
    return u >= v ? h00 + u * (h10 - h00) + v * (h11 - h10) : h00 + v * (h01 - h00) + u * (h11 - h01);
}

// The skirt's ground on the neighborhood's grid around a lot, in the neighborhood's coordinates:
// heights, normals and fades at its grid points, and its height at any point.
struct SkirtGround {
    // The lot's own ground is drawn through the same points as the lot's terrain (see
    // Heights::drawnGrid), four to a tile.
    static constexpr std::uint32_t kLotSteps = 4;
    // A tile joined to the lot is split into this many quads each way.
    static constexpr int kJoinSteps = 40;

    const PaintedTerrain& terrain;
    const LotFootprint& lot;
    SkirtArea area;
    float tile = 0;
    // The lot's tiles on the neighborhood's grid.
    int lx0 = 0, ly0 = 0, lx1 = 0, ly1 = 0;
    Transform toLot;
    const Heights* lotHeights = nullptr;
    int groundLevel = 0;
    bool lotGrid = false;
    // Not in the original game: with smooth lot skirts, a skirt tile sharing an edge with the lot
    // follows the lot's own ground along that edge, where the game's meets it only at its tile
    // corners and leaves a sliver of skirt between them. The difference eases out across the tile,
    // so its other edges and its corners stay the game's.
    bool joinLot = false;
    std::vector<float> drawn;
    std::uint32_t drawnColumns = 0, drawnRows = 0;

    SkirtGround(const PaintedTerrain& t, const LotFootprint& l, const SkirtArea& a, const Heights* heights, int level, bool smooth)
        : terrain(t), lot(l), area(a), tile(t.tileSize), toLot(l.place.rigidInverse()), lotHeights(heights), groundLevel(level),
          lotGrid(heights != nullptr && heights->grid() != nullptr), joinLot(smooth && lotGrid) {
        lx0 = static_cast<int>(std::lround(lot.x0 / tile));
        ly0 = static_cast<int>(std::lround(lot.y0 / tile));
        lx1 = static_cast<int>(std::lround(lot.x1 / tile));
        ly1 = static_cast<int>(std::lround(lot.y1 / tile));
        if (!lotGrid) {
            return;
        }
        std::optional<float> lotSea;
        if (smooth) {
            float w[3] = {0, 0, terrain.waterLevel};
            toLot.apply(w, true);
            lotSea = w[2];
        }
        drawn = lotHeights->drawnGrid(groundLevel, kLotSteps, lotSea);
        drawnColumns = (lotHeights->grid()->width - 1) * kLotSteps + 1;
        drawnRows = (lotHeights->grid()->height - 1) * kLotSteps + 1;
    }

    // Whether tile (x, y) is the lot's own.
    bool onLot(int x, int y) const { return x >= lx0 && x < lx1 && y >= ly0 && y < ly1; }

    // A grid point. Points on the lot's edge take the lot's own corner heights.
    std::array<float, 3> corner(int x, int y) const {
        const float wx = static_cast<float>(x) * tile, wy = static_cast<float>(y) * tile;
        const bool onLotEdge = (x == lx0 || x == lx1 || y == ly0 || y == ly1) && x >= lx0 && x <= lx1 && y >= ly0 && y <= ly1;
        if (lotGrid && onLotEdge) {
            float p[3] = {wx, wy, 0};
            toLot.apply(p, true);
            p[2] = lotHeights->corner(groundLevel, p[0], p[1]);
            lot.place.apply(p, true);
            return std::array<float, 3>{wx, wy, p[2]};
        }
        const std::size_t i = static_cast<std::size_t>(y) * terrain.columns + static_cast<std::size_t>(x);
        return std::array<float, 3>{wx, wy, terrain.heights[i]};
    }

    std::array<float, 3> normal(int x, int y) const {
        const std::size_t i = (static_cast<std::size_t>(y) * terrain.columns + static_cast<std::size_t>(x)) * 3;
        if (i + 2 >= terrain.normals.size()) {
            return std::array<float, 3>{0, 0, 1};
        }
        const std::array<float, 3> n{terrain.normals[i], terrain.normals[i + 1], terrain.normals[i + 2]};
        return n[0] == 0 && n[1] == 0 && n[2] == 0 ? std::array<float, 3>{0, 0, 1} : n;
    }

    // How opaque the skirt is at a grid point: it fades out over its two outermost rings of points.
    float fade(int x, int y) const {
        if (x == area.x0 || x == area.x1 || y == area.y0 || y == area.y1) {
            return 0.0f;
        }
        if (x == area.x0 + 1 || x == area.x1 - 1 || y == area.y0 + 1 || y == area.y1 - 1) {
            return 0.5f;
        }
        return 1.0f;
    }

    // The lot's own ground at a point: its terrain blends its tile corners' heights.
    float lotGround(float wx, float wy) const {
        float p[3] = {wx, wy, 0};
        toLot.apply(p, true);
        const float u = std::clamp(p[0] * kLotSteps, 0.0f, static_cast<float>(drawnColumns - 1));
        const float v = std::clamp(p[1] * kLotSteps, 0.0f, static_cast<float>(drawnRows - 1));
        const auto i0 = std::min(static_cast<std::uint32_t>(u), drawnColumns - 2), j0 = std::min(static_cast<std::uint32_t>(v), drawnRows - 2);
        const float fx = u - static_cast<float>(i0), fy = v - static_cast<float>(j0);
        auto at = [&](std::uint32_t i, std::uint32_t j) { return drawn[static_cast<std::size_t>(i) * drawnRows + j]; };
        const float low = at(i0, j0) * (1 - fx) + at(i0 + 1, j0) * fx;
        const float high = at(i0, j0 + 1) * (1 - fx) + at(i0 + 1, j0 + 1) * fx;
        p[2] = low * (1 - fy) + high * fy;
        lot.place.apply(p, true);
        return p[2];
    }

    // The skirt's ground in tile (x, y) at a point: two triangles a tile, as the game builds it.
    float skirtTriangles(int x, int y, float wx, float wy) const {
        const float fx = std::clamp(wx / tile - static_cast<float>(x), 0.0f, 1.0f), fy = std::clamp(wy / tile - static_cast<float>(y), 0.0f, 1.0f);
        const float a = corner(x, y)[2], b = corner(x + 1, y)[2], c = corner(x + 1, y + 1)[2], d = corner(x, y + 1)[2];
        return fx >= fy ? a + fx * (b - a) + fy * (c - b) : a + fy * (d - a) + fx * (c - d);
    }

    struct SharedEdge {
        bool alongY = false;
        float at = 0;
    };
    // The edge tile (x, y) shares with the lot, and whether it runs along y, if it has one and
    // follows the lot's ground there.
    std::optional<SharedEdge> sharedEdge(int x, int y) const {
        if (!joinLot) {
            return std::nullopt;
        }
        if (y >= ly0 && y < ly1 && (x == lx1 || x == lx0 - 1)) {
            return SharedEdge{true, static_cast<float>(x == lx1 ? lx1 : lx0) * tile};
        }
        if (x >= lx0 && x < lx1 && (y == ly1 || y == ly0 - 1)) {
            return SharedEdge{false, static_cast<float>(y == ly1 ? ly1 : ly0) * tile};
        }
        return std::nullopt;
    }

    // The skirt's ground in tile (x, y) at a point.
    float tileGround(int x, int y, float wx, float wy) const {
        float h = skirtTriangles(x, y, wx, wy);
        if (const auto edge = sharedEdge(x, y)) {
            const float ex = edge->alongY ? edge->at : wx, ey = edge->alongY ? wy : edge->at;
            const float away = edge->alongY ? std::abs(wx - edge->at) : std::abs(wy - edge->at);
            h += (lotGround(ex, ey) - skirtTriangles(x, y, ex, ey)) * std::max(0.0f, 1 - away / tile);
        }
        return h;
    }

    // The ground at a point: the lot's own on the lot, else the skirt's.
    float surfaceAt(float wx, float wy) const {
        if (lotGrid && wx >= lot.x0 && wx <= lot.x1 && wy >= lot.y0 && wy <= lot.y1) {
            return lotGround(wx, wy);
        }
        const int ix = std::clamp(static_cast<int>(std::floor(wx / tile)), 0, static_cast<int>(terrain.columns) - 2);
        const int iy = std::clamp(static_cast<int>(std::floor(wy / tile)), 0, static_cast<int>(terrain.rows) - 2);
        return tileGround(ix, iy, wx, wy);
    }

    // A tile's quads: the whole tile, or a tile joined to the lot in kJoinSteps quads each way. Calls
    // `add(fx, fy)` for each point (fractions across the tile, corners in order), then indexes them.
    void tileQuads(Primitive& p, int x, int y, const std::function<void(float, float)>& add) const {
        const int steps = sharedEdge(x, y) ? kJoinSteps : 1;
        for (int i = 0; i < steps; ++i) {
            for (int j = 0; j < steps; ++j) {
                const auto base = static_cast<std::uint32_t>(p.positions.size() / 3);
                const float f0 = static_cast<float>(i) / static_cast<float>(steps), f1 = static_cast<float>(i + 1) / static_cast<float>(steps);
                const float g0 = static_cast<float>(j) / static_cast<float>(steps), g1 = static_cast<float>(j + 1) / static_cast<float>(steps);
                add(f0, g0);
                add(f1, g0);
                add(f1, g1);
                add(f0, g1);
                p.indices.insert(p.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
            }
        }
    }
};

// A value at a point in a tile from its four corners' values (in order around it).
float blendCorners(const std::array<float, 4>& v, float fx, float fy) {
    return (v[0] * (1 - fx) + v[1] * fx) * (1 - fy) + (v[3] * (1 - fx) + v[2] * fx) * fy;
}

// The ground: two triangles a tile, its texture once a tile (from the tile's grid position).
// Tiles touching the fading rings are blended, the rest opaque. Returns the ground's material, or
// -1 without its texture.
int addSkirtGround(SceneBuilder& builder, const SkirtGround& g) {
    Surfaces ground;
    const int material = builder.textureMaterial(g.lot.texture);
    int faded = -1;
    if (material >= 0) {
        builder.scene().materials[static_cast<std::size_t>(material)].layer = kSkirtLayer;
        Material m = builder.scene().materials[static_cast<std::size_t>(material)];
        m.name += " (fading)";
        m.alpha = AlphaMode::Blend;
        builder.scene().materials.push_back(std::move(m));
        faded = static_cast<int>(builder.scene().materials.size() - 1);
    }
    const float tile = g.tile;
    for (int y = g.area.y0; y < g.area.y1; ++y) {
        for (int x = g.area.x0; x < g.area.x1; ++x) {
            if (g.onLot(x, y)) {
                continue;  // the lot's own ground
            }
            const std::array<std::array<int, 2>, 4> c{{{x, y}, {x + 1, y}, {x + 1, y + 1}, {x, y + 1}}};
            bool solid = true;
            for (const auto& [cx, cy] : c) {
                solid = solid && g.fade(cx, cy) >= 1.0f;
            }
            Primitive& p = ground[solid ? material : faded];
            std::array<float, 4> fades{};
            std::array<std::array<float, 3>, 4> normals{};
            for (std::size_t k = 0; k < 4; ++k) {
                fades[k] = g.fade(c[k][0], c[k][1]);
                normals[k] = g.normal(c[k][0], c[k][1]);
            }
            g.tileQuads(p, x, y, [&](float fx, float fy) {
                const float wx = (static_cast<float>(x) + fx) * tile, wy = (static_cast<float>(y) + fy) * tile;
                p.positions.insert(p.positions.end(), {wx, wy, g.tileGround(x, y, wx, wy)});
                std::array<float, 3> n{};
                if (g.sharedEdge(x, y)) {
                    // A tile following the lot's edge is lit by its own slope, turned up under the
                    // sea, as the lot's ground beside it is, so the two meet without a line of light
                    // and shade.
                    constexpr float d = 0.25f;
                    const float sx = (g.surfaceAt(wx + d, wy) - g.surfaceAt(wx - d, wy)) / (2 * d);
                    const float sy = (g.surfaceAt(wx, wy + d) - g.surfaceAt(wx, wy - d)) / (2 * d);
                    const float length = std::sqrt(sx * sx + sy * sy + 1);
                    n = lotlightmaps::underwaterNormal({-sx / length, -sy / length, 1 / length}, g.terrain.waterLevel - g.tileGround(x, y, wx, wy));
                } else {
                    for (std::size_t k = 0; k < 3; ++k) {
                        n[k] = blendCorners({normals[0][k], normals[1][k], normals[2][k], normals[3][k]}, fx, fy);
                    }
                }
                p.normals.insert(p.normals.end(), n.begin(), n.end());
                p.uvs.insert(p.uvs.end(), {static_cast<float>(x) + fx, static_cast<float>(y) + fy});
                if (!solid) {
                    p.alphas.push_back(blendCorners(fades, fx, fy));
                }
            });
        }
    }
    // The boundary curtain (the game's lotSkirtBoundaryClip): from 200 below the lowest corner of the
    // skirt to 200 above the lot.
    float lowest = std::numeric_limits<float>::max();
    const SkirtArea& area = g.area;
    for (const auto& [cx, cy] : {std::pair{area.x0, area.y0}, std::pair{area.x1, area.y0}, std::pair{area.x1, area.y1}, std::pair{area.x0, area.y1}}) {
        lowest = std::min(lowest, g.corner(cx, cy)[2]);
    }
    builder.scene().clip = Scene::BoundaryClip{static_cast<float>(area.x0) * tile, static_cast<float>(area.y0) * tile,
                                               static_cast<float>(area.x1) * tile, static_cast<float>(area.y1) * tile,
                                               lowest - 200, g.lot.z + 200};

    // As low as the lot's own ground, which meets it at the lot's edges.
    if (auto* i = builder.place(builder.addMesh("lot skirt", std::move(ground)), "lot skirt", Transform::at(0, 0, -kGroundDrop, 0), Part::Skirt)) {
        i->seaReflected = true;
    }
    return material;
}

// The sand over the ground by the water, as the skirt's two sand passes draw it: "lot-sand-02"
// from the water's level up to 2 above it, and "terrain-deepwatersand" from 20 below it up to 5
// below, each fading out along its height range and with the ground's edge fade. Both only
// where some point within 2 tiles has ground at or below the water (points off the neighborhood
// count as 0 high), except that the first covers all ground below the water.
void addSkirtSand(SceneBuilder& builder, const SkirtGround& g) {
    const float sea = g.terrain.waterLevel, tile = g.tile;
    const int cols = static_cast<int>(g.terrain.columns), rows = static_cast<int>(g.terrain.rows);
    auto nearWater = [&](int x, int y) {
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                const int nx = x + dx, ny = y + dy;
                const float z = nx >= 0 && ny >= 0 && nx < cols && ny < rows ? g.corner(nx, ny)[2] : 0.0f;
                if (z <= sea) {
                    return true;
                }
            }
        }
        return false;
    };
    // How much of each sand shows at a grid point (0 to 1), before the edge fade.
    auto sandAt = [&](int x, int y) {
        const float z = g.corner(x, y)[2];
        const bool near = nearWater(x, y);
        const float h = z - sea, deep = z - (sea - 20);
        const float shallowRamp = h < 0 ? 0 : h < 2 && near ? h * 0.5f : 1;
        const float deepRamp = deep < 0 ? 0 : deep < 15 && near ? deep / 15 : 1;
        return std::array<float, 2>{1 - shallowRamp, 1 - deepRamp};
    };
    const std::array<const char*, 2> sandTextures{"lot-sand-02", "terrain-deepwatersand"};
    for (std::size_t k = 0; k < sandTextures.size(); ++k) {
        // Drawn after the ground, the deep water's sand last.
        const int sandMaterial = builder.blendedTextureMaterial(sandTextures[k], kSkirtLayer + 1 + 2 * static_cast<int>(k));
        if (sandMaterial < 0) {
            continue;
        }
        Surfaces sand;
        Primitive& p = sand[sandMaterial];
        for (int y = g.area.y0; y < g.area.y1; ++y) {
            for (int x = g.area.x0; x < g.area.x1; ++x) {
                if (g.onLot(x, y)) {
                    continue;
                }
                const std::array<std::array<int, 2>, 4> c{{{x, y}, {x + 1, y}, {x + 1, y + 1}, {x, y + 1}}};
                std::array<float, 4> alphas{};
                bool shows = false;
                for (std::size_t i = 0; i < c.size(); ++i) {
                    alphas[i] = sandAt(c[i][0], c[i][1])[k] * g.fade(c[i][0], c[i][1]);
                    shows = shows || alphas[i] > 0;
                }
                if (!shows) {
                    continue;
                }
                g.tileQuads(p, x, y, [&](float fx, float fy) {
                    const float wx = (static_cast<float>(x) + fx) * tile, wy = (static_cast<float>(y) + fy) * tile;
                    p.positions.insert(p.positions.end(), {wx, wy, g.tileGround(x, y, wx, wy)});
                    p.normals.insert(p.normals.end(), {0, 0, 1});
                    p.uvs.insert(p.uvs.end(), {static_cast<float>(x) + fx, static_cast<float>(y) + fy});
                    p.alphas.push_back(blendCorners(alphas, fx, fy));
                });
            }
        }
        if (p.indices.empty()) {
            continue;
        }
        if (auto* i = builder.place(builder.addMesh(sandTextures[k], std::move(sand)), "lot skirt sand", Transform::at(0, 0, -kGroundDrop, 0))) {
            i->skirtSand = static_cast<int>(k) + 1;
            i->seaReflected = true;
        }
    }
}

// Not in the original game: with smooth lot skirts the lot's own ground, its canvas and paints,
// carries on over the skirt beside it and fades out 4 from the lot's edge, so the change from
// one to the other is soft. Each paint is as strong as at the nearest point of the lot's edge,
// taken one point in, as the lot's saved strength maps hold 0 along their last row and column.
// `material` is the skirt ground's, whose lighting the blend keeps.
void addLotGroundBlend(SceneBuilder& builder, const SkirtGround& g, const LotGroundLook& look, int material) {
    constexpr float kBlend = 4, kRepeat = 20, kShade = 0.9f, kLift = 0.01f;
    constexpr int kDetailRepeats = 5;
    // Lit as the skirt's own ground is, with the lot's texture.
    auto blended = [&](int m, int layer) {
        if (m < 0 || material < 0) {
            return -1;
        }
        Material copy = builder.scene().materials[static_cast<std::size_t>(material)];
        const Material& source = builder.scene().materials[static_cast<std::size_t>(m)];
        copy.image = source.image;
        copy.terrain = source.terrain;
        copy.detail = source.detail;
        copy.detailRepeats = source.detailRepeats;
        copy.name = builder.scene().materials[static_cast<std::size_t>(m)].name + " (skirt blend)";
        copy.alpha = AlphaMode::Blend;
        copy.layer = layer;
        builder.scene().materials.push_back(std::move(copy));
        return static_cast<int>(builder.scene().materials.size() - 1);
    };
    // Over the skirt's ground and sand, the canvas first, then each paint.
    std::vector<int> layers{blended(builder.terrainMaterial(look.canvas, kDetailRepeats, kShade, false), kSkirtLayer + 4)};
    for (std::size_t n = 0; n < look.paints.size(); ++n) {
        layers.push_back(blended(builder.terrainMaterial(look.paints[n].first, kDetailRepeats, kShade, true), kSkirtLayer + 5 + static_cast<int>(n)));
    }
    const float lotX = static_cast<float>(g.lotHeights->grid()->width - 1), lotY = static_cast<float>(g.lotHeights->grid()->height - 1);
    auto strength = [&](const ts2::terrain::Array2D& map, float lx, float ly) {
        if (map.width < 2 || map.height < 2) {
            return 0.0f;
        }
        const auto i = std::clamp(static_cast<std::int32_t>(std::lround(std::clamp(lx, 0.0f, lotX) * 4)), 0, static_cast<std::int32_t>(map.width) - 2);
        const auto j = std::clamp(static_cast<std::int32_t>(std::lround(std::clamp(ly, 0.0f, lotY) * 4)), 0, static_cast<std::int32_t>(map.height) - 2);
        return static_cast<float>(map.byte(i, j)) / 255;
    };
    const LotFootprint& lot = g.lot;
    const float tile = g.tile;
    Surfaces band;
    for (int y = g.area.y0; y < g.area.y1; ++y) {
        for (int x = g.area.x0; x < g.area.x1; ++x) {
            if (g.onLot(x, y)) {
                continue;
            }
            const float tx0 = static_cast<float>(x) * tile, ty0 = static_cast<float>(y) * tile;
            const float away = std::max({lot.x0 - (tx0 + tile), tx0 - lot.x1, lot.y0 - (ty0 + tile), ty0 - lot.y1, 0.0f});
            if (away >= kBlend) {
                continue;
            }
            for (std::size_t k = 0; k < layers.size(); ++k) {
                if (layers[k] < 0) {
                    continue;
                }
                Primitive& p = band[layers[k]];
                const auto before = p.indices.size();
                bool shows = false;
                constexpr int kSteps = 40;
                for (int i = 0; i < kSteps; ++i) {
                    for (int j = 0; j < kSteps; ++j) {
                        const auto base = static_cast<std::uint32_t>(p.positions.size() / 3);
                        bool quad = false;
                        for (const auto& [di, dj] : {std::pair{0, 0}, std::pair{1, 0}, std::pair{1, 1}, std::pair{0, 1}}) {
                            const float wx = tx0 + static_cast<float>(i + di) * tile / kSteps, wy = ty0 + static_cast<float>(j + dj) * tile / kSteps;
                            const float d = std::max({lot.x0 - wx, wx - lot.x1, lot.y0 - wy, wy - lot.y1, 0.0f});
                            float l[3] = {wx, wy, 0};
                            g.toLot.apply(l, true);
                            const float fadeOut = std::clamp(1 - d / kBlend, 0.0f, 1.0f) * g.fade(x, y);
                            const float alpha = k == 0 ? fadeOut : strength(look.paints[k - 1].second, l[0], l[1]) * fadeOut;
                            quad = quad || alpha > 0;
                            p.positions.insert(p.positions.end(), {wx, wy, g.tileGround(x, y, wx, wy) + kLift});
                            p.normals.insert(p.normals.end(), {0, 0, 1});
                            p.uvs.insert(p.uvs.end(), {l[0] / kRepeat, l[1] / kRepeat});
                            p.alphas.push_back(alpha);
                        }
                        if (quad) {
                            p.indices.insert(p.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
                            shows = true;
                        } else {
                            p.positions.resize(p.positions.size() - 12);
                            p.normals.resize(p.normals.size() - 12);
                            p.uvs.resize(p.uvs.size() - 8);
                            p.alphas.resize(p.alphas.size() - 4);
                        }
                    }
                }
                if (!shows) {
                    p.indices.resize(before);
                }
            }
        }
    }
    std::erase_if(band, [](const auto& entry) { return entry.second.indices.empty(); });
    if (!band.empty()) {
        if (auto* i = builder.place(builder.addMesh("lot skirt blend", std::move(band)), "lot skirt blend", Transform::at(0, 0, -kGroundDrop, 0),
                                    Part::Skirt)) {
            i->seaReflected = true;
        }
    }
}

// The roads in the area, with the lot view's road textures, blended over the ground. `overBlend`
// draws them after the lot's ground blended over the skirt, which would otherwise cover them.
void addSkirtRoads(SceneBuilder& builder, const SkirtGround& g, const std::string& terrainType, const std::map<std::string, std::string>& roadTextures,
                   bool overBlend) {
    const PaintedTerrain& terrain = g.terrain;
    const LotFootprint& lot = g.lot;
    const SkirtArea& area = g.area;
    const float tile = g.tile;
    Surfaces roads;
    for (const auto& road : terrain.roads) {
        const float cx = (road.corners[0][0] + road.corners[2][0]) / 2, cy = (road.corners[0][1] + road.corners[2][1]) / 2;
        if (cx < static_cast<float>(area.x0) * tile || cx > static_cast<float>(area.x1) * tile || cy < static_cast<float>(area.y0) * tile ||
            cy > static_cast<float>(area.y1) * tile || (cx > lot.x0 && cx < lot.x1 && cy > lot.y0 && cy < lot.y1)) {
            continue;
        }
        char name[96];
        std::snprintf(name, sizeof name, "lotSkirtRoad_%s_%08x", terrainType.c_str(), road.piece);
        const auto texture = roadTextures.find(name);
        if (texture == roadTextures.end()) {
            continue;
        }
        // Lit, blended over the ground, the texture clamped (the game's LotSkirtRoadMaterialDefinition).
        const int m = builder.textureMaterial(texture->second, 1.0f, true);
        builder.scene().materials[static_cast<std::size_t>(m)].alpha = AlphaMode::Blend;
        builder.scene().materials[static_cast<std::size_t>(m)].layer = overBlend ? kSkirtLayer + 40 : kSkirtLayer;
        builder.scene().materials[static_cast<std::size_t>(m)].clamp = true;
        Primitive& r = roads[m];
        const auto base = static_cast<std::uint32_t>(r.positions.size() / 3);
        // Corners lie on the ground (the neighborhood keeps its roads raised for its own view).
        for (const auto& c : road.corners) {
            const int gx = std::clamp(static_cast<int>(std::lround(c[0] / tile)), 0, static_cast<int>(terrain.columns) - 1);
            const int gy = std::clamp(static_cast<int>(std::lround(c[1] / tile)), 0, static_cast<int>(terrain.rows) - 1);
            r.positions.insert(r.positions.end(), {c[0], c[1], g.corner(gx, gy)[2]});
            r.alphas.push_back(g.fade(gx, gy));
            r.normals.insert(r.normals.end(), {0, 0, 1});
            r.uvs.insert(r.uvs.end(), {c[3], c[4]});
        }
        r.indices.insert(r.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
    }
    if (!roads.empty()) {
        if (auto* i = builder.place(builder.addMesh("lot skirt roads", std::move(roads)), "lot skirt roads", Transform{}, Part::Skirt)) {
            i->skirtAmbient = 4;
            i->skirtRoad = true;
        }
    }
}

} // namespace

SkirtArea lotSkirtArea(const PaintedTerrain& terrain, const LotFootprint& lot, int margin) {
    const float tile = terrain.tileSize;
    const int tilesX = static_cast<int>(terrain.columns) - 1, tilesY = static_cast<int>(terrain.rows) - 1;
    SkirtArea a;
    a.x0 = std::max(0, static_cast<int>(std::lround(lot.x0 / tile)) - margin);
    a.y0 = std::max(0, static_cast<int>(std::lround(lot.y0 / tile)) - margin);
    a.x1 = std::min(tilesX, static_cast<int>(std::lround(lot.x1 / tile)) + margin);
    a.y1 = std::min(tilesY, static_cast<int>(std::lround(lot.y1 / tile)) + margin);
    return a;
}

LotGroundLook lotGroundLook(const content::Lot& lot) {
    LotGroundLook look;
    if (!lot.terrainTextures) {
        return look;
    }
    look.canvas = lot.terrainTextures->canvas;
    for (std::size_t n = 0; n < lot.terrainTextures->paints.size(); ++n) {
        if (const auto map = lot.flatGrids.find(content::kFirstPaintMapInstance + static_cast<std::uint32_t>(n)); map != lot.flatGrids.end()) {
            look.paints.emplace_back(lot.terrainTextures->paints[n], map->second);
        }
    }
    return look;
}

void addLotSkirt(SceneBuilder& builder, const PaintedTerrain& terrain, const LotFootprint& lot, int margin,
                 const std::string& terrainType, const std::map<std::string, std::string>& roadTextures,
                 const Heights* lotHeights, int groundLevel, const content::WorldBounds* bounds, const LotGroundLook* look) {
    if (terrain.columns < 2 || terrain.rows < 2) {
        return;
    }
    const SkirtGround g(terrain, lot, lotSkirtArea(terrain, lot, margin), lotHeights, groundLevel, builder.smoothLotSkirts());
    const int material = addSkirtGround(builder, g);
    addSkirtSand(builder, g);
    const bool blendLot = g.joinLot && look != nullptr;
    if (blendLot && !look->canvas.empty()) {
        addLotGroundBlend(builder, g, *look, material);
    }
    const auto surfaceAt = [&](float x, float y) { return g.surfaceAt(x, y); };
    const auto seaCells = addSkirtSea(builder, terrain, lot, g.area, bounds, [&](int x, int y) { return g.corner(x, y)[2]; }, surfaceAt);
    if (builder.poolStyleSea() && !seaCells.empty()) {
        addPoolStyleSea(builder, terrain, &lot, seaCells, surfaceAt);
    }
    addSkirtRoads(builder, g, terrainType, roadTextures, blendLot);
}

void bakeSeaDepth(PaintedTerrain& terrain, const LotFootprint& lot, const Heights* lotHeights, int groundLevel, bool smoothLotSkirts) {
    terrain.seaDepthMap.clear();
    terrain.seaDepthSize = {0, 0};
    if (terrain.columns < 2 || terrain.rows < 2 || terrain.heights.size() < static_cast<std::size_t>(terrain.columns) * terrain.rows) {
        return;
    }
    constexpr int kMarginTiles = 8;
    const SkirtArea area = lotSkirtArea(terrain, lot, kMarginTiles);
    const SkirtGround g(terrain, lot, area, lotHeights, groundLevel, smoothLotSkirts);
    const float tile = terrain.tileSize;
    const float x0 = static_cast<float>(area.x0) * tile, y0 = static_cast<float>(area.y0) * tile;
    const float x1 = static_cast<float>(area.x1) * tile, y1 = static_cast<float>(area.y1) * tile;
    // A point a unit. A lot is at most 64 units a side, so with the margin the map stays under 300 a
    // side. A larger one from a broken neighborhood is left out.
    constexpr int kMostPoints = 512;
    const int w = static_cast<int>(std::lround(x1 - x0)) + 1, h = static_cast<int>(std::lround(y1 - y0)) + 1;
    if (w < 2 || h < 2 || w > kMostPoints || h > kMostPoints) {
        return;
    }
    const float stepX = (x1 - x0) / static_cast<float>(w - 1), stepY = (y1 - y0) / static_cast<float>(h - 1);
    const float sea = terrain.waterLevel;
    std::vector<float> wet(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            const float x = x0 + static_cast<float>(i) * stepX, y = y0 + static_cast<float>(j) * stepY;
            wet[static_cast<std::size_t>(j) * static_cast<std::size_t>(w) + static_cast<std::size_t>(i)] = std::max(sea - g.surfaceAt(x, y), 0.0f);
        }
    }
    // Averaged over 6 around, 3 apart, as far as the map reaches.
    constexpr int kAround = 6, kStep = 3;
    constexpr float kLotDepth = 0.5f, kOutDepth = 2, kEase = 6;
    terrain.seaDepthMap.resize(wet.size() * 2);
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            float sum = 0;
            int n = 0;
            for (int dy = -kAround; dy <= kAround; dy += kStep) {
                for (int dx = -kAround; dx <= kAround; dx += kStep) {
                    const int si = std::clamp(i + dx, 0, w - 1), sj = std::clamp(j + dy, 0, h - 1);
                    sum += wet[static_cast<std::size_t>(sj) * static_cast<std::size_t>(w) + static_cast<std::size_t>(si)];
                    ++n;
                }
            }
            const float x = x0 + static_cast<float>(i) * stepX, y = y0 + static_cast<float>(j) * stepY;
            const float outside = std::max({lot.x0 - x, x - lot.x1, lot.y0 - y, y - lot.y1, 0.0f});
            const std::size_t at = (static_cast<std::size_t>(j) * static_cast<std::size_t>(w) + static_cast<std::size_t>(i)) * 2;
            terrain.seaDepthMap[at] = sum / static_cast<float>(n);
            terrain.seaDepthMap[at + 1] = kLotDepth + (kOutDepth - kLotDepth) * std::min(outside / kEase, 1.0f);
        }
    }
    terrain.seaDepthSize = {w, h};
    terrain.seaDepthArea = {x0, y0, x1, y1};
}

bool addNeighborhoodSea(SceneBuilder& builder, const PaintedTerrain& terrain, bool night) {
    if (terrain.columns < 2 || terrain.rows < 2 || terrain.heights.size() < static_cast<std::size_t>(terrain.columns) * terrain.rows ||
        terrain.waterTiles.size() < static_cast<std::size_t>(terrain.columns - 1) * (terrain.rows - 1)) {
        return false;
    }
    constexpr std::array<float, 3> kSky{0.4f, 0.6f, 0.9f};
    constexpr float kOpacity = 0.4f;
    const int material = builder.skirtSeaMaterial("lotSkirtSea", "poolShape-body-bump", "neighborhood-sky2-envcube", kSky, kOpacity, 6, "causticsTiled");
    if (material < 0) {
        return false;
    }
    const auto groundAt = [&](float x, float y) { return neighborhoodGround(terrain, x, y); };
    const std::function<void(float (&)[3])> same = [](float (&)[3]) {};
    const auto corner = [&](int cx, int cy) { return terrain.heights[static_cast<std::size_t>(cy) * terrain.columns + static_cast<std::size_t>(cx)]; };
    std::vector<std::array<int, 2>> cells;
    Surfaces sea;
    Primitive& p = sea[material];
    const float tile = terrain.tileSize, z = terrain.waterLevel;
    std::array<float, 6> box{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    for (std::uint32_t y = 0; y + 1 < terrain.rows; ++y) {
        for (std::uint32_t x = 0; x + 1 < terrain.columns; ++x) {
            if (!terrain.waterTiles[static_cast<std::size_t>(y) * (terrain.columns - 1) + x]) {
                continue;
            }
            cells.push_back({static_cast<int>(x), static_cast<int>(y)});
            // Seen from far off, the neighborhood's fades over more, so the shallows stay light, one
            // quad to a tile from its corners' heights.
            constexpr float kHoodShoreFade = 2.0f;
            const auto cornerAt = [&](float px, float py) {
                return corner(static_cast<int>(std::lround(px / tile)), static_cast<int>(std::lround(py / tile)));
            };
            addPoolStyleSurface(p, terrain, static_cast<int>(x), static_cast<int>(y), cornerAt, same, kHoodShoreFade, 1);
            for (std::uint32_t dy = 0; dy < 2; ++dy) {
                for (std::uint32_t dx = 0; dx < 2; ++dx) {
                    const float h = terrain.heights[static_cast<std::size_t>(y + dy) * terrain.columns + x + dx];
                    const float at[3] = {static_cast<float>(x + dx) * tile, static_cast<float>(y + dy) * tile, std::min(h, z)};
                    for (std::size_t k = 0; k < 3; ++k) {
                        box[k] = std::min(box[k], at[k]);
                        box[k + 3] = std::max(box[k + 3], k == 2 ? z : at[k]);
                    }
                }
            }
        }
    }
    if (cells.empty()) {
        return false;
    }
    auto* i = builder.place(builder.addMesh(kNeighborhoodSeaName, std::move(sea)), kNeighborhoodSeaName, Transform{}, Part::Other);
    if (i == nullptr) {
        return false;
    }
    i->poolWater = true;
    i->poolBox = box;
    i->poolNight = night ? 0.2f : 0.75f;
    i->seaNight = night;
    addPoolStyleSea(builder, terrain, nullptr, cells, groundAt);
    return true;
}

void addNeighborhoodPonds(SceneBuilder& builder, const PaintedTerrain& terrain, const std::vector<LotFootprint>& lots,
                          const std::filesystem::path& neighborhood, bool night, bool highPictures) {
    constexpr std::array<float, 3> kSky{0.4f, 0.6f, 0.9f};
    constexpr float kOpacity = 0.4f;
    const int material = builder.skirtSeaMaterial("lotSkirtSea", "poolShape-body-bump", "neighborhood-sky2-envcube", kSky, kOpacity, 6, "causticsTiled");
    if (material < 0) {
        return;
    }
    const int poolMaterial = builder.skirtSeaMaterial("neighborhoodPoolWater", "poolShape-body-bump", "neighborhood-sky2-envcube", kSky, kOpacity, 6, "");
    if (poolMaterial >= 0) {
        builder.scene().materials[static_cast<std::size_t>(poolMaterial)].seaPoolWater = true;
    }
    // A lot is at most 64 tiles a side. A larger water grid in a broken file is left out.
    constexpr std::uint32_t kMostCorners = 65;
    // The surface fades in over this depth at the banks, and grows less see-through with depth as the
    // sea's own does, so the imposter's picture of the pond barely shows through deep water.
    constexpr float kShoreFade = 0.5f, kPerUnit = 0.04f, kDeepest = 15;
    const std::string hood = neighborhood.filename().string();
    auto toWorld = [&](const LotFootprint& lot, float x, float y, float z) {
        float at[3] = {x, y, z};
        lot.place.apply(at, true);
        return std::array<float, 3>{at[0], at[1], at[2] + terrain.imposterLift};
    };
    for (const LotFootprint& lot : lots) {
        if (!lot.hasImposter) {
            continue;
        }
        const auto file = neighborhood / "Lots" / (hood + "_Lot" + std::to_string(lot.id) + ".package");
        std::error_code missing;
        if (!std::filesystem::exists(file, missing)) {
            continue;
        }
        content::Lot ground;
        try {
            ground = content::loadLotGround(file);
        } catch (const std::exception&) {
            continue;
        }
        const Heights heights(ground);
        const int level = content::groundGridLevel(ground);
        // Each lot's water is an instance of its own, so that lot view can leave out its own lot's.
        Surfaces ponds;
        // The pools: their surface where the lot's own is, see-through over the imposter's pool.
        if (const auto quarters = poolwater::waterQuarters(ground); poolMaterial >= 0 && !quarters.empty() && quarters.size() <= kMostCorners * kMostCorners * 4) {
            auto height = [&](float x, float y) {
                const float below = x == std::floor(x) && y == std::floor(y) ? heights.corner(level - 1, x, y) : heights.tile(level - 1, x, y);
                return poolwater::surfaceHeight(below);
            };
            const auto s = poolwater::surface(quarters, height);
            Primitive& pool = ponds[poolMaterial];
            const auto base = static_cast<std::uint32_t>(pool.positions.size() / 3);
            for (std::size_t v = 0; v + 2 < s.positions.size(); v += 3) {
                const auto at = toWorld(lot, s.positions[v], s.positions[v + 1], s.positions[v + 2]);
                pool.positions.insert(pool.positions.end(), at.begin(), at.end());
                pool.normals.insert(pool.normals.end(), {0, 0, 1});
                pool.uvs.insert(pool.uvs.end(), {0, 0});
                pool.alphas.push_back(1);
            }
            for (const std::uint32_t i : s.indices) {
                pool.indices.push_back(base + i);
            }
        }
        const auto water = ground.flatGrids.find(content::kWaterHeightGridInstance);
        const bool hasWater = water != ground.flatGrids.end() && water->second.width <= kMostCorners && water->second.height <= kMostCorners;
        Primitive& p = ponds[highPictures && poolMaterial >= 0 ? poolMaterial : material];
        auto groundAt = [&](int x, int y) { return heights.corner(level, static_cast<float>(x), static_cast<float>(y)); };
        for (int y = 0; hasWater && y + 1 < static_cast<int>(water->second.height); ++y) {
            for (int x = 0; x + 1 < static_cast<int>(water->second.width); ++x) {
                // The water's level over the tile: the highest of its corners under water.
                std::optional<float> top;
                for (const auto& [dx, dy] : {std::pair{0, 0}, std::pair{1, 0}, std::pair{1, 1}, std::pair{0, 1}}) {
                    if (const auto w = terrainwater::cornerHeight(water->second, x + dx, y + dy); w && groundAt(x + dx, y + dy) < *w && (!top || *w > *top)) {
                        top = *w;
                    }
                }
                if (!top) {
                    continue;
                }
                const auto base = static_cast<std::uint32_t>(p.positions.size() / 3);
                for (const auto& [cx, cy] : {std::pair{x, y}, std::pair{x + 1, y}, std::pair{x + 1, y + 1}, std::pair{x, y + 1}}) {
                    const auto at = toWorld(lot, static_cast<float>(cx), static_cast<float>(cy), *top);
                    p.positions.insert(p.positions.end(), at.begin(), at.end());
                    p.normals.insert(p.normals.end(), {0, 0, 1});
                    const float depth = *top - groundAt(cx, cy);
                    const float opacity = highPictures ? 0.0f : std::clamp(depth, 0.0f, kDeepest) * kPerUnit;
                    p.uvs.insert(p.uvs.end(), {opacity, opacity});
                    p.alphas.push_back(std::clamp(depth / kShoreFade, 0.0f, 1.0f));
                }
                p.indices.insert(p.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
            }
        }
        std::erase_if(ponds, [](const auto& entry) { return entry.second.indices.empty(); });
        if (ponds.empty()) {
            continue;
        }
        const std::string mesh = std::string(kNeighborhoodPondsName) + " " + std::to_string(lot.id);
        if (auto* i = builder.place(builder.addMesh(mesh, std::move(ponds)), kNeighborhoodPondsName, Transform{}, Part::Other); i != nullptr) {
            i->seaNight = night;
            i->imposterLot = lot.id;
        }
    }
}

std::vector<std::size_t> lightLotSkirt(Scene& scene, bool day) {
    // From the game's "Shaders for lot skirt" material script: by day the summer scale (seasons and
    // clouds pick others), at night a grey for the ground and a blue grey for the roads.
    constexpr std::array<float, 3> kDay{1, 1, 1};
    constexpr std::array<float, 3> kNightGround{0.96f, 0.96f, 0.96f};
    constexpr std::array<float, 3> kNightRoads{0.83f, 0.83f, 0.94f};
    // Its sand passes: one scale by day, and at night a blue for the sand by the water and a grey
    // for the deep water's sand.
    constexpr std::array<float, 4> kSandDay{0.965f, 0.965f, 1, 1};
    constexpr std::array<float, 4> kNightSand{0.3f, 0.3f, 0.7f, 1};
    constexpr std::array<float, 4> kNightDeepSand{0.4f, 0.4f, 0.4f, 1};
    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        auto& instance = scene.instances[i];
        if (instance.name == "lot skirt sea") {
            if (instance.seaNight != !day) {
                instance.seaNight = !day;
                changed.push_back(i);
            }
            continue;
        }
        if (instance.skirtSand != 0) {
            const auto& sand = day ? kSandDay : instance.skirtSand == 1 ? kNightSand : kNightDeepSand;
            if (instance.shapeColor != sand) {
                instance.shapeColor = sand;
                changed.push_back(i);
            }
            continue;
        }
        if (instance.part != Part::Skirt) {
            continue;
        }
        const auto& scale = day ? kDay : instance.skirtRoad ? kNightRoads : kNightGround;
        if (instance.skirtScale != scale) {
            instance.skirtScale = scale;
            changed.push_back(i);
        }
    }
    return changed;
}

} // namespace ts2::scene
