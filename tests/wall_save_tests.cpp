// Saving walls: walls built and taken down in the room graph made and removed alike in the lot's
// other wall graphs, with the wall layer, room layers and room grids.
#include "test_support.hpp"

#include "formats/dbpf_writer.hpp"
#include "formats/resource_types.hpp"
#include "sim/wall_build.hpp"
#include "sim/wall_save.hpp"

#include <set>
#include <sstream>

namespace {

namespace sim = ts2::sim;
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint32_t kGroup = 0xFFFFFFFF;

void u32(Bytes& b, std::uint32_t v) {
    for (int s = 0; s < 32; s += 8) b.push_back(static_cast<std::uint8_t>(v >> s));
}
Bytes header(std::uint32_t type, const std::string& cls) {
    Bytes b(64, 'w');
    u32(b, type);
    u32(b, 2);
    b.push_back(static_cast<std::uint8_t>(cls.size()));  // a byte, or a 7-bit length: the same under 128
    b.insert(b.end(), cls.begin(), cls.end());
    return b;
}
Bytes emptyLayer() {
    auto b = header(0x8A84D7B0, "cWallLayer");
    u32(b, 0);
    return b;
}
Bytes emptyPatterns() {
    auto b = header(0xCAC4FC40, "cStringMap");
    u32(b, 0);
    return b;
}
Bytes grid(const ts2::array3d::Array3D& g) {
    auto b = header(0x2A51171B, "c3DArray");
    u32(b, g.width), u32(b, g.height), u32(b, g.levels);
    b.insert(b.end(), g.elements.begin(), g.elements.end());
    return b;
}
ts2::array3d::Array3D roomGrid() {
    ts2::array3d::Array3D g;
    g.width = g.height = 6;
    g.levels = 1;
    g.elementSize = 8;
    g.elements.assign(6 * 6 * 8, 0);
    return g;
}

std::vector<sim::WallStep> line(int x0, int y0, int x1, int y1) { return sim::wallSteps(0, x0, y0, x1, y1); }

// A 6 x 6 lot with a 2 x 2 room of plain walls from (1, 1) to (3, 3), as the game would save it.
struct SavedLot {
    ts2::content::Lot lot;  // as loadLot gives it
    std::string file;
    std::unique_ptr<ts2::dbpf::Package> package;
};

SavedLot squareRoom() {
    SavedLot s;
    auto& lot = s.lot;
    ts2::wallgraph::WallGraph g;
    g.className = "cWallGraph";
    g.version = 2;
    g.width = g.height = 6;
    g.levels = 1;
    g.rooms = {0};
    std::vector<sim::WallStep> square;
    for (const auto& l : {line(1, 1, 3, 1), line(3, 1, 3, 3), line(3, 3, 1, 3), line(1, 3, 1, 1)}) square.insert(square.end(), l.begin(), l.end());
    sim::buildWalls(g, lot.wallCoverings, lot.wallPatterns, square, "blank", nullptr);
    sim::renumberRooms(g, 0);
    lot.roomGraph = g;
    auto rooms = roomGrid();
    sim::updateRoomGrid(rooms, g, 0);
    lot.grids[ts2::content::kRoomGridInstance] = rooms;

    auto plain = g;  // without rooms
    plain.rooms = {0};
    for (auto& w : plain.walls) w.leftRoom = w.rightRoom = 0;
    Bytes layer = ts2::walllayer::write(emptyLayer(), lot.wallCoverings);
    Bytes patterns = ts2::stringmap::write(emptyPatterns(), lot.wallPatterns);
    const Bytes name(64, 'g');
    std::ostringstream out(std::ios::binary);
    {
        ts2::dbpf::PackageWriter writer(out);
        auto add = [&](std::uint32_t type, std::uint32_t instance, const Bytes& b) { writer.add({type, kGroup, instance, 0}, b, false, 0); };
        add(ts2::types::kWallGraph, 5, ts2::wallgraph::write(g, name));
        add(ts2::types::kWallGraph, 24, ts2::wallgraph::write(plain, name));
        add(ts2::types::kWallGraph, 22, ts2::wallgraph::write(plain, name));
        add(ts2::types::kWallGraph, 6, ts2::wallgraph::write(g, name));
        add(ts2::types::kWallGraph, 7, ts2::wallgraph::write(g, name));
        add(ts2::types::kWallLayer, 4, layer);
        add(ts2::types::kPatternMap, 0xD, patterns);
        add(ts2::types::kArray3D, 9, grid(rooms));
        add(ts2::types::kArray3D, 0xA, grid(rooms));
        add(ts2::types::kArray3D, 0xB, grid(rooms));
        writer.finish(0);
    }
    s.file = out.str();
    s.package = std::make_unique<ts2::dbpf::Package>(std::span(reinterpret_cast<const std::uint8_t*>(s.file.data()), s.file.size()));
    return s;
}

const ts2::content::ReplacedResource* changed(const sim::WallChanges& c, std::uint32_t type, std::uint32_t instance) {
    for (const auto& r : c.resources)
        if (r.key.type == type && r.key.instance == instance) return &r;
    return nullptr;
}

std::set<std::int16_t> roomsIn(const Bytes& b) {
    const auto g = ts2::array3d::parse(b);
    std::set<std::int16_t> out;
    for (int x = 0; x < 6; ++x)
        for (int y = 0; y < 6; ++y)
            for (int q = 0; q < 4; ++q) out.insert(g.i16(0, x, y, static_cast<std::size_t>(q)));
    return out;
}

// Nothing changed, nothing written.
void unchangedWallsSaveNothing() {
    auto s = squareRoom();
    const auto c = sim::wallChanges(*s.package, s.lot);
    CHECK(c.problems.empty() && c.resources.empty() && c.built == 0 && c.removed == 0);
}

// A wall across the room: it joins every graph, the wall layer names it by the wall graph's id, and
// the room layers split the room in two as the room graph does.
void builtWallsJoinEveryGraph() {
    auto s = squareRoom();
    auto& lot = s.lot;
    sim::buildWalls(*lot.roomGraph, lot.wallCoverings, lot.wallPatterns, line(2, 1, 2, 3), "blank", nullptr);
    sim::renumberRooms(*lot.roomGraph, 0);
    sim::updateRoomGrid(lot.grids[ts2::content::kRoomGridInstance], *lot.roomGraph, 0);
    const auto c = sim::wallChanges(*s.package, lot);
    CHECK(c.problems.empty() && c.built == 2 && c.removed == 0);
    const auto* walls = changed(c, ts2::types::kWallGraph, 24);
    const auto* layer = changed(c, ts2::types::kWallLayer, 4);
    CHECK(walls != nullptr && layer != nullptr);
    if (walls != nullptr && layer != nullptr) {
        const auto g = ts2::wallgraph::parse(walls->bytes);
        const auto entries = ts2::walllayer::parse(layer->bytes);
        CHECK(g.walls.size() == 10 && entries.size() == 10 && g.points.size() == 9);  // one new point, mid-room
        std::set<std::uint32_t> ids;
        for (const auto& w : g.walls) ids.insert(static_cast<std::uint32_t>(w.id));
        bool named = true;
        for (const auto& e : entries) named = named && ids.contains(e.wall);
        CHECK(named && entries.back().type == sim::kPlainWallStyle && static_cast<std::uint32_t>(g.counters[1]) > *ids.rbegin());
        // The new walls keep the room graph's ids, as on lots the game saves.
        std::set<std::uint32_t> roomIds;
        for (const auto& w : lot.roomGraph->walls) roomIds.insert(static_cast<std::uint32_t>(w.id));
        CHECK(ids == roomIds && g.counters == lot.roomGraph->counters);
    }
    const auto* full = changed(c, ts2::types::kWallGraph, 22);
    CHECK(full != nullptr && ts2::wallgraph::parse(full->bytes).walls.size() == 10);
    for (const std::uint32_t graph : {6u, 7u}) {
        const auto* g = changed(c, ts2::types::kWallGraph, graph);
        CHECK(g != nullptr && ts2::wallgraph::parse(g->bytes).walls.size() == 10);
    }
    for (const std::uint32_t instance : {9u, 0xAu, 0xBu}) {
        const auto* g = changed(c, ts2::types::kArray3D, instance);
        CHECK(g != nullptr && roomsIn(g->bytes).size() == 3);  // outdoors and two rooms
    }
}

// Taking a wall of the room down takes it out of every graph, and the room opens to the outdoors.
void removedWallsLeaveEveryGraph() {
    auto s = squareRoom();
    auto& lot = s.lot;
    const auto gone = sim::removeWalls(*lot.roomGraph, lot.wallCoverings, lot.wallPatterns, line(1, 1, 3, 1), nullptr);
    CHECK(gone.size() == 2);
    sim::renumberRooms(*lot.roomGraph, 0);
    sim::updateRoomGrid(lot.grids[ts2::content::kRoomGridInstance], *lot.roomGraph, 0);
    const auto c = sim::wallChanges(*s.package, lot);
    CHECK(c.problems.empty() && c.built == 0 && c.removed == 2);
    for (const std::uint32_t graph : {24u, 22u, 6u, 7u}) {
        const auto* g = changed(c, ts2::types::kWallGraph, graph);
        CHECK(g != nullptr && ts2::wallgraph::parse(g->bytes).walls.size() == 6);
    }
    const auto* layer = changed(c, ts2::types::kWallLayer, 4);
    CHECK(layer != nullptr && ts2::walllayer::parse(layer->bytes).size() == 6);
    const auto* rooms = changed(c, ts2::types::kArray3D, 0xA);
    CHECK(rooms != nullptr && roomsIn(rooms->bytes) == std::set<std::int16_t>{0});
}

// A covering changed: only the wall layer and the pattern list are written.
void coveringsAloneChangeTheLayer() {
    auto s = squareRoom();
    auto& lot = s.lot;
    lot.wallPatterns[7] = {"blue", 1};
    lot.wallCoverings[0].left = 7;
    const auto c = sim::wallChanges(*s.package, lot);
    CHECK(c.problems.empty() && c.resources.size() == 2);
    const auto* layer = changed(c, ts2::types::kWallLayer, 4);
    CHECK(layer != nullptr && ts2::walllayer::parse(layer->bytes)[0].left == 7);
    CHECK(changed(c, ts2::types::kPatternMap, 0xD) != nullptr);
}

// Ids for new walls follow the graph's counters, as the game's do.
void newWallIdsFollowTheCounters() {
    ts2::wallgraph::WallGraph g;
    g.width = g.height = 6;
    g.levels = 1;
    g.counters = {40, 90};
    std::vector<ts2::walllayer::Entry> layer;
    std::map<std::uint16_t, ts2::stringmap::Entry> patterns;
    const auto built = sim::buildWalls(g, layer, patterns, line(1, 1, 2, 1), "blank", nullptr);
    CHECK(built.size() == 1 && built[0] == 90 && g.counters[1] == 91);
    CHECK(g.points.size() == 2 && g.points[0].id == 40 && g.points[1].id == 41 && g.counters[0] == 42);
}

} // namespace

std::vector<Test> wallSaveTests() {
    return {
        {"unchanged walls save nothing", unchangedWallsSaveNothing},
        {"built walls join every wall graph", builtWallsJoinEveryGraph},
        {"removed walls leave every wall graph", removedWallsLeaveEveryGraph},
        {"coverings alone change the wall layer", coveringsAloneChangeTheLayer},
        {"new wall ids follow the graph's counters", newWallIdsFollowTheCounters},
    };
}
