// The lot's roofs, shaped before its walls so that attic walls can follow their gable ends.
#include "scene/lot_scene_builder.hpp"

#include "diag/log.hpp"

#include <algorithm>

namespace ts2::scene::lotscene {

// Roofs are shaped before the walls, as attic walls follow the slopes of the gable ends they close.
void LotSceneBuilder::shapeRoofs() {
    // The most greenhouse pieces placed on one lot. A whole 64 x 64 lot under glass needs about
    // 8,300, and a broken file's many large roofs could otherwise ask for millions.
    constexpr std::size_t kMaxGreenhousePieces = 65536;
    std::size_t glassPieces = 0;
    roofs_.reserve(lot_.roofs.size());
    for (const auto& roof : lot_.roofs) {
        if (!sensibleRoof(roof)) {
            diag::warnOnce("roofs: a roof reaching past 256 tiles or with broken numbers is left out");
            continue;
        }
        RoofLook look;
        if (const auto* pattern = assets_.roofPattern(roof.pattern)) {
            // The top texture is a 5 x 5 grid of roof pieces (plain courses, ridge and hip caps, each
            // cell one repeat of the courses), cell (4, 3) plain, the cap piece along the top. The
            // colors are those of the game's roof materials.
            const std::array<float, 3> slopeColor{0.4f, 0.385f, 0.38f};
            look.top = b_.textureCellMaterial(pattern->top, 5, 5, 4, 3, slopeColor);
            look.under = b_.textureMaterial(pattern->under, 0.8f);
            look.trim = b_.textureMaterial(pattern->trim, 0.8f);
            look.edges = b_.textureMaterial(pattern->edges, 0.8f);
            look.atlas = b_.textureRegionMaterial(pattern->top, 0, 0, 1, 1, 512, 512, slopeColor);
            look.cap = b_.textureRegionMaterial(pattern->top, 0.4, 0.0, 0.6, 37.0 / 512, 128, 48, slopeColor);
            for (const int m : {look.top, look.atlas, look.cap}) {
                if (m >= 0) {
                    b_.scene().materials[static_cast<std::size_t>(m)].roofLight = true;
                }
            }
            // Valleys where roofs meet: the game's RoofEdges material (the edges texture at 0.8). Which
            // part of the texture the game shows there is not known yet.
            look.valley = b_.textureMaterial(pattern->edges, 0.8f);
        } else {
            look.top = look.under = look.trim = b_.colorMaterial("roof plain", {0.35f, 0.27f, 0.22f});
        }
        // A roof may keep its own slope (half the tangent of its angle), otherwise 45 degrees.
        const float slope = roof.slope > 0 ? 2 * roof.slope : 1.0f;
        ShapedRoof& r = roofs_.emplace_back();
        const int eaveLevel = content::gridIndex(lot_, roof.level0);
        r.level = eaveLevel - 1;  // a roof belongs to the storey under its eaves
        const float eave = heights_.corner(eaveLevel, roof.x0, roof.y0);
        r.look = look;
        RoofLook round = look;
        round.top = look.atlas >= 0 ? look.atlas : look.top;
        const auto* g = heights_.grid();
        const float lotWidth = g ? static_cast<float>(g->width - 1) : 1e9f, lotHeight = g ? static_cast<float>(g->height - 1) : 1e9f;
        if (auto grid = RoofGrid::of(roof, eave)) {
            r.shape = gridShape(roof, eave, std::move(*grid));
        } else if (addRoundRoof(r.surfaces, roof, eave, round, lotWidth, lotHeight)) {
            r.shape.eave = eave;
        } else if (roof.type == kGreenhouseRoof) {
            // Its glass pieces are the whole roof. The gable's shape still guides the walls.
            Surfaces none;
            r.shape = addRoof(none, roof, eave, slope, look);
            r.greenhouse = greenhousePieces(roof, eave);
            glassPieces += r.greenhouse.size();
            if (glassPieces > kMaxGreenhousePieces) {
                diag::warnOnce("roofs: greenhouse roofs needing more than 65536 glass pieces are left out");
                glassPieces -= r.greenhouse.size();
                r.greenhouse.clear();
            }
        } else {
            r.shape = addRoof(r.surfaces, roof, eave, slope, look);
        }
        r.valley = look.valley;
    }
    // Grid roofs are built once all are known, as each is cut by and meets the others.
    const int lowestLevel = lot_.roomGraph ? lot_.roomGraph->firstLevel : 0;
    std::vector<const RoofGrid*> grids;
    for (const auto& r : roofs_) {
        if (r.shape.grid) {
            grids.push_back(&*r.shape.grid);
        }
    }
    for (auto& r : roofs_) {
        if (!r.shape.grid) {
            continue;
        }
        // Only roofs within reach of this one's level whose grid overlaps its own can cover or
        // meet it: their heights are none outside their own grid. Leaving the rest out keeps a
        // lot of many roofs from costing roofs x roofs x points.
        const RoofGrid& own = *r.shape.grid;
        auto overlaps = [&own](const RoofGrid& g) {
            constexpr float kSlack = 1e-3f;
            return g.minX <= own.maxX() + kSlack && own.minX <= g.maxX() + kSlack && g.minY <= own.maxY() + kSlack &&
                   own.minY <= g.maxY() + kSlack;
        };
        std::vector<const RoofGrid*> others;
        for (const auto* g : grids) {
            if (g != &own && overlaps(*g) && roofWithinReach(own.level, g->level, lowestLevel)) {
                others.push_back(g);
            }
        }
        RoofLook look = r.look;
        look.top = look.atlas >= 0 ? look.atlas : look.top;
        addRoofGrid(r.surfaces, *r.shape.grid, others, look);
    }
    for (std::size_t i = 0; i < roofs_.size(); ++i) {
        for (std::size_t j = i + 1; j < roofs_.size(); ++j) {
            if (!roofs_[i].shape.grid && !roofs_[j].shape.grid) {
                addValleys(roofs_[i].surfaces, roofs_[i].shape, roofs_[j].shape, roofs_[i].valley);
            }
        }
    }
    // An attic wall closes the gable ends it lies along, of roofs whose eaves are at its floor.
    for (auto& w : walls_) {
        if (!w.toRoof) {
            continue;
        }
        for (const auto& r : roofs_) {
            if (r.level + 1 != w.level) {
                continue;
            }
            for (const auto& gable : r.shape.gables) {
                const float gx = gable.b[0] - gable.a[0], gy = gable.b[1] - gable.a[1], glen = std::sqrt(gx * gx + gy * gy);
                auto onLine = [&](float x, float y) {
                    const float off = ((x - gable.a[0]) * gy - (y - gable.a[1]) * gx) / glen;
                    const float t = ((x - gable.a[0]) * gx + (y - gable.a[1]) * gy) / glen;
                    return std::abs(off) < 1e-3f && t > -1e-3f && t < glen + 1e-3f;
                };
                if (glen > 0 && onLine(w.q.x0, w.q.y0) && onLine(w.q.x1, w.q.y1)) {
                    w.ceiling.gables.push_back(&r.shape);
                    break;
                }
            }
        }
    }
}

void LotSceneBuilder::placeRoofs() {
    for (auto& r : roofs_) {
        b_.place(b_.addMesh("roof", std::move(r.surfaces)), "roof", Transform{}, Part::Roof, r.level);
        for (const auto& p : r.greenhouse) {
            if (const int mesh = b_.model(p.model, {}); mesh >= 0) {
                b_.place(mesh, p.model, Transform::at(p.x, p.y, p.z, p.angle), Part::Roof, r.level);
            }
        }
    }
    stats_.roofs = lot_.roofs.size();
}

// A grid roof's shape for the walls: its surface, slab depth and gable-end lines.
RoofShape LotSceneBuilder::gridShape(const roofs::Roof& roof, float eave, RoofGrid grid) {
    RoofShape s;
    s.eave = eave;
    s.slope = 2 * grid.scale;
    s.thickness = grid.z(0, 0, 0) - grid.z(0, 0, 2);
    const float lx = std::min(roof.x0, roof.x1), hx = std::max(roof.x0, roof.x1);
    const float ly = std::min(roof.y0, roof.y1), hy = std::max(roof.y0, roof.y1);
    // The high side of a shed or a slope rises like a gable end, and is closed the same way.
    if (grid.kind == RoofGrid::Kind::shed || grid.kind == RoofGrid::Kind::slope) {
        switch (grid.high) {
        case 0: s.gables.push_back({{lx, ly}, {lx, hy}, {-1, 0}}); break;
        case 1: s.gables.push_back({{hx, ly}, {hx, hy}, {1, 0}}); break;
        case 2: s.gables.push_back({{lx, ly}, {hx, ly}, {0, -1}}); break;
        case 3: s.gables.push_back({{lx, hy}, {hx, hy}, {0, 1}}); break;
        default: break;
        }
    }
    if (grid.kind == RoofGrid::Kind::slope) {
        // A slope's sides along its fall are gable ends.
        if (grid.high <= 1) {
            s.gables.push_back({{lx, ly}, {hx, ly}, {0, -1}});
            s.gables.push_back({{lx, hy}, {hx, hy}, {0, 1}});
        } else {
            s.gables.push_back({{lx, ly}, {lx, hy}, {-1, 0}});
            s.gables.push_back({{hx, ly}, {hx, hy}, {1, 0}});
        }
    } else if (grid.kind != RoofGrid::Kind::gable) {
        // Hips slope on every side.
    } else if (grid.ridgeAlongX) {
        s.gables.push_back({{lx, ly}, {lx, hy}, {-1, 0}});
        s.gables.push_back({{hx, ly}, {hx, hy}, {1, 0}});
    } else {
        s.gables.push_back({{lx, ly}, {hx, ly}, {0, -1}});
        s.gables.push_back({{lx, hy}, {hx, hy}, {0, 1}});
    }
    s.grid = std::move(grid);
    return s;
}

} // namespace ts2::scene::lotscene
