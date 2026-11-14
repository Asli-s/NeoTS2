// Build mode on a lot: the panel opens over the paused game. Its floor tool lays the floor picked
// from the catalog over the tiles dragged across (or one clicked), paying for it a tile at a time and
// getting back what the floor it covers cost. Ctrl held as the drag starts takes floor up instead,
// paying back what it cost. On the ground storey floor goes anywhere but over a pool. Above it only
// floors already there can be covered, until walls and floors that make new rooms are built. Its wall
// covering tool covers the wall side clicked, or with Shift every side facing the same room on that
// storey, a wall side at a time, giving back what the coverings it replaces cost. What is changed lasts
// until the lot is left, as lots aren't saved yet.
#include "game.hpp"

#include "content/floor_edit.hpp"
#include "content/wall_edit.hpp"
#include "diag/log.hpp"
#include "scene/lot_parts.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <map>
#include <set>

namespace app {

namespace pf = ts2::platform;

namespace {

// "§1,234".
std::string simoleons(std::int32_t amount) {
    std::string digits = std::to_string(std::abs(static_cast<long long>(amount)));
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<std::size_t>(i), ",");
    }
    return "\xC2\xA7" + digits;
}

// The price per tile of a floor the lot's pattern list names by its catalog GUID in hex. None for a
// floor not in the catalog (a built-in one, such as the lot's road).
std::optional<std::int32_t> catalogPrice(const std::vector<ts2::scene::Assets::CatalogPattern>& catalog, const std::string& name) {
    std::uint32_t guid = 0;
    const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), guid, 16);
    if (error != std::errc{} || end != name.data() + name.size() || name.empty()) {
        return std::nullopt;
    }
    const auto it = std::find_if(catalog.begin(), catalog.end(), [&](const auto& floor) { return floor.guid == guid; });
    return it != catalog.end() ? std::optional(it->price) : std::nullopt;
}

} // namespace

void Game::pickPattern(std::uint32_t guid, bool wall) {
    if (!buildActive_ || !lotLife_) {
        return;
    }
    const auto& catalog = wall ? assets_.get()->wallCatalog() : assets_.get()->floorCatalog();
    const auto it = std::find_if(catalog.begin(), catalog.end(), [&](const auto& pattern) { return pattern.guid == guid; });
    if (it == catalog.end()) {
        return;
    }
    // Catalog floors and wall coverings are both named by their GUID in hex in the lot's lists.
    BuildTool tool;
    tool.wall = wall;
    tool.guid = guid;
    tool.pattern = ts2::content::floorPatternName(guid);
    tool.name = it->name;
    tool.price = it->price;
    buildTool_ = std::move(tool);
    floorDragFrom_.reset();
    floorTileAt_.reset();
    ts2::diag::step(std::format("build mode: {} {} ({:08x}) picked", wall ? "wall covering" : "floor", it->name, guid));
}

void Game::dropBuildTool() {
    if (buildTool_) {
        ts2::diag::step(buildTool_->buildsWalls ? "build mode: wall tool put down"
                        : buildTool_->wall      ? "build mode: wall covering tool put down"
                                                : "build mode: floor tool put down");
    }
    buildTool_.reset();
    floorDragFrom_.reset();
    floorTileAt_.reset();
    wallDragFrom_.reset();
    wallCornerAt_.reset();
    buildTip({});
}

// The wall covering tool: the tip names the covering and its price over a wall side it can cover,
// and a click covers that side, or with Shift every side facing the same room.
bool Game::wallMouse(const pf::Event& e, float x, float y, bool overPanels) {
    if (e.type == pf::Event::Type::MouseMotion) {
        const auto side = overPanels ? std::nullopt : wallSideAt(x, y);
        buildTip(side ? std::format("{}: {} a wall", buildTool_->name, simoleons(buildTool_->price)) : std::string());
        return false;  // the camera may still move
    }
    if (e.type != pf::Event::Type::MouseDown || e.button != pf::MouseButton::Left || overPanels) {
        return false;
    }
    if (const auto side = wallSideAt(x, y)) {
        const std::string message = coverWalls(side->first, side->second, e.shift);
        buildTip(message.empty() ? std::format("{}: {} a wall", buildTool_->name, simoleons(buildTool_->price)) : message);
    }
    return true;
}

void Game::buildTip(const std::string& text) {
    screen_.setSpotTip(text);
    if (!text.empty() && buildMouseAt_) {
        const auto [ux, uy] = toUi((*buildMouseAt_)[0], (*buildMouseAt_)[1]);
        screen_.moveSpotTip(ux, uy);
    }
}

bool Game::buildMouse(const pf::Event& e, float x, float y) {
    if (!buildActive_ || !lotLife_ || !buildTool_) {
        return false;
    }
    const auto [ix, iy] = toUi(x, y);
    const bool overPanels = screen_.over(ix, iy);
    if (buildTool_->buildsWalls) {
        if (!overPanels || wallDragFrom_) {
            buildMouseAt_ = {x, y};
        }
        return wallToolMouse(e, x, y, overPanels);
    }
    if (buildTool_->wall) {
        if (!overPanels) {
            buildMouseAt_ = {x, y};
        }
        return wallMouse(e, x, y, overPanels);
    }
    if (!overPanels || floorDragFrom_) {
        buildMouseAt_ = {x, y};
    }
    // What the tip says: the floor and its price, and while dragging how much it covers.
    auto showTip = [&] {
        if (!floorTileAt_ || (overPanels && !floorDragFrom_)) {
            buildTip({});
            return;
        }
        if (!floorDragFrom_) {
            buildTip(std::format("{}: {} a tile", buildTool_->name, simoleons(buildTool_->price)));
            return;
        }
        const ts2::content::TileRect rect{(*floorDragFrom_)[0], (*floorDragFrom_)[1], (*floorTileAt_)[0], (*floorTileAt_)[1]};
        const std::int32_t tiles = rect.width() * rect.height();
        buildTip(floorRemoving_ ? std::format("Take up {} x {}", rect.width(), rect.height())
                                : std::format("{} x {}: {}", rect.width(), rect.height(), simoleons(buildTool_->price * tiles)));
    };
    if (e.type == pf::Event::Type::MouseMotion) {
        if (const auto tile = buildTileAt(x, y)) {
            floorTileAt_ = tile;
        } else if (!floorDragFrom_) {
            floorTileAt_.reset();
        }
        showTip();
        return false;  // the camera may still move
    }
    if (e.button != pf::MouseButton::Left) {
        return false;
    }
    if (e.type == pf::Event::Type::MouseDown && !overPanels) {
        if (const auto tile = buildTileAt(x, y)) {
            floorDragFrom_ = tile;
            floorTileAt_ = tile;
            floorRemoving_ = e.ctrl;
            ts2::diag::step(std::format("build mode: drag from tile {} {} at grid level {}{}", (*tile)[0], (*tile)[1], lotView_.view.level, e.ctrl ? ", taking floor up" : ""));
            showTip();
        }
        return true;
    }
    if (e.type == pf::Event::Type::MouseUp && floorDragFrom_) {
        const std::string message = layFloors(floorTileAt_.value_or(*floorDragFrom_));
        floorDragFrom_.reset();
        if (const auto tile = buildTileAt(x, y)) {
            floorTileAt_ = tile;
        }
        // The tip goes back to the floor and its price, or says why nothing was laid.
        if (message.empty()) {
            showTip();
        } else {
            buildTip(message);
        }
        return true;
    }
    return false;
}

bool Game::buildKey(const pf::Event& e) {
    if (!buildActive_ || !buildTool_ || e.type != pf::Event::Type::KeyDown || e.key != pf::Key::Escape) {
        return false;
    }
    // Esc lets go of a drag, then of the tool.
    if (!e.repeat) {
        if (floorDragFrom_ || wallDragFrom_) {
            floorDragFrom_.reset();
            wallDragFrom_.reset();
            buildTip({});
        } else {
            dropBuildTool();
        }
    }
    return true;
}

std::string Game::layFloors(std::array<std::int32_t, 2> to) {
    if (!buildTool_ || !floorDragFrom_ || !buildLot_ || !lotLife_) {
        return {};
    }
    auto& lot = *buildLot_;
    const auto grid = lot.grids.find(ts2::content::kFloorGridInstance);
    if (grid == lot.grids.end()) {
        return {};
    }
    const int level = lotView_.view.level;
    const int ground = ts2::content::groundGridLevel(lot);
    const ts2::content::TileRect rect{(*floorDragFrom_)[0], (*floorDragFrom_)[1], to[0], to[1]};
    if (floorRemoving_ && level != ground) {
        return "Only floors on the ground can be taken up yet.";
    }
    // Worked out on copies, kept only once paid for.
    auto floors = grid->second;
    auto patterns = lot.floorPatterns;
    const auto surface = lot.grids.find(ts2::content::kSurfaceGridInstance);
    const auto change = floorRemoving_ ? ts2::content::removeFloor(floors, patterns, level, rect)
                                       : ts2::content::layFloor(floors, patterns, level, rect, buildTool_->pattern, level == ground,
                                                                surface == lot.grids.end() ? nullptr : &surface->second);
    if (change.quarters == 0) {
        ts2::diag::step(std::format("build mode: nothing to change on storey {} from {} {} to {} {}", level - ground + 1, rect.x0, rect.y0, rect.x1, rect.y1));
        if (!floorRemoving_ && level != ground) {
            return "Above the ground, only floor already there can be covered yet.";
        }
        return {};
    }
    // What the floor covered or taken up cost comes back.
    const auto& catalog = assets_.get()->floorCatalog();
    std::int32_t refund = 0;
    for (const auto& [name, quarters] : change.replaced) {
        if (const auto price = catalogPrice(catalog, name)) {
            refund += ts2::content::floorPrice(*price, quarters);
        }
    }
    const std::int32_t cost = floorRemoving_ ? 0 : ts2::content::floorPrice(buildTool_->price, change.quarters);
    if (cost - refund > lotLife_->funds()) {
        return "You can't afford that.";
    }
    lotLife_->addFunds(refund - cost);
    grid->second = std::move(floors);
    lot.floorPatterns = std::move(patterns);
    redrawFloors(level);
    lotLife_->setFloors(grid->second, ground);
    ts2::diag::step(std::format("build mode: {} {} quarter tiles on storey {} from {} {} to {} {}: paid {}, got back {}",
                                floorRemoving_ ? "took up" : "laid " + buildTool_->name + " on", change.quarters, level - ground + 1, rect.x0,
                                rect.y0, rect.x1, rect.y1, cost, refund));
    return {};
}

void Game::redrawFloors(int level) {
    if (!buildLot_ || !lotLife_) {
        return;
    }
    const ts2::scene::Heights heights(*buildLot_);
    const auto materialOf = [&](const std::string& pattern) { return lotLife_->floorMaterial(pattern, lot_); };
    auto drawn = ts2::scene::floorSurfaces(*buildLot_, heights, level, materialOf, lot_.materials);
    for (const bool stage : {false, true}) {
        const std::string name = stage ? "stage floors" : "floors";
        ts2::scene::Mesh mesh;
        mesh.name = name;
        for (auto& [material, p] : stage ? drawn.stage : drawn.floors) {
            if (!p.indices.empty()) {
                p.material = material;
                mesh.primitives.push_back(std::move(p));
            }
        }
        const auto found = std::find_if(lot_.instances.begin(), lot_.instances.end(), [&](const ts2::scene::Instance& i) {
            return i.object == 0 && i.part == ts2::scene::Part::Floor && i.level == level && i.name == name;
        });
        std::size_t index = static_cast<std::size_t>(found - lot_.instances.begin());
        if (mesh.primitives.empty()) {
            // No floor left there: the old floor isn't drawn.
            if (found != lot_.instances.end() && found->mesh >= 0) {
                const auto& old = lot_.meshes[static_cast<std::size_t>(found->mesh)];
                renderer_.setMaterials(index, std::vector<int>(old.primitives.size(), ts2::scene::kHiddenMaterial));
            }
            continue;
        }
        lot_.meshes.push_back(std::move(mesh));
        const int meshIndex = static_cast<int>(lot_.meshes.size() - 1);
        if (found == lot_.instances.end()) {
            // The storey had no floor: a new instance, lit by the ground's light map on the ground.
            ts2::scene::Instance added;
            added.mesh = meshIndex;
            added.name = name;
            added.transform = lotPlace_;
            added.part = ts2::scene::Part::Floor;
            added.level = level;
            added.onStage = stage;
            const auto terrain = std::find_if(lot_.instances.begin(), lot_.instances.end(),
                                              [](const ts2::scene::Instance& i) { return i.object == 0 && i.name == "terrain"; });
            if (terrain != lot_.instances.end() && level == lot_.groundLevel) {
                added.lightMap = terrain->lightMap;
            }
            lot_.instances.push_back(std::move(added));
            index = lot_.instances.size() - 1;
        } else {
            found->mesh = meshIndex;
        }
        renderer_.addMaterials(lot_);
        renderer_.addMeshes(lot_);
        renderer_.setInstance(lot_, index);
        lotPicker_.update(lot_, index);
    }
}

void Game::findDrawnWalls() {
    wallInstances_.clear();
    wallOfInstance_.clear();
    wallTopMaterial_ = -1;
    if (!drawnWalls_) {
        return;
    }
    // The lot's walls are the last the scene has: its own are the neighborhood's skirt's before it.
    const std::size_t count = drawnWalls_->walls.size();
    if (lot_.walls.size() < count) {
        drawnWalls_.reset();
        return;
    }
    const std::size_t first = lot_.walls.size() - count;
    wallInstances_.assign(count, {-1, -1});
    for (std::size_t i = 0; i < count; ++i) {
        drawnWalls_->walls[i].sceneWall = static_cast<int>(first + i);
    }
    for (std::size_t i = 0; i < lot_.instances.size(); ++i) {
        const auto& instance = lot_.instances[i];
        if (instance.part != ts2::scene::Part::Wall || instance.walls.size() != 1 || instance.walls.front() < static_cast<int>(first)) {
            continue;
        }
        const auto wall = static_cast<std::size_t>(instance.walls.front()) - first;
        if (wall < count) {
            wallInstances_[wall][instance.cut ? 1 : 0] = static_cast<int>(i);
            wallOfInstance_[i] = wall;
        }
    }
    for (std::size_t m = 0; m < lot_.materials.size(); ++m) {
        if (lot_.materials[m].name == drawnWalls_->topMaterial) {
            wallTopMaterial_ = static_cast<int>(m);
        }
    }
}

std::string Game::coverWalls(std::size_t wall, bool left, bool room) {
    if (!buildTool_ || !buildTool_->wall || !buildLot_ || !lotLife_ || !drawnWalls_ || wall >= drawnWalls_->walls.size()) {
        return {};
    }
    auto& lot = *buildLot_;
    auto& drawn = drawnWalls_->walls;
    if (!drawn[wall].coverable || !lot.roomGraph) {
        return "This wall can't be covered.";
    }
    // The drawn walls by their room graph wall, for those that take coverings.
    std::map<std::int32_t, std::size_t> drawnByGraph;
    for (std::size_t i = 0; i < drawn.size(); ++i) {
        if (drawn[i].coverable && drawn[i].graphWall >= 0) {
            drawnByGraph.emplace(drawn[i].graphWall, i);
        }
    }
    const ts2::content::WallSide clicked{drawn[wall].graphWall, left};
    const auto sides = room ? ts2::content::roomWallSides(*lot.roomGraph, lot.wallCoverings, clicked,
                                                          [&](std::int32_t id) { return drawnByGraph.contains(id); })
                            : std::vector<ts2::content::WallSide>{clicked};
    // Worked out on copies, kept only once paid for.
    auto coverings = lot.wallCoverings;
    auto patterns = lot.wallPatterns;
    const auto& catalog = assets_.get()->wallCatalog();
    std::vector<ts2::content::WallSide> covered;
    std::int32_t refund = 0;
    for (const auto& side : sides) {
        if (const auto before = ts2::content::coverWallSide(coverings, patterns, side, buildTool_->pattern)) {
            covered.push_back(side);
            if (const auto price = catalogPrice(catalog, *before)) {
                refund += *price;
            }
        }
    }
    if (covered.empty()) {
        return {};
    }
    const std::int32_t cost = buildTool_->price * static_cast<std::int32_t>(covered.size());
    if (cost - refund > lotLife_->funds()) {
        return "You can't afford that.";
    }
    lotLife_->addFunds(refund - cost);
    lot.wallCoverings = std::move(coverings);
    lot.wallPatterns = std::move(patterns);
    // Each covered side shows the new covering, its wall drawn again in full and cut down.
    const std::string material = assets_.get()->patternMaterial(buildTool_->pattern, true);
    std::set<std::size_t> redraw;
    for (const auto& side : covered) {
        const auto it = drawnByGraph.find(side.wall);
        if (it == drawnByGraph.end()) {
            continue;
        }
        auto& w = drawn[it->second].wall;
        (side.left ? w.leftCovering : w.rightCovering) = material;
        redraw.insert(it->second);
    }
    renderer_.addMaterials(lot_);
    for (const std::size_t i : redraw) {
        for (const bool cut : {false, true}) {
            const int instance = wallInstances_[i][cut ? 1 : 0];
            if (instance < 0) {
                continue;
            }
            const int mesh = lotLife_->wallMesh(drawn[i], wallTopMaterial_, cut, lot_);
            if (mesh < 0) {
                continue;
            }
            lot_.instances[static_cast<std::size_t>(instance)].mesh = mesh;
            renderer_.addMaterials(lot_);
            renderer_.addMeshes(lot_);
            renderer_.setInstance(lot_, static_cast<std::size_t>(instance));
            lotPicker_.update(lot_, static_cast<std::size_t>(instance));
        }
    }
    const auto& q = drawn[wall].wall.q;
    ts2::diag::step(std::format("build mode: covered {} wall side{} with {}{} from the {} side of the wall from {} {} to {} {} (grid level {}{}): paid {}, got back {}",
                                covered.size(), covered.size() == 1 ? "" : "s", buildTool_->name, room ? " round the room" : "", left ? "left" : "right",
                                q.x0, q.y0, q.x1, q.y1, drawn[wall].wall.level, (left ? drawn[wall].wall.leftMask : drawn[wall].wall.rightMask).empty() ? "" : ", cut by a door or window", cost, refund));
    return {};
}

} // namespace app
