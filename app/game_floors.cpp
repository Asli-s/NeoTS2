#include "game.hpp"
#include "lot_floor_rebuild.hpp"
#include "placement_errors.hpp"
#include "diag/timing.hpp"
#include "scene/lot_scene.hpp"
#include "scene/lot_parts.hpp"
#include "content/floor_edit.hpp"
#include "content/terrain_edit.hpp"
#include "platform/platform.hpp"

#include <algorithm>
#include <optional>
#include <tuple>
#include <limits>
#include <cmath>
#include <format>
#include <utility>

namespace app {

void Game::floorsChanged() {
    gridSupport_.reset();
    redrawFloors();
    lotShapeChanged();
}

std::vector<std::uint64_t> Game::twistedFloors() const {
    std::vector<std::uint64_t> twisted;
    const auto& lot = *lotContent_;
    const auto floors = lot.grids.find(ts2::content::kFloorGridInstance);
    const auto heights = lot.grids.find(ts2::content::kFloorHeightGridInstance);
    if (floors == lot.grids.end() || heights == lot.grids.end() || floors->second.elementSize != 8 || heights->second.elementSize != 4) {
        return twisted;
    }
    const auto& g = floors->second;
    const auto& h = heights->second;
    const std::int32_t ground = ts2::content::groundGridLevel(lot);
    for (std::int32_t x = 0; x + 1 < static_cast<std::int32_t>(h.width); ++x) {
        for (std::int32_t y = 0; y + 1 < static_cast<std::int32_t>(h.height); ++y) {
            if (!g.contains(ground, x, y) || !h.contains(ground, x + 1, y + 1)) {
                continue;
            }
            std::uint64_t quarters = 0;
            for (std::size_t q = 0; q < 4; ++q) {
                quarters |= (g.i16(ground, x, y, q) != 0 ? 1u : 0u) << q;
            }
            if (quarters == 0) {
                continue;
            }
            const float a = h.f32(ground, x, y, 0), b = h.f32(ground, x + 1, y, 0), c = h.f32(ground, x, y + 1, 0), d = h.f32(ground, x + 1, y + 1, 0);
            constexpr float kFlat = 1e-4f;
            if (std::abs(a + d - b - c) > kFlat) {
                twisted.push_back(static_cast<std::uint64_t>(x) << 40 | static_cast<std::uint64_t>(y) << 8 | quarters);
            }
        }
    }
    return twisted;
}

void Game::redrawFloors(const std::set<std::int32_t>& levels) {
    if (!lotContent_ || !lot_.lightMaps) {
        return;
    }
    ts2::diag::Timer timer("floors: draw again");
    // Kept for the lot, so the floors' materials and textures are read once.
    if (!floorBuilder_) {
        floorBuilder_ = std::make_unique<ts2::scene::SceneBuilder>(*assets_.get());
    }
    auto& built = floorBuilder_->scene();
    built.meshes.clear();
    built.instances.clear();
    built.walls.clear();
    built.lotWalls.reset();
    if (!gridSupport_) {
        gridSupport_.emplace(app::floorSupport(*lotContent_, *assets_.get()));
    }
    floorBuilder_->setFloorAllowed([this](int level, int x, int y) { return gridSupport_->allowed(level, x, y); });
    ts2::scene::rebuildLotFloors(*floorBuilder_, *lotContent_, *lot_.lightMaps, levels);
    floorBuilder_->setFloorAllowed({});
    auto up = app::putRebuiltFloors(lot_, built, floorBuilder_->imageKeys(), sharedImages_, levels);
    renderer_.addMaterials(lot_);
    renderer_.addMeshes(lot_);
    renderer_.replaceMeshes(lot_, up.meshes);
    std::sort(up.instances.begin(), up.instances.end());
    up.instances.erase(std::unique(up.instances.begin(), up.instances.end()), up.instances.end());
    // The mouse picks the parts where they are drawn now.
    for (const std::size_t i : up.instances) {
        renderer_.setInstance(lot_, i);
        lotPicker_.update(lot_, i);
    }
    if (!up.images.empty()) {
        renderer_.setImages(lot_, up.images);
    }
    // The ground is kept under floored tiles whose corners aren't in one plane, so it moves when those change.
    if (auto twisted = twistedFloors(); twisted != twistedFloors_) {
        twistedFloors_ = std::move(twisted);
        groundChanged();
    }
    renderer_.show(lotShown());
}

void Game::showFloorPlan(const std::vector<ts2::content::FloorQuarter>& plan) {
    if (!lotContent_ || (plan.empty() && floorPlanWas_.empty())) {
        floorPlan_ = plan;
        return;
    }
    ts2::content::setFloors(*lotContent_, floorPlanWas_);
    floorPlanWas_ = ts2::content::setFloors(*lotContent_, plan);
    floorPlan_ = plan;
    std::set<std::int32_t> levels = std::exchange(floorPlanLevels_, {});
    for (const auto& q : plan) {
        floorPlanLevels_.insert(q.level);
    }
    levels.insert(floorPlanLevels_.begin(), floorPlanLevels_.end());
    redrawFloors(levels);
}

bool Game::floorObjectsBlock(std::int32_t level, std::int32_t x, std::int32_t y) {
    if (!lotContent_) {
        return false;
    }
    const auto key = std::array<std::int32_t, 3>{level, x, y};
    if (const auto known = floorBlocked_.find(key); known != floorBlocked_.end()) {
        return known->second;
    }
    constexpr std::size_t kPlacementOnFloor = 1;  // object data 0x2A bit 0
    constexpr float kFullStorey = 3.0f;
    const std::int32_t ground = ts2::content::groundGridLevel(*lotContent_);
    bool blocked = false;
    for (const auto& o : terrainObjects(x, y, level - ground)) {
        if ((o.placement & kPlacementOnFloor) == 0) {
            blocked = true;
        }
    }
    // Under a storey less than a full storey high, anything on the tile below blocks, but the exempt
    // objects (see TerrainObject::belowExempt).
    if (!blocked && level > 0) {
        const ts2::scene::Heights heights(*lotContent_);
        bool low = false;
        for (const auto [cx, cy] : {std::pair{x, y}, std::pair{x + 1, y}, std::pair{x, y + 1}, std::pair{x + 1, y + 1}}) {
            low = low || heights.corner(level, static_cast<float>(cx), static_cast<float>(cy)) - heights.corner(level - 1, static_cast<float>(cx), static_cast<float>(cy)) < kFullStorey;
        }
        if (low) {
            const auto below = terrainObjects(x, y, level - 1 - ground);
            blocked = std::any_of(below.begin(), below.end(), [](const auto& o) { return !o.belowExempt; });
        }
    }
    floorBlocked_.emplace(key, blocked);
    return blocked;
}

void Game::stopFloorTool() {
    showFloorPlan({});
    floorPlan_.clear();
    floorPlanWas_.clear();
    floorPlanKey_.clear();
    floorStart_.reset();
    floorHover_.reset();
    floorSupport_.reset();
    gridSupport_.reset();
    floorBlocked_.clear();
    wallLabels_.hide(screen_);
    if (floorTipShown_) {
        hideWorldTip();
        floorTipShown_ = false;
    }
}

std::optional<std::array<float, 2>> Game::storeyPoint(int storey, const std::array<float, 3>& ray, std::array<float, 2> near) const {
    const auto eye = orbit_.camera().eye;
    std::optional<std::array<float, 2>> at;
    constexpr int kTries = 4;
    for (int i = 0; i < kTries; ++i) {
        const auto p0 = lotLife_->scenePointAt(near[0], near[1], storey, lot_);
        const auto p1 = lotLife_->scenePointAt(near[0] + 1, near[1], storey, lot_);
        const auto p2 = lotLife_->scenePointAt(near[0], near[1] + 1, storey, lot_);
        const std::array<float, 3> a{p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, b{p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        const std::array<float, 3> n{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        const float along = n[0] * ray[0] + n[1] * ray[1] + n[2] * ray[2];
        if (std::abs(along) <= 1e-6f) {
            return std::nullopt;
        }
        const float t = (n[0] * (p0[0] - eye[0]) + n[1] * (p0[1] - eye[1]) + n[2] * (p0[2] - eye[2])) / along;
        if (t <= 0) {
            return std::nullopt;
        }
        at = lotLife_->routePoint({eye[0] + t * ray[0], eye[1] + t * ray[1], eye[2] + t * ray[2]});
        const std::array<float, 2> tile{std::floor((*at)[0]) + 0.5f, std::floor((*at)[1]) + 0.5f};
        if (tile == near) {
            return at;
        }
        near = tile;
    }
    // The mouse meets the storey only where it crosses a tile's own plane inside that tile. Near a
    // foundation the tiles' planes can send it back and forth, and then it meets none.
    return std::nullopt;
}

void Game::updateFloorTool(std::span<const std::uint8_t>, const std::optional<std::array<float, 3>>& ray) {
    using Tool = app::LotUi::Tool;
    const auto choice = lotUi_.floorChoice();
    if (!lotLife_ || !lotContent_ || buyTool_ != Tool::Floor || lotUi_.mode() != app::LotUi::Mode::Build || !choice) {
        if (floorHover_ || !floorPlanWas_.empty()) {
            stopFloorTool();
        }
        return;
    }
    const std::int32_t ground = ts2::content::groundGridLevel(*lotContent_);
    if (ray && !freeCam_) {
        const auto eye = orbit_.camera().eye;
        const auto onPlane = [&](std::int32_t level) -> std::optional<app::FloorSpot> {
            std::array<float, 2> near{0.5f, 0.5f};
            if (floorHover_) {
                near = {static_cast<float>(floorHover_->y) + 0.5f, static_cast<float>(floorHover_->x) + 0.5f};
            }
            const auto at = storeyPoint(level - ground + 1, *ray, near);
            // The floor grid's x and y are the route position's y and x.
            return at ? std::optional<app::FloorSpot>(app::floorSpotAt((*at)[1], (*at)[0], level)) : std::nullopt;
        };
        // The first storey the mouse meets decides: its floor, or its grid where a floor can go, else the
        // ground. The floors tested are the lot's own, not the plan shown.
        std::optional<app::FloorSpot> picked;
        const auto floored = [&](const app::FloorSpot& s) {
            const auto was = std::find_if(floorPlanWas_.begin(), floorPlanWas_.end(), [&](const auto& q) {
                return q.level == s.level && q.x == s.x && q.y == s.y && q.quarter == s.quarter;
            });
            return was != floorPlanWas_.end() ? !was->pattern.empty() : !ts2::content::floorPatternAt(*lotContent_, s.level, s.x, s.y, s.quarter).empty();
        };
        if (!floorSupport_) {
            floorSupport_.emplace(app::floorSupport(*lotContent_, *assets_.get()));
        }
        if (floorStart_) {
            // A drag stays on the storey it started on.
            picked = onPlane(floorStart_->level);
        } else {
            for (std::int32_t level = lotView_.view.level; level > ground && !picked; --level) {
                if (const auto s = onPlane(level); s && app::floorSurfaceAt(*lotContent_, *s) && (floored(*s) || floorSupport_->allowed(level, s->x, s->y))) {
                    picked = s;
                }
            }
        }
        if (!picked) {
            if (const auto hit = groundHit(lot_, *lotContent_, eye, *ray)) {
                const auto at = lotLife_->routePoint({eye[0] + (*ray)[0] * *hit, eye[1] + (*ray)[1] * *hit, eye[2] + (*ray)[2] * *hit});
                picked = app::floorSpotAt(at[1], at[0], ground);
            }
            // Over a pool the mouse reaches the pool's bottom.
            if (!picked || !app::floorSurfaceAt(*lotContent_, *picked)) {
                picked.reset();
                for (std::int32_t level = ground - 1; level >= 0 && !picked; --level) {
                    if (const auto s = onPlane(level); s && floored(*s)) {
                        picked = s;
                    }
                }
            }
        }
        if (picked) {
            floorHover_ = picked;
        }
    }
    if (!floorHover_) {
        return;
    }
    const bool remove = ts2::platform::ctrlHeld();
    const bool room = ts2::platform::keyHeld(ts2::platform::Key::Shift);
    const app::FloorSpot from = floorStart_.value_or(*floorHover_);
    // Dragging off the quarter pressed ends the one-quarter mode.
    if (quarterTiles_ && floorStart_ && !(*floorHover_ == from)) {
        quarterTiles_ = false;
    }
    const std::string pattern = remove ? std::string() : choice->pattern;
    const std::string key = std::format("{} {} {} {} {} {} {} {} {} {} {} {}", from.level, from.x, from.y, from.quarter, floorHover_->x, floorHover_->y,
                                        floorHover_->quarter, remove, room, quarterTiles_, pattern, lotLife_->funds());
    // A room's size counts each quarter as a quarter tile.
    const auto showLabels = [&] {
        const auto cost = static_cast<std::int32_t>(floorPlanCost_);
        const std::size_t quarters = floorPlan_.size();
        const std::string roomSize = quarters % 4 == 0 ? std::format("{}", quarters / 4) : std::format("{:g}", static_cast<double>(quarters) / 4);
        const std::string size = quarterTiles_ ? std::string("1 x 1")
                                 : room        ? roomSize
                                               : std::format("{} x {}", std::abs(floorHover_->x - from.x) + 1, std::abs(floorHover_->y - from.y) + 1);
        wallLabels_.show(screen_, cost, lotLife_->funds() < cost, size, lotMouseUi_[0], lotMouseUi_[1]);
    };
    if (key == floorPlanKey_) {
        showLabels();
        return;
    }
    floorPlanKey_ = key;
    // The plan is worked out without the one shown.
    ts2::content::setFloors(*lotContent_, floorPlanWas_);
    const bool wasShown = !floorPlanWas_.empty();
    floorPlanWas_.clear();
    if (!floorSupport_) {
        floorSupport_.emplace(app::floorSupport(*lotContent_, *assets_.get()));
    }
    const auto& support = *floorSupport_;
    const auto allowed = [&](std::int32_t level, std::int32_t x, std::int32_t y) {
        return (level <= ground || support.allowed(level, x, y)) && !floorObjectsBlock(level, x, y);
    };
    // A diagonal wall across the tile: 1 from its low corner, 2 the other way.
    const auto diagonal = [&](std::int32_t level, std::int32_t x, std::int32_t y) {
        if (!lot_.lotWalls) {
            return 0;
        }
        const float fx = static_cast<float>(x), fy = static_cast<float>(y);
        for (const auto& w : lot_.lotWalls->walls) {
            if (w.level != level) {
                continue;
            }
            const float lowX = std::min(w.q.x0, w.q.x1), lowY = std::min(w.q.y0, w.q.y1);
            if (std::abs(w.q.x1 - w.q.x0) != 1 || std::abs(w.q.y1 - w.q.y0) != 1 || lowX != fx || lowY != fy) {
                continue;
            }
            return (w.q.x1 - w.q.x0) * (w.q.y1 - w.q.y0) > 0 ? 1 : 2;
        }
        return 0;
    };
    const auto plan = app::keepPlatformFloors(*lotContent_, app::floorStroke(*lotContent_, from, *floorHover_, pattern, {quarterTiles_, room}, diagonal, allowed),
                                              assets_.get()->buildScripts().platformFloors);
    floorPlanCost_ = app::floorStrokeCost(*lotContent_, plan, pattern.empty() ? app::FloorStroke::Remove : app::FloorStroke::Lay, [&](const std::string& name) { return lotUi_.floorPrice(name); });
    const bool refused = room && !quarterTiles_ && !app::floorFillable(*lotContent_, from);
    if (refused) {
        constexpr std::uint32_t kUiText = 0x7FA7DC7C;  // string id, leakcheck: ok
        constexpr int kNotEnclosed = 0x58;
        const std::string text = app::placementErrorText(
            [&](std::uint32_t table, std::uint32_t line) { return screen_.data().text(kUiText, table << 16 | line); }, kNotEnclosed, 0, 0);
        const auto at = lotLife_->scenePointAt(static_cast<float>(from.y) + 0.5f, static_cast<float>(from.x) + 0.5f, from.level - ground + 1, lot_);
        if (!text.empty()) {
            showWorldTip(text, at);
            floorTipShown_ = true;
        }
    } else if (floorTipShown_) {
        hideWorldTip();
        floorTipShown_ = false;
    }
    if (plan == floorPlan_ && wasShown) {
        floorPlanWas_ = ts2::content::setFloors(*lotContent_, plan);
    } else if (plan.empty() && wasShown) {
        floorPlan_.clear();
        redrawFloors(std::exchange(floorPlanLevels_, {}));
    } else {
        showFloorPlan(plan);
    }
    showLabels();
}

void Game::floorButton(bool down) {
    if (!lotLife_ || !lotContent_) {
        return;
    }
    if (down) {
        floorStart_ = floorHover_;
        floorPlanKey_.clear();
        return;
    }
    if (!floorStart_) {
        return;
    }
    const app::FloorSpot start = *floorStart_;
    floorStart_.reset();
    floorPlanKey_.clear();
    const auto cost = static_cast<std::int32_t>(floorPlanCost_);
    if (floorPlan_.empty()) {
        return;
    }
    if (cost > lotLife_->funds()) {
        showFloorPlan({});
        floorPlanWas_.clear();
        if (audio_) {
            (void)audio_->playUi("ui_error");
        }
        return;
    }
    // The plan shown stays laid. A stroke on the ground storey first levels the box from its start to its
    // end to the start tile's corner height, where the terrain check allows.
    const std::int32_t ground = ts2::content::groundGridLevel(*lotContent_);
    ts2::content::TerrainChanges groundLevelled;
    const bool room = ts2::platform::keyHeld(ts2::platform::Key::Shift) && !quarterTiles_;
    // A Ctrl stroke takes floors up and levels nothing, even where it puts a platform's own floor back.
    const bool removing = ts2::platform::ctrlHeld();
    const bool laying = !removing && std::any_of(floorPlan_.begin(), floorPlan_.end(), [](const auto& q) { return !q.pattern.empty(); });
    if (laying && !room && start.level == ground && floorHover_) {
        startWallManager();
        ts2::content::TerrainEdit edit(*lotContent_, *wallManager_, assets_.get()->buildScripts(),
                                       [this](std::int32_t x, std::int32_t y, std::int32_t level) { return terrainObjects(x, y, level); },
                                       groundLevelled, lotLife_ ? lotLife_->simulation().world().lotShape().beachSide : std::nullopt);
        // The game levels before it lays the floors. The plan's own floors would make the check refuse
        // every corner.
        ts2::content::setFloors(*lotContent_, floorPlanWas_);
        const auto end = quarterTiles_ ? start : *floorHover_;
        // The terrain edit counts corners from the lot's world bounds, the floor grid from 0.
        const auto bounds = lotContent_->worldBounds.value_or(ts2::content::WorldBounds{});
        const ts2::content::TerrainCorner a{std::min(start.x, end.x) + bounds.minX, std::min(start.y, end.y) + bounds.minY, 0};
        const ts2::content::TerrainCorner b{std::max(start.x, end.x) + 1 + bounds.minX, std::max(start.y, end.y) + 1 + bounds.minY, 0};
        edit.flattenFloorBox(a, b, edit.height({start.x + bounds.minX, start.y + bounds.minY, 0}));
        floorPlanWas_ = ts2::content::setFloors(*lotContent_, floorPlan_);
    }
    auto heightsBefore = removing ? decltype(ts2::content::levelAroundFloors(*lotContent_, floorPlan_, ground)){}
                                  : ts2::content::levelAroundFloors(*lotContent_, floorPlan_, ground);
    auto heightsAfter = heightsBefore;
    if (const auto heights = lotContent_->grids.find(ts2::content::kFloorHeightGridInstance); heights != lotContent_->grids.end()) {
        for (auto& c : heightsAfter) {
            c.z = heights->second.f32(c.level, c.x, c.y, 0);
        }
    }
    floorsLaid_.push_back({floorPlanWas_, floorPlan_, std::move(heightsBefore), std::move(heightsAfter), std::move(groundLevelled)});
    keepFloorPlan(cost, "ui_build_flooring_plop");
}

void Game::keepFloorPlan(std::int32_t cost, const char* sound) {
    // A lot with no household and no Sim selected builds free, though the funds are still tested.
    const std::int32_t charged = buildFree() ? 0 : cost;
    buyHand_.recordFloors(static_cast<std::uint32_t>(floorsLaid_.size() - 1), charged);
    lotLife_->handPay(charged);
    floorPlan_.clear();
    floorPlanWas_.clear();
    floorPlanLevels_.clear();
    floorSupport_.reset();
    // The levelling moved corners, so which tiles hold too short a storey may have changed.
    floorBlocked_.clear();
    if (audio_ && sound != nullptr) {
        (void)audio_->playUi(sound);
    }
    // A change kept in the history plays the buy or the sell sound by what it cost.
    if (charged != 0) {
        moneyText_.show(screen_, charged, lotMouseUi_[0], lotMouseUi_[1]);
        if (audio_) {
            (void)audio_->playUi(charged > 0 ? "ui_object_buy" : "ui_object_sell");
        }
    }
    const auto& laid = floorsLaid_.back();
    if (!laid.ground.empty()) {
        groundChanged();
    }
    if (!laid.heightsBefore.empty() || !laid.ground.empty()) {
        floorsChanged();
    } else {
        lotShapeChanged();
    }
}

void Game::smashFloor(std::int32_t level, const std::array<float, 2>& at) {
    if (!lotLife_ || !lotContent_) {
        return;
    }
    // The game takes up the whole tile's floor, all four quarters. Over a foundation or deck it takes
    // out the platform's tile instead. Pools and stages are not done yet.
    const auto spot = app::floorSpotAt(at[1], at[0], level);
    if (smashPlatform(spot.level, spot.x, spot.y) || app::platformTile(*lotContent_, spot.level, spot.x, spot.y)) {
        return;
    }
    std::vector<ts2::content::FloorQuarter> plan;
    for (int quarter = 0; quarter < 4; ++quarter) {
        if (!ts2::content::floorPatternAt(*lotContent_, spot.level, spot.x, spot.y, quarter).empty()) {
            plan.push_back({spot.level, spot.x, spot.y, quarter, {}, 1});
        }
    }
    if (plan.empty()) {
        return;
    }
    stopFloorTool();
    const auto cost = static_cast<std::int32_t>(app::floorStrokeCost(*lotContent_, plan, app::FloorStroke::Remove,
                                                                     [&](const std::string& name) { return lotUi_.floorPrice(name); }));
    showFloorPlan(plan);
    floorsLaid_.push_back({floorPlanWas_, floorPlan_, {}, {}, {}});
    keepFloorPlan(cost, nullptr);
}

void Game::toggleFloors(std::uint32_t id, bool in) {
    if (!lotContent_ || id >= floorsLaid_.size()) {
        return;
    }
    // A plan on show goes first, so it doesn't stay laid over what comes back.
    stopFloorTool();
    auto& laid = floorsLaid_[id];
    ts2::content::setFloors(*lotContent_, in ? laid.after : laid.before);
    ts2::content::setCornerHeights(*lotContent_, in ? laid.heightsAfter : laid.heightsBefore);
    if (!laid.ground.empty()) {
        laid.ground.toggle(*lotContent_);
        groundChanged();
    }
    floorsChanged();
}

} // namespace app
