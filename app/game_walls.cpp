// Build mode's Wall tool: a drag from one tile corner to another along a tile edge line builds plain
// walls there (blank on both sides), Ctrl held as it starts takes such walls down. Rooms are numbered
// again on that storey, the simulation's walls, rooms and route grids follow, and the storey's walls
// are drawn again where their corners changed. On the lowest storey walls go anywhere on the lot
// (not along its edge), above it only beside floor. No wall goes through a multi-tile object, and
// none comes down with a door or window in it or something hung on it. New walls are lit as things
// outdoors until the lot's light is worked out again. Diagonal walls, half walls and the room tool
// aren't done.
#include "game.hpp"

#include "content/floor_edit.hpp"
#include "diag/log.hpp"
#include "scene/lot_parts.hpp"
#include "sim/wall_build.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>

namespace app {

namespace pf = ts2::platform;

namespace {

// What a wall costs a tile, and gives back taken down. Not read from the game's files: chosen for now.
constexpr std::int32_t kWallPrice = 70;
// The covering new walls get (walls.txt: wall 1's defaultPattern).
constexpr const char* kNewWallPattern = "blank";

std::string simoleons(std::int32_t amount) {
    std::string digits = std::to_string(std::abs(static_cast<long long>(amount)));
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<std::size_t>(i), ",");
    }
    return "\xC2\xA7" + digits;
}

} // namespace

void Game::pickWallTool() {
    sledgeActive_ = false;
    if (!buildActive_ || !lotLife_) {
        return;
    }
    BuildTool tool;
    tool.buildsWalls = true;
    tool.name = "Wall";
    tool.price = kWallPrice;
    buildTool_ = std::move(tool);
    wallDragFrom_.reset();
    wallCornerAt_.reset();
    ts2::diag::step("build mode: wall tool picked");
}

bool Game::wallToolMouse(const pf::Event& e, float x, float y, bool overPanels) {
    auto showTip = [&] {
        if (!wallCornerAt_ || (overPanels && !wallDragFrom_)) {
            buildTip({});
            return;
        }
        if (!wallDragFrom_) {
            buildTip(std::format("Wall: {} a tile", simoleons(kWallPrice)));
            return;
        }
        const auto steps = ts2::sim::wallSteps(0, (*wallDragFrom_)[0], (*wallDragFrom_)[1], (*wallCornerAt_)[0], (*wallCornerAt_)[1]);
        const auto n = static_cast<std::int32_t>(steps.size());
        buildTip(wallRemoving_ ? std::format("Take down {} wall{}", n, n == 1 ? "" : "s")
                               : std::format("{} wall{}: {}", n, n == 1 ? "" : "s", simoleons(kWallPrice * n)));
    };
    if (e.type == pf::Event::Type::MouseMotion) {
        if (const auto corner = buildCornerAt(x, y)) {
            wallCornerAt_ = corner;
        } else if (!wallDragFrom_) {
            wallCornerAt_.reset();
        }
        showTip();
        return false;  // the camera may still move
    }
    if (e.button != pf::MouseButton::Left) {
        return false;
    }
    if (e.type == pf::Event::Type::MouseDown && !overPanels) {
        if (const auto corner = buildCornerAt(x, y)) {
            wallDragFrom_ = corner;
            wallCornerAt_ = corner;
            wallRemoving_ = e.ctrl;
            showTip();
        }
        return true;
    }
    if (e.type == pf::Event::Type::MouseUp && wallDragFrom_) {
        const auto to = wallCornerAt_.value_or(*wallDragFrom_);
        const std::string message = buildWallLine(to);
        if (!message.empty()) {
            ts2::diag::step(std::format("build mode: no walls from {} {} to {} {}: {}", (*wallDragFrom_)[0], (*wallDragFrom_)[1], to[0], to[1], message));
        }
        wallDragFrom_.reset();
        if (const auto corner = buildCornerAt(x, y)) {
            wallCornerAt_ = corner;
        }
        if (message.empty()) {
            showTip();
        } else {
            buildTip(message);
        }
        return true;
    }
    return false;
}

std::string Game::buildWallLine(std::array<std::int32_t, 2> to) {
    if (!buildTool_ || !buildTool_->buildsWalls || !wallDragFrom_ || !buildLot_ || !buildLot_->roomGraph || !lotLife_ || !drawnWalls_) {
        return {};
    }
    auto& lot = *buildLot_;
    const int level = lotView_.view.level;  // a grid level
    const int ground = ts2::content::groundGridLevel(lot);
    const std::int32_t graphLevel = level + lot.roomGraph->firstLevel;
    const std::int32_t storey = level - ground + 1;
    const auto steps = ts2::sim::wallSteps(graphLevel, (*wallDragFrom_)[0], (*wallDragFrom_)[1], to[0], to[1]);
    if (steps.empty()) {
        return {};
    }
    // Worked out on copies, kept only once paid for.
    auto graph = *lot.roomGraph;
    auto coverings = lot.wallCoverings;
    auto patterns = lot.wallPatterns;
    std::vector<std::int32_t> built, removed;
    std::int32_t cost = 0;
    if (wallRemoving_) {
        // Not a wall with a door or window in it.
        std::set<std::int32_t> holed;
        for (const auto& d : drawnWalls_->walls) {
            if (!d.removed && d.graphWall >= 0 && (!d.wall.leftMask.empty() || !d.wall.rightMask.empty())) {
                holed.insert(d.graphWall);
            }
        }
        bool doorInTheWay = false;
        const ts2::wallgraph::PointIndex points(graph);
        auto holedAt = [&](const ts2::sim::WallStep& s) {
            for (const auto& w : graph.walls) {
                const auto* a = points(w.from);
                const auto* b = points(w.to);
                if (a == nullptr || b == nullptr || a->level != s.level || !holed.contains(w.id)) {
                    continue;
                }
                const auto same = [](const ts2::wallgraph::Point* p, std::int32_t px, std::int32_t py) {
                    return std::lround(p->x) == px && std::lround(p->y) == py;
                };
                if ((same(a, s.x0, s.y0) && same(b, s.x1, s.y1)) || (same(a, s.x1, s.y1) && same(b, s.x0, s.y0))) {
                    return true;
                }
            }
            return false;
        };
        removed = ts2::sim::removeWalls(graph, coverings, patterns, steps, [&](const ts2::sim::WallStep& s) {
            const bool holedHere = holedAt(s);
            doorInTheWay = doorInTheWay || holedHere;
            return !holedHere;
        });
        if (removed.empty()) {
            return doorInTheWay ? "Take the door or window out of the wall first." : std::string();
        }
        ts2::content::Lot candidate = lot;
        candidate.roomGraph = graph;
        candidate.wallCoverings = coverings;
        if (const std::int16_t object = lotLife_->objectNeedingWalls(candidate); object != 0) {
            ts2::diag::step(std::format("build mode: object {} needs the walls that would come down", object));
            return "Something on the wall needs it.";
        }
        cost = -kWallPrice * static_cast<std::int32_t>(removed.size());
    } else {
        // Above the lowest storey a wall stands beside floor.
        const auto floors = lot.grids.find(ts2::content::kFloorGridInstance);
        auto floored = [&](std::int32_t x, std::int32_t y) {
            if (floors == lot.grids.end() || !floors->second.contains(level, x, y)) {
                return false;
            }
            for (std::size_t q = 0; q < 4; ++q) {
                if (floors->second.i16(level, x, y, q) != 0) {
                    return true;
                }
            }
            return false;
        };
        bool objectInTheWay = false;
        built = ts2::sim::buildWalls(graph, coverings, patterns, steps, kNewWallPattern, [&](const ts2::sim::WallStep& s) {
            if (level != ground) {
                const bool alongY = s.x0 == s.x1;
                const std::int32_t lowX = std::min(s.x0, s.x1), lowY = std::min(s.y0, s.y1);
                const bool beside = alongY ? (floored(s.x0 - 1, lowY) || floored(s.x0, lowY)) : (floored(lowX, s.y0 - 1) || floored(lowX, s.y0));
                if (!beside) {
                    return false;
                }
            }
            if (lotLife_->objectAcross(storey, s.x0, s.y0, s.x1, s.y1)) {
                objectInTheWay = true;
                return false;
            }
            return true;
        });
        if (built.empty()) {
            return objectInTheWay ? "Something is in the way."
                   : level != ground ? "Upstairs, walls go beside floor."
                                     : "No new wall can go there: walls are there already, or it's the lot's edge.";
        }
        cost = kWallPrice * static_cast<std::int32_t>(built.size());
        if (cost > lotLife_->funds()) {
            return "You can't afford that.";
        }
    }
    const WallState before = wallState();  // for undoing it
    edits_.walls = true;  // walls aren't saved yet
    ts2::sim::renumberRooms(graph, graphLevel);
    lotLife_->addFunds(-cost);
    lot.roomGraph = std::move(graph);
    lot.wallCoverings = std::move(coverings);
    lot.wallPatterns = std::move(patterns);
    redrawLevelWalls(level, built, removed);
    if (const auto rooms = lot.grids.find(ts2::content::kRoomGridInstance); rooms != lot.grids.end() && lot.roomGraph) {
        ts2::sim::updateRoomGrid(rooms->second, *lot.roomGraph, graphLevel);
    }
    lotLife_->wallsChanged(lot);
    recordStep(wallRemoving_ ? "taking walls down" : "building walls",
               [this, level, before, cost] {
                   applyWallState(level, before);
                   lotLife_->addFunds(cost);
               },
               [this, level, after = wallState(), cost] {
                   applyWallState(level, after);
                   lotLife_->addFunds(-cost);
               });
    ts2::diag::step(std::format("build mode: {} {} wall{} on storey {} from {} {} to {} {}: {} {}", wallRemoving_ ? "took down" : "built",
                                wallRemoving_ ? removed.size() : built.size(), (wallRemoving_ ? removed.size() : built.size()) == 1 ? "" : "s",
                                storey, (*wallDragFrom_)[0], (*wallDragFrom_)[1], to[0], to[1], cost < 0 ? "got back" : "paid", std::abs(cost)));
    return {};
}

void Game::redrawLevelWalls(int level, const std::vector<std::int32_t>& built, const std::vector<std::int32_t>& removed, bool force) {
    if (!drawnWalls_ || !buildLot_ || !buildLot_->roomGraph || !lotLife_) {
        return;
    }
    auto& drawn = drawnWalls_->walls;
    const auto& graph = *buildLot_->roomGraph;
    const ts2::wallgraph::PointIndex points(graph);
    std::map<std::int32_t, const ts2::wallgraph::Wall*> graphWalls;
    for (const auto& w : graph.walls) {
        graphWalls[w.id] = &w;
    }
    auto hide = [&](int instance) {
        if (instance >= 0) {
            const auto& mesh = lot_.instances[static_cast<std::size_t>(instance)].mesh;
            const std::size_t parts = mesh >= 0 ? lot_.meshes[static_cast<std::size_t>(mesh)].primitives.size() : 0;
            renderer_.setMaterials(static_cast<std::size_t>(instance), std::vector<int>(parts, ts2::scene::kHiddenMaterial));
        }
    };
    // Walls taken down draw nothing.
    for (std::size_t i = 0; i < drawn.size(); ++i) {
        if (!drawn[i].removed && std::find(removed.begin(), removed.end(), drawn[i].graphWall) != removed.end()) {
            drawn[i].removed = true;
            for (const int instance : wallInstances_[i]) {
                hide(instance);
                wallOfInstance_.erase(static_cast<std::size_t>(instance));
            }
            if (drawn[i].sceneWall >= 0) {
                auto& info = lot_.walls[static_cast<std::size_t>(drawn[i].sceneWall)];
                info.roomLeft = info.roomRight = false;
            }
        }
    }
    // Walls built join the drawn ones, blank on both sides, a storey high.
    const ts2::scene::Heights heights(*buildLot_);
    const std::string blank = assets_.get()->patternMaterial(kNewWallPattern, true);
    for (const std::int32_t id : built) {
        const auto w = graphWalls.find(id);
        const auto* a = w != graphWalls.end() ? points(w->second->from) : nullptr;
        const auto* b = w != graphWalls.end() ? points(w->second->to) : nullptr;
        if (a == nullptr || b == nullptr) {
            continue;
        }
        ts2::scene::DrawnWall d;
        auto& t = d.wall;
        // Up to the storey above's floor, or a storey high where the lot keeps no height for it there
        // (open ground, where the grid's next level isn't a storey up).
        auto top = [&](const ts2::wallgraph::Point* p, float bottom) {
            const float above = heights.corner(level + 1, p->x, p->y);
            return above - bottom < ts2::scene::kLevelHeight / 2 || above - bottom > ts2::scene::kLevelHeight * 2 ? bottom + ts2::scene::kLevelHeight
                                                                                                                    : above;
        };
        const float bottomA = heights.corner(level, a->x, a->y), bottomB = heights.corner(level, b->x, b->y);
        t.q = {a->x, a->y, b->x, b->y, bottomA, top(a, bottomA), bottomB, top(b, bottomB)};
        t.points = {w->second->from, w->second->to};
        t.level = level;
        t.left = t.leftCovering = t.right = t.rightCovering = blank;
        d.graphWall = id;
        d.coverable = true;
        float from[3] = {a->x, a->y, 0}, toPoint[3] = {b->x, b->y, 0};
        lotPlace_.apply(from, true);
        lotPlace_.apply(toPoint, true);
        d.sceneWall = static_cast<int>(lot_.walls.size());
        lot_.walls.push_back({from[0], from[1], toPoint[0], toPoint[1], level, false, false, true, false});
        drawn.push_back(std::move(d));
        wallInstances_.push_back({-1, -1});
    }
    // The storey's rooms: which sides of each wall look into one, for the views' cutaway.
    std::vector<std::size_t> onLevel;
    for (std::size_t i = 0; i < drawn.size(); ++i) {
        if (drawn[i].removed || drawn[i].wall.level != level) {
            continue;
        }
        onLevel.push_back(i);
        if (const auto w = graphWalls.find(drawn[i].graphWall); w != graphWalls.end()) {
            drawn[i].wall.roomLeft = w->second->leftRoom != 0;
            drawn[i].wall.roomRight = w->second->rightRoom != 0;
            if (drawn[i].sceneWall >= 0) {
                auto& info = lot_.walls[static_cast<std::size_t>(drawn[i].sceneWall)];
                info.roomLeft = drawn[i].wall.roomLeft;
                info.roomRight = drawn[i].wall.roomRight;
            }
        }
    }
    // Where the storey's walls meet: each one's corners, and the hubs where three or more meet.
    std::vector<ts2::scene::ThickWall> thick;
    for (const std::size_t i : onLevel) {
        thick.push_back(drawn[i].wall);
    }
    const auto hubs = ts2::scene::joinWalls(thick);
    renderer_.addMaterials(lot_);
    auto place = [&](int mesh, const char* name, ts2::scene::Part part, bool cut, std::vector<int> walls, bool litOutdoors) {
        ts2::scene::Instance added;
        added.mesh = mesh;
        added.name = name;
        added.transform = lotPlace_;
        added.part = part;
        added.level = level;
        added.cut = cut;
        added.walls = std::move(walls);
        added.litOutdoors = litOutdoors;
        lot_.instances.push_back(std::move(added));
        return lot_.instances.size() - 1;
    };
    std::size_t redrawn = 0;
    for (std::size_t k = 0; k < onLevel.size(); ++k) {
        auto& d = drawn[onLevel[k]];
        const auto& now = thick[k];
        const bool isNew = wallInstances_[onLevel[k]][0] < 0 && wallInstances_[onLevel[k]][1] < 0;
        const bool moved = now.leftA != d.wall.leftA || now.leftB != d.wall.leftB || now.rightA != d.wall.rightA || now.rightB != d.wall.rightB ||
                           now.joined != d.wall.joined;
        if (!isNew && !moved && !force) {
            continue;
        }
        d.wall = now;
        ++redrawn;
        for (const bool cut : {false, true}) {
            if (cut && !d.wall.cuttable) {
                continue;
            }
            const int mesh = lotLife_->wallMesh(d, wallTopMaterial_, cut, lot_);
            if (mesh < 0) {
                continue;
            }
            int& instance = wallInstances_[onLevel[k]][cut ? 1 : 0];
            if (instance < 0) {
                // A new wall has no light-map rect yet: it is lit as things outdoors are.
                instance = static_cast<int>(place(mesh, "wall", ts2::scene::Part::Wall, cut, {d.sceneWall}, true));
                wallOfInstance_[static_cast<std::size_t>(instance)] = onLevel[k];
            } else {
                lot_.instances[static_cast<std::size_t>(instance)].mesh = mesh;
            }
            renderer_.addMaterials(lot_);
            renderer_.addMeshes(lot_);
            renderer_.setInstance(lot_, static_cast<std::size_t>(instance));
            lotPicker_.update(lot_, static_cast<std::size_t>(instance));
        }
    }
    // The storey's hubs drawn again: the old ones draw nothing.
    for (std::size_t i = 0; i < lot_.instances.size(); ++i) {
        const auto& instance = lot_.instances[i];
        if (instance.part == ts2::scene::Part::WallHub && instance.level == level && instance.object == 0) {
            hide(static_cast<int>(i));
        }
    }
    for (const auto& hub : hubs) {
        std::vector<int> walls;
        for (const std::size_t i : hub.walls) {
            walls.push_back(drawn[onLevel[i]].sceneWall);
        }
        for (const bool cut : {false, true}) {
            const float top = cut ? std::min(hub.top, hub.bottom + ts2::scene::kCutWallHeight) : hub.top;
            ts2::scene::Primitive fan;
            for (std::size_t i = 0; i < hub.corners.size(); ++i) {
                const auto& c0 = hub.corners[i];
                const auto& c1 = hub.corners[(i + 1) % hub.corners.size()];
                for (const auto& p : {std::array<float, 3>{hub.x, hub.y, top}, std::array<float, 3>{c0[0], c0[1], top},
                                      std::array<float, 3>{c1[0], c1[1], top}}) {
                    ts2::scene::addVertex(fan, p, {0, 0, 1}, {p[0], p[1]});
                }
            }
            if (fan.indices.empty() || wallTopMaterial_ < 0) {
                continue;
            }
            fan.material = wallTopMaterial_;
            ts2::scene::Mesh mesh;
            mesh.name = "wall hub";
            mesh.primitives.push_back(std::move(fan));
            lot_.meshes.push_back(std::move(mesh));
            const auto instance = place(static_cast<int>(lot_.meshes.size() - 1), "wall hub", ts2::scene::Part::WallHub, cut, walls, false);
            renderer_.addMeshes(lot_);
            renderer_.setInstance(lot_, instance);
        }
    }
    ts2::diag::info(std::format("build mode: {} walls drawn again on grid level {}, {} hubs", redrawn, level, hubs.size()));
}

} // namespace app
