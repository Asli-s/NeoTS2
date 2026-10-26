// Route planning: the route grid and its items, the path search over levels and stairs, the path
// passes, slot and placement goals, footprints, Snap, and walks and steps as the game plans them.
#include "test_support.hpp"

#include "sim/animation.hpp"
#include "sim/ground_click.hpp"
#include "sim/idle_spot.hpp"
#include "sim/route_portals.hpp"
#include "sim/walking.hpp"
#include "sim/path_passes.hpp"
#include "sim/route_world.hpp"
#include "sim/turn.hpp"
#include "sim/walk_path.hpp"
#include "sim/walk_plan.hpp"
#include <map>
#include <random>

namespace {

// The Sim clock: two seconds a tick, carried into minutes, hours, the date and the day of the week,
// shown with the Day Of Week global, as the game shows it.
// The route grid splits leaves along an item's edges down to an eighth of a tile, and a leaf's passage
// follows its items: floor walks, walls block, joints cost, a doorway's solid square overrides a wall.
// Floor route items: one tile per floored tile with roads costing more, bare ground on the ground
// level, a foundation under raised floors, and strips along floor edges.
// A fence post routes as its post's footprint around the post's point (route axes swap the wall
// graph's), padded by an eighth of a tile. Fences that don't stop Sims give nothing.
void postRouteItemsUseThePostFootprint() {
    using namespace ts2::sim;
    const std::vector<ts2::fencepostlayer::Post> posts{{1, 13, 9, 0xAD0DABD2}, {1, 5, 5, 0x0D0EACC7}, {2, 1, 1, 0xAD0DABD2}};
    const auto items = postRouteItems(posts, 1, [](std::uint32_t guid) -> std::optional<RouteRect> {
        if (guid != 0xAD0DABD2) return std::nullopt;
        return RouteRect{-0.08f, -0.07f, 0.08f, 0.07f};
    });
    CHECK(items.size() == 2);
    CHECK(items[0].rect.x0 == 9 - 0.08f && items[0].rect.y1 == 13 + 0.07f && items[0].type == 15);
    CHECK(items[1].rect.x0 == 9 - 0.08f - 0.125f && items[1].type == 14);
}

void objectFootprintsFollowTheModel() {
    using namespace ts2::sim;
    namespace sg = ts2::scenegraph;
    // A resource node with one data list: footprint / "0" / "(0,0)", a 16x16 mask with only the
    // first row's first two bits set (one cell at the tile's -x, +y corner once halved).
    std::vector<std::uint8_t> data;
    auto u8 = [&](std::uint8_t v) { data.push_back(v); };
    auto u32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i))); };
    auto str = [&](std::string_view s) { u8(static_cast<std::uint8_t>(s.size())); data.insert(data.end(), s.begin(), s.end()); };
    u32(0);  // no links
    u32(1);  // one block...
    u32(0x6A836D56);  // ...a data list
    str("cDataListExtension"); u32(0x6A836D56); u32(1);
    str("cExtension"); u32(0); u32(3);
    u8(7); str("footprint"); u32(1);
    u8(7); str("0"); u32(2);
    u8(2); str("minx"); u32(0);
    u8(9); str("(0,0)"); u32(32); u8(0x03); for (int i = 1; i < 32; ++i) u8(0);
    const auto lists = sg::parseDataLists(data);
    CHECK(lists.size() == 1 && lists[0].name == "footprint");

    FootprintShape shape;
    shape.masks = &lists[0];
    const auto local = footprintRects(shape);
    CHECK(local.size() == 1);
    CHECK(std::abs(local[0].x0 + 0.49f) < 1e-4f && std::abs(local[0].y1 - 0.49f) < 1e-4f && std::abs(local[0].x1 + 0.385f) < 1e-4f);
    // Turned a quarter (direction 2), the corner moves to the tile's -x, -y corner.
    const auto placed = placeFootprint(local, 3.5f, 4.5f, 2);
    CHECK(std::abs(placed[0].x0 - 3.01f) < 1e-4f && std::abs(placed[0].y0 - 4.01f) < 1e-4f && std::abs(placed[0].x1 - 3.115f) < 1e-4f);

    // A stair piece: a square tile, inset half a tile on one side and a quarter on the other.
    FootprintShape stair;
    stair.square = true;
    stair.insets = 0x0408;
    const auto step = placeFootprint(footprintRects(stair), 10.5f, 27.5f, 4);
    CHECK(std::abs(step[0].x0 - 10.01f) < 1e-4f && std::abs(step[0].y0 - 27.26f) < 1e-4f && std::abs(step[0].x1 - 10.99f) < 1e-4f &&
          std::abs(step[0].y1 - 27.49f) < 1e-4f);

    // Types and costs, with the game's own flags for these objects.
    CHECK(footprintRouting(0x90, 300, 0x20a0, 4, false).passable && footprintRouting(0x90, 300, 0x20a0, 4, false).cost == 4);  // daisies
    CHECK(footprintRouting(0x0, 0, 0x30a4, 4, false).type == 9);   // a table
    CHECK(footprintRouting(0x8000, 0, 0x20a4, 4, false).type == 11);  // a fridge
    CHECK(footprintRouting(0x400, 0, 0x24, 4, true).type == 5);   // a curtain
    CHECK(!footprintRouting(0x4, 0, 0x2024, 4, false).added);     // a wall lamp-like object
    CHECK(footprintRouting(0x4, 0, 0x2024, 4, true).cost == 1);   // a mirror

    // A multi-tile part one tile along +x of a lead turned a quarter is the model's tile (0, -1).
    int tx = 9, ty = 9;
    CHECK(footprintTileOf(5.5f, 5.5f, 2, 6.5f, 5.5f, tx, ty));
    CHECK(tx == 0 && ty == -1);

    // Bad data: far or broken positions, lists nested too deep, rectangles that aren't numbers.
    CHECK(!footprintTileOf(5.5f, 5.5f, 0, 1e9f, 5.5f, tx, ty) && tx == 0 && ty == 0);
    CHECK(!footprintTileOf(5.5f, 5.5f, 0, std::nanf(""), 5.5f, tx, ty));
    CHECK(footprintTile(lists[0], 0, 0, -2147483647 - 1).empty());
    CHECK(placeFootprint({{std::nanf(""), 0, 1, 1}}, 3.5f, 4.5f, 0).empty());
    std::vector<std::uint8_t> deep;
    auto deepU32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) deep.push_back(static_cast<std::uint8_t>(v >> (8 * i))); };
    auto deepStr = [&](std::string_view s) { deep.push_back(static_cast<std::uint8_t>(s.size())); deep.insert(deep.end(), s.begin(), s.end()); };
    deepU32(0); deepU32(1); deepU32(0x6A836D56);
    deepStr("cDataListExtension"); deepU32(0x6A836D56); deepU32(1);
    deepStr("cExtension"); deepU32(0); deepU32(3);
    for (int i = 0; i < 40; ++i) { deep.push_back(7); deepStr("x"); deepU32(1); }
    bool threw = false;
    try {
        sg::parseDataLists(deep);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    RouteGrid huge(std::numeric_limits<float>::infinity(), 1e30f);
    huge.add({{std::nanf(""), 0, 1, 1}});
    huge.add({{0, 0, 1e30f, 0.5f}});
    CHECK(huge.items().size() == 1 && huge.cells().size() < 100000);
}

void floorRouteItemsFollowTheFloors() {
    using namespace ts2::sim;
    // A 3 x 2 lot (route tiles) with two levels. The floor grid is indexed (level, y, x) in route terms.
    ts2::array3d::Array3D floors;
    floors.width = 2;   // route height
    floors.height = 3;  // route width
    floors.levels = 2;
    floors.elementSize = 8;
    floors.elements.assign(2 * 2 * 3 * 8, 0);
    auto setFloor = [&](int level, int x, int y, std::int16_t pattern) {
        const std::size_t at = ((static_cast<std::size_t>(level) * 2 + static_cast<std::size_t>(y)) * 3 + static_cast<std::size_t>(x)) * 8;
        for (std::size_t q = 0; q < 4; ++q) floors.elements[at + q * 2] = static_cast<std::uint8_t>(pattern);
    };
    setFloor(0, 0, 0, 1);  // a road tile on the ground
    setFloor(1, 2, 1, 2);  // one raised tile
    std::map<std::uint16_t, ts2::stringmap::Entry> patterns{{1, {"road_asphalt", 1}}, {2, {"6cc25ec0", 1}}};

    const auto ground = floorRouteItems(floors, patterns, 0, 0, 3, 2);
    auto has = [](const std::vector<RouteItem>& items, RouteRect r, int type, float cost) {
        return std::any_of(items.begin(), items.end(), [&](const RouteItem& i) {
            return i.rect.x0 == r.x0 && i.rect.y0 == r.y0 && i.rect.x1 == r.x1 && i.rect.y1 == r.y1 && i.type == type && i.cost == cost;
        });
    };
    CHECK(has(ground, {0, 0, 1, 1}, 0, 4));          // the road
    CHECK(has(ground, {1, 0, 2, 1}, 0, 1.3f));       // bare ground
    CHECK(!has(ground, {2, 1, 3, 2}, 0, 1.3f));      // no ground under the raised floor...
    CHECK(has(ground, {2, 1, 3, 2}, 7, kBlockedCost));  // ...but its foundation
    CHECK(has(ground, {0, 0, 0.125f, 1}, 2, 1));     // the lot's edge

    const auto upper = floorRouteItems(floors, patterns, 1, 0, 3, 2);
    CHECK(has(upper, {2, 1, 3, 2}, 0, 1));
    CHECK(has(upper, {2, 1, 2.125f, 2}, 2, 1));      // edges all round the lone tile
    CHECK(upper.size() == 5);

    // On a lot whose grid starts a level below the ground, grid level 1 is the ground storey.
    const auto pool = floorRouteItems(floors, patterns, 0, -1, 3, 2);
    CHECK(has(pool, {2, 1, 3, 2}, 0, 1));
    CHECK(!has(pool, {0, 0, 1, 1}, 0, 4));
}

void objectRouteItemsFollowTheGameRules() {
    using namespace ts2::sim;
    MemoryCodeSource code;
    ts2::objd::ObjectDefinition definition;
    definition.fields.assign(0x70, 0);
    definition.fields[ts2::objd::field::kType] = 4;
    code.addObjectType(kObjectGroup, definition);
    const ObjectType& type = *code.objectType(definition.guid());

    // A square footprint (data 8 bit 0x80): a blocking core and a padded copy.
    SimObject table;
    table.id = 7;
    table.x = 3 * 65536 + 32768;  // 3.5
    table.y = 4 * 65536 + 32768;  // 4.5
    table.data[8] = 0x80;
    auto items = objectRouteItems(table, table, type, code, nullptr);
    CHECK(items.size() == 2 && items[0].type == 11 && items[1].type == 10 && items[0].owner == 7);
    CHECK(std::abs(items[0].rect.x0 - 3.01f) < 1e-4f && std::abs(items[1].rect.x0 - 2.885f) < 1e-4f);

    // Held in a slot, off the lot, or a door: nothing.
    SimObject held = table;
    held.data[2] = 3;
    CHECK(objectRouteItems(held, held, type, code, nullptr).empty());
    SimObject offLot = table;
    offLot.x = kOffLot;
    CHECK(objectRouteItems(offLot, offLot, type, code, nullptr).empty());
    ts2::objd::ObjectDefinition door = definition;
    door.fields[ts2::objd::field::kType] = 8;
    door.fields[ts2::objd::field::kGuid1] = 1;
    code.addObjectType(kObjectGroup, door);
    // A door (no model here): only the doorway square through its wall, half a tile in front of it.
    const auto doorItems = objectRouteItems(table, table, *code.objectType(door.guid()), code, nullptr);
    CHECK(doorItems.size() == 1 && doorItems[0].kind == RouteItemKind::Solid && doorItems[0].cost == 1);
    CHECK(std::abs(doorItems[0].rect.y0 - 4.8f) < 1e-4f && std::abs(doorItems[0].rect.y1 - 5.2f) < 1e-4f);
}

// Leaves by place, each with its items' values, for comparing two grids whatever their cell order.
std::map<std::tuple<int, int, int>, std::vector<std::tuple<float, float, float, float, int, std::uint32_t>>> routeLeaves(
    const ts2::sim::RouteGrid& grid) {
    std::map<std::tuple<int, int, int>, std::vector<std::tuple<float, float, float, float, int, std::uint32_t>>> leaves;
    for (const auto& cell : grid.cells()) {
        if (!cell.isLeaf()) continue;
        auto& items = leaves[{cell.level, cell.x, cell.y}];
        for (const int i : cell.items) {
            const auto& item = grid.items()[static_cast<std::size_t>(i)];
            items.emplace_back(item.rect.x0, item.rect.y0, item.rect.x1, item.rect.y1, item.type, item.owner);
        }
        std::sort(items.begin(), items.end());
    }
    return leaves;
}

// Removing an object's items merges the cells it split, leaving the grid a fresh build of the rest
// would give. Items of another kind under the same id stay.
void routeGridRemovalMergesCells() {
    using namespace ts2::sim;
    const RouteItem floor{{0, 0, 8, 8}, 1, 0, RouteItemKind::Floor, true, 0};
    const RouteItem wall{{3.925f, 0, 4.075f, 4}, kBlockedCost, 15, RouteItemKind::Wall, false, 7};
    const RouteItem table{{1.01f, 5.01f, 2.99f, 6.99f}, kBlockedCost, 11, RouteItemKind::Object, false, 7};
    const RouteItem tablePadding{{0.885f, 4.885f, 3.115f, 7.115f}, kBlockedCost, 10, RouteItemKind::Object, false, 7};
    const RouteItem chair{{5.2f, 5.2f, 5.8f, 5.8f}, kBlockedCost, 11, RouteItemKind::Object, false, 9};

    RouteGrid grid(8, 8);
    for (const auto& item : {floor, wall, table, tablePadding, chair}) grid.add(item);
    const std::size_t cellsBefore = grid.cells().size();
    CHECK(grid.itemsOf(RouteItemKind::Object, 7).size() == 2);
    const std::uint32_t owners[] = {7};
    grid.remove(RouteItemKind::Object, owners);

    RouteGrid fresh(8, 8);
    for (const auto& item : {floor, wall, chair}) fresh.add(item);
    CHECK(grid.items().size() == 3 && grid.itemsOf(RouteItemKind::Object, 7).empty());
    CHECK(grid.itemsOf(RouteItemKind::Wall, 7).size() == 1);
    CHECK(grid.cells().size() == fresh.cells().size() && grid.cells().size() < cellsBefore);
    CHECK(routeLeaves(grid) == routeLeaves(fresh));
    const int leaf = grid.leafAt(1.5f, 5.5f);
    CHECK(leaf >= 0 && grid.passage(grid.cells()[static_cast<std::size_t>(leaf)]).walkable());

    // Removing what isn't there changes nothing.
    const std::uint32_t nobody[] = {5};
    grid.remove(RouteItemKind::Object, nobody);
    CHECK(routeLeaves(grid) == routeLeaves(fresh));
}

// The path search links leaves that share an edge (both ways), walks around a wall to the goal, and
// takes the best-scoring goal it can reach over a nearer one.
void routeSearchWalksAroundAWall() {
    using namespace ts2::sim;
    RouteGrid grid(8, 8);
    grid.add({{0, 0, 8, 8}, 1, 0, RouteItemKind::Floor, true, 0});
    grid.add({{3.925f, 0, 4.075f, 6}, kBlockedCost, 15, RouteItemKind::Wall, false, 7});
    const RouteLinks links = RouteLinks::build(grid);
    bool linksOk = true;
    for (std::size_t a = 0; a < links.of.size(); ++a) {
        for (const int b : links.of[a]) {
            const auto& other = links.of[static_cast<std::size_t>(b)];
            const RouteRect ra = grid.cells()[a].rect(), rb = grid.cells()[static_cast<std::size_t>(b)].rect();
            const bool sideBySide = (ra.x1 == rb.x0 || rb.x1 == ra.x0) && std::max(ra.y0, rb.y0) < std::min(ra.y1, rb.y1);
            const bool aboveBelow = (ra.y1 == rb.y0 || rb.y1 == ra.y0) && std::max(ra.x0, rb.x0) < std::min(ra.x1, rb.x1);
            linksOk &= std::find(other.begin(), other.end(), static_cast<int>(a)) != other.end() && (sideBySide || aboveBelow);
        }
    }
    CHECK(linksOk);

    RouteRandom jitter(1), heuristic(2);
    const RouteStart start[] = {{2, 2}};
    const RouteGoal across[] = {{6, 2, 1}};
    const auto plan = planRoute(grid, links, start, across, 6, 2, {}, jitter, heuristic);
    CHECK(plan && plan->goal == 0 && plan->waypoints.size() >= 3);
    if (plan) {
        const auto& w = plan->waypoints;
        CHECK(w.front().kind == WaypointKind::Start && w.front().x == 2 && w.front().y == 2);
        CHECK(w.back().kind == WaypointKind::Goal && w.back().x == 6 && w.back().y == 2);
        bool aroundTheEnd = false, onFloor = true;
        for (const auto& p : w) {
            aroundTheEnd |= p.y > 6;
            const int leaf = grid.leafAt(p.x, p.y);
            onFloor &= leaf >= 0 && grid.passage(grid.cells()[static_cast<std::size_t>(leaf)]).walkable();
        }
        CHECK(aroundTheEnd && onFloor && w.back().cost > 8);
    }

    const RouteGoal two[] = {{1, 1, 0}, {6, 2, 5}};
    const auto best = planRoute(grid, links, start, two, 3.5f, 1.5f, {}, jitter, heuristic);
    CHECK(best && best->goal == 1);
}

// A Sim standing inside an obstacle walks out of it. A goal off the floor, a start off the grid, a
// non-finite point and too many goals give no path.
void routeSearchEscapesAndRefuses() {
    using namespace ts2::sim;
    RouteGrid grid(8, 8);
    grid.add({{0, 0, 4, 8}, 1, 0, RouteItemKind::Floor, true, 0});
    grid.add({{1.01f, 5.01f, 1.99f, 5.99f}, kBlockedCost, 11, RouteItemKind::Object, false, 3});
    grid.add({{0.885f, 4.885f, 2.115f, 6.115f}, kBlockedCost, 10, RouteItemKind::Object, false, 3});
    const RouteLinks links = RouteLinks::build(grid);
    RouteRandom jitter(3), heuristic(4);
    const RouteStart inside[] = {{1.5f, 5.5f}};
    const RouteGoal open[] = {{3, 1, 1}};
    CHECK(planRoute(grid, links, inside, open, 3, 1, {}, jitter, heuristic).has_value());

    const RouteStart start[] = {{1, 1}};
    const RouteGoal offFloor[] = {{6, 2, 1}};
    CHECK(!planRoute(grid, links, start, offFloor, 6, 2, {}, jitter, heuristic));
    const RouteStart offGrid[] = {{-3, 1}};
    CHECK(!planRoute(grid, links, offGrid, open, 3, 1, {}, jitter, heuristic));
    const RouteStart nan[] = {{std::numeric_limits<float>::quiet_NaN(), 1}};
    CHECK(!planRoute(grid, links, nan, open, 3, 1, {}, jitter, heuristic));
    CHECK(!planRoute(grid, links, start, open, std::numeric_limits<float>::infinity(), 1, {}, jitter, heuristic));
    const std::vector<RouteGoal> many(kMaxRouteGoals + 1, RouteGoal{3, 1, 1});
    CHECK(!planRoute(grid, links, start, many, 3, 1, {}, jitter, heuristic));
}

// One search runs over every level: a stair (a portal) takes the path from one level's grid to the
// other's, marked by the stair's entry (LevelEnd) and exit (LevelStart). A one-way stair can't be
// gone back up, a refused one not at all, a two-way one both ways.
void routeSearchCrossesLevelsOverStairs() {
    using namespace ts2::sim;
    RouteGrid ground(8, 8), upstairs(8, 8);
    ground.add({{0, 0, 8, 8}, 1, 0, RouteItemKind::Floor, true, 0});
    upstairs.add({{0, 0, 8, 8}, 1, 0, RouteItemKind::Floor, true, 0});
    const RouteLinks groundLinks = RouteLinks::build(ground), upLinks = RouteLinks::build(upstairs);
    const RouteLevel levels[] = {{&ground, &groundLinks}, {&upstairs, &upLinks}};
    RoutePortal stair;
    stair.x[0] = 2.5f;
    stair.y[0] = 4.5f;
    stair.level[0] = 0;
    stair.x[1] = 5.5f;
    stair.y[1] = 4.5f;
    stair.level[1] = 1;
    stair.cost = 1.5f;
    RouteRandom jitter(3), heuristic(4);
    const RouteStart below[] = {{1.5f, 1.5f, 0}};
    RouteGoal up{6.5f, 6.5f, 1};
    up.level = 1;
    const RouteGoal above[] = {up};
    const RoutePortal one[] = {stair};
    const auto plan = planRoute(levels, one, below, above, 6.5f, 6.5f, {}, jitter, heuristic);
    CHECK(plan.has_value());
    if (plan) {
        const auto entry = std::find_if(plan->walk.begin(), plan->walk.end(), [](const RouteWaypoint& w) { return w.kind == WaypointKind::LevelEnd; });
        CHECK(entry != plan->walk.end() && entry + 1 != plan->walk.end());
        if (entry != plan->walk.end() && entry + 1 != plan->walk.end()) {
            CHECK(entry->x == 2.5f && entry->y == 4.5f && entry->level == 0 && entry->portal == 0);
            const RouteWaypoint& exit = *(entry + 1);
            CHECK(exit.kind == WaypointKind::LevelStart && exit.x == 5.5f && exit.y == 4.5f && exit.level == 1 && exit.portal == 0);
        }
        CHECK(plan->walk.back().kind == WaypointKind::Goal && plan->walk.back().level == 1);
    }
    // Back down a one-way stair: no way.
    const RouteStart aboveStart[] = {{6.5f, 6.5f, 1}};
    RouteGoal down{1.5f, 1.5f, 1};
    down.level = 0;
    const RouteGoal belowGoal[] = {down};
    CHECK(!planRoute(levels, one, aboveStart, belowGoal, 1.5f, 1.5f, {}, jitter, heuristic));
    RoutePortal both = stair;
    both.twoWay = true;
    const RoutePortal twoWay[] = {both};
    CHECK(planRoute(levels, twoWay, aboveStart, belowGoal, 1.5f, 1.5f, {}, jitter, heuristic).has_value());
    RoutePortal refused = stair;
    refused.refused = true;
    const RoutePortal closed[] = {refused};
    CHECK(!planRoute(levels, closed, below, above, 6.5f, 6.5f, {}, jitter, heuristic));
    // A portal end on a level without a grid can't be used.
    RoutePortal nowhere = stair;
    nowhere.level[1] = 5;
    const RoutePortal off[] = {nowhere};
    CHECK(!planRoute(levels, off, below, above, 6.5f, 6.5f, {}, jitter, heuristic));
}

// A stair slot pairs with the slot whose name has the opposite last suffix.
void stairSlotsPairByName() {
    using ts2::sim::partnerSlotName;
    CHECK(partnerSlotName("route_modularstair_0_top") == "route_modularstair_0_bottom");
    CHECK(partnerSlotName("route_modularstair_1_bottom") == "route_modularstair_1_top");
    CHECK(partnerSlotName("route_x_nw") == "route_x_se");
    CHECK(partnerSlotName("route_x_sw") == "route_x_ne");
    CHECK(partnerSlotName("route_x_n") == "route_x_s");
    CHECK(partnerSlotName("route_x_e") == "route_x_w");
    CHECK(partnerSlotName("route_x_middle").empty());
    CHECK(partnerSlotName("_top").empty());
}

// A route over a stair is walked a stretch at a time: up to the stair's entry, then from its exit,
// with the facing at the goal kept for the last stretch.
void stairRoutesAreWalkedInStretches() {
    using namespace ts2::sim;
    Walking walk;
    walk.route.points = {{0.0f, 0.0f}, {1.0f, 0.0f}, {2.0f, 0.0f}, {2.0f, 3.0f}, {4.0f, 3.0f}, {5.0f, 3.0f}, {5.0f, 6.0f}, {7.0f, 6.0f}};
    walk.route.facing = static_cast<std::int16_t>(4);
    walk.route.stairs = {StairCrossing{2, 10, 11, 0, 1, 2, 1, static_cast<std::int16_t>(6)}, StairCrossing{5, 20, 21, 0, 1, 1, 2}};
    cutAtStair(walk);
    CHECK(walk.route.points.size() == 3 && walk.route.points.back() == std::make_pair(2.0f, 0.0f));
    CHECK(walk.route.stairs.size() == 1 && walk.route.stairs.front().entryPart == 10 && walk.route.facing == 6);
    CHECK(walk.ahead && walk.ahead->points.front() == std::make_pair(2.0f, 3.0f));
    CHECK(walk.ahead && walk.ahead->stairs.size() == 1 && walk.ahead->stairs.front().at == 2 && walk.ahead->facing == 4);
    walk.route = std::move(*walk.ahead);
    walk.ahead.reset();
    cutAtStair(walk);
    CHECK(walk.route.points.size() == 3 && walk.ahead && walk.ahead->points.size() == 2 && walk.ahead->stairs.empty());
    walk.route = std::move(*walk.ahead);
    cutAtStair(walk);
    CHECK(!walk.ahead && walk.route.facing == 4 && walk.route.points.size() == 2);
}

// The path passes' line tests: a line within the tolerance of a blocked cell is blocked, a line's
// cost adds up each cell's cost by the length inside it, and a point beside a blocked cell is pushed
// out through its nearest side.
void pathPassLinesSeeTheGrid() {
    using namespace ts2::sim;
    RouteGrid grid(8, 8);
    grid.add({{0, 0, 8, 8}, 1, 0, RouteItemKind::Floor, true, 0});
    grid.add({{4, 2, 6, 3}, 2, 0, RouteItemKind::Soft, false, 5});
    grid.add({{2, 4, 3, 5}, kBlockedCost, 11, RouteItemKind::Object, false, 3});
    const RouteLines lines(grid, [&](int cell) {
        const auto p = grid.passage(grid.cells()[static_cast<std::size_t>(cell)]);
        return LeafPassage{p.walkable(), p.cost};
    });
    CHECK(!lines.blocked(0.5f, 4.5f, 1.9f, 4.5f, 0.04f));
    CHECK(lines.blocked(0.5f, 4.5f, 1.97f, 4.5f, 0.04f));
    CHECK(std::fabs(lines.cost(3, 2.5f, 7, 2.5f) - 6) < 0.05);
    CHECK(lines.cost(0.5f, 4.5f, 3.5f, 4.5f) == kBlockedCost);
    CHECK(lines.evenCost(0.5f, 1, 3.5f, 1, 0.04f) && !lines.evenCost(3, 2.5f, 7, 2.5f, 0.04f));
    const auto [x, y] = lines.pushClear(1.98f, 4.5f, 0.0625f);
    CHECK(std::fabs(x - 1.9375f) < 1e-4f && y == 4.5f);
    const auto [fx, fy] = lines.pushClear(1, 1, 0.0625f);
    CHECK(fx == 1 && fy == 1);

    // A snapping heading turns a goal's way to the nearest eighth of a turn, and a start's in eighths
    // from the Sim's facing until within 40 degrees.
    constexpr float kDeg = 0.017453292f;
    const RouteHeading off{}, snap{true, true, 0};
    CHECK(off.leaving(1.0f, HeadingEnd::Goal) == 1.0f);
    CHECK(std::fabs(snap.leaving(51.6f * kDeg, HeadingEnd::Goal) - 45 * kDeg) < 1e-5f);
    CHECK(std::fabs(snap.leaving(100 * kDeg, HeadingEnd::Start) - 90 * kDeg) < 1e-5f);
    CHECK(std::fabs(snap.leaving(-30 * kDeg, HeadingEnd::Start)) < 1e-5f);
    CHECK(snap.leaving(std::numeric_limits<float>::infinity(), HeadingEnd::Start) == std::numeric_limits<float>::infinity());
}

// The path passes: long legs are halved down to half a tile, points the straight line doesn't need
// are dropped while a corner round a blocked cell stays, points are laid along a leg that brushes a
// blocked cell and pushed clear of it, and a corner moves toward its neighbours' middle. Starts and
// goals stay put.
void pathPassesSmoothAPath() {
    using namespace ts2::sim;
    RouteGrid grid(8, 8);
    grid.add({{0, 0, 8, 8}, 1, 0, RouteItemKind::Floor, true, 0});
    grid.add({{2, 4, 3, 5}, kBlockedCost, 11, RouteItemKind::Object, false, 3});
    const RouteLines lines(grid, [&](int cell) {
        const auto p = grid.passage(grid.cells()[static_cast<std::size_t>(cell)]);
        return LeafPassage{p.walkable(), p.cost};
    });
    const auto pt = [](float x, float y, float cost, WaypointKind kind = WaypointKind::Plain) { return RouteWaypoint{x, y, cost, kind}; };

    std::vector<RouteWaypoint> legs = {pt(1, 1, 0, WaypointKind::Start), pt(3.2f, 1, 2.2f), pt(3.2f, 3, 4.2f, WaypointKind::Goal)};
    splitLongLegs(legs);
    bool short_ = true;
    for (std::size_t i = 1; i < legs.size(); ++i) short_ &= std::hypot(legs[i].x - legs[i - 1].x, legs[i].y - legs[i - 1].y) <= 0.5f;
    CHECK(legs.size() == 13 && short_ && legs.front().kind == WaypointKind::Start && legs.back().kind == WaypointKind::Goal);

    std::vector<RouteWaypoint> straight = {pt(1, 1, 0, WaypointKind::Start), pt(1.5f, 1, 0.5f), pt(2, 1, 1), pt(3, 1, 2, WaypointKind::Goal)};
    keepTurns(straight, lines);
    CHECK(straight.size() == 2);
    std::vector<RouteWaypoint> around = {pt(1.5f, 3.8f, 0, WaypointKind::Start), pt(3.5f, 3.8f, 2), pt(3.5f, 5.5f, 3.7f, WaypointKind::Goal)};
    keepTurns(around, lines);
    CHECK(around.size() == 3 && around[1].x == 3.5f);

    std::vector<RouteWaypoint> brushing = {pt(1.5f, 3.97f, 0, WaypointKind::Start), pt(3.5f, 3.97f, 2, WaypointKind::Goal)};
    layClearOfObstacles(brushing, lines);
    bool clear = true;
    for (std::size_t i = 1; i + 1 < brushing.size(); ++i) {
        if (brushing[i].x >= 2 && brushing[i].x <= 3) clear &= std::fabs(brushing[i].y - 3.9375f) < 1e-4f;
    }
    CHECK(brushing.size() == 16 && clear && brushing.front().y == 3.97f && brushing.back().y == 3.97f);

    std::vector<RouteWaypoint> corner = {pt(1, 1, 0, WaypointKind::Start), pt(2, 1, 1), pt(2, 2, 2, WaypointKind::Goal)};
    roundCorners(corner, lines, 0.45f, true, false);
    CHECK(std::fabs(corner[1].x - 1.775f) < 1e-4f && std::fabs(corner[1].y - 1.225f) < 1e-4f && corner[0].x == 1 && corner[2].y == 2);

    // A goal facing holds the last points to it, fading 0.35 per tile. With no facings and an inactive
    // heading nothing moves.
    std::vector<RouteWaypoint> line;
    for (int i = 0; i <= 8; ++i) line.push_back(pt(1 + i * 0.25f, 1, i * 0.25f, i == 0 ? WaypointKind::Start : i == 8 ? WaypointKind::Goal : WaypointKind::Plain));
    auto plain = line;
    holdToEnds(plain, lines, {});
    bool still = true;
    for (std::size_t i = 0; i < plain.size(); ++i) still &= plain[i].y == 1 && plain[i].startHold == 0 && plain[i].endHold == 0;
    CHECK(still);
    line.back().facing = 1.5707964f;
    holdToEnds(line, lines, {});
    const float hold = std::pow(0.35f, 0.25f);
    CHECK(std::fabs(line[7].endHold - hold) < 1e-4f && std::fabs(line[7].y - (0.75f * hold + (1 - hold))) < 1e-4f);
    CHECK(line[6].y < line[7].y && line.back().y == 1 && line.front().startHold == 0);
    // The last rounds move a fully held point not at all.
    std::vector<RouteWaypoint> held = {pt(1, 1, 0, WaypointKind::Start), pt(2, 1, 1), pt(2, 2, 2, WaypointKind::Goal)};
    held[1].startHold = 1;
    roundCorners(held, lines, 0.325f, false, true);
    CHECK(held[1].x == 2 && held[1].y == 1);
    // An active heading holds a start's first leg at 0.2.
    auto active = plain;
    holdToEnds(active, lines, {true, false, 0});
    CHECK(std::fabs(active[0].startHold - 0.2f) < 1e-6f && active[1].startHold < 0.2f && active[1].startHold > 0.1f);

    std::vector<RouteWaypoint> broken ={pt(1, 1, 0, WaypointKind::Start), pt(std::numeric_limits<float>::quiet_NaN(), 1, 1), pt(3, 1, 2, WaypointKind::Goal)};
    smoothPath(broken, lines);
    CHECK(broken.size() == 3);
    std::vector<RouteWaypoint> none;
    smoothPath(none, lines);
    CHECK(none.empty());
}

// A door through a wall: crossing its line costs its extra and leaves a door entry point just past
// the line and a door exit halfway to the next. A door the Sim may not use is a wall. A swing area
// counts only on an active side (the door's own setting, or the side a step through it enters).
void routeSearchGoesThroughDoors() {
    using namespace ts2::sim;
    const RouteDoor door{3.5f, 4, 4.5f, 4};  // the line is x = 4
    RouteGrid grid(8, 8);
    grid.add({{0, 0, 8, 8}, 1, 0, RouteItemKind::Floor, true, 0});
    grid.add({{3.925f, 0, 4.075f, 8}, kBlockedCost, 15, RouteItemKind::Wall, false, 7});
    grid.add({{3.8f, 3.8f, 4.2f, 4.2f}, 1, 0, RouteItemKind::Solid, false, 50, door});
    grid.add({{3.0f, 3.0f, 3.925f, 5.0f}, 17, 1, RouteItemKind::Area, false, 50, door});
    const RouteLinks links = RouteLinks::build(grid);
    const RouteStart start[] = {{2, 4}};
    const RouteGoal goal[] = {{6, 4, 1}};
    const auto plan = [&](RouteDoorState state) {
        RouteRandom jitter(5), heuristic(6);
        state.owner = 50;
        const RouteDoorState doors[] = {state};
        return planRoute(grid, links, start, goal, 6, 4, doors, jitter, heuristic);
    };

    RouteDoorState open;
    open.extra = 3.8f;
    const auto through = plan(open);
    CHECK(through && through->waypoints.back().cost > 4 + 3.8f && through->waypoints.back().cost < 4 + 3.8f + 0.5f);
    if (through) {
        const auto& w = through->waypoints;
        const auto entry = std::find_if(w.begin(), w.end(), [](const RouteWaypoint& p) { return p.kind == WaypointKind::DoorEntry; });
        CHECK(entry != w.end() && entry + 1 != w.end() && (entry + 1)->kind == WaypointKind::DoorExit);
        CHECK(entry != w.end() && entry->x > 4 && entry->x < 4.1f);
    }

    RouteDoorState locked = open;
    locked.refused[0] = true;
    CHECK(!plan(locked));

    RouteDoorState swinging = open;
    swinging.active[0] = true;
    const auto costly = plan(swinging);
    CHECK(through && costly && costly->waypoints.back().cost > through->waypoints.back().cost + 5);
}

// A bone slot gives a goal per joint the model has, where Snap would put the Sim: 125 in the open,
// + 20 at the Sim's own spot. A wall across the way drops a goal, and one on the owner's tile toward the goal takes the 25 away.
void boneSlotGoalsFollowTheJoints() {
    using namespace ts2::sim;
    const std::vector<std::string> names{"route_0_n", "route_0_nw"};
    CHECK(slotJointNames("Route_0_N,route_0_NW") == names);
    CHECK(slotJointNames("Route_0_N, route_0_NW") == names);
    ts2::slot::Entry slot;
    slot.type = ts2::slot::kRoutingSlot;
    slot.objectFlags = 0x20000;
    slot.bone = 0;
    SimObject owner;
    owner.x = owner.y = 360448;  // 5.5
    owner.level = 2;
    owner.data[1] = 2;
    const std::optional<ModelBone> joints[] = {ModelBone{0, -1, 0}, std::nullopt, ModelBone{-0.9f, 1.4f, 0}};
    SimObject sim;
    sim.x = 294912;  // 4.5, on the first joint's spot
    sim.y = 360448;
    WallTiles walls;
    SlotGoalWorld world;
    world.walls = &walls;
    RouteRandom random(1);

    const auto open = boneSlotGoals(owner, slot, joints, sim, world, random);
    CHECK(open.size() == 2);
    if (open.size() == 2) {
        CHECK(open[0].x == 4.5f && open[0].y == 5.5f && open[0].score == 145);
        CHECK(std::fabs(open[1].x - 6.9f) < 1e-4f && std::fabs(open[1].y - 4.6f) < 1e-4f && open[1].score == 125);
    }

    walls.add(1, 5, 5, 6, 5);  // the owner tile's low y side, toward the second goal
    walls.add(1, 5, 5, 5, 6);  // its low x side, across the way to the first
    const auto walled = boneSlotGoals(owner, slot, joints, sim, world, random);
    CHECK(walled.size() == 1 && walled[0].score == 100);
}

// A clicked spot closer than 0.45 tiles to a wall of its tile moves to just past 0.45 from it: along
// an edge straight away from it, across the tile along the wall's direction. A spot clear of walls
// and one off the lot stay where they are.
void groundClicksStayClearOfWalls() {
    using namespace ts2::sim;
    WallTiles walls;
    walls.add(0, 5, 5, 6, 5);  // tile (5, 5)'s low y side, on the ground
    walls.add(0, 8, 8, 9, 9);  // across tile (8, 8), rising
    const GroundPoint edge = clearOfWalls(walls, {5.3f, 5.2f, 1});
    CHECK(edge.x == 5.3f && std::fabs(edge.y - 5.451f) < 1e-5f && edge.level == 1);
    const GroundPoint across = clearOfWalls(walls, {8.5f, 8.6f, 1});
    const float shift = (0.45f - 0.1f / 1.41421356f + 0.001f) / 1.41421356f;
    CHECK(std::fabs(across.x - (8.5f + shift)) < 1e-5f && std::fabs(across.y - (8.6f + shift)) < 1e-5f);
    const GroundPoint upstairs = clearOfWalls(walls, {5.3f, 5.2f, 2});
    CHECK(upstairs.x == 5.3f && upstairs.y == 5.2f);
    const GroundPoint clear = clearOfWalls(walls, {5.3f, 5.6f, 1});
    CHECK(clear.x == 5.3f && clear.y == 5.6f);
    const GroundPoint off = clearOfWalls(walls, {-1.0f, 5.2f, 1});
    CHECK(off.x == -1.0f && off.y == 5.2f);
}

// Is It Ok to Idle Here: another Sim standing within 0.6 tiles makes a spot not ok, one partway
// through a route doesn't, and a walk its tree has left behind counts as standing.
void idleSpotIgnoresSimsOnTheirWay() {
    using namespace ts2::sim;
    World world;
    SimObject& idle = world.create(kObjectGroup, typed(0x51515151, kPersonType));
    SimObject& other = world.create(kObjectGroup, typed(0x51515151, kPersonType));
    idle.x = idle.y = other.y = 360448;  // 5.5
    other.x = 380109;                    // 5.8
    CHECK(!okToIdleHere(world, idle));

    Frame routeFrame;
    routeFrame.group = kObjectGroup;
    routeFrame.instance = 0x1000;
    routeFrame.pc = 3;
    other.thread.frames.push_back(routeFrame);
    Walking walk;
    walk.group = kObjectGroup;
    walk.tree = 0x1000;
    walk.pc = 3;
    other.walking = walk;
    CHECK(okToIdleHere(world, idle));

    other.thread.frames.clear();
    CHECK(!okToIdleHere(world, idle));
    other.x = 406323;  // 6.2, too far to count
    CHECK(okToIdleHere(world, idle));
}

// A slot that isn't a bone slot is swept around its owner: one goal per allowed direction at the
// optimal distance, 100 + the standing weight 10 + 25. A wall across the way drops one, another Sim
// on a spot costs it 30. A spot
// blocked by another object can't be stood on, one blocked only by the owner can.
void sweptSlotGoalsCircleTheOwner() {
    using namespace ts2::sim;
    ts2::slot::Entry slot;
    slot.type = ts2::slot::kRoutingSlot;
    slot.routingFlags = 0x55;  // directions 0, 2, 4 and 6
    slot.routingInts = {1, 0, 0};
    SimObject owner;
    owner.x = owner.y = 360448;  // 5.5
    owner.level = 2;
    SimObject sim;
    sim.x = sim.y = -1;
    WallTiles walls;
    SlotGoalWorld world;
    world.walls = &walls;
    RouteRandom random(1);
    const auto ring = sweptSlotGoals(owner, slot, sim, world, random);
    CHECK(ring.size() == 4);
    if (ring.size() == 4) {
        CHECK(ring[0].x == 5.5f && ring[0].y == 4.5f && ring[1].x == 6.5f && ring[1].y == 5.5f);
        CHECK(ring[2].x == 5.5f && ring[2].y == 6.5f && ring[3].x == 4.5f && ring[3].y == 5.5f);
        CHECK(ring[0].score == 135 && ring[3].score == 135);
    }
    // The Sim standing on one of the spots: that spot is offered first as well, both 20 higher. Not
    // while it sits in something.
    SimObject standing = sim;
    standing.x = 425984;  // 6.5
    standing.y = 360448;  // 5.5
    const auto own = sweptSlotGoals(owner, slot, standing, world, random);
    CHECK(own.size() == 5 && own[0].x == 6.5f && own[0].y == 5.5f && own[0].score == 155 && own[2].x == 6.5f && own[2].score == 155);
    standing.data[2] = 77;
    CHECK(sweptSlotGoals(owner, slot, standing, world, random).size() == 4);
    walls.add(1, 5, 5, 6, 5);  // across the way to direction 0
    CHECK(sweptSlotGoals(owner, slot, sim, world, random).size() == 3);
    // Another Sim standing on a spot keeps it, 30 lower. A bone slot's owner doesn't count.
    world.simNear = [](int, float x, float y, std::int16_t ownerSim) { return ownerSim < 0 && x == 6.5f && y == 5.5f; };
    const auto taken = sweptSlotGoals(owner, slot, sim, world, random);
    CHECK(taken.size() == 3 && taken[0].x == 6.5f && taken[0].score == 105 && taken[1].score == 135);

    RouteGrid grid(8, 8);
    grid.add({{0, 0, 8, 4}, 1, 0, RouteItemKind::Floor, true, 0});
    grid.add({{1, 1, 2, 2}, kBlockedCost, 9, RouteItemKind::Object, false, 7});
    CHECK(canStandAt(grid, 3.5f, 1.5f, 0) && !canStandAt(grid, 1.5f, 1.5f, 0) && canStandAt(grid, 1.5f, 1.5f, 7));
    CHECK(!canStandAt(grid, 3.5f, 6.5f, 0) && floorCostAt(grid, 3.5f, 1.5f) == 1);

    // A dynamic footprint blocks unless its object lets the Sim through, then costs what it says.
    RouteItem marker{{5, 1, 6, 2}, kBlockedCost, 5, RouteItemKind::Object, false, 9};
    marker.dynamic = true;
    grid.add(marker);
    const RouteIntersects lets = [](std::uint32_t owner) { return owner == 9 ? std::optional<float>(1.5f) : std::nullopt; };
    const RouteIntersects refuses = [](std::uint32_t) { return std::optional<float>(); };
    CHECK(!canStandAt(grid, 5.5f, 1.5f, 0) && !canStandAt(grid, 5.5f, 1.5f, 0, refuses) && canStandAt(grid, 5.5f, 1.5f, 0, lets));
    const auto& leaf = grid.cells()[static_cast<std::size_t>(grid.leafAt(5.5f, 1.5f))];
    CHECK(!grid.passage(leaf).walkable() && grid.passage(leaf, &lets).walkable() && grid.passage(leaf, &lets).cost == 1.5f);

    // Goal fits, against a chair's footprint ending at y 3.74 (its padding reaching 3.865): the Sim's
    // snapped square at y 3.80 reaches down to 3.76 only, so it fits, though the padding covers the
    // spot. A wide sweep's spot must keep 0.3 clear of the footprint itself, round its corners too.
    RouteGrid chairs(8, 8);
    chairs.add({{0, 0, 8, 8}, 1, 0, RouteItemKind::Floor, true, 0});
    RouteItem chair{{3.51f, 3.26f, 3.99f, 3.74f}, kBlockedCost, 9, RouteItemKind::Object, false, 5};
    chairs.add(chair);
    chair.rect = {3.385f, 3.135f, 4.115f, 3.865f};
    chair.type = 8;
    chair.padding = true;
    chairs.add(chair);
    CHECK(!canStandAt(chairs, 3.92f, 3.80f, 0) && canStandAt(chairs, 3.92f, 3.80f, 0, {}, GoalFit::Square));
    CHECK(!canStandAt(chairs, 3.92f, 3.70f, 0, {}, GoalFit::Square));
    CHECK(!canStandAt(chairs, 3.92f, 4.00f, 0, {}, GoalFit::Clearance) && canStandAt(chairs, 3.92f, 4.10f, 0, {}, GoalFit::Clearance));
    CHECK(!canStandAt(chairs, 4.15f, 3.95f, 0, {}, GoalFit::Clearance) && canStandAt(chairs, 4.25f, 4.05f, 0, {}, GoalFit::Clearance));
}

// A slot that prefers seats (its second number 1000) offers the seats on the swept spots' tiles that
// face back toward the owner: a goal at each joint of the seat's bone slot, listed twice, scored as the
// game scores them (10000 + 125 less the distance terms from the slot's spot), before the standing
// spots. A seat someone else sits in is left out and reported taken.
void sweptSlotGoalsOfferSeats() {
    using namespace ts2::sim;
    ts2::slot::Entry slot;
    slot.type = ts2::slot::kRoutingSlot;
    slot.routingFlags = 0x1;  // direction 0, turned with the owner
    slot.routingInts = {1, 1000, 0};
    slot.minDistance = 1;
    slot.maxDistance = 8;
    slot.optimalDistance = 3;
    slot.step = 1;
    slot.routingFloat = 0.35f;
    SimObject owner;
    owner.x = owner.y = 360448;  // 5.5
    owner.level = 2;
    owner.data[1] = 6;
    SimObject sim;
    sim.id = 5;
    sim.x = sim.y = -1;
    SimObject seat;
    seat.id = 9;
    seat.x = 163840 + 32768;  // 2.5
    seat.y = 360448;
    seat.level = 2;
    seat.data[1] = 2;  // facing back toward the owner
    seat.slots = {0};
    SeatSlot place;
    place.entry.type = ts2::slot::kRoutingSlot;
    place.entry.objectFlags = 0x20000;
    place.entry.routingFlags = 0x1000;
    place.joints = {ModelBone{0.09f, 0.717f, -3.14159f}, ModelBone{0.468f, 0.68f, 2.356f}};
    WallTiles walls;
    SlotGoalWorld world;
    world.walls = &walls;
    world.seatsOnTile = [&](int, int x, int y) {
        return x == 2 && y == 5 ? std::vector<Seat>{Seat{&seat, true, true, {place}}} : std::vector<Seat>{};
    };
    RouteRandom random(1);
    bool taken = false;
    const auto goals = sweptSlotGoals(owner, slot, sim, world, random, &taken);
    CHECK(goals.size() > 4 && !taken);
    if (goals.size() > 4) {
        for (std::size_t i = 0; i < 4; ++i) {
            const auto spot = routingSlotTarget(seat, place.entry, place.joints[i % 2]);
            const float gx = spot->x / 65536.0f, gy = spot->y / 65536.0f;
            const float expected = 10000 + 125 - std::fabs(std::hypot(gx - 5.5f, gy - 5.5f) - 3) * 0.35f * 16 - std::fabs(gy - 5.5f) * 0.35f * 0.01f;
            CHECK(goals[i].object == 9 && goals[i].token == static_cast<std::int16_t>(i % 2) && goals[i].facing == spot->facing);
            CHECK(std::fabs(goals[i].x - gx) < 1e-4f && std::fabs(goals[i].y - gy) < 1e-4f && std::fabs(goals[i].score - expected) < 0.01f);
        }
        CHECK(goals[4].object == 0 && goals[4].score < 200);
    }
    // With only the second number set there are no standing spots, so every goal is the seat's.
    slot.routingInts = {0, 1, 0};
    const auto seatsOnly = sweptSlotGoals(owner, slot, sim, world, random, &taken);
    CHECK(seatsOnly.size() == 4 && std::all_of(seatsOnly.begin(), seatsOnly.end(), [](const RouteGoal& g) { return g.object == 9; }));
    slot.routingInts = {1, 1000, 0};
    seat.slots = {77};
    const auto none = sweptSlotGoals(owner, slot, sim, world, random, &taken);
    CHECK(taken && std::none_of(none.begin(), none.end(), [](const RouteGoal& g) { return g.object != 0; }));
}

// Placing an object: a slot with no facing gives one goal at the owner's spot, scored 100. The
// placement goals skip the wall tests, so a wall across the way still leaves the swept goal. The
// goal's facing follows the slot's: -1 away from the owner (its own facing on its spot), -2 toward it.
void placementGoalsSkipWalls() {
    using namespace ts2::sim;
    ts2::slot::Entry point;
    point.type = ts2::slot::kRoutingSlot;
    point.routingFlags = 0x41000;
    point.facing = -1;
    SimObject owner;
    owner.x = owner.y = 360448;  // 5.5
    owner.level = 2;
    owner.data[1] = 3;
    WallTiles walls;
    walls.add(1, 5, 5, 6, 5);
    SlotGoalWorld world;
    world.walls = &walls;
    world.placing = true;
    CHECK(isPointSlot(point));
    const auto one = pointSlotGoal(owner, point, world);
    CHECK(one.size() == 1 && one[0].x == 5.5f && one[0].y == 5.5f && one[0].score == 100);
    CHECK(slotGoalFacing(owner, point, 5.5f, 5.5f) == 3 && slotGoalFacing(owner, point, 7.5f, 5.5f) == 2);
    point.facing = -2;
    CHECK(slotGoalFacing(owner, point, 7.5f, 5.5f) == 6);
    point.facing = -3;
    CHECK(!slotGoalFacing(owner, point, 7.5f, 5.5f));

    ts2::slot::Entry swept;
    swept.type = ts2::slot::kRoutingSlot;
    swept.routingFlags = 0x01;
    swept.routingInts = {1, 0, 0};
    owner.data[1] = 0;
    SimObject placed;
    RouteRandom random(1);
    CHECK(!isPointSlot(swept) && sweptSlotGoals(owner, swept, placed, world, random).size() == 1);
    world.placing = false;
    CHECK(sweptSlotGoals(owner, swept, placed, world, random).empty());
    world.canStand = [](int, float, float, std::int16_t, ts2::sim::GoalFit) { return false; };
    world.placing = true;
    CHECK(pointSlotGoal(owner, point, world).empty());
}

// A point goal left out says why, for the route's failure reason: a wall within 0.3 tiles gives 9
// (walls are tested first), something in the way where there is floor 7, no floor neither (6).
void pointGoalMissesGiveTheReason() {
    using namespace ts2::sim;
    ts2::slot::Entry point;
    point.type = ts2::slot::kRoutingSlot;
    point.routingFlags = 0x41000;
    SimObject owner;
    owner.x = 360448;                                 // 5.5
    owner.y = static_cast<std::int32_t>(5.2 * 65536);  // 0.2 from the wall
    owner.level = 2;
    WallTiles walls;
    walls.add(1, 5, 5, 6, 5);
    SlotGoalWorld world;
    world.walls = &walls;
    world.canStand = [](int, float, float, std::int16_t, GoalFit) { return false; };
    GoalMisses misses;
    CHECK(pointSlotGoal(owner, point, world, &misses).empty() && misses.wall && !misses.object && noGoalReason(misses) == 9);

    owner.y = 360448;  // 5.5, clear of the wall
    misses = {};
    CHECK(pointSlotGoal(owner, point, world, &misses).empty() && misses.object && !misses.wall && noGoalReason(misses) == 7);
    world.hasFloor = [](int, float, float) { return false; };
    misses = {};
    CHECK(pointSlotGoal(owner, point, world, &misses).empty() && !misses.object && !misses.wall && noGoalReason(misses) == 6);
}

// Update Footprint (generic call 0x21) marks the stack object's footprint as changed. New objects
// are marked too. Without a stack object it is an error.
void updateFootprintMarksTheStackObject() {
    MemoryCodeSource code;
    Instruction update = primitiveCall(0x0001, T, F);
    update.operands[0] = 0x21;
    code.add(kObjectGroup, 0x1000, bhav({update}));
    ts2::sim::World world;
    auto& me = world.create(kObjectGroup, definition("Me", 0));
    auto& table = world.create(kObjectGroup, definition("Table", 0));
    CHECK((world.takeFootprintChanges() == std::set<std::int16_t>{me.id, table.id}));
    me.thread.stackObjectId = table.id;
    Vm vm(code, {}, &world);
    CHECK(vm.run(me, 0x1000).outcome == Outcome::True);
    CHECK((world.takeFootprintChanges() == std::set<std::int16_t>{table.id}));
    world.remove(table);
    CHECK((world.takeFootprintChanges() == std::set<std::int16_t>{table.id}));
    me.thread.stackObjectId = 99;
    const RunResult r = vm.run(me, 0x1000);
    CHECK(r.outcome == Outcome::Error && r.reason == "stack object does not exist");
    CHECK(world.takeFootprintChanges().empty());
}

// Update Footprint on any part of a multi-tile object marks every part of it (they route by the
// lead), and no other object.
void updateFootprintMarksEveryMultiTilePart() {
    MemoryCodeSource code;
    Instruction update = primitiveCall(0x0001, T, F);
    update.operands[0] = 0x21;
    code.add(kObjectGroup, 0x1000, bhav({update}));
    ts2::sim::World world;
    auto& me = world.create(kObjectGroup, definition("Me", 0));
    auto multiTile = [&](std::int16_t lead) -> ts2::sim::SimObject& {
        auto& part = world.create(kObjectGroup, definition("Part", 0));
        part.definition.fields[ts2::objd::field::kMultiTileMasterId] = 0x55;
        part.data[0x13] = lead != 0 ? lead : part.id;
        return part;
    };
    auto& lead = multiTile(0);
    auto& left = multiTile(lead.id);
    auto& right = multiTile(lead.id);
    auto& otherLead = multiTile(0);
    auto& single = world.create(kObjectGroup, definition("Single", 0));
    single.data[0x13] = lead.id;
    world.takeFootprintChanges();

    me.thread.stackObjectId = right.id;
    Vm vm(code, {}, &world);
    CHECK(vm.run(me, 0x1000).outcome == Outcome::True);
    CHECK((world.takeFootprintChanges() == std::set<std::int16_t>{lead.id, left.id, right.id}));
    me.thread.stackObjectId = otherLead.id;
    CHECK(vm.run(me, 0x1000).outcome == Outcome::True);
    CHECK((world.takeFootprintChanges() == std::set<std::int16_t>{otherLead.id}));
}

// Snap (0x2E) mode 3 puts me at a routing slot of the stack object: the slot's (x, -y) turned with
// the object's facing, facing the slot's way with flag bit 12. A wall in the way or a spot off the
// lot refuses it. A test only moves nothing. Slots are counted over routing slots from 0.
void boneSlotWithABrokenTurnHasNoSpot() {
    ts2::sim::SimObject chair;
    ts2::slot::Entry slot;
    slot.objectFlags = 0x20000;
    slot.routingFlags = 0x1000;
    CHECK(ts2::sim::routingSlotTarget(chair, slot, ts2::sim::ModelBone{0.5f, 0, 0}).has_value());
    CHECK(!ts2::sim::routingSlotTarget(chair, slot, ts2::sim::ModelBone{0.5f, 0, std::nanf("")}));
    CHECK(!ts2::sim::routingSlotTarget(chair, slot, ts2::sim::ModelBone{0.5f, 0, std::numeric_limits<float>::infinity()}));
}

// A bone slot's facing is the owner's turned by the bone. Snap only faces it with flag bit 12, as a
// fridge's slot (without it) shows: a route there still ends facing the fridge.
void boneSlotFacingTurnsWithTheBone() {
    ts2::sim::SimObject fridge;
    fridge.data[1] = 6;
    ts2::slot::Entry slot;
    slot.objectFlags = 0x20000;
    const ts2::sim::ModelBone back{0, -0.5f, -3.14159265f};  // turned half round
    CHECK(ts2::sim::boneSlotFacing(fridge, back) == 2);
    CHECK(!ts2::sim::routingSlotTarget(fridge, slot, back)->facing);
    slot.routingFlags = 0x1000;
    CHECK(ts2::sim::routingSlotTarget(fridge, slot, back)->facing == 2);
    CHECK(ts2::sim::boneSlotFacing(fridge, ts2::sim::ModelBone{0, 0, 1.5707963f}) == 4);
}

// A Sim in a container's slot (on a sofa) has its goals measured from where that slot holds it, as
// Snap would place it there. Out of it, they are measured from its own place.
void slotGoalsMeasureFromTheContainersSlot() {
    MemoryCodeSource code;
    ts2::slot::Entry seat;
    seat.type = ts2::slot::kContainerSlot;
    seat.y = -1;
    auto sofaType = definition("Sofa", 0);
    sofaType.fields[ts2::objd::field::kGuid1] = 0x4321;
    code.addObjectType(kObjectGroup, sofaType, {}, {seat});
    ts2::sim::World world;
    auto& sitter = world.create(kObjectGroup, definition("Me", 0));
    auto& sofa = world.create(kObjectGroup, sofaType);
    sofa.x = sofa.y = 5 * 65536 + 32768;  // 5.5
    sofa.data[1] = 2;
    sitter.x = sofa.x;
    sitter.y = sofa.y;
    sitter.data[2] = sofa.id;
    const auto base = ts2::sim::slotGoalBase(world, code, sitter);
    CHECK(base.x == 6 * 65536 + 32768 && base.y == 5 * 65536 + 32768 && base.id == sitter.id);
    sitter.data[2] = 0;
    CHECK(ts2::sim::slotGoalBase(world, code, sitter).x == sitter.x);
}

void snapPutsMeAtARoutingSlot() {
    MemoryCodeSource code;
    ts2::slot::Entry front;
    front.type = ts2::slot::kRoutingSlot;
    front.y = -1;
    front.routingFlags = (1u << 12) | (1u << 4);  // face the slot's way: facing 4 (bit 4)
    ts2::slot::Entry farLeft = front;
    farLeft.x = -9;
    auto chairType = definition("Chair", 0);
    chairType.fields[ts2::objd::field::kGuid1] = 0x1234;
    code.addObjectType(kObjectGroup, chairType, {front, farLeft});
    auto snap = [](std::uint8_t slot, std::uint8_t mode, std::uint8_t flags) {
        Instruction in = primitiveCall(0x002E, T, F);
        in.operands[0] = slot;
        in.operands[2] = mode;
        in.operands[4] = flags;
        return in;
    };
    code.add(kObjectGroup, 0x1000, bhav({snap(0, 3, 0)}));
    code.add(kObjectGroup, 0x1001, bhav({snap(0, 3, 0x10)}));
    code.add(kObjectGroup, 0x1002, bhav({snap(1, 3, 0)}));
    code.add(kObjectGroup, 0x1003, bhav({snap(2, 3, 0)}));
    code.add(kObjectGroup, 0x1004, bhav({snap(0, 2, 0)}));
    code.add(kObjectGroup, 0x1005, bhav({snap(0, 7, 0)}));
    code.add(kObjectGroup, 0x1006, bhav({snap(0, 4, 0)}));  // global slot 0
    code.add(kObjectGroup, 0x1007, bhav({snap(1, 4, 0)}));
    code.globals = {front};

    ts2::sim::World world;
    auto& me = world.create(kObjectGroup, definition("Me", 0));
    auto& chair = world.create(kObjectGroup, chairType);
    chair.x = chair.y = 5 * 65536 + 32768;  // 5.5
    chair.data[1] = 2;
    me.x = 6 * 65536 + 32768;
    me.y = 3 * 65536 + 32768;
    me.thread.stackObjectId = chair.id;
    world.takeFootprintChanges();
    Vm vm(code, {}, &world);

    me.data[2] = chair.id;  // sitting in it, as a seat sets it
    CHECK(vm.run(me, 0x1001).outcome == Outcome::True && me.y == 3 * 65536 + 32768);  // test only
    CHECK(me.data[2] == chair.id);
    CHECK(vm.run(me, 0x1000).outcome == Outcome::True);
    CHECK(me.x == 6 * 65536 + 32768 && me.y == 5 * 65536 + 32768 && me.data[1] == 6);
    CHECK(me.data[2] == 0);  // standing up out of it
    CHECK(me.tile && me.tile->x == 6 && me.tile->y == 5);
    CHECK((world.takeFootprintChanges() == std::set<std::int16_t>{me.id}));

    // Facing 0 turns the offset a quarter the other way.
    chair.data[1] = 0;
    CHECK(vm.run(me, 0x1000).outcome == Outcome::True);
    CHECK(me.x == 5 * 65536 + 32768 && me.y == 4 * 65536 + 32768 && me.data[1] == 4);

    // A wall across the way refuses.
    me.y = 3 * 65536;
    ts2::sim::WallTiles walls;
    walls.add(0, 5, 4, 6, 4);
    world.setWalls(walls);
    CHECK(vm.run(me, 0x1000).outcome == Outcome::False && me.y == 3 * 65536);

    // Off the lot: refused, data 0x34 = 2.
    chair.data[1] = 2;
    chair.x = chair.y = 32768;
    CHECK(vm.run(me, 0x1002).outcome == Outcome::False && me.data[0x34] == 2);

    CHECK(vm.run(me, 0x1003).reason == "slot number out of range");
    CHECK(vm.run(me, 0x1004).outcome == Outcome::Unsupported);
    CHECK(vm.run(me, 0x1005).reason == "invalid snapping mode");

    // Mode 4: a global slot of the stack object, counted the same way.
    chair.x = chair.y = 5 * 65536 + 32768;
    me.x = 6 * 65536 + 32768;
    me.y = 3 * 65536 + 32768;
    world.setWalls({});
    CHECK(vm.run(me, 0x1006).outcome == Outcome::True && me.x == 6 * 65536 + 32768 && me.y == 5 * 65536 + 32768);
    CHECK(vm.run(me, 0x1007).reason == "slot number out of range");
}

// A bone slot (flag word 0x20000) sits at the bone named by its entry of the "Model - Bones" string
// table (the first name before a comma), turned with the object. Its facing is the object's less the
// bone's turn in 45-degree steps. The numbers are an armchair's in the game, where a Sim snapped to
// its slot 5 stands exactly there.
void snapUsesTheSlotsBone() {
    MemoryCodeSource code;
    ts2::slot::Entry nw;
    nw.type = ts2::slot::kRoutingSlot;
    nw.routingFlags = 1u << 12;
    nw.objectFlags = 0x20000;
    nw.bone = 1;
    auto chairType = definition("Armchair", 0);
    chairType.fields[ts2::objd::field::kGuid1] = 0x1234;
    code.addObjectType(kObjectGroup, chairType, {nw});
    code.addStrings(kObjectGroup, 0x90, {"route_0_N", "route_0_NW,route_0_W"});
    Instruction snap = primitiveCall(0x002E, T, F);
    snap.operands[2] = 3;
    code.add(kObjectGroup, 0x1000, bhav({snap}));

    ts2::sim::World world;
    auto& me = world.create(kObjectGroup, definition("Me", 0));
    auto& chair = world.create(kObjectGroup, chairType);
    chair.x = 360448;  // 5.5
    chair.y = 1540096; // 23.5
    chair.level = me.level = 2;
    chair.data[1] = 2;
    me.x = chair.x;
    me.y = chair.y;
    me.thread.stackObjectId = chair.id;
    Vm vm(code, {}, &world);
    CHECK(vm.run(me, 0x1000).outcome == Outcome::Unsupported);  // no model to find the bone in
    world.setModelBones([&](std::int16_t object, const std::string& bone) -> std::optional<ts2::sim::ModelBone> {
        if (object != chair.id || bone != "route_0_nw") return std::nullopt;
        return ts2::sim::ModelBone{-0.378f, 0.68f, -2.35619449f};  // -135 degrees
    });
    CHECK(vm.run(me, 0x1000).outcome == Outcome::True);
    CHECK(me.x == 405012 && me.y == 1515323 && me.data[1] == 5);
}

void routeGridSplitsAndPasses() {
    using namespace ts2::sim;
    RouteGrid grid(2, 2);
    CHECK(grid.cells().front().size == 2);
    RouteItem floor{{0, 0, 2, 2}, 1, 0, RouteItemKind::Floor, true, 0};
    grid.add(floor);
    CHECK(grid.cells().size() == 1);  // covers the root whole: no split
    RouteItem wall{{0.925f, 0, 1.075f, 1}, kBlockedCost, 15, RouteItemKind::Wall, false, 0};
    grid.add(wall);
    const auto& onWall = grid.cells()[static_cast<std::size_t>(grid.leafAt(1.0f, 0.5f))];
    CHECK(onWall.size == RouteGrid::kFinestSize);
    CHECK(onWall.items.size() == 2);
    CHECK(!grid.passage(onWall).walkable());
    const auto& open = grid.cells()[static_cast<std::size_t>(grid.leafAt(0.25f, 1.5f))];
    CHECK(open.size == 1);  // far from the wall the leaves stay large
    CHECK(grid.passage(open).walkable() && grid.passage(open).cost == 1);
    CHECK(grid.leafAt(-1, 0) == -1);
    CHECK(grid.leafAt(std::nanf(""), 0.5f) == -1 && grid.leafAt(0.5f, std::nanf("")) == -1);
    CHECK(RouteGrid::accepts(floor) && !RouteGrid::accepts({{1, 0, 0, 1}}) &&
          !RouteGrid::accepts({{0, 0, std::numeric_limits<float>::infinity(), 1}}) &&
          !RouteGrid::accepts({{0, 0, 1, 1}, std::nanf("")}));

    RouteGrid bare(1, 1);
    bare.add({{0, 0, 0.5f, 0.5f}, 12, 0, RouteItemKind::WallJoint, false, 0});
    CHECK(bare.passage(bare.cells()[static_cast<std::size_t>(bare.leafAt(0.1f, 0.1f))]).type == kNoFloorType);
    bare.add({{0, 0, 1, 1}, 1, 0, RouteItemKind::Floor, true, 0});
    const auto joint = bare.passage(bare.cells()[static_cast<std::size_t>(bare.leafAt(0.1f, 0.1f))]);
    CHECK(joint.walkable() && joint.cost == 12);
    bare.add({{0, 0, 0.5f, 0.5f}, kBlockedCost, 15, RouteItemKind::Wall, false, 0});
    CHECK(!bare.passage(bare.cells()[static_cast<std::size_t>(bare.leafAt(0.1f, 0.1f))]).walkable());
    bare.add({{0, 0, 0.5f, 0.5f}, 1, 0, RouteItemKind::Solid, false, 0});
    CHECK(bare.passage(bare.cells()[static_cast<std::size_t>(bare.leafAt(0.1f, 0.1f))]).walkable());
}

// A walk planned as the game plans it: an adult woman's 4-tile walk with straight start and stop
// clips (the start mirrored), checked against the game's own segments and speed curve for it.
void walksArePlannedAsTheGamePlansThem() {
    using ts2::sim::LocoClip;
    const LocoClip goShort{"a-turnAndGoShort-alt-0", 750, 0.347207f, 0.5f, 0.246718f, 0.000462943f, 0};
    const LocoClip go{"a-turnAndGo-alt-0", 750, 0.853361f, 0.5f, 0.699262f, 0.00113781f, 0};
    const LocoClip stopShort{"a-turnAndStopShort-alt-0", 491, 0.156264f, 0.5f, 0.244213f, 0.000318256f, 0};
    const LocoClip stop{"a-turnAndStop-alt-0", 491, 0.337463f, 0.5f, 0.712212f, 0.000687298f, 0};
    const LocoClip walkShort{"a-female-walkShort-alt0-normal", 691, 0.663381f, 0, 0.269277f, 0.000960031f, 0};
    const LocoClip walk{"a-female-walk-alt0-normal", 725, 1.45818f, 0, 0.611771f, 0.00201128f, 0};
    ts2::sim::WalkPlanInput in;
    in.length = 4.00007f;
    in.start = ts2::sim::LocoPair{goShort, go, true};
    in.stop = ts2::sim::LocoPair{stopShort, stop, false};
    in.loops = {{ts2::sim::LocoPair{walkShort, walk, false}, 1}};
    in.speed = ts2::sim::walkSpeed(walk.speed, in.length);
    in.topLoopSpeed = walk.speed;
    in.topStraightSpeed = go.speed;
    const auto plan = ts2::sim::planWalk(in);
    const auto near = [](float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; };
    CHECK(plan.segments.size() == 3);
    const auto& startPart = plan.segments[0];
    const auto& loop = plan.segments[1];
    const auto& stopPart = plan.segments[2];
    CHECK(near(startPart.end, 0.64844f, 2e-4f) && near(startPart.blend, 0.2f, 1e-6f));
    CHECK(near(loop.start, 0.59908f, 2e-4f) && near(loop.end, 3.7531f, 2e-4f));
    CHECK(near(loop.strideStart, 1.23605f, 2e-4f) && near(loop.originStart, 0.64844f, 2e-4f) && near(loop.originEnd, 3.12054f, 3e-4f));
    CHECK(near(stopPart.start, 3.73856f, 2e-4f) && near(stopPart.end, 4.00007f, 1e-5f));
    // The game's curve: the start at 1.14 tiles a second for 569.9 ms, up from 1.75 to 2.01 over
    // 821.7 ms, a 5 ms hold, down again, and the stop at 0.69 for 380.5 ms.
    CHECK(plan.speed.size() == 5);
    if (plan.speed.size() == 5) {
        CHECK(near(plan.speed[0].from, 0.00113781f, 1e-7f) && near(plan.speed[0].ms, 569.9f, 0.5f));
        CHECK(near(plan.speed[1].from, 0.0017492f, 2e-6f) && near(plan.speed[1].to, 0.00201128f, 1e-7f) && near(plan.speed[1].ms, 821.7f, 1.0f));
        CHECK(near(plan.speed[2].ms, 5.0f, 0.2f));
        CHECK(near(plan.speed[4].from, 0.000687298f, 1e-7f) && near(plan.speed[4].ms, 380.5f, 0.5f));
    }
    // The walk covers its length (and stops there), and the loop's phase where the start ends is
    // the start clip's own end phase, mirrored (the other foot).
    CHECK(near(ts2::sim::walkedAt(plan, ts2::sim::walkMs(plan)), 4.00007f, 1e-4f));
    std::vector<ts2::sim::WalkPose> poses;
    ts2::sim::walkPoses(plan, 0.64844f, poses);
    float total = 0, loopPhase = -1;
    for (const auto& p : poses) {
        total += p.weight;
        if (p.clip == &loop.clips.clip || p.clip == &*loop.clips.alt) {
            loopPhase = p.phase;
        }
    }
    CHECK(near(total, 1, 1e-5f) && loopPhase >= 0 && std::min(loopPhase, 1 - loopPhase) < 1e-4f);
    // Halfway through the start only the start's clips show, mixed to travel its distance.
    ts2::sim::walkPoses(plan, 0.3f, poses);
    CHECK(poses.size() == 2 && poses[0].mirrored && near(poses[0].phase, 0.3f / 0.64844f, 1e-4f));
    // A teen boy's walk with two styles of walk loop (person data 0x70 and 0x71 at 42 and 57) and
    // the teens' size 0.94, which shortens the loop but not the start and stop, checked against
    // the game's own plan of the same 3.19-tile walk: start 0.77907, stride 1.40202, stop 0.31078.
    {
        const LocoClip tShort0{"t-male-walkShort-alt0-normal", 800, 0.721145f, 0, 0.316090f, 0.000901432f, 0};
        const LocoClip tLong0{"t-male-walk-alt0-normal", 800, 1.484713f, 0, 0.664439f, 0.001855891f, 0};
        const LocoClip tShort1{"t-male-walkShort-alt1-normal", 800, 0.865374f, 0, 0.383234f, 0.001081718f, 0};
        const LocoClip tLong1{"t-male-walk-alt1-normal", 800, 1.781655f, 0, 0.803798f, 0.002227069f, 0};
        auto teen = in;
        teen.length = 3.19288f;
        teen.start->mirrored = false;
        // Its loop counts 3 half cycles, so it ends on the other foot: the game picks the straight
        // stop again for that foot, mirrored.
        teen.stopOtherFoot = ts2::sim::turnClipsForFoot({stopShort, stop}, 0, 0);
        teen.loops = {{{tShort0, tLong0, false}, 0.42f}, {{tShort1, tLong1, false}, 0.57f}};
        teen.speed = ts2::sim::walkSpeed(0.42f / 0.99f * tLong0.speed + 0.57f / 0.99f * tLong1.speed, teen.length);
        teen.topLoopSpeed = tLong0.speed;
        teen.scale = 0.94f;
        const auto t = ts2::sim::planWalk(teen);
        CHECK(t.segments.size() == 4);
        if (t.segments.size() == 4) {
            CHECK(near(t.segments[0].end, 0.77907f, 2e-5f));
            CHECK(t.segments[1].kind == ts2::sim::WalkSegment::Kind::Loop && t.segments[2].kind == ts2::sim::WalkSegment::Kind::Loop);
            CHECK(near(t.segments[1].weight, 0.42424f, 1e-5f) && near(t.segments[2].weight, 0.57576f, 1e-5f));
            CHECK(near(t.segments[1].strideStart, 1.40202f, 2e-5f) && t.segments[2].strideStart == t.segments[1].strideStart);
            CHECK(near(t.segments[3].end - t.segments[3].start, 0.31078f, 2e-5f) && t.segments[3].clips.mirrored);
        }
        // Both styles show in the loop, by their shares.
        ts2::sim::walkPoses(t, 1.5f, poses);
        float style1 = 0;
        for (const auto& p : poses) {
            if (p.clip->name.find("alt1") != std::string::npos) {
                style1 += p.weight;
            }
        }
        CHECK(near(style1, 0.57576f, 1e-4f));
        // Stopped soon, both styles' loop parts end together and the stop follows.
        auto stopped = t;
        CHECK(ts2::sim::stopSoon(stopped, 1000, teen.stop, 0.94f, teen.topLoopSpeed) == ts2::sim::StopSoon::Soon);
        CHECK(stopped.segments.size() == 4 && stopped.segments.back().kind == ts2::sim::WalkSegment::Kind::Stop);
        if (stopped.segments.size() == 4) {
            CHECK(stopped.segments[1].end == stopped.segments[2].end && stopped.segments[1].end > stopped.segments[3].start);
        }
    }
    // Broken clip values (not numbers, infinite, negative, huge) neither hang the planner nor give
    // a walk that isn't finite.
    const float odd[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1.0f, 0.0f, 1e30f, 1e-30f};
    std::mt19937 random(7);
    std::uniform_int_distribution<int> pick(0, 5);
    for (int i = 0; i < 300; ++i) {
        auto broken = in;
        for (float* f : {&broken.length, &broken.start->clip.travel, &broken.stop->alt->match, &broken.loops[0].clips.clip.travel,
                         &broken.loops[0].clips.alt->speed, &broken.loops[0].share, &broken.speed, &broken.topStraightSpeed}) {
            if (pick(random) < 2) {
                *f = odd[pick(random)];
            }
        }
        const auto p = ts2::sim::planWalk(broken);
        const float ms = ts2::sim::walkMs(p);
        const float walked = ts2::sim::walkedAt(p, std::isfinite(ms) ? ms : 0.0f);
        ts2::sim::walkPoses(p, walked, poses);
        CHECK(std::isfinite(walked) && walked >= 0);
        for (const auto& pose : poses) {
            CHECK(std::isfinite(pose.weight) && std::isfinite(pose.phase));
        }
    }
}

// Short straight moves that keep the facing are steps, checked against the game's own steps: their
// clips, mixes and weights, and the speed the game moved the Sim at.
// The game's picker chooses a turning stop by its turn and the foot it begins on: the clips that
// turn that way on that foot, else the ones beginning on the other foot that turn the other way,
// mirrored. A straight stop has no other-foot clips, so it is mirrored for the other foot.
void turnClipsArePickedByFoot() {
    using ts2::sim::LocoClip;
    constexpr float kEighth = 0.785398f;
    const LocoClip plainShort{"a-turnAndStopShort-alt-45", 550, 0.109717f, 0.5f, 0.244212f, 0.000199486f, -kEighth};
    const LocoClip plain{"a-turnAndStop-alt-45", 550, 0.285327f, 0.5f, 0.712212f, 0.000518776f, -kEighth};
    const LocoClip otherShort{"a-turnAndStopShortR-alt-45", 316, 0.181322f, 0, 0.248811f, 0.000573802f, -kEighth};
    const LocoClip other{"a-turnAndStopR-alt-45", 316, 0.422061f, 0, 0.751949f, 0.001335635f, -kEighth};
    const std::vector<LocoClip> clips{other, plain, otherShort, plainShort};
    const auto is = [](const std::optional<ts2::sim::LocoPair>& p, const LocoClip& a, const LocoClip& b, bool mirrored) {
        return p && p->clip.name == a.name && p->alt && p->alt->name == b.name && p->mirrored == mirrored;
    };
    CHECK(is(ts2::sim::turnClipsForFoot(clips, -kEighth, 0.5f), plainShort, plain, false));
    CHECK(is(ts2::sim::turnClipsForFoot(clips, -kEighth, 0), otherShort, other, false));
    CHECK(is(ts2::sim::turnClipsForFoot(clips, kEighth, 0), plainShort, plain, true));
    CHECK(is(ts2::sim::turnClipsForFoot(clips, kEighth, 0.5f), otherShort, other, true));
    CHECK(!ts2::sim::turnClipsForFoot(clips, 2 * kEighth, 0.5f) && !ts2::sim::turnClipsForFoot({}, 0, 0));
    const LocoClip straightShort{"a-turnAndStopShort-alt-0", 491, 0.156264f, 0.5f, 0.244213f, 0.000318256f, 0};
    const LocoClip straight{"a-turnAndStop-alt-0", 491, 0.337463f, 0.5f, 0.712212f, 0.000687298f, 0};
    CHECK(is(ts2::sim::turnClipsForFoot({straight, straightShort}, 0, 0.5f), straightShort, straight, false));
    CHECK(is(ts2::sim::turnClipsForFoot({straight, straightShort}, 0, 0), straightShort, straight, true));
}

// A walk told to stop soon, as an interrupted route stops it: the loop runs on to the next half of
// its cycle, then the straight stop plays on the foot that comes down next, its travel mixed to
// match the loop and shortened by the Sim's size, while the speed holds and then slows to the
// stop's own. A start plays to its end first. A walk already stopping goes on as planned.
void walksToldToStopSoonStopAtTheNextFoot() {
    using ts2::sim::LocoClip;
    using ts2::sim::StopSoon;
    using Kind = ts2::sim::WalkSegment::Kind;
    const LocoClip goShort{"a-turnAndGoShort-alt-0", 750, 0.347207f, 0.5f, 0.246718f, 0.000462943f, 0};
    const LocoClip go{"a-turnAndGo-alt-0", 750, 0.853361f, 0.5f, 0.699262f, 0.00113781f, 0};
    const LocoClip stopShort{"a-turnAndStopShort-alt-0", 491, 0.156264f, 0.5f, 0.244213f, 0.000318256f, 0};
    const LocoClip stop{"a-turnAndStop-alt-0", 491, 0.337463f, 0.5f, 0.712212f, 0.000687298f, 0};
    const LocoClip walkShort{"a-female-walkShort-alt0-normal", 691, 0.663381f, 0, 0.269277f, 0.000960031f, 0};
    const LocoClip walk{"a-female-walk-alt0-normal", 725, 1.45818f, 0, 0.611771f, 0.00201128f, 0};
    const ts2::sim::LocoPair straight{stopShort, stop, false};
    ts2::sim::WalkPlanInput in;
    in.length = 10;
    in.start = ts2::sim::LocoPair{goShort, go, false};
    in.stop = straight;
    in.loops = {{ts2::sim::LocoPair{walkShort, walk, false}, 1}};
    in.speed = ts2::sim::walkSpeed(walk.speed, in.length);
    in.topLoopSpeed = walk.speed;
    in.topStraightSpeed = go.speed;
    const auto planned = ts2::sim::planWalk(in);
    const auto near = [](float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; };
    const float scale = 0.94f;
    // In the loop, at each foot.
    for (const float ms : {2000.0f, 2150.0f, 2300.0f, 2450.0f}) {
        auto plan = planned;
        const float x0 = ts2::sim::walkedAt(plan, ms), speed = ts2::sim::speedAt(plan, ms);
        const auto& loop = plan.segments[1];
        const float cycles = (x0 - loop.originStart) / loop.strideStart, phase = cycles - std::floor(cycles);
        CHECK(ts2::sim::stopSoon(plan, ms, straight, scale, walk.speed) == StopSoon::Soon);
        CHECK(plan.segments.size() == 3 && plan.segments[2].kind == Kind::Stop);
        if (plan.segments.size() != 3) {
            continue;
        }
        const float from = x0 + ((phase < 0.5f ? 0.5f : 1.0f) - phase) * loop.strideStart;
        const auto& s = plan.segments[2];
        CHECK(near(s.start, from, 1e-4f) && near(plan.segments[1].end, from + 0.05f * loop.strideStart, 1e-4f));
        // The second half of the cycle comes down on the other foot: the stop is mirrored.
        CHECK(s.clips.mirrored == (phase > 0.5f) && s.blend == 0);
        const float share = std::clamp((walk.travel - loop.strideStart) / (walk.travel - walkShort.travel), 0.0f, 1.0f);
        const float match = share * walkShort.match + (1 - share) * walk.match;
        const float w = std::clamp((stop.match - match) / (stop.match - stopShort.match), 0.0f, 1.0f);
        const float travel = w * stopShort.travel + (1 - w) * stop.travel;
        CHECK(near(s.end - s.start, travel * scale, 1e-4f) && near(s.strideStart, travel * scale, 1e-5f) && near(plan.length, s.end, 1e-6f));
        // The speed goes on from where it was, and the walk ends at the stop's own speed, scaled
        // as the walk's.
        CHECK(near(ts2::sim::walkedAt(plan, ms), x0, 2e-3f) && near(ts2::sim::speedAt(plan, ms), speed, 2e-4f));
        CHECK(!plan.speed.empty() && near(plan.speed.back().to, stop.speed * speed / walk.speed, 1e-7f));
        CHECK(near(ts2::sim::walkedAt(plan, ts2::sim::walkMs(plan)), plan.length, 1e-3f));
    }
    // In the start: it plays to its end, then the stop at its own speed.
    {
        auto plan = planned;
        CHECK(ts2::sim::stopSoon(plan, 200, straight, 1, walk.speed) == StopSoon::Soon);
        CHECK(plan.segments.size() == 2 && plan.segments[0].kind == Kind::Start && plan.segments[1].kind == Kind::Stop);
        if (plan.segments.size() == 2) {
            CHECK(near(plan.segments[1].start, planned.segments[0].end, 1e-6f));
        }
    }
    // Already stopping, or not moving at all.
    {
        auto plan = planned;
        const float ms = ts2::sim::walkMs(plan) - 50;
        CHECK(ts2::sim::stopSoon(plan, ms, straight, 1, walk.speed) == StopSoon::AsPlanned && plan.segments.size() == 3 && plan.length == planned.length);
        CHECK(ts2::sim::stopSoon(plan, ts2::sim::walkMs(plan) + 100, straight, 1, walk.speed) == StopSoon::Now);
    }
    // No stop clip for the foot: the planned stop is dropped and the loop plays on.
    {
        auto plan = planned;
        auto odd = straight;
        odd.clip.loopPhase = odd.alt->loopPhase = 0.25f;
        CHECK(ts2::sim::stopSoon(plan, 2000, odd, 1, walk.speed) == StopSoon::Soon && plan.segments.size() == 2 && plan.length == planned.length);
    }
    // Broken clip values and times give a walk that is finite.
    const float odd[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1.0f, 0.0f, 1e30f, 1e-30f};
    std::mt19937 random(11);
    std::uniform_int_distribution<int> pick(0, 5);
    for (int i = 0; i < 300; ++i) {
        auto plan = planned;
        auto broken = straight;
        float ms = 2000, size = scale, top = walk.speed;
        for (float* f : {&broken.clip.travel, &broken.clip.loopPhase, &broken.alt->match, &broken.alt->speed, &ms, &size, &top}) {
            if (pick(random) < 2) {
                *f = odd[pick(random)];
            }
        }
        ts2::sim::stopSoon(plan, ms, broken, size, top);
        CHECK(std::isfinite(plan.length) && std::isfinite(ts2::sim::walkMs(plan)) && std::isfinite(ts2::sim::walkedAt(plan, 3000)));
    }
}

void stepsArePlannedAsTheGamePlansThem() {
    using ts2::sim::LocoClip;
    const std::map<std::string, LocoClip> clips = {
        {"a-stepShort-0", {"a-stepShort-0", 691, 0.303956f, 0, 0, 0.000439878f, 0}},
        {"a-step-0", {"a-step-0", 1216, 0.607912f, 0, 0, 0.000499928f, 0}},
        {"a-stepShort-45", {"a-stepShort-45", 1158, 0.339095f, 0, 0, 0.000292828f, -0.785398f}},
        {"a-step-45", {"a-step-45", 1158, 0.645294f, 0, 0, 0.000557248f, -0.785398f}},
        {"a-stepInPlace", {"a-stepInPlace", 691, -1.6e-5f, 0, 0, 0, 0}},
        {"c-stepShort-180", {"c-stepShort-180", 1216, 0.242395f, 0, 0, 0.000199338f, -3.14159f}},
        {"c-step-180", {"c-step-180", 1216, 0.483944f, 0, 0, 0.000397981f, -3.14159f}},
        {"c-stepShort-135", {"c-stepShort-135", 1158, 0.196793f, 0, 0, 0.000169943f, -2.35619f}},
        {"c-step-135", {"c-step-135", 1158, 0.409987f, 0, 0, 0.000354047f, -2.35619f}},
    };
    const auto clipAt = [&](std::int16_t stage) {
        return [&clips, stage](float angle) -> std::optional<ts2::sim::StepClip> {
            const auto names = ts2::sim::stepClips(angle, stage);
            if (!names || !clips.count(names->shortOne) || !clips.count(names->full)) {
                return std::nullopt;
            }
            return ts2::sim::StepClip{{clips.at(names->shortOne), clips.at(names->full), names->mirrored}, names->turn};
        };
    };
    const auto near = [](float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; };
    const auto inPlace = std::optional<LocoClip>(clips.at("a-stepInPlace"));
    CHECK(ts2::sim::takesStep(0.3f, 0.3f, 0.005f) && !ts2::sim::takesStep(1.0f, 1.0f, 0) && !ts2::sim::takesStep(0.5f, 0.45f, 0) &&
          !ts2::sim::takesStep(0.5f, 0.5f, 0.02f));
    // A teen's step forward: the short and full steps mixed 0.95 / 0.05, at 0.62 tiles a second.
    const auto ahead = ts2::sim::planStep(0.3f, 0, 0.94f, clipAt(17), inPlace);
    CHECK(ahead && ahead->keepsFacing && ahead->segments.size() == 1 && ahead->speed.size() == 1);
    std::vector<ts2::sim::WalkPose> poses;
    if (ahead && !ahead->speed.empty()) {
        CHECK(near(ahead->speed[0].from, 0.00062003f, 2e-7f));
        ts2::sim::walkPoses(*ahead, 0.15f, poses);
        CHECK(poses.size() == 2 && near(poses[0].weight, 0.95f, 2e-3f) && near(poses[0].phase, 0.5f, 1e-5f));
    }
    // 36.9 degrees to either side: the straight step 0.181 and the 45-degree one 0.819, both mirrored
    // on the positive side.
    for (const float side : {1.0f, -1.0f}) {
        const auto diagonal = ts2::sim::planStep(0.5f, side * 0.643501f, 1, clipAt(27), inPlace);
        CHECK(diagonal && diagonal->segments.size() == 2);
        if (diagonal && diagonal->segments.size() == 2) {
            const auto& straight = diagonal->segments[0];
            const auto& angled = diagonal->segments[1];
            CHECK(near(straight.weight, 0.18068f, 1e-4f) && near(angled.weight, 0.81932f, 1e-4f));
            CHECK(straight.clips.mirrored == (side > 0) && angled.clips.mirrored == (side > 0));
            CHECK(angled.clips.alt && angled.clips.alt->name == "a-step-45");
            ts2::sim::walkPoses(*diagonal, 0.25f, poses);
            CHECK(poses.size() == 4 && near(poses[2].turn, side * 0.785398f, 1e-5f) && poses[0].turn == 0);
        }
    }
    // A child's step back a little off straight: the step back 0.981 and the 135-degree one 0.019.
    const auto back = ts2::sim::planStep(0.34499f, -3.12697f, 1, clipAt(10), std::nullopt);
    CHECK(back && back->segments.size() == 2);
    if (back && back->segments.size() == 2) {
        CHECK(near(back->segments[0].weight, 0.98142f, 1e-4f) && back->segments[1].clips.clip.name == "c-stepShort-135");
    }
    // Too far for any step: walked instead. A move of almost nothing mixes the step on the spot in.
    CHECK(!ts2::sim::planStep(0.96703f, -0.50303f, 1, clipAt(27), inPlace));
    const auto tiny = ts2::sim::planStep(0.0002f, 0, 1, clipAt(27), inPlace);
    CHECK(tiny && tiny->segments.size() == 1 && tiny->segments[0].clips.clip.name == "a-stepInPlace");
    // The step on the spot plays unturned beside a turned step (the game's root stays on the facing).
    const auto sideways = ts2::sim::planStep(0.2f, -0.785398f, 1, clipAt(27), inPlace);
    if (sideways) {
        ts2::sim::walkPoses(*sideways, 0.1f, poses);
        CHECK(poses.size() == 2 && poses[0].clip->name == "a-stepInPlace" && poses[0].turn == 0 && near(poses[1].turn, -0.785398f, 1e-5f));
    }
    // Broken clip values give no step or a finite one.
    std::map<std::string, LocoClip> broken = clips;
    broken["a-step-0"].travel = std::numeric_limits<float>::infinity();
    broken["a-stepShort-0"].speed = std::numeric_limits<float>::quiet_NaN();
    const auto brokenAt = [&broken](float angle) -> std::optional<ts2::sim::StepClip> {
        const auto names = ts2::sim::stepClips(angle, 27);
        if (!names || !broken.count(names->shortOne)) {
            return std::nullopt;
        }
        return ts2::sim::StepClip{{broken.at(names->shortOne), broken.at(names->full), names->mirrored}, names->turn};
    };
    for (const float d : {0.3f, std::numeric_limits<float>::quiet_NaN(), -1.0f, 1e30f}) {
        if (const auto p = ts2::sim::planStep(d, std::numeric_limits<float>::infinity(), 1e30f, brokenAt, inPlace)) {
            CHECK(std::isfinite(ts2::sim::walkMs(*p)) && ts2::sim::walkMs(*p) > 0);
        }
    }
}

// A walk's path is laid over the points the game makes from the route (a point ahead along the Sim's
// facing, the end point along the end facing, long stretches cut in pieces), as a B-spline that
// starts and ends on the route and turns smoothly.
void walkPathsRoundCorners() {
    using Points = std::vector<std::pair<float, float>>;
    const auto near = [](float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; };
    const Points route{{25.89716f, 16.92915f}, {26.06250f, 17.00999f}, {26.09187f, 17.26300f}, {26.12125f, 17.51599f}, {26.15062f, 17.76900f}, {26.17999f, 18.02200f}};
    const Points game{{25.89716f, 16.92915f}, {25.94053f, 16.97253f}, {26.06250f, 17.00999f}, {26.09187f, 17.26300f},
                      {26.12125f, 17.51599f}, {26.15062f, 17.76900f}, {26.17999f, 17.93711f}, {26.17999f, 18.02200f}};
    const auto points = ts2::sim::walkPathPoints(route, -0.785398f, -2.356194f);
    CHECK(points.size() == game.size());
    for (std::size_t i = 0; i < points.size() && i < game.size(); ++i) {
        CHECK(near(points[i].first, game[i].first, 5e-4f) && near(points[i].second, game[i].second, 5e-4f));
    }
    const ts2::sim::WalkPath straight(Points{{0.0f, 0.0f}, {1.0f, 0.0f}, {3.0f, 0.0f}}, 0, 0);
    CHECK(near(straight.length(), 3, 1e-4f) && near(straight.at(1.5f).x, 1.5f, 1e-3f) && near(straight.at(1.5f).y, 0, 1e-5f));
    const ts2::sim::WalkPath corner(Points{{0.0f, 0.0f}, {2.0f, 0.0f}, {2.0f, 2.0f}}, 0, -1.5707963f);
    const auto start = corner.at(0), end = corner.at(corner.length());
    CHECK(corner.length() < 4 && near(start.x, 0, 1e-5f) && near(end.x, 2, 1e-4f) && near(end.y, 2, 1e-4f));
    CHECK(start.heading && near(*start.heading, 0, 1e-2f) && end.heading && near(*end.heading, -1.5707963f, 1e-2f));
    float biggestTurn = 0;
    std::optional<float> last;
    for (float d = 0; d <= corner.length(); d += 0.01f) {
        const auto p = corner.at(d);
        if (last && p.heading) {
            biggestTurn = std::max(biggestTurn, std::fabs(std::remainder(*p.heading - *last, 6.2831853f)));
        }
        last = p.heading;
    }
    CHECK(biggestTurn < 0.2f);
    // Broken or empty routes give a finite path.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const ts2::sim::WalkPath broken(Points{{nan, 1.0f}, {std::numeric_limits<float>::infinity(), nan}}, nan, nan);
    CHECK(std::isfinite(broken.length()) && std::isfinite(broken.at(nan).x));
    CHECK(ts2::sim::WalkPath(Points{}, 0, 0).length() == 0 && ts2::sim::WalkPath(Points{{1.0f, 1.0f}}, 0, 0).length() < 1e-5f);
}

// Short routes take the game's shapes: a turn on the spot to the end facing and a straight walk, or
// (a Sim with no step clips) the path with turns before and after.
void shortRoutesTurnOnTheSpot() {
    using Points = std::vector<std::pair<float, float>>;
    const auto near = [](float a, float b) { return std::fabs(std::remainder(a - b, 6.2831853f)) < 1e-3f; };
    const float pi = 3.14159265f;
    // 0.365 tiles from facing -180 to -225: turn to -225, then straight.
    const auto c = ts2::sim::routeShape(Points{{32.812f, 16.334f}, {32.476f, 16.476f}}, -pi, -3.926991f, true);
    CHECK(c.straight && near(c.turnTo, -3.926991f) && near(c.walkFrom, -3.926991f) && near(c.walkTo, -3.926991f) && !c.turnAfter);
    // 0.27 tiles, longer than a quarter: the same.
    const auto d = ts2::sim::routeShape(Points{{33.195f, 18.765f}, {33.217f, 18.5f}}, -pi, -pi, true);
    CHECK(d.straight && near(d.turnTo, -pi));
    // 0.06 tiles keeping the facing: straight (a step).
    const auto b = ts2::sim::routeShape(Points{{19.735f, 17.36f}, {19.705f, 17.305f}}, -0.785398f, -0.785398f, true);
    CHECK(b.straight && near(b.walkFrom, -0.785398f));
    // 0.08 tiles by a Sim with no step clips: the path from -180 to 135, then a turn to -180.
    const auto a = ts2::sim::routeShape(Points{{21.36f, 16.485f}, {21.296f, 16.443f}}, -pi, -pi, false);
    CHECK(!a.straight && near(a.walkFrom, -pi) && near(a.walkTo, 2.356194f) && a.turnAfter && near(*a.turnAfter, -pi));
    // Longer routes are one walk.
    const auto longer = ts2::sim::routeShape(Points{{0.0f, 0.0f}, {2.0f, 0.0f}}, 1.0f, -1.5707963f, true);
    CHECK(!longer.straight && near(longer.turnTo, 1.0f) && near(longer.walkTo, -1.5707963f) && !longer.turnAfter);
}

} // namespace

std::vector<Test> routePlanningTests() {
    return {
        {"the route grid splits along items and passes by their types", routeGridSplitsAndPasses},
        {"floor route items follow the floors", floorRouteItemsFollowTheFloors},
        {"post route items use the post footprint", postRouteItemsUseThePostFootprint},
        {"object footprints follow the model", objectFootprintsFollowTheModel},
        {"object route items follow the game rules", objectRouteItemsFollowTheGameRules},
        {"route grid removal merges cells", routeGridRemovalMergesCells},
        {"route search walks around a wall", routeSearchWalksAroundAWall},
        {"path pass lines see the grid", pathPassLinesSeeTheGrid},
        {"path passes smooth a path", pathPassesSmoothAPath},
        {"route search escapes and refuses", routeSearchEscapesAndRefuses},
        {"route search crosses levels over stairs", routeSearchCrossesLevelsOverStairs},
        {"stair slots pair by name", stairSlotsPairByName},
        {"stair routes are walked in stretches", stairRoutesAreWalkedInStretches},
        {"route search goes through doors", routeSearchGoesThroughDoors},
        {"bone slot goals follow the joints", boneSlotGoalsFollowTheJoints},
        {"ground clicks stay clear of walls", groundClicksStayClearOfWalls},
        {"idle spot ignores Sims on their way", idleSpotIgnoresSimsOnTheirWay},
        {"swept slot goals circle the owner", sweptSlotGoalsCircleTheOwner},
        {"swept slot goals offer seats", sweptSlotGoalsOfferSeats},
        {"slot goals measure from the container's slot", slotGoalsMeasureFromTheContainersSlot},
        {"placement goals skip walls", placementGoalsSkipWalls},
        {"point goal misses give the reason", pointGoalMissesGiveTheReason},
        {"update footprint marks the stack object", updateFootprintMarksTheStackObject},
        {"update footprint marks every multi-tile part", updateFootprintMarksEveryMultiTilePart},
        {"snap puts me at a routing slot", snapPutsMeAtARoutingSlot},
        {"a bone slot with a broken turn has no spot", boneSlotWithABrokenTurnHasNoSpot},
        {"a bone slot's facing turns with its bone", boneSlotFacingTurnsWithTheBone},
        {"snap uses the slot's bone", snapUsesTheSlotsBone},
        {"walks are planned as the game plans them", walksArePlannedAsTheGamePlansThem},
        {"turn clips are picked by foot", turnClipsArePickedByFoot},
        {"walks told to stop soon stop at the next foot", walksToldToStopSoonStopAtTheNextFoot},
        {"steps are planned as the game plans them", stepsArePlannedAsTheGamePlansThem},
        {"walk paths round corners", walkPathsRoundCorners},
        {"short routes turn on the spot", shortRoutesTurnOnTheSpot},
    };
}
