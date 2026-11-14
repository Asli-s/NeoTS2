// Saving a lot's first stage: floors, terrain paint and money written into the resources they were
// loaded from, the rest of each kept byte for byte.
#include "test_support.hpp"

#include "content/lot_save.hpp"
#include "content/terrain_paint.hpp"
#include "formats/dbpf_writer.hpp"
#include "formats/fami.hpp"
#include "formats/resource_types.hpp"
#include "formats/simi.hpp"

#include <sstream>

namespace {

using Bytes = std::vector<std::uint8_t>;
constexpr std::uint32_t kGroup = 0xFFFFFFFF;

void u8(Bytes& b, std::uint8_t v) { b.push_back(v); }
void u16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
void u32(Bytes& b, std::uint32_t v) {
    for (int s = 0; s < 32; s += 8) b.push_back(static_cast<std::uint8_t>(v >> s));
}
void text(Bytes& b, const std::string& s) {
    u8(b, static_cast<std::uint8_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}
Bytes header(std::uint32_t type, std::uint32_t version) {
    Bytes b(64, 'n');
    u32(b, type);
    u32(b, version);
    return b;
}

// A 2 x 2 tile, one level floor grid, a quarter tile's floor the pattern id.
Bytes floorGrid(std::int16_t pattern) {
    auto b = header(0x2A51171B, 1);
    text(b, "c3DArray");
    u32(b, 2), u32(b, 2), u32(b, 1);
    for (int cell = 0; cell < 4; ++cell)
        for (int q = 0; q < 4; ++q) u16(b, static_cast<std::uint16_t>(pattern));
    return b;
}
Bytes floorPatterns() {
    auto b = header(0xCAC4FC40, 2);
    text(b, "cStringMap");
    u32(b, 1);
    text(b, "tile");
    u16(b, 1);
    u32(b, 16);
    return b;
}
Bytes lotTexture() {
    auto b = header(0x4B58975B, 7);
    u32(b, 2), u32(b, 2);
    text(b, "lottexture-canvas");
    u32(b, 1);
    text(b, "grass");
    u32(b, 0xABCDEF01);  // something after the list, kept
    return b;
}
Bytes paintMap() {
    auto b = header(0x6B943B43, 1);
    text(b, "c2DArray");
    u32(b, 9), u32(b, 9);
    b.insert(b.end(), 81, 0x40);
    return b;
}
Bytes simulatorState() {
    auto b = Bytes(64, 's');
    u32(b, 0x53494D49), u32(b, 0xC0), u32(b, 0);
    for (std::size_t i = 0; i < ts2::simi::kGlobalCount; ++i) u16(b, static_cast<std::uint16_t>(i));
    u32(b, 0x12345678);
    return b;
}

struct TestLot {
    std::string file;
    std::unique_ptr<ts2::dbpf::Package> package;
    ts2::content::Lot lot;
};

TestLot testLot() {
    std::ostringstream out(std::ios::binary);
    {
        ts2::dbpf::PackageWriter writer(out);
        auto add = [&](std::uint32_t type, std::uint32_t instance, const Bytes& b) { writer.add({type, kGroup, instance, 0}, b, false, 0); };
        add(ts2::types::kArray3D, ts2::content::kFloorGridInstance, floorGrid(1));
        add(ts2::types::kPatternMap, ts2::content::kFloorPatternMapInstance, floorPatterns());
        add(ts2::types::kLotTexture, 0, lotTexture());
        add(ts2::types::kArray2D, ts2::content::kFirstPaintMapInstance, paintMap());
        add(ts2::simi::kType, 1, simulatorState());
        writer.finish(0);
    }
    TestLot t;
    t.file = out.str();
    t.package = std::make_unique<ts2::dbpf::Package>(std::span(reinterpret_cast<const std::uint8_t*>(t.file.data()), t.file.size()));
    t.lot.grids[ts2::content::kFloorGridInstance] = ts2::array3d::parse(floorGrid(1));
    t.lot.floorPatterns = ts2::stringmap::parse(floorPatterns());
    t.lot.terrainTextures = ts2::terrain::parseLotTexture(lotTexture());
    t.lot.flatGrids[ts2::content::kFirstPaintMapInstance] = ts2::terrain::parseArray2D(paintMap());
    return t;
}

const ts2::content::ReplacedResource* changed(const ts2::content::LotChanges& c, std::uint32_t type, std::uint32_t instance) {
    for (const auto& r : c.resources)
        if (r.key.type == type && r.key.instance == instance) return &r;
    return nullptr;
}

// A lot saved as it was loaded changes nothing.
void anUneditedLotChangesNothing() {
    auto t = testLot();
    const auto c = ts2::content::lotChanges(*t.package, t.lot, {true, false, true, 0});
    CHECK(c.resources.empty() && c.problems.empty());
}

// Floors laid: the grid's elements and the pattern map are written, the grid's header as saved.
void floorsAreSaved() {
    auto t = testLot();
    auto& grid = t.lot.grids[ts2::content::kFloorGridInstance];
    grid = ts2::array3d::parse(floorGrid(2));
    t.lot.floorPatterns[1].uses = 0;
    t.lot.floorPatterns[2] = {"carpet", 16};
    const auto c = ts2::content::lotChanges(*t.package, t.lot, {true, false, false, 0});
    CHECK(c.problems.empty() && c.resources.size() == 2);
    const auto* g = changed(c, ts2::types::kArray3D, ts2::content::kFloorGridInstance);
    CHECK(g != nullptr && g->bytes == floorGrid(2) && g->key.group == kGroup);
    const auto* p = changed(c, ts2::types::kPatternMap, ts2::content::kFloorPatternMapInstance);
    CHECK(p != nullptr);
    if (p != nullptr) {
        const auto patterns = ts2::stringmap::parse(p->bytes);
        CHECK(patterns.size() == 2 && patterns.at(2).name == "carpet" && patterns.at(2).uses == 16 && patterns.at(1).uses == 0);
    }
}

// A new paint: the terrain textures list it (what followed the list kept), and it gets a strength
// map of its own with the first map's header.
void terrainPaintIsSaved() {
    auto t = testLot();
    const auto dirt = ts2::content::terrainPaintIndex(t.lot, "dirt");
    CHECK(dirt == std::optional<std::size_t>(1));
    CHECK(ts2::content::paintTerrain(t.lot, dirt, 1.0f, 1.0f, 0.5f));
    const auto c = ts2::content::lotChanges(*t.package, t.lot, {false, false, true, 0});
    CHECK(c.problems.empty() && c.resources.size() == 3);
    const auto* textures = changed(c, ts2::types::kLotTexture, 0);
    CHECK(textures != nullptr);
    if (textures != nullptr) {
        const auto read = ts2::terrain::parseLotTexture(textures->bytes);
        CHECK(read.paints == std::vector<std::string>({"grass", "dirt"}) && read.canvas == "lottexture-canvas" && read.width == 2);
        const Bytes tail(textures->bytes.end() - 4, textures->bytes.end());
        CHECK(tail == Bytes({0x01, 0xEF, 0xCD, 0xAB}));
    }
    const auto* map = changed(c, ts2::types::kArray2D, ts2::content::kFirstPaintMapInstance + 1);
    CHECK(map != nullptr);
    if (map != nullptr) {
        const auto read = ts2::terrain::parseArray2D(map->bytes);
        CHECK(read.width == 9 && read.byte(4, 4) == 255 && read.byte(0, 0) == 0);
        CHECK(std::equal(map->bytes.begin(), map->bytes.begin() + 64, paintMap().begin()));
    }
    CHECK(changed(c, ts2::types::kArray2D, ts2::content::kFirstPaintMapInstance) != nullptr);  // grass rubbed under the dirt
}

// Money spent goes into the simulator's budget globals (here 14 * 10000 + 13 as saved), the rest of
// its state as saved.
void fundsAreSaved() {
    auto t = testLot();
    const auto c = ts2::content::lotChanges(*t.package, t.lot, {false, false, false, 511632 - 140013});
    CHECK(c.problems.empty() && c.resources.size() == 1);
    if (c.resources.size() == 1) {
        const auto state = ts2::simi::parse(c.resources[0].bytes);
        CHECK(c.funds == std::optional<std::int32_t>(511632));
        CHECK(state.globals.at(0x0D) == 1632 && state.globals.at(0x0E) == 51 && state.globals.at(0x0C) == 0x0C);
        CHECK(c.resources[0].bytes.size() == simulatorState().size());
    }
    // The floors asked for on a lot without them are a problem, not a throw.
    ts2::content::Lot bare;
    const auto d = ts2::content::lotChanges(*t.package, bare, {true, false, false, 0});
    CHECK(d.resources.empty() && d.problems.size() == 1);
}

Bytes family(std::uint32_t version, std::int32_t lot, std::int32_t funds) {
    Bytes b;
    u32(b, 0x46414D49), u32(b, version), u32(b, 0);
    u32(b, static_cast<std::uint32_t>(lot));
    if (version >= 0x50) u32(b, static_cast<std::uint32_t>(lot));
    if (version >= 0x55) u32(b, 0);
    u32(b, 4);  // value4
    u32(b, static_cast<std::uint32_t>(funds));
    if (version < 0x4D) u32(b, 0);
    u32(b, 6), u32(b, 0);  // value6, flags
    u32(b, 1), u32(b, 0xAAAA);  // one member
    if (version > 0x4A) u32(b, 7);
    if (version > 0x4E) u32(b, 8);
    if (version >= 0x51) u32(b, static_cast<std::uint32_t>(funds));
    u32(b, 0xEEEE);  // left after the last value
    return b;
}

// The family living on the lot has its money set; others are left alone.
void familyFundsAreSaved() {
    std::ostringstream out(std::ios::binary);
    {
        ts2::dbpf::PackageWriter writer(out);
        writer.add({ts2::types::kFamily, kGroup, 1, 0}, family(0x4E, 11, 20000), false, 0);
        writer.add({ts2::types::kFamily, kGroup, 2, 0}, family(0x55, 12, 300), false, 0);
        writer.add({ts2::types::kFamily, kGroup, 3, 0}, family(0x55, 13, 300), false, 0);
        writer.finish(0);
    }
    const std::string file = out.str();
    ts2::dbpf::Package hood(std::span(reinterpret_cast<const std::uint8_t*>(file.data()), file.size()));
    const auto old = ts2::content::familyFundsChanges(hood, 11, 511632);
    CHECK(old.size() == 1 && old[0].key.instance == 1);
    if (old.size() == 1) {
        const auto f = ts2::fami::parse(old[0].bytes);
        CHECK(f.funds == 511632 && f.lot == 11 && f.members.size() == 1 && f.extraBytes == 4);
    }
    const auto newer = ts2::content::familyFundsChanges(hood, 12, 999);
    CHECK(newer.size() == 1 && newer[0].key.instance == 2);
    if (newer.size() == 1) {
        const auto f = ts2::fami::parse(newer[0].bytes);
        CHECK(f.funds == 999 && f.funds2 == 999 && f.value8 == 8);
    }
    CHECK(ts2::content::familyFundsChanges(hood, 13, 300).empty());  // already so
}

} // namespace

std::vector<Test> lotSaveTests() {
    return {
        {"an unedited lot saves no changes", anUneditedLotChangesNothing},
        {"floors are saved", floorsAreSaved},
        {"terrain paint is saved", terrainPaintIsSaved},
        {"household funds are saved with the lot", fundsAreSaved},
        {"household funds are saved with the family", familyFundsAreSaved},
    };
}
