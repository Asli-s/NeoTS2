// The lot's terrain with its paints, and its floors with the slabs and ceilings under them.
#include "scene/lot_scene_builder.hpp"

#include "scene/curved_pool.hpp"
#include "scene/terrain_water.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace ts2::scene {

int floorTurnSteps(int nibble) {
    return nibble == 2 ? 3 : nibble == 4 ? 2 : nibble == 8 ? 1 : 0;
}

std::array<float, 2> turnedFloorPoint(const std::array<float, 2>& point, const std::array<float, 2>& centre, int steps) {
    float dx = point[0] - centre[0], dy = point[1] - centre[1];
    for (int k = 0; k < steps; ++k) {
        dx = std::exchange(dy, -dx);
    }
    return {centre[0] + dx, centre[1] + dy};
}

namespace {

// Keeps the blended ground under the ground storey's floor quarters, which are flat triangles folded
// along the tile's diagonals, where a floored tile's corners aren't in one plane.
void keepUnderFloors(const content::Lot& lot, const Heights& heights, int level, std::uint32_t steps, std::uint32_t columns, std::uint32_t rows,
                     std::vector<float>& drawn) {
    const auto floors = lot.grids.find(content::kFloorGridInstance);
    if (floors == lot.grids.end() || floors->second.elementSize != 8 || steps == 0 || drawn.size() != static_cast<std::size_t>(columns) * rows) {
        return;
    }
    const auto& g = floors->second;
    const auto tilesX = static_cast<std::int32_t>((columns - 1) / steps), tilesY = static_cast<std::int32_t>((rows - 1) / steps);
    // The lowest floor at a point on the tile (x, y), none where the tile has no floor there. A point on
    // an edge between quarters takes the lower.
    const auto floorOnTile = [&](std::int32_t x, std::int32_t y, float px, float py) -> std::optional<float> {
        if (x < 0 || y < 0 || x >= tilesX || y >= tilesY || !g.contains(level, x, y)) {
            return std::nullopt;
        }
        const float fx = static_cast<float>(x), fy = static_cast<float>(y);
        // The tile edge of each quarter, in the floor grid's order: x low, y high, x high, y low.
        const std::array<std::array<float, 2>, 4> corners{{{fx, fy}, {fx, fy + 1}, {fx + 1, fy + 1}, {fx + 1, fy}}};
        const float cx = fx + 0.5f, cy = fy + 0.5f, centre = heights.tile(level, fx, fy);
        std::optional<float> lowest;
        for (std::size_t q = 0; q < 4; ++q) {
            if (g.i16(level, x, y, q) == 0) {
                continue;
            }
            const auto& a = corners[q];
            const auto& b = corners[(q + 1) % 4];
            // The point's weights on the triangle's corners: a, b and the tile's centre.
            const float d = (b[1] - cy) * (a[0] - cx) + (cx - b[0]) * (a[1] - cy);
            const float wa = ((b[1] - cy) * (px - cx) + (cx - b[0]) * (py - cy)) / d;
            const float wb = ((cy - a[1]) * (px - cx) + (a[0] - cx) * (py - cy)) / d;
            const float wc = 1 - wa - wb;
            constexpr float kEdge = -1e-4f;
            if (wa < kEdge || wb < kEdge || wc < kEdge) {
                continue;
            }
            const float z = wa * heights.corner(level, a[0], a[1]) + wb * heights.corner(level, b[0], b[1]) + wc * centre;
            lowest = lowest ? std::min(*lowest, z) : z;
        }
        return lowest;
    };
    // Each cell is two triangles meeting on its rising diagonal. Where a floor folds the other way, the
    // diagonal's ends go down until its middle is under the floor too.
    const float step = 1.0f / static_cast<float>(steps);
    for (std::int32_t x = 0; x < tilesX; ++x) {
        for (std::int32_t y = 0; y < tilesY; ++y) {
            if (!g.contains(level, x, y) || (g.i16(level, x, y, 0) | g.i16(level, x, y, 1) | g.i16(level, x, y, 2) | g.i16(level, x, y, 3)) == 0) {
                continue;
            }
            // On a tile whose corners are in one plane the blend is that plane, the floor's own.
            const float fx = static_cast<float>(x), fy = static_cast<float>(y);
            constexpr float kFlat = 1e-4f;
            if (std::abs(heights.corner(level, fx, fy) + heights.corner(level, fx + 1, fy + 1) - heights.corner(level, fx + 1, fy) -
                         heights.corner(level, fx, fy + 1)) <= kFlat) {
                continue;
            }
            const auto i0 = static_cast<std::uint32_t>(x) * steps, j0 = static_cast<std::uint32_t>(y) * steps;
            for (std::uint32_t i = i0; i <= i0 + steps; ++i) {
                for (std::uint32_t j = j0; j <= j0 + steps; ++j) {
                    if (const auto z = floorOnTile(x, y, static_cast<float>(i) * step, static_cast<float>(j) * step)) {
                        float& at = drawn[static_cast<std::size_t>(i) * rows + j];
                        at = std::min(at, *z);
                    }
                }
            }
            for (std::uint32_t i = i0; i < i0 + steps; ++i) {
                for (std::uint32_t j = j0; j < j0 + steps; ++j) {
                    const auto z = floorOnTile(x, y, (static_cast<float>(i) + 0.5f) * step, (static_cast<float>(j) + 0.5f) * step);
                    if (!z) {
                        continue;
                    }
                    float& a = drawn[static_cast<std::size_t>(i) * rows + j];
                    float& b = drawn[static_cast<std::size_t>(i + 1) * rows + j + 1];
                    const float over = (a + b) / 2 - *z;
                    if (over > 0) {
                        a -= over;
                        b -= over;
                    }
                }
            }
        }
    }
}

} // namespace

namespace lotscene {

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
    keepUnderFloors(lot_, heights_, ground, kSteps, columns, rows, drawn);
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
    // A curved pool corner's terrain stops at its quarter circle: the cells it reaches are drawn as
    // the pieces outside it, with points of their own between the cell's corners.
    struct Between {
        std::uint32_t i, j;
        float fx, fy;  // across the cell, 0 to 1
    };
    std::vector<Between> between;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::uint32_t>> cutCells;
    const auto roundedCorners = curvedpool::byTile(lot_.curvedPools);
    for (const auto& [tile, corners] : roundedCorners) {
        if (tile.first < 0 || tile.second < 0) {
            continue;
        }
        for (std::uint32_t di = 0; di < kSteps; ++di) {
            for (std::uint32_t dj = 0; dj < kSteps; ++dj) {
                const std::uint32_t i = static_cast<std::uint32_t>(tile.first) * kSteps + di, j = static_cast<std::uint32_t>(tile.second) * kSteps + dj;
                if (i + 1 >= columns || j + 1 >= rows) {
                    continue;
                }
                const float x0 = static_cast<float>(i) * step, y0 = static_cast<float>(j) * step;
                auto& indices = cutCells[{i, j}];
                for (const auto& piece : curvedpool::outside({{x0, y0}, {x0 + step, y0}, {x0 + step, y0 + step}, {x0, y0 + step}}, corners)) {
                    const auto first = static_cast<std::uint32_t>(canvas.positions.size() / 3);
                    for (const auto& p : piece) {
                        const float fx = std::clamp((p[0] - x0) / step, 0.0f, 1.0f), fy = std::clamp((p[1] - y0) / step, 0.0f, 1.0f);
                        between.push_back({i, j, fx, fy});
                        const std::uint32_t a = i * rows + j, b = (i + 1) * rows + j;
                        auto mix = [&](const std::vector<float>& v, std::size_t stride, std::size_t k) {
                            return (v[a * stride + k] * (1 - fx) + v[b * stride + k] * fx) * (1 - fy) +
                                   (v[(a + 1) * stride + k] * (1 - fx) + v[(b + 1) * stride + k] * fx) * fy;
                        };
                        canvas.positions.insert(canvas.positions.end(), {p[0], p[1], mix(canvas.positions, 3, 2)});
                        std::array<float, 3> n{mix(canvas.normals, 3, 0), mix(canvas.normals, 3, 1), mix(canvas.normals, 3, 2)};
                        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                        canvas.normals.insert(canvas.normals.end(), {n[0] / len, n[1] / len, n[2] / len});
                        canvas.uvs.insert(canvas.uvs.end(), {p[0] / kRepeat, p[1] / kRepeat});
                        canvas.lightUvs.insert(canvas.lightUvs.end(), {p[0] / lotWidth, p[1] / lotHeight});
                    }
                    for (std::uint32_t k = 1; k + 1 < piece.size(); ++k) {
                        indices.insert(indices.end(), {first, first + k, first + k + 1});
                    }
                }
            }
        }
    }
    // A value kept per terrain point, for the points between a cut cell's corners.
    auto addBetween = [&](std::vector<float>& values) {
        for (const auto& p : between) {
            const std::uint32_t a = p.i * rows + p.j, b = (p.i + 1) * rows + p.j;
            values.push_back((values[a] * (1 - p.fx) + values[b] * p.fx) * (1 - p.fy) + (values[a + 1] * (1 - p.fx) + values[b + 1] * p.fx) * p.fy);
        }
    };
    auto cell = [&](std::vector<std::uint32_t>& indices, std::uint32_t i, std::uint32_t j) {
        if (open(i, j)) {
            return;
        }
        if (const auto cut = cutCells.find({i, j}); cut != cutCells.end()) {
            indices.insert(indices.end(), cut->second.begin(), cut->second.end());
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
        addBetween(paint.alphas);
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
        addBetween(caustics.alphas);
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
    auto drawnGround = std::make_shared<LotGround>();
    drawnGround->meshes.push_back(static_cast<int>(b_.scene().meshes.size() - 1));
    drawnGround->steps = kSteps;
    drawnGround->columns = columns;
    drawnGround->rows = rows;
    drawnGround->level = ground;
    drawnGround->sea = b_.edgeSea();
    for (const auto& p : between) {
        drawnGround->between.push_back({p.i, p.j, p.fx, p.fy});
    }
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
    // The ground storey's grid for Buy and Build modes, the texture once per tile over the whole
    // ground. The game draws it only where no floor is, which the floors standing just above the
    // ground do here by hiding it.
    if (const int material = b_.groundGridMaterial(); instance && material >= 0) {
        Primitive grid;
        grid.material = material;
        grid.positions = canvas.positions;
        grid.normals = canvas.normals;
        grid.indices = canvas.indices;
        grid.uvs.reserve(canvas.positions.size() / 3 * 2);
        for (std::size_t p = 0; p + 2 < canvas.positions.size(); p += 3) {
            grid.uvs.insert(grid.uvs.end(), {canvas.positions[p], canvas.positions[p + 1]});
        }
        Mesh gridMesh;
        gridMesh.name = "ground grid";
        gridMesh.primitives.push_back(std::move(grid));
        b_.scene().meshes.push_back(std::move(gridMesh));
        const int level = instance->level;
        if (auto* placed = b_.place(static_cast<int>(b_.scene().meshes.size() - 1), "ground grid", Transform::at(0, 0, -kGroundDrop, 0))) {
            placed->part = Part::Grid;
            placed->level = level;
            drawnGround->meshes.push_back(static_cast<int>(b_.scene().meshes.size() - 1));
        }
    }
    b_.scene().lotGround = std::move(drawnGround);
}


// The Buy and Build grid of each storey above the ground: the game lays it on that storey's quarters
// without a floor where a floor may go (see SceneBuilder::floorAllowed). Each storey gets it twice, in
// white for when a higher storey is looked at and in the brighter active look for its own.
void LotSceneBuilder::drawUpperGrids() {
    const auto floors = lot_.grids.find(content::kFloorGridInstance);
    const auto& allowed = b_.floorAllowed();
    if (floors == lot_.grids.end() || floors->second.elementSize != 8 || !allowed) {
        return;
    }
    const auto& g = floors->second;
    const int white = b_.upperGridMaterial(false), active = b_.upperGridMaterial(true);
    if (white < 0 || active < 0) {
        return;
    }
    const int ground = content::groundGridLevel(lot_);
    for (std::int32_t level = ground + 1; level < static_cast<std::int32_t>(g.levels); ++level) {
        if (!drawsLevel(level)) {
            continue;
        }
        Primitive grid;
        for (std::int32_t x = 0; x < static_cast<std::int32_t>(g.width); ++x) {
            for (std::int32_t y = 0; y < static_cast<std::int32_t>(g.height); ++y) {
                if (!allowed(level, x, y)) {
                    continue;
                }
                const float fx = static_cast<float>(x), fy = static_cast<float>(y);
                const std::array<std::array<float, 2>, 4> corners{{{fx, fy}, {fx + 1, fy}, {fx + 1, fy + 1}, {fx, fy + 1}}};
                std::array<float, 4> h{};
                float center = 0;
                for (std::size_t c = 0; c < 4; ++c) {
                    h[c] = heights_.corner(level, corners[c][0], corners[c][1]) + 0.004f;
                    center += h[c] / 4;
                }
                for (std::size_t q = 0; q < 4; ++q) {
                    if (g.i16(level, x, y, 3 - q) != 0) {
                        continue;
                    }
                    const std::size_t n = (q + 1) % 4;
                    for (const auto& [p, z] : {std::pair{corners[q], h[q]}, std::pair{corners[n], h[n]}, std::pair{std::array<float, 2>{fx + 0.5f, fy + 0.5f}, center}}) {
                        const auto first = static_cast<std::uint32_t>(grid.positions.size() / 3);
                        grid.positions.insert(grid.positions.end(), {p[0], p[1], z});
                        grid.normals.insert(grid.normals.end(), {0, 0, 1});
                        grid.uvs.insert(grid.uvs.end(), {p[0], p[1]});
                        grid.indices.push_back(first);
                    }
                }
            }
        }
        if (grid.indices.empty()) {
            continue;
        }
        for (const bool lookedAt : {false, true}) {
            const std::string name = lookedAt ? "active storey grid" : "storey grid";
            Mesh mesh;
            mesh.name = name;
            grid.material = lookedAt ? active : white;
            mesh.primitives.push_back(grid);
            b_.scene().meshes.push_back(std::move(mesh));
            if (auto* placed = b_.place(static_cast<int>(b_.scene().meshes.size() - 1), name, Transform{})) {
                placed->part = lookedAt ? Part::ActiveGrid : Part::UpperGrid;
                placed->level = level;
            }
        }
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
    const auto roundedCorners = curvedpool::byTile(lot_.curvedPools);
    // A quarter's saved texture turn (see floorTurnSteps), a nibble per quarter in the floor grid's order.
    const auto turns = lot_.grids.find(content::kFloorTurnGridInstance);
    auto floorTurn = [&](std::int32_t level, std::int32_t x, std::int32_t y, std::size_t quarter) {
        if (turns == lot_.grids.end() || turns->second.elementSize != 2 || !turns->second.contains(level, x, y)) {
            return 0;
        }
        const auto cell = turns->second.at(level, x, y);
        const int nibble = (cell[quarter / 2] >> (4 * (quarter % 2))) & 0xF;
        return floorTurnSteps(nibble);
    };
    for (std::int32_t level = 0; level < static_cast<std::int32_t>(g.levels); ++level) {
        if (!drawsLevel(level)) {
            continue;
        }
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
                    const std::array<float, 2> middle{fx + 0.5f, fy + 0.5f};
                    // The quarter's texture is turned about the tile's centre by its saved turn. A
                    // pattern of four tiles, such as the sun on a pool's floor, is laid this way.
                    const int steps = floorTurn(level, x, y, 3 - q);
                    auto emit = [&](const std::array<float, 2>& c, float z) {
                        const auto t = turnedFloorPoint(c, middle, steps);
                        addVertex(s, {c[0], c[1], z + lift}, {0, 0, 1}, {t[1] / span[0], t[0] / span[1]});
                        const auto uv = lightUv(level, fx, fy, c[0] - fx, c[1] - fy);
                        s.lightUvs.insert(s.lightUvs.end(), uv.begin(), uv.end());
                    };
                    const auto rounded = level == ground ? roundedCorners.find({x, y}) : roundedCorners.end();
                    if (rounded == roundedCorners.end()) {
                        emit(corners[q], h[q]);
                        emit(corners[n], h[n]);
                        emit(middle, center);
                        continue;
                    }
                    // A curved pool corner's ground floor stops at its quarter circle.
                    const auto heightAt = [&](const std::array<float, 2>& p) {
                        const auto& a = corners[q];
                        const auto& b = corners[n];
                        const float d = (b[1] - middle[1]) * (a[0] - middle[0]) + (middle[0] - b[0]) * (a[1] - middle[1]);
                        const float wa = ((b[1] - middle[1]) * (p[0] - middle[0]) + (middle[0] - b[0]) * (p[1] - middle[1])) / d;
                        const float wb = ((middle[1] - a[1]) * (p[0] - middle[0]) + (a[0] - middle[0]) * (p[1] - middle[1])) / d;
                        return wa * h[q] + wb * h[n] + (1 - wa - wb) * center;
                    };
                    for (const auto& piece : curvedpool::outside({corners[q], corners[n], middle}, rounded->second)) {
                        for (std::size_t k = 1; k + 1 < piece.size(); ++k) {
                            for (const auto& p : {piece[0], piece[k], piece[k + 1]}) {
                                emit(p, heightAt(p));
                            }
                        }
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
        if (!drawsLevel(level)) {
            continue;
        }
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
                // A curved pool corner's floor ends at its quarter circle, under the curved wall's top.
                if (level == ground && roundedCorners.contains({x, y})) {
                    continue;
                }
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

} // namespace lotscene

std::vector<std::size_t> moveLotGround(Scene& scene, const content::Lot& lot, std::vector<std::pair<std::uint32_t, std::uint32_t>>* runs) {
    std::vector<std::size_t> changed;
    if (!scene.lotGround) {
        return changed;
    }
    const LotGround& g = *scene.lotGround;
    const Heights heights(lot);
    std::vector<float> drawn = heights.drawnGrid(g.level, g.steps, g.sea);
    if (drawn.size() != static_cast<std::size_t>(g.columns) * g.rows) {
        return changed;
    }
    keepUnderFloors(lot, heights, g.level, g.steps, g.columns, g.rows, drawn);
    const float step = 1.0f / static_cast<float>(g.steps);
    auto height = [&](std::int64_t i, std::int64_t j) {
        const auto ci = static_cast<std::size_t>(std::clamp<std::int64_t>(i, 0, g.columns - 1));
        const auto cj = static_cast<std::size_t>(std::clamp<std::int64_t>(j, 0, g.rows - 1));
        return drawn[ci * g.rows + cj];
    };
    // Each point's height and normal as drawTerrain makes them, then the points between.
    const std::size_t points = static_cast<std::size_t>(g.columns) * g.rows;
    std::vector<float> z(points), normals(points * 3);
    for (std::uint32_t i = 0; i < g.columns; ++i) {
        for (std::uint32_t j = 0; j < g.rows; ++j) {
            const std::size_t k = static_cast<std::size_t>(i) * g.rows + j;
            const float sx = (height(i + 1, j) - height(static_cast<std::int64_t>(i) - 1, j)) / (2 * step);
            const float sy = (height(i, j + 1) - height(i, static_cast<std::int64_t>(j) - 1)) / (2 * step);
            const float len = std::sqrt(sx * sx + sy * sy + 1);
            z[k] = height(i, j);
            normals[k * 3] = -sx / len;
            normals[k * 3 + 1] = -sy / len;
            normals[k * 3 + 2] = 1 / len;
        }
    }
    for (const auto& p : g.between) {
        const std::size_t a = static_cast<std::size_t>(p.i) * g.rows + p.j, b = static_cast<std::size_t>(p.i + 1) * g.rows + p.j;
        auto mix = [&](const std::vector<float>& v, std::size_t stride, std::size_t c) {
            return (v[a * stride + c] * (1 - p.fx) + v[b * stride + c] * p.fx) * (1 - p.fy) +
                   (v[(a + 1) * stride + c] * (1 - p.fx) + v[(b + 1) * stride + c] * p.fx) * p.fy;
        };
        z.push_back(mix(z, 1, 0));
        std::array<float, 3> n{mix(normals, 3, 0), mix(normals, 3, 1), mix(normals, 3, 2)};
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        normals.insert(normals.end(), {n[0] / len, n[1] / len, n[2] / len});
    }
    // The vertices that moved, from the first primitive drawn, which all of them share.
    if (runs != nullptr) {
        runs->clear();
        const Primitive* first = nullptr;
        for (const int m : g.meshes) {
            if (m >= 0 && static_cast<std::size_t>(m) < scene.meshes.size()) {
                for (const auto& p : scene.meshes[static_cast<std::size_t>(m)].primitives) {
                    if (first == nullptr && p.positions.size() == z.size() * 3 && p.normals.size() == normals.size()) {
                        first = &p;
                    }
                }
            }
        }
        for (std::size_t k = 0; first != nullptr && k < z.size(); ++k) {
            const bool moved = first->positions[k * 3 + 2] != z[k] || first->normals[k * 3] != normals[k * 3] ||
                               first->normals[k * 3 + 1] != normals[k * 3 + 1] || first->normals[k * 3 + 2] != normals[k * 3 + 2];
            if (!moved) {
                continue;
            }
            const auto at = static_cast<std::uint32_t>(k);
            if (!runs->empty() && runs->back().first + runs->back().second == at) {
                ++runs->back().second;
            } else {
                runs->push_back({at, 1});
            }
        }
    }
    for (const int m : g.meshes) {
        if (m < 0 || static_cast<std::size_t>(m) >= scene.meshes.size()) {
            continue;
        }
        for (auto& p : scene.meshes[static_cast<std::size_t>(m)].primitives) {
            if (p.positions.size() != z.size() * 3 || p.normals.size() != normals.size()) {
                continue;
            }
            for (std::size_t k = 0; k < z.size(); ++k) {
                p.positions[k * 3 + 2] = z[k];
            }
            p.normals = normals;
        }
        changed.push_back(static_cast<std::size_t>(m));
    }
    if (scene.drawnGround.columns == g.columns && scene.drawnGround.rows == g.rows) {
        scene.drawnGround.heights = std::move(drawn);
    }
    return changed;
}

} // namespace ts2::scene
