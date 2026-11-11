// The lot's terrain with its paints, and its floors with the slabs and ceilings under them.
#include "scene/lot_scene_builder.hpp"

#include "scene/terrain_water.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>

namespace ts2::scene::lotscene {

// The terrain: a grid of four points to a tile (as the paint strengths are kept), between the
// height grid's corners. Like the game, the canvas texture repeats every 20 tiles and its detail
// every 4, and each paint is laid over the canvas the same way, as strong as its strength there.
void LotSceneBuilder::drawTerrain() {
    const auto* g = heights_.grid();
    if (g == nullptr || g->width < 2 || g->height < 2) {
        return;
    }
    constexpr std::uint32_t kSteps = 4;
    constexpr float kRepeat = 20, kShade = 0.9f;
    constexpr int kDetailRepeats = 5;  // detail every 4 tiles
    const std::uint32_t columns = (g->width - 1) * kSteps + 1, rows = (g->height - 1) * kSteps + 1;
    const int ground = g->contains(content::groundGridLevel(lot_), 0, 0) ? content::groundGridLevel(lot_) : 0;
    std::vector<float> drawn = heights_.drawnGrid(ground, kSteps, b_.edgeSea());
    auto height = [&](std::int64_t i, std::int64_t j) {
        const auto ci = static_cast<std::size_t>(std::clamp<std::int64_t>(i, 0, columns - 1));
        const auto cj = static_cast<std::size_t>(std::clamp<std::int64_t>(j, 0, rows - 1));
        return drawn[ci * rows + cj];
    };

    // The ground is lit by the ground level's floor light map, as the game lights it.
    makeFloorLightMaps();
    const float lotWidth = static_cast<float>(g->width - 1), lotHeight = static_cast<float>(g->height - 1);
    b_.scene().lotSize = {lotWidth, lotHeight};
    Primitive canvas;
    constexpr float step = 1.0f / kSteps;
    for (std::uint32_t i = 0; i < columns; ++i) {
        for (std::uint32_t j = 0; j < rows; ++j) {
            const float x = static_cast<float>(i) * step, y = static_cast<float>(j) * step;
            const float sx = (height(i + 1, j) - height(static_cast<std::int64_t>(i) - 1, j)) / (2 * step);
            const float sy = (height(i, j + 1) - height(i, static_cast<std::int64_t>(j) - 1)) / (2 * step);
            const float len = std::sqrt(sx * sx + sy * sy + 1);
            canvas.positions.insert(canvas.positions.end(), {x, y, height(i, j)});
            canvas.normals.insert(canvas.normals.end(), {-sx / len, -sy / len, 1 / len});
            canvas.uvs.insert(canvas.uvs.end(), {x / kRepeat, y / kRepeat});
            canvas.lightUvs.insert(canvas.lightUvs.end(), {x / lotWidth, y / lotHeight});
        }
    }
    // Ground with no surface (over a pool or a sunken pond) is left open.
    const auto surface = lot_.grids.find(content::kSurfaceGridInstance);
    auto open = [&](std::uint32_t i, std::uint32_t j) {
        return surface != lot_.grids.end() &&
               surfaceOpen(surface->second, ground, (static_cast<float>(i) + 0.5f) * step, (static_cast<float>(j) + 0.5f) * step);
    };
    auto cell = [&](std::vector<std::uint32_t>& indices, std::uint32_t i, std::uint32_t j) {
        if (open(i, j)) {
            return;
        }
        const std::uint32_t a = i * rows + j, b = (i + 1) * rows + j;
        indices.insert(indices.end(), {a, b, b + 1, a, b + 1, a + 1});
    };
    for (std::uint32_t i = 0; i + 1 < columns; ++i) {
        for (std::uint32_t j = 0; j + 1 < rows; ++j) {
            cell(canvas.indices, i, j);
        }
    }

    Mesh mesh;
    mesh.name = "terrain";
    const auto* textures = lot_.terrainTextures ? &*lot_.terrainTextures : nullptr;
    canvas.material = textures ? b_.terrainMaterial(textures->canvas, kDetailRepeats, kShade, false) : -1;
    if (canvas.material < 0) {
        canvas.material = b_.colorMaterial("terrain", {0.32f, 0.45f, 0.25f}, true);
        if (canvas.material >= 0) {
            b_.scene().materials[static_cast<std::size_t>(canvas.material)].terrainCanvas = true;
        }
    }
    mesh.primitives.push_back(canvas);
    for (std::size_t n = 0; textures != nullptr && n < textures->paints.size(); ++n) {
        const auto map = lot_.flatGrids.find(content::kFirstPaintMapInstance + static_cast<std::uint32_t>(n));
        if (map == lot_.flatGrids.end() || map->second.width != columns || map->second.height != rows) {
            continue;
        }
        const auto& strength = map->second;
        Primitive paint;
        for (std::uint32_t i = 0; i + 1 < columns; ++i) {
            for (std::uint32_t j = 0; j + 1 < rows; ++j) {
                const auto x = static_cast<std::int32_t>(i), y = static_cast<std::int32_t>(j);
                // The game draws a paint where its strength is above 10 of 255.
                if (std::max({strength.byte(x, y), strength.byte(x + 1, y), strength.byte(x, y + 1), strength.byte(x + 1, y + 1)}) > 10) {
                    cell(paint.indices, i, j);
                }
            }
        }
        if (paint.indices.empty()) {
            continue;
        }
        paint.material = b_.terrainMaterial(textures->paints[n], kDetailRepeats, kShade, true);
        if (paint.material < 0) {
            continue;
        }
        paint.positions = canvas.positions;
        paint.normals = canvas.normals;
        paint.uvs = canvas.uvs;
        paint.lightUvs = canvas.lightUvs;
        // A lot's saved strength maps can hold 0 along their last row and column, so each paint fades
        // out over the lot's last quarter tile there and the canvas shows as a thin line. Lots
        // with smooth skirts (not in the original game) take the row and column before instead.
        const bool smooth = b_.smoothLotSkirts();
        for (std::uint32_t i = 0; i < columns; ++i) {
            for (std::uint32_t j = 0; j < rows; ++j) {
                const std::uint32_t si = smooth && i + 1 == columns && i > 0 ? i - 1 : i;
                const std::uint32_t sj = smooth && j + 1 == rows && j > 0 ? j - 1 : j;
                paint.alphas.push_back(static_cast<float>(strength.byte(static_cast<std::int32_t>(si), static_cast<std::int32_t>(sj))) / 255);
            }
        }
        mesh.primitives.push_back(std::move(paint));
        ++stats_.terrainPaints;
    }
    // Ponds drawn like pools (not in the original game): the ground under their water shows the
    // pools' caustics, fading in from the waterline to full 0.75 under it (the first depth layer).
    // Each vertex keeps its depth under the water in those units, negative above it, so the fade
    // reaches 0 just where the ground, drawn kGroundDrop lower, meets the water.
    const auto water = lot_.flatGrids.find(content::kWaterHeightGridInstance);
    if (b_.poolStylePonds() && water != lot_.flatGrids.end()) {
        constexpr float kFadeDepth = 0.75f;
        Primitive caustics;
        caustics.alphas.reserve(static_cast<std::size_t>(columns) * rows);
        for (std::uint32_t i = 0; i < columns; ++i) {
            for (std::uint32_t j = 0; j < rows; ++j) {
                const auto top = terrainwater::heightAt(water->second, static_cast<float>(i) * step, static_cast<float>(j) * step);
                caustics.alphas.push_back(top ? (*top - (height(i, j) - kGroundDrop)) / kFadeDepth : -1.0f);
            }
        }
        for (std::uint32_t i = 0; i + 1 < columns; ++i) {
            for (std::uint32_t j = 0; j + 1 < rows; ++j) {
                const std::uint32_t a = i * rows + j, b = (i + 1) * rows + j;
                if (std::max({caustics.alphas[a], caustics.alphas[a + 1], caustics.alphas[b], caustics.alphas[b + 1]}) > 0) {
                    cell(caustics.indices, i, j);
                }
            }
        }
        caustics.material = caustics.indices.empty() ? -1 : b_.causticsOverlayMaterial("pond caustics");
        if (caustics.material >= 0) {
            caustics.positions = canvas.positions;
            caustics.normals = canvas.normals;
            caustics.uvs = canvas.uvs;
            caustics.lightUvs = canvas.lightUvs;
            mesh.primitives.push_back(std::move(caustics));
        }
    }
    b_.scene().meshes.push_back(std::move(mesh));
    if (b_.edgeSea()) {
        b_.scene().drawnGround = {kSteps, columns, rows, std::move(drawn), *b_.edgeSea()};
    }
    // Slightly under the floors standing on it.
    auto* instance = b_.place(static_cast<int>(b_.scene().meshes.size() - 1), "terrain", Transform::at(0, 0, -kGroundDrop, 0));
    if (instance) {
        instance->seaReflected = true;
    }
    if (instance && -lightMaps().lowest >= 0 && static_cast<std::size_t>(-lightMaps().lowest) < lightMaps().floors.size()) {
        // The game's lot terrain: its paint times twice its diffuse (0.9) times the light map, at most 1.
        instance->lightMap = lightMaps().floors[static_cast<std::size_t>(-lightMaps().lowest)];
        instance->lightMapGain = 1.8f;
        instance->lightMapOnly = true;
        instance->lightMapMultiply = true;
    }
}

// Floors: a pattern per quarter tile, each quarter the triangle from one tile edge to the tile's
// center (stored in the order left, bottom, right, top, as the room grid is). Then the slab under
// floors above the ground.
void LotSceneBuilder::drawFloors() {
    const auto floors = lot_.grids.find(content::kFloorGridInstance);
    if (floors == lot_.grids.end() || floors->second.elementSize != 8) {
        return;
    }
    const auto& g = floors->second;
    std::set<int> patterns;
    // Light-map coordinates: the ground level's tiles run on into each other. Above it each tile
    // keeps to its own texels, its edges on their centres, as the game's light-map slots do.
    makeFloorLightMaps();
    auto& maps = lightMaps();
    const float mapWidth = static_cast<float>(g.width), mapHeight = static_cast<float>(g.height);
    const int ground = content::groundGridLevel(lot_);
    const auto surface = lot_.grids.find(content::kSurfaceGridInstance);
    // A quarter on a stage (a split-level platform, region 4) is its own instance: imposters
    // draw it with the floors above the ground.
    const auto regions = lot_.grids.find(content::kPlatformRegionGridInstance);
    const auto onStage = [&](std::int32_t level, std::int32_t x, std::int32_t y, std::size_t quarter) {
        constexpr std::int32_t kStage = 4;
        if (regions == lot_.grids.end() || regions->second.elementSize != 16) {
            return false;
        }
        const auto cell = regions->second.at(level, x, y);
        std::int32_t region = -1;
        if (cell.size() == 16 && quarter < 4) {
            std::memcpy(&region, cell.data() + quarter * sizeof region, sizeof region);
        }
        return region == kStage;
    };
    auto lightUv = [&](std::int32_t level, float tileX, float tileY, float fx, float fy) {
        if (level == ground) {
            return std::array<float, 2>{(tileX + fx) / mapWidth, (tileY + fy) / mapHeight};
        }
        return std::array<float, 2>{(tileX + (0.5f + 7 * fx) / 8) / mapWidth, (tileY + (0.5f + 7 * fy) / 8) / mapHeight};
    };
    for (std::int32_t level = 0; level < static_cast<std::int32_t>(g.levels); ++level) {
        Surfaces surfaces, stageSurfaces;
        for (std::int32_t x = 0; x < static_cast<std::int32_t>(g.width); ++x) {
            for (std::int32_t y = 0; y < static_cast<std::int32_t>(g.height); ++y) {
                const float fx = static_cast<float>(x), fy = static_cast<float>(y);
                const std::array<std::array<float, 2>, 4> corners{{{fx, fy}, {fx + 1, fy}, {fx + 1, fy + 1}, {fx, fy + 1}}};
                std::array<float, 4> h{};
                float center = 0;
                for (std::size_t c = 0; c < 4; ++c) {
                    h[c] = heights_.corner(level, corners[c][0], corners[c][1]) + 0.004f;
                    center += h[c] / 4;
                }
                for (std::size_t q = 0; q < 4; ++q) {
                    const auto id = static_cast<std::uint16_t>(g.i16(level, x, y, 3 - q));
                    const auto pattern = lot_.floorPatterns.find(id);
                    if (id == 0 || pattern == lot_.floorPatterns.end()) {
                        continue;
                    }
                    // A quarter with no surface draws no floor on any storey, as the game gives it
                    // an empty pattern: a hot tub's basin stands below it.
                    if (surface != lot_.grids.end() && surfaceQuarterOpen(surface->second, level, x, y, 3 - q)) {
                        continue;
                    }
                    const int material = b_.material(assets_.patternMaterial(pattern->second.name, false));
                    patterns.insert(material);
                    const bool stage = onStage(level, x, y, 3 - q);
                    auto& s = stage ? stageSurfaces[material] : surfaces[material];
                    const std::size_t n = (q + 1) % 4;
                    // The game adds a stage's height to its floor as it draws it. The saved heights don't hold it.
                    const float lift = stage ? kStageHeight : 0.0f;
                    // Textures run along the game's own tile axes (world y, x here), a repeat
                    // spanning the material's tiles.
                    std::array<float, 2> span{1, 1};
                    if (material >= 0) {
                        span = b_.scene().materials[static_cast<std::size_t>(material)].floorTiles;
                    }
                    addVertex(s, {corners[q][0], corners[q][1], h[q] + lift}, {0, 0, 1}, {corners[q][1] / span[0], corners[q][0] / span[1]});
                    addVertex(s, {corners[n][0], corners[n][1], h[n] + lift}, {0, 0, 1}, {corners[n][1] / span[0], corners[n][0] / span[1]});
                    addVertex(s, {fx + 0.5f, fy + 0.5f, center + lift}, {0, 0, 1}, {(fy + 0.5f) / span[0], (fx + 0.5f) / span[1]});
                    for (const auto& c : {corners[q], corners[n], std::array<float, 2>{fx + 0.5f, fy + 0.5f}}) {
                        const auto uv = lightUv(level, fx, fy, c[0] - fx, c[1] - fy);
                        s.lightUvs.insert(s.lightUvs.end(), uv.begin(), uv.end());
                    }
                }
            }
        }
        for (const bool stage : {false, true}) {
            auto& drawn = stage ? stageSurfaces : surfaces;
            if (stage && drawn.empty()) {
                continue;
            }
            const char* name = stage ? "stage floors" : "floors";
            if (auto* instance = b_.place(b_.addMesh(name, std::move(drawn)), name, Transform{}, Part::Floor, level)) {
                instance->onStage = stage;
                if (static_cast<std::size_t>(level) < maps.floors.size()) {
                    instance->lightMap = maps.floors[static_cast<std::size_t>(level)];
                    instance->incidenceMap = maps.floorIncidence[static_cast<std::size_t>(level)];
                }
            }
        }
    }
    stats_.floorPatterns = patterns.size();

    // The slab under each floored quarter tile above the ground: a ceiling under it and a band down each
    // edge where the floor stops (see visibleInstances), shown even under a wall with none under it, as
    // over a porch. Bands use the game's FloorThickness material (floor-edge at diffuse 0.5), the texture
    // once across each tile edge or half diagonal, left to right from outside. Ceilings take the White
    // Top Ceiling pattern lit by the storey below's light map (where the game finds it is not known yet).
    const int ceiling = b_.material(assets_.patternMaterial("ceilingfuzzywhite", false));
    std::array<float, 2> ceilingSpan{1, 1};
    if (ceiling >= 0) {
        ceilingSpan = b_.scene().materials[static_cast<std::size_t>(ceiling)].floorTiles;
    }
    int floorEdge = b_.textureMaterial("floor-edge", 0.5f);
    if (floorEdge < 0) {
        floorEdge = edge_;
    }
    std::set<EdgeKey> wallEdges;
    for (const auto& w : walls_) {
        if (w.thick) {
            wallEdges.insert(edgeKey(w.q.x0, w.q.y0, w.q.x1, w.q.y1, w.level));
        }
    }
    // Quarter e of a tile is the triangle on its edge e (corners e, e + 1 and the centre).
    auto floored = [&](std::int32_t level, std::int32_t x, std::int32_t y, std::size_t e) {
        return g.contains(level, x, y) && g.i16(level, x, y, 3 - e % 4) != 0;
    };
    const std::array<std::array<float, 2>, 4> bandUv{{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
    constexpr float kHalf = 0.70710678f;  // a diagonal band's normal (1 / sqrt 2 each way)
    for (std::int32_t level = 1; level < static_cast<std::int32_t>(g.levels); ++level) {
        Surfaces ceilings, edges, wallEdgeBands;
        // The storey's lowest floor, which the camera must be below to see its undersides.
        float lowestFloor = std::numeric_limits<float>::max();
        for (std::int32_t x = 0; x < static_cast<std::int32_t>(g.width); ++x) {
            for (std::int32_t y = 0; y < static_cast<std::int32_t>(g.height); ++y) {
                const float fx = static_cast<float>(x), fy = static_cast<float>(y);
                const std::array<std::array<float, 2>, 4> c{{{fx, fy}, {fx + 1, fy}, {fx + 1, fy + 1}, {fx, fy + 1}}};
                std::array<float, 4> top{};
                float middle = 0;
                for (std::size_t k = 0; k < 4; ++k) {
                    top[k] = heights_.corner(level, c[k][0], c[k][1]);
                    middle += top[k] / 4;
                }
                const std::array<float, 2> centre{fx + 0.5f, fy + 0.5f};
                // A band from `a` (left, seen from outside) to `b`, facing `out`, above a wall below or not.
                auto band = [&](const std::array<float, 2>& a, float ta, const std::array<float, 2>& b, float tb, const std::array<float, 3>& out,
                                bool overWall) {
                    addQuad((overWall ? wallEdgeBands : edges)[floorEdge], {{{a[0], a[1], ta - kFloorThickness}, {b[0], b[1], tb - kFloorThickness}, {b[0], b[1], tb}, {a[0], a[1], ta}}},
                            out, bandUv);
                };
                const int neighbor[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};  // across edges c0-c1, c1-c2, ...
                for (std::size_t e = 0; e < 4; ++e) {
                    if (!floored(level, x, y, e)) {
                        continue;
                    }
                    const auto& a = c[e];
                    const auto& b = c[(e + 1) % 4];
                    const float ta = top[e], tb = top[(e + 1) % 4];
                    lowestFloor = std::min({lowestFloor, ta, tb});
                    // The ceiling's quarter, its texture along the tile axes as a floor's. A quarter
                    // with no surface has none, as it has no floor.
                    if (surface == lot_.grids.end() || !surfaceQuarterOpen(surface->second, level, x, y, 3 - e)) {
                        auto& s = ceilings[ceiling];
                        for (const auto& [p, t] : {std::pair{b, tb}, std::pair{a, ta}, std::pair{centre, middle}}) {
                            addVertex(s, {p[0], p[1], t - kFloorThickness}, {0, 0, -1}, {p[1] / ceilingSpan[0], p[0] / ceilingSpan[1]});
                            const auto uv = lightUv(level - 1, fx, fy, p[0] - fx, p[1] - fy);
                            s.lightUvs.insert(s.lightUvs.end(), uv.begin(), uv.end());
                        }
                    }
                    const std::array<float, 3> out{static_cast<float>(neighbor[e][0]), static_cast<float>(neighbor[e][1]), 0};
                    if (!floored(level, x + neighbor[e][0], y + neighbor[e][1], e + 2)) {
                        band(a, ta, b, tb, out, wallEdges.contains(edgeKey(a[0], a[1], b[0], b[1], level - 1)));
                    }
                    // Where only half the tile is floored, along the diagonal. It faces the two
                    // unfloored quarters beyond it.
                    const bool diagonalWall = wallEdges.contains(edgeKey(fx, fy, fx + 1, fy + 1, level - 1));
                    auto facing = [&](std::size_t p, std::size_t q) {
                        return std::array<float, 3>{kHalf * static_cast<float>(neighbor[p % 4][0] + neighbor[q % 4][0]),
                                                    kHalf * static_cast<float>(neighbor[p % 4][1] + neighbor[q % 4][1]), 0};
                    };
                    if (!floored(level, x, y, e + 1)) {
                        band(b, tb, centre, middle, facing(e + 1, e + 2), diagonalWall);
                    }
                    if (!floored(level, x, y, e + 3)) {
                        band(centre, middle, a, ta, facing(e + 2, e + 3), diagonalWall);
                    }
                }
            }
        }
        if (auto* instance = b_.place(b_.addMesh("ceilings", std::move(ceilings)), "ceilings", Transform{}, Part::Floor, level)) {
            instance->underside = Underside::Floor;
            instance->floorHeight = lowestFloor;
            if (const auto below = static_cast<std::size_t>(level - 1); below < maps.floors.size()) {
                instance->lightMap = maps.floors[below];
                instance->incidenceMap = maps.floorIncidence[below];
            }
        }
        // Every floor edge on the lot is lit by the outdoor lights without the sun, even inside.
        auto placeEdges = [&](const char* name, Surfaces&& surfaces, Underside underside) {
            if (auto* instance = b_.place(b_.addMesh(name, std::move(surfaces)), name, Transform{}, Part::Floor, level)) {
                instance->litOutdoors = true;
                instance->sunTest = SunTest::NoSun;
                instance->underside = underside;
                instance->floorHeight = lowestFloor;
            }
        };
        placeEdges("floor edges", std::move(edges), Underside::Floor);
        placeEdges("wall edge bands", std::move(wallEdgeBands), Underside::OverWalls);
    }
}

} // namespace ts2::scene::lotscene
