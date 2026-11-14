// Build mode's terrain paint: a paint picked from the catalog is brushed onto the lot's ground with a
// drag (small, medium or large brush), over the paints already there; Ctrl held as the drag starts
// rubs paint off back to the bare ground. Paint is free, as in the game. The ground's shape doesn't
// change, so its paints are drawn again apart from it: the lot's own paint layers are hidden and
// every paint is drawn over the ground anew, a few times a second while painting and when a stroke
// ends. What is painted is saved with the lot (game_save.cpp).
#include "game.hpp"

#include "content/terrain_paint.hpp"
#include "diag/log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace app {

namespace pf = ts2::platform;

namespace {

// The brush's radius in tiles by size (small, medium, large).
constexpr std::array<float, 3> kBrushRadius = {0.75f, 1.5f, 3.0f};
// The brush paints again once moved this far (tiles), and the paints are drawn again this often while painting.
constexpr float kPaintStep = 0.2f;
constexpr std::uint64_t kRedrawNs = 250'000'000;
// The game draws a paint where its strength is above 10 of 255 (see LotSceneBuilder::drawTerrain).
constexpr std::uint8_t kShownStrength = 10;

} // namespace

void Game::pickPaint(std::uint32_t guid) {
    sledgeActive_ = false;
    if (!buildActive_ || !lotLife_) {
        return;
    }
    cancelHeld();
    const auto& catalog = assets_.get()->paintCatalog();
    const auto it = std::find_if(catalog.begin(), catalog.end(), [&](const auto& p) { return p.guid == guid; });
    if (it == catalog.end()) {
        return;
    }
    BuildTool tool;
    tool.paints = true;
    tool.guid = guid;
    tool.pattern = it->material;
    tool.name = it->name;
    tool.price = it->price;
    buildTool_ = std::move(tool);
    painting_ = false;
    ts2::diag::step(std::format("build mode: terrain paint {} ({}) picked", it->name, it->material));
}

bool Game::paintMouse(const pf::Event& e, float x, float y, bool overPanels) {
    if (e.type == pf::Event::Type::MouseMotion) {
        if (painting_) {
            paintAt(x, y);
            const std::uint64_t now = ts2::platform::ticksNs();
            if (paintChanged_ && now - paintDrawnAt_ > kRedrawNs) {
                redrawTerrainPaint();
            }
            return true;
        }
        buildTip(overPanels ? std::string() : buildTool_->name + (lotUi_.buildBrushSize() == 0 ? " (small brush)" : lotUi_.buildBrushSize() == 1 ? "" : " (large brush)"));
        return false;  // the camera may still move
    }
    if (e.button != pf::MouseButton::Left) {
        return false;
    }
    if (e.type == pf::Event::Type::MouseDown && !overPanels) {
        painting_ = true;
        erasing_ = e.ctrl;
        if (buildLot_) {
            paintBefore_ = PaintState{buildLot_->terrainTextures, buildLot_->flatGrids};
        }
        paintedAt_.reset();
        paintChanged_ = false;
        buildTip({});
        paintAt(x, y);
        return true;
    }
    if (e.type == pf::Event::Type::MouseUp && painting_) {
        painting_ = false;
        if (paintChanged_) {
            redrawTerrainPaint();
        }
        ts2::diag::step(std::format("build mode: {} terrain", erasing_ ? "rubbed paint off the" : "painted " + buildTool_->name + " on the"));
        // Saved when the lot is (lotChanges writes only what differs).
        edits_.paint = true;
        // Undone, the paints go back as they were before the stroke.
        if (paintBefore_ && buildLot_) {
            auto setPaints = [this](const PaintState& state) {
                if (buildLot_) {
                    buildLot_->terrainTextures = state.textures;
                    buildLot_->flatGrids = state.grids;
                    edits_.paint = true;
                    redrawTerrainPaint();
                }
            };
            recordStep(erasing_ ? "rubbing paint off" : "painting the ground", [setPaints, before = *paintBefore_] { setPaints(before); },
                       [setPaints, after = PaintState{buildLot_->terrainTextures, buildLot_->flatGrids}] { setPaints(after); });
            paintBefore_.reset();
        }
        return true;
    }
    return false;
}

void Game::paintAt(float x, float y) {
    if (!buildTool_ || !buildTool_->paints || !buildLot_) {
        return;
    }
    // The ground storey's ground, whatever storey is in view.
    const auto point = buildPointAt(x, y, ts2::content::groundGridLevel(*buildLot_));
    if (!point) {
        return;
    }
    if (paintedAt_ && std::hypot((*point)[0] - (*paintedAt_)[0], (*point)[1] - (*paintedAt_)[1]) < kPaintStep) {
        return;
    }
    paintedAt_ = point;
    std::optional<std::size_t> paint;
    if (!erasing_) {
        paint = ts2::content::terrainPaintIndex(*buildLot_, buildTool_->pattern);
        if (!paint) {
            return;
        }
    }
    const float radius = kBrushRadius[static_cast<std::size_t>(std::clamp(lotUi_.buildBrushSize(), 0, 2))];
    paintChanged_ = ts2::content::paintTerrain(*buildLot_, paint, (*point)[0], (*point)[1], radius) || paintChanged_;
}

void Game::redrawTerrainPaint() {
    paintDrawnAt_ = ts2::platform::ticksNs();
    paintChanged_ = false;
    if (!buildLot_ || !buildLot_->terrainTextures || !lotLife_) {
        return;
    }
    const auto& textures = *buildLot_->terrainTextures;
    // The lot's ground: the terrain instance whose canvas is as large as the lot's paint maps.
    const auto first = buildLot_->flatGrids.find(ts2::content::kFirstPaintMapInstance);
    if (first == buildLot_->flatGrids.end()) {
        return;
    }
    const std::uint32_t columns = first->second.width, rows = first->second.height;
    if (terrainInstance_ < 0) {
        for (std::size_t i = 0; i < lot_.instances.size(); ++i) {
            const auto& instance = lot_.instances[i];
            if (instance.object != 0 || instance.name != "terrain" || instance.mesh < 0) {
                continue;
            }
            const auto& mesh = lot_.meshes[static_cast<std::size_t>(instance.mesh)];
            if (!mesh.primitives.empty() && mesh.primitives[0].positions.size() == static_cast<std::size_t>(columns) * rows * 3) {
                terrainInstance_ = static_cast<int>(i);
                break;
            }
        }
        if (terrainInstance_ < 0) {
            ts2::diag::warnOnce("build mode: the lot's ground wasn't found, so its paint can't be drawn again");
            return;
        }
        // Its own paint layers are drawn no more; its canvas and the ponds' caustics stay.
        const auto& mesh = lot_.meshes[static_cast<std::size_t>(lot_.instances[static_cast<std::size_t>(terrainInstance_)].mesh)];
        std::vector<int> materials;
        for (std::size_t p = 0; p < mesh.primitives.size(); ++p) {
            const int m = mesh.primitives[p].material;
            const bool keep = p == 0 || (m >= 0 && static_cast<std::size_t>(m) < lot_.materials.size() && lot_.materials[static_cast<std::size_t>(m)].caustics != 0);
            materials.push_back(keep ? m : ts2::scene::kHiddenMaterial);
        }
        renderer_.setMaterials(static_cast<std::size_t>(terrainInstance_), materials);
    }
    const auto& ground = lot_.instances[static_cast<std::size_t>(terrainInstance_)];
    const auto& canvas = lot_.meshes[static_cast<std::size_t>(ground.mesh)].primitives[0];
    // Each paint over the cells of the canvas it shows on, its vertices those cells use, each with the
    // paint's strength there as its alpha.
    ts2::scene::Mesh mesh;
    mesh.name = "terrain paint";
    const bool smooth = options_.smoothLotSkirts;
    for (std::size_t n = 0; n < textures.paints.size(); ++n) {
        const auto map = buildLot_->flatGrids.find(ts2::content::kFirstPaintMapInstance + static_cast<std::uint32_t>(n));
        if (map == buildLot_->flatGrids.end() || map->second.width != columns || map->second.height != rows) {
            continue;
        }
        const auto& strength = map->second;
        ts2::scene::Primitive paint;
        std::vector<std::int32_t> remap(static_cast<std::size_t>(columns) * rows, -1);
        auto vertex = [&](std::uint32_t v) {
            auto& at = remap[v];
            if (at < 0) {
                at = static_cast<std::int32_t>(paint.positions.size() / 3);
                for (std::size_t k = 0; k < 3; ++k) {
                    paint.positions.push_back(canvas.positions[v * 3 + k]);
                    paint.normals.push_back(canvas.normals[v * 3 + k]);
                }
                for (std::size_t k = 0; k < 2; ++k) {
                    paint.uvs.push_back(canvas.uvs[v * 2 + k]);
                    if (v * 2 + k < canvas.lightUvs.size()) {
                        paint.lightUvs.push_back(canvas.lightUvs[v * 2 + k]);
                    }
                }
                // A lot's saved maps can hold 0 along their last row and column; with smooth lot skirts
                // (not in the game) those take the row and column before.
                const std::uint32_t i = v / rows, j = v % rows;
                const std::uint32_t si = smooth && i + 1 == columns && i > 0 ? i - 1 : i;
                const std::uint32_t sj = smooth && j + 1 == rows && j > 0 ? j - 1 : j;
                paint.alphas.push_back(static_cast<float>(strength.byte(static_cast<std::int32_t>(si), static_cast<std::int32_t>(sj))) / 255);
            }
            paint.indices.push_back(static_cast<std::uint32_t>(at));
        };
        // The canvas's cells come as two triangles, a b b+1 and a b+1 a+1, from corner a.
        for (std::size_t k = 0; k + 5 < canvas.indices.size(); k += 6) {
            const std::uint32_t a = canvas.indices[k], b = a + rows;
            const auto at = [&](std::uint32_t v) {
                return strength.byte(static_cast<std::int32_t>(v / rows), static_cast<std::int32_t>(v % rows));
            };
            if (std::max({at(a), at(a + 1), at(b), at(b + 1)}) <= kShownStrength) {
                continue;
            }
            for (const std::uint32_t v : {a, b, b + 1, a, b + 1, a + 1}) {
                vertex(v);
            }
        }
        if (paint.indices.empty()) {
            continue;
        }
        paint.material = lotLife_->terrainPaintMaterial(textures.paints[n], lot_);
        if (paint.material >= 0) {
            mesh.primitives.push_back(std::move(paint));
        }
    }
    renderer_.addMaterials(lot_);
    if (mesh.primitives.empty()) {
        if (terrainPaintInstance_ >= 0) {
            const auto& old = lot_.meshes[static_cast<std::size_t>(lot_.instances[static_cast<std::size_t>(terrainPaintInstance_)].mesh)];
            renderer_.setMaterials(static_cast<std::size_t>(terrainPaintInstance_), std::vector<int>(old.primitives.size(), ts2::scene::kHiddenMaterial));
        }
        return;
    }
    lot_.meshes.push_back(std::move(mesh));
    const int meshIndex = static_cast<int>(lot_.meshes.size() - 1);
    if (terrainPaintInstance_ < 0) {
        // Placed and lit as the ground it lies on.
        ts2::scene::Instance paints = ground;
        paints.mesh = meshIndex;
        paints.name = "terrain paint";
        lot_.instances.push_back(std::move(paints));
        terrainPaintInstance_ = static_cast<int>(lot_.instances.size() - 1);
    } else {
        lot_.instances[static_cast<std::size_t>(terrainPaintInstance_)].mesh = meshIndex;
    }
    renderer_.addMeshes(lot_);
    renderer_.setInstance(lot_, static_cast<std::size_t>(terrainPaintInstance_));
}

} // namespace app
