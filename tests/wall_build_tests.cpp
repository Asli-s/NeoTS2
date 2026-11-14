// Build mode's wall tool: walls built and taken down on the room wall graph, and rooms numbered again.
#include "test_support.hpp"

#include "sim/wall_build.hpp"

#include <set>

namespace {

namespace sim = ts2::sim;

ts2::wallgraph::WallGraph emptyLot() {
    ts2::wallgraph::WallGraph g;
    g.width = 6;
    g.height = 6;
    g.levels = 1;
    return g;
}

const ts2::wallgraph::Wall* wallBetween(const ts2::wallgraph::WallGraph& g, int x0, int y0, int x1, int y1) {
    const ts2::wallgraph::PointIndex points(g);
    for (const auto& w : g.walls) {
        const auto* a = points(w.from);
        const auto* b = points(w.to);
        if (a && b && a->x == static_cast<float>(x0) && a->y == static_cast<float>(y0) && b->x == static_cast<float>(x1) &&
            b->y == static_cast<float>(y1)) {
            return &w;
        }
    }
    return nullptr;
}

// A drag gives one tile edge a tile along the longer way; the lot's border takes no wall.
void wallsRunAlongTileEdges() {
    const auto steps = sim::wallSteps(0, 1, 1, 4, 2);
    CHECK(steps.size() == 3 && steps[0].x0 == 1 && steps[0].x1 == 2 && steps[2].x1 == 4 && steps[2].y1 == 1);
    CHECK(sim::wallSteps(0, 2, 5, 2, 3).size() == 2 && sim::wallSteps(0, 2, 5, 2, 3)[1].y1 == 3);
    CHECK(sim::wallSteps(0, 2, 2, 2, 2).empty());
    const auto g = emptyLot();
    CHECK(sim::insideLot(g, {0, 1, 1, 2, 1}));
    CHECK(sim::insideLot(g, {0, 0, 1, 1, 1}));    // from the border inwards
    CHECK(!sim::insideLot(g, {0, 0, 1, 0, 2}));   // along it
    CHECK(!sim::insideLot(g, {0, 5, 6, 6, 6}));
    CHECK(!sim::insideLot(g, {0, 6, 1, 7, 1}));   // off the lot
}

// Four walls make a room; a wall across it makes two; taking that down makes one again.
void wallsMakeAndJoinRooms() {
    auto g = emptyLot();
    std::vector<ts2::walllayer::Entry> layer;
    std::map<std::uint16_t, ts2::stringmap::Entry> patterns;
    std::vector<sim::WallStep> square;
    for (const auto& line : {std::array{1, 1, 3, 1}, std::array{3, 1, 3, 3}, std::array{3, 3, 1, 3}, std::array{1, 3, 1, 1}}) {
        const auto steps = sim::wallSteps(0, line[0], line[1], line[2], line[3]);
        square.insert(square.end(), steps.begin(), steps.end());
    }
    const auto built = sim::buildWalls(g, layer, patterns, square, "blank", nullptr);
    CHECK(built.size() == 8 && layer.size() == 8 && g.points.size() == 8);
    CHECK(layer[0].type == sim::kPlainWallStyle && layer[0].left != 0 && layer[0].left == layer[0].right);
    CHECK(patterns.size() == 1 && patterns.begin()->second.name == "blank" && patterns.begin()->second.uses == 16);
    // Again, nothing more is built.
    CHECK(sim::buildWalls(g, layer, patterns, square, "blank", nullptr).empty());
    sim::renumberRooms(g, 0);
    const auto* top = wallBetween(g, 1, 1, 2, 1);
    CHECK(top != nullptr && top->leftRoom == 0 && top->rightRoom > 0);
    const std::int32_t room = top ? top->rightRoom : -1;
    const auto* side = wallBetween(g, 3, 2, 3, 3);  // walking down x = 3: the room is on the right (low x)
    CHECK(side != nullptr && side->rightRoom == room && side->leftRoom == 0);

    // Across the middle at x = 2.
    const auto across = sim::wallSteps(0, 2, 1, 2, 3);
    CHECK(sim::buildWalls(g, layer, patterns, across, "blank", nullptr).size() == 2);
    sim::renumberRooms(g, 0);
    std::set<std::int32_t> inside;
    for (const auto& w : g.walls) {
        for (const auto r : {w.leftRoom, w.rightRoom}) {
            CHECK(r != sim::kUnnumberedRoom);
            if (r != 0) {
                inside.insert(r);
            }
        }
    }
    CHECK(inside.size() == 2 && inside.contains(room));

    // Taken down (an edge without a wall is passed over): one room again, and its points go.
    std::vector<sim::WallStep> down = across;
    down.push_back({0, 4, 4, 5, 4});
    CHECK(sim::removeWalls(g, layer, patterns, down, nullptr).size() == 2);
    CHECK(layer.size() == 8 && g.walls.size() == 8 && g.points.size() == 8);
    sim::renumberRooms(g, 0);
    inside.clear();
    for (const auto& w : g.walls) {
        for (const auto r : {w.leftRoom, w.rightRoom}) {
            if (r != 0) {
                inside.insert(r);
            }
        }
    }
    CHECK(inside.size() == 1);
}

// The tool only takes down its own kind of wall, and builds only where it's let.
void theWallToolKeepsToItsWalls() {
    auto g = emptyLot();
    std::vector<ts2::walllayer::Entry> layer;
    std::map<std::uint16_t, ts2::stringmap::Entry> patterns;
    const auto steps = sim::wallSteps(0, 1, 2, 4, 2);
    const auto built = sim::buildWalls(g, layer, patterns, steps, "blank", [](const sim::WallStep& s) { return s.x0 != 2; });
    CHECK(built.size() == 2);
    layer[0].type = 0x2F00DDEE;  // a fence there now
    CHECK(sim::removeWalls(g, layer, patterns, steps, nullptr).size() == 1);
    CHECK(layer.size() == 1 && layer[0].type == 0x2F00DDEE);
}

} // namespace

std::vector<Test> wallBuildTests() {
    return {
        {"walls run along tile edges", wallsRunAlongTileEdges},
        {"walls make and join rooms", wallsMakeAndJoinRooms},
        {"the wall tool keeps to its walls", theWallToolKeepsToItsWalls},
    };
}
