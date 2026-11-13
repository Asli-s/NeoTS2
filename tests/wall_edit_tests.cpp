// Build mode's wall covering tool: covering wall sides, one or a whole room's.
#include "test_support.hpp"

#include "content/wall_edit.hpp"

namespace {

using ts2::content::WallSide;

// A room (5) of three walls on level 0, a fourth segment that only divides it (no covering), and an
// outdoor wall upstairs.
ts2::wallgraph::WallGraph graph() {
    ts2::wallgraph::WallGraph g;
    g.points = {{1, 0, 0, 0}, {2, 4, 0, 0}, {3, 4, 4, 0}, {4, 0, 4, 0}, {5, 0, 0, 1}, {6, 4, 0, 1}};
    g.walls = {{10, 1, 5, 2, 0}, {11, 2, 5, 3, 0}, {12, 3, 0, 4, 5}, {13, 4, 5, 1, 5}, {14, 5, 0, 6, 7}};
    return g;
}

std::vector<ts2::walllayer::Entry> coverings() {
    return {{10, 1, 1, 2}, {11, 1, 1, 2}, {12, 1, 2, 1}, {14, 1, 2, 2}};
}

// A side takes the new covering and says what it had; the pattern list follows.
void coveringAWallSide() {
    auto layer = coverings();
    std::map<std::uint16_t, ts2::stringmap::Entry> patterns{{1, {"wallpaper_a", 3}}, {2, {"0000beef", 5}}};
    const auto before = ts2::content::coverWallSide(layer, patterns, WallSide{10, true}, "0000cafe");
    CHECK(before && *before == "wallpaper_a");
    CHECK(layer[0].left == 3 && layer[0].right == 2);
    CHECK(patterns[3].name == "0000cafe" && patterns[3].uses == 1 && patterns[1].uses == 2);
    // The same again changes nothing, nor does a segment without a covering entry.
    CHECK(!ts2::content::coverWallSide(layer, patterns, WallSide{10, true}, "0000CAFE"));
    CHECK(!ts2::content::coverWallSide(layer, patterns, WallSide{13, true}, "0000cafe"));
    CHECK(ts2::content::coverWallSide(layer, patterns, WallSide{10, false}, "0000cafe") == std::optional<std::string>("0000beef"));
    CHECK(layer[0].right == 3);
}

// A room's sides are those facing it on its level; the outdoors' every outdoor side on the level.
void aRoomsWallSides() {
    const auto g = graph();
    const auto layer = coverings();
    const auto any = [](std::int32_t) { return true; };
    const auto inside = ts2::content::roomWallSides(g, layer, WallSide{11, true}, any);
    CHECK((inside == std::vector<WallSide>{{10, true}, {11, true}, {12, false}}));
    const auto outside = ts2::content::roomWallSides(g, layer, WallSide{10, false}, any);
    CHECK((outside == std::vector<WallSide>{{10, false}, {11, false}, {12, true}}));
    const auto upstairs = ts2::content::roomWallSides(g, layer, WallSide{14, true}, any);
    CHECK((upstairs == std::vector<WallSide>{{14, true}}));
    // Walls that can't be covered are left out.
    const auto notEleven = ts2::content::roomWallSides(g, layer, WallSide{10, true}, [](std::int32_t w) { return w != 11; });
    CHECK((notEleven == std::vector<WallSide>{{10, true}, {12, false}}));
    CHECK(ts2::content::roomWallSides(g, layer, WallSide{99, true}, any).empty());
}

} // namespace

std::vector<Test> wallEditTests() {
    return {
        {"covering a wall side", coveringAWallSide},
        {"a room's wall sides", aRoomsWallSides},
    };
}
