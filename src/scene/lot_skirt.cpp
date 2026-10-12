#include "scene/lot_skirt.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

namespace ts2::scene {

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

void addLotSkirt(SceneBuilder& builder, const PaintedTerrain& terrain, const LotFootprint& lot, int margin,
                 const std::string& terrainType, const std::map<std::string, std::string>& roadTextures) {
    if (terrain.columns < 2 || terrain.rows < 2) {
        return;
    }
    const SkirtArea area = lotSkirtArea(terrain, lot, margin);
    const float tile = terrain.tileSize;
    const int lx0 = static_cast<int>(std::lround(lot.x0 / tile)), ly0 = static_cast<int>(std::lround(lot.y0 / tile));
    const int lx1 = static_cast<int>(std::lround(lot.x1 / tile)), ly1 = static_cast<int>(std::lround(lot.y1 / tile));
    auto corner = [&](int x, int y) {
        const std::size_t i = static_cast<std::size_t>(y) * terrain.columns + static_cast<std::size_t>(x);
        return std::array<float, 3>{static_cast<float>(x) * tile, static_cast<float>(y) * tile, terrain.heights[i]};
    };
    auto normal = [&](int x, int y) {
        const std::size_t i = (static_cast<std::size_t>(y) * terrain.columns + static_cast<std::size_t>(x)) * 3;
        if (i + 2 >= terrain.normals.size()) {
            return std::array<float, 3>{0, 0, 1};
        }
        const std::array<float, 3> n{terrain.normals[i], terrain.normals[i + 1], terrain.normals[i + 2]};
        return n[0] == 0 && n[1] == 0 && n[2] == 0 ? std::array<float, 3>{0, 0, 1} : n;
    };

    // How opaque the skirt is at a grid point: it fades out over its two outermost rings of points.
    auto fade = [&](int x, int y) {
        if (x == area.x0 || x == area.x1 || y == area.y0 || y == area.y1) {
            return 0.0f;
        }
        if (x == area.x0 + 1 || x == area.x1 - 1 || y == area.y0 + 1 || y == area.y1 - 1) {
            return 0.5f;
        }
        return 1.0f;
    };

    // The ground: two triangles a tile, its texture once a tile (from the tile's grid position).
    // Tiles touching the fading rings are blended, the rest opaque.
    Surfaces ground;
    const int material = builder.textureMaterial(lot.texture);
    int faded = -1;
    if (material >= 0) {
        Material m = builder.scene().materials[static_cast<std::size_t>(material)];
        m.name += " (fading)";
        m.alpha = AlphaMode::Blend;
        builder.scene().materials.push_back(std::move(m));
        faded = static_cast<int>(builder.scene().materials.size() - 1);
    }
    for (int y = area.y0; y < area.y1; ++y) {
        for (int x = area.x0; x < area.x1; ++x) {
            if (x >= lx0 && x < lx1 && y >= ly0 && y < ly1) {
                continue;  // the lot's own ground
            }
            const std::array<std::array<int, 2>, 4> c{{{x, y}, {x + 1, y}, {x + 1, y + 1}, {x, y + 1}}};
            bool solid = true;
            for (const auto& [cx, cy] : c) {
                solid = solid && fade(cx, cy) >= 1.0f;
            }
            Primitive& p = ground[solid ? material : faded];
            const auto base = static_cast<std::uint32_t>(p.positions.size() / 3);
            for (const auto& [cx, cy] : c) {
                const auto pos = corner(cx, cy);
                const auto n = normal(cx, cy);
                p.positions.insert(p.positions.end(), pos.begin(), pos.end());
                p.normals.insert(p.normals.end(), n.begin(), n.end());
                p.uvs.insert(p.uvs.end(), {static_cast<float>(cx), static_cast<float>(cy)});
                if (!solid) {
                    p.alphas.push_back(fade(cx, cy));
                }
            }
            p.indices.insert(p.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
    }
    // The boundary curtain (the game's lotSkirtBoundaryClip): from 200 below the lowest corner of the
    // skirt to 200 above the lot.
    float lowest = std::numeric_limits<float>::max();
    for (const auto& [cx, cy] : {std::pair{area.x0, area.y0}, std::pair{area.x1, area.y0}, std::pair{area.x1, area.y1}, std::pair{area.x0, area.y1}}) {
        lowest = std::min(lowest, corner(cx, cy)[2]);
    }
    builder.scene().clip = Scene::BoundaryClip{static_cast<float>(area.x0) * tile, static_cast<float>(area.y0) * tile,
                                               static_cast<float>(area.x1) * tile, static_cast<float>(area.y1) * tile,
                                               lowest - 200, lot.z + 200};

    // As low as the lot's own ground, which meets it at the lot's edges.
    builder.place(builder.addMesh("lot skirt", std::move(ground)), "lot skirt", Transform::at(0, 0, -kGroundDrop, 0), Part::Skirt);

    // The roads in the area, with the lot view's road textures, blended over the ground.
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
        builder.scene().materials[static_cast<std::size_t>(m)].clamp = true;
        Primitive& r = roads[m];
        const auto base = static_cast<std::uint32_t>(r.positions.size() / 3);
        // Corners lie on the ground (the neighborhood keeps its roads raised for its own view).
        for (const auto& c : road.corners) {
            const int gx = std::clamp(static_cast<int>(std::lround(c[0] / tile)), 0, static_cast<int>(terrain.columns) - 1);
            const int gy = std::clamp(static_cast<int>(std::lround(c[1] / tile)), 0, static_cast<int>(terrain.rows) - 1);
            r.positions.insert(r.positions.end(), {c[0], c[1], corner(gx, gy)[2]});
            r.alphas.push_back(fade(gx, gy));
            r.normals.insert(r.normals.end(), {0, 0, 1});
            r.uvs.insert(r.uvs.end(), {c[3], c[4]});
        }
        r.indices.insert(r.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
    }
    if (!roads.empty()) {
        if (auto* i = builder.place(builder.addMesh("lot skirt roads", std::move(roads)), "lot skirt roads", Transform{}, Part::Skirt)) {
            i->skirtAmbient = 4;
        }
    }
}

} // namespace ts2::scene
