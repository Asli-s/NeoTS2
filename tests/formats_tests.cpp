// File formats: packages, object data, lot and neighborhood resources.
#include "test_support.hpp"

#include "formats/cursor.hpp"
#include "formats/dbpf_writer.hpp"
#include "formats/effects.hpp"

#include <sstream>

namespace {

// A lot's floor pattern list (cStringMap): names by the id the floor grid stores.
void patternMapReadsNamesById() {
    std::vector<std::uint8_t> d(64, 0);
    auto u32 = [&d](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    auto text = [&d](const std::string& t) {
        d.push_back(static_cast<std::uint8_t>(t.size()));
        d.insert(d.end(), t.begin(), t.end());
    };
    u32(0xCAC4FC40);
    u32(1);
    text("cStringMap");
    u32(2);
    for (const auto& [name, id, uses] : {std::tuple{std::string("sidewalk"), 1, 36}, std::tuple{std::string("ccd66b6e"), 11, 12}}) {
        text(name);
        d.push_back(static_cast<std::uint8_t>(id));
        d.push_back(0);
        u32(static_cast<std::uint32_t>(uses));
    }
    const auto m = ts2::stringmap::parse(d);
    CHECK(m.size() == 2 && m.at(1).name == "sidewalk" && m.at(11).name == "ccd66b6e" && m.at(11).uses == 12);
}

// Pattern names longer than 127 bytes take a second length byte, as the game's 7-bit string length.
void patternMapReadsLongNames() {
    std::vector<std::uint8_t> d(64, 0);
    auto u32 = [&d](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    u32(0xCAC4FC40);
    u32(1);
    d.push_back(10);
    for (char c : std::string("cStringMap")) d.push_back(static_cast<std::uint8_t>(c));
    u32(1);
    const std::string name(130, 'x');
    d.push_back(0x82);
    d.push_back(0x01);
    d.insert(d.end(), name.begin(), name.end());
    d.push_back(3);
    d.push_back(0);
    u32(4);
    const auto m = ts2::stringmap::parse(d);
    CHECK(m.size() == 1 && m.at(3).name == name && m.at(3).uses == 4);
}

// A wall layer: each wall segment's covering on its left and right side.
void wallLayerReadsCoveringsPerSide() {
    std::vector<std::uint8_t> d(64, 0);
    auto u32 = [&d](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    u32(0x8A84D7B0);
    u32(1);
    const std::string cls = "cWallLayer";
    d.push_back(static_cast<std::uint8_t>(cls.size()));
    d.insert(d.end(), cls.begin(), cls.end());
    u32(1);
    u32(64);
    u32(23);
    u32(6 | 12u << 16);
    const auto w = ts2::walllayer::parse(d);
    CHECK(w.size() == 1 && w[0].wall == 64 && w[0].type == 23 && w[0].left == 6 && w[0].right == 12);
}

// A package's name map: each resource's group, instance and name.
void nameMapReadsNames() {
    std::vector<std::uint8_t> d;
    auto u32 = [&d](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    const std::string name = "doorvalue_0_0_0_n_wallmask_txmt";
    u32(1);
    u32(0x1C0532FA);
    u32(0xFF3AF167);
    u32(static_cast<std::uint32_t>(name.size()));
    d.insert(d.end(), name.begin(), name.end());
    const auto m = ts2::namemap::parse(d);
    CHECK(m.size() == 1 && m[0].group == 0x1C0532FA && m[0].instance == 0xFF3AF167 && m[0].name == name);
}

// A lot's fence posts: level, point and the fence's GUID.
void fencePostLayerReadsPosts() {
    std::vector<std::uint8_t> d(64, 0);
    auto u32 = [&d](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    u32(0xAB4BA572);
    u32(1);
    const std::string cls = "cFencePostLayer";
    d.push_back(static_cast<std::uint8_t>(cls.size()));
    d.insert(d.end(), cls.begin(), cls.end());
    u32(1);
    u32(1);
    u32(0x41500000);  // 13.0
    u32(0x41100000);  // 9.0
    u32(0xAD0DABD2);
    const auto posts = ts2::fencepostlayer::parse(d);
    CHECK(posts.size() == 1 && posts[0].level == 1 && posts[0].x == 13.0f && posts[0].y == 9.0f && posts[0].fence == 0xAD0DABD2);
}

// A lot's neighbor remap tables: only table 6 is kept, its last value for a repeated key wins.
void neighborRemapKeepsTableSix() {
    std::vector<std::uint8_t> d;
    auto u32 = [&d](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    u32(2);
    u32(1); u32(1); u32(2);          // table 1: one pair
    u32(0);                          // table 2
    u32(2); u32(5); u32(6);          // table 3: two ids
    u32(0);                          // table 4
    u32(1); u32(1); u32(0x724CD298); // table 5
    u32(3); u32(1); u32(7); u32(0x38); u32(0x151); u32(1); u32(9);  // table 6
    u32(0);                          // table 7
    const auto remap = ts2::neighbor_remap::parse(d);
    CHECK(remap.version == 2 && remap.neighborIds.size() == 2);
    CHECK(remap.neighborIds.at(1) == 9 && remap.neighborIds.at(0x38) == 0x151);

    const std::vector<std::uint8_t> versionOne{1, 0, 0, 0, 0, 0, 0, 0};
    CHECK(ts2::neighbor_remap::parse(versionOne).neighborIds.empty());
    d.resize(d.size() - 8);
    bool refused = false;
    try {
        ts2::neighbor_remap::parse(d);
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);
}

// A saved object's posed joints and playing animations: the cObject joints, then the cAnimatable
// entries after the cObject data. An empty entry is skipped, cut-off data is refused.
void savedObjectReadsPlayingAnimations() {
    std::vector<std::uint8_t> d(64, 0);
    auto u32 = [&d](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    auto f32 = [&](float v) { u32(std::bit_cast<std::uint32_t>(v)); };
    auto floats = [&](int n) {
        for (int i = 0; i < n; ++i) f32(0);
    };
    auto str = [&d](const std::string& s) {
        d.push_back(static_cast<std::uint8_t>(s.size()));
        d.insert(d.end(), s.begin(), s.end());
    };
    u32(0x7D4F2BB8), u32(16), str("cAnimatable");
    u32(0xFA1C39F7), u32(17), str("cObject");
    str(""), u32(0), floats(7);                               // model, mesh groups, transform
    u32(1), str("slot_chairdining_trans");                    // a joint, slid 0.373 along y
    f32(0), f32(0.373f), f32(0), f32(0), f32(0), f32(0), f32(1);
    u32(0);                                                   // blends
    f32(1), u32(0), u32(1166), u32(4), u32(0), u32(0), f32(0), u32(0), u32(0), floats(7);
    u32(2);                                                   // entries
    u32(0);
    u32(1);
    u32(0x09D0CE69), u32(11), u32(0x60000000), str("a-idle"), str("ovl"), u32(6), u32(4);
    d.push_back(43), d.push_back(0), d.push_back(0x00), d.push_back(0x20);  // event object 43, tree 0x2000
    u32(0), f32(1), f32(-1), f32(-1), u32(1), u32(1), u32(0x7FFFFF00), u32(0), floats(7), u32(0xFFFFFFFF), u32(1);
    u32(0x584BA0BB), u32(0), u32(0), str(""), u32(0), u32(0), u32(0), u32(0);
    u32(0xC9DE69B6), u32(1), u32(0x3AFF30DB), u32(7), u32(1), u32(0), u32(100), u32(0x7FFFFF00), f32(0.1f), f32(1);
    u32(5), u32(1), f32(0.5f), u32(1);                        // layer 1, weight 0.5, blend curve:
    u32(0x8B969D64), u32(1), u32(1), f32(0), u32(3);
    for (const float ms : {200.0f, 3000.0f, 200.0f}) {
        u32(0), f32(0), f32(1), f32(ms);
    }
    u32(0), u32(1), f32(1), f32(0), floats(24), u32(454), u32(1), u32(0), u32(0x7FFFFF00), f32(0), u32(1), f32(354);
    str("a-idle_anim"), u32(1);

    const auto saved = ts2::cobject::parse(d);
    CHECK(saved.classes == std::vector<std::string>({"cAnimatable", "cObject"}));
    CHECK(saved.joints.size() == 1 && saved.joints[0].name == "slot_chairdining_trans" && saved.joints[0].translation[1] == 0.373f &&
          saved.joints[0].rotation[3] == 1.0f);
    CHECK(saved.animations.size() == 1);
    const auto& a = saved.animations.front();
    CHECK(a.name == "a-idle" && a.type == "ovl" && a.category == 4 && a.eventObject == 43 && a.eventTree == 0x2000 &&
          a.eventObjectGuid == 0x584BA0BB);
    CHECK(a.state == 1 && a.layer == 1 && a.plays == 1 && a.weight == 0.5f && a.lengthMs == 3400.0f && a.startedMsAgo == 354);
    d.pop_back();
    bool refused = false;
    try {
        ts2::cobject::parse(d);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    CHECK(refused);
}

// A lot's roofs: version 1 entries, version 2 adding each roof's slope, version 3 one more field.
void roofsReadEveryVersion() {
    for (const std::uint32_t version : {1u, 2u, 3u}) {
        std::vector<std::uint8_t> d(64, 0);
        auto u32 = [&d](std::uint32_t v) {
            for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        };
        u32(0xAB9406AA);
        u32(version);
        u32(1);
        for (const std::uint32_t v : {0x42u, 0x41C80000u, 0x40A00000u, 2u, 0x41B80000u, 0x41700000u, 2u, 2u, 0x8CDCBEF5u}) {
            u32(v);
        }
        if (version >= 2) {
            u32(0x3F000000);  // 0.5: 45 degrees
        }
        if (version == 3) {
            u32(4);
        }
        const auto roofs = ts2::roofs::parse(d);
        CHECK(roofs.size() == 1 && roofs[0].id == 0x42 && roofs[0].x0 == 25.0f && roofs[0].y1 == 15.0f &&
              roofs[0].level1 == 2 && roofs[0].type == 2 && roofs[0].pattern == 0x8CDCBEF5);
        CHECK(roofs[0].slope == (version >= 2 ? 0.5f : 0.0f) && roofs[0].extra == (version == 3 ? 4u : 0u));
    }
}

// A lot's terrain textures (canvas, then the paints) and a c2DArray of paint strengths, row by row.
void terrainResourcesParse() {
    std::vector<std::uint8_t> t(64, 0);
    auto u32 = [](std::vector<std::uint8_t>& d, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    auto text = [](std::vector<std::uint8_t>& d, std::string_view s) {
        d.push_back(static_cast<std::uint8_t>(s.size()));
        d.insert(d.end(), s.begin(), s.end());
    };
    u32(t, 0x4B58975B);
    u32(t, 7);
    u32(t, 40);
    u32(t, 30);
    text(t, "lottexture-test-01");
    u32(t, 2);
    text(t, "terrain-cliff");
    text(t, "terrain-grass-a-paint");
    const auto lot = ts2::terrain::parseLotTexture(t);
    CHECK(lot.width == 40 && lot.height == 30 && lot.canvas == "lottexture-test-01");
    CHECK((lot.paints == std::vector<std::string>{"terrain-cliff", "terrain-grass-a-paint"}));

    std::vector<std::uint8_t> a(64, 0);
    u32(a, 0x6B943B43);
    u32(a, 1);
    text(a, "c2DArray");
    u32(a, 3);
    u32(a, 2);
    for (std::uint8_t v = 0; v < 6; ++v) a.push_back(v);
    const auto grid = ts2::terrain::parseArray2D(a);
    CHECK(grid.width == 3 && grid.height == 2 && grid.elementSize == 1);
    CHECK(grid.byte(2, 0) == 2 && grid.byte(0, 1) == 3 && grid.byte(3, 0) == 0);
    a.pop_back();
    bool threw = false;
    try {
        ts2::terrain::parseArray2D(a);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

// Lot grids saved with a variable size per tile (instances 0x5CEE and 0x0C), and empty grids.
void lotGridsWithVariableTilesParse() {
    auto u32 = [](std::vector<std::uint8_t>& d, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    auto header = [&](std::uint32_t type, std::string_view cls, std::initializer_list<std::uint32_t> sides) {
        std::vector<std::uint8_t> d(64, 0);
        u32(d, type);
        u32(d, 1);
        d.push_back(static_cast<std::uint8_t>(cls.size()));
        d.insert(d.end(), cls.begin(), cls.end());
        for (const auto side : sides) u32(d, side);
        return d;
    };
    auto throws = [](auto f) {
        try {
            f();
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };

    auto bytes = header(0x6B943B43, "c2DArray", {2, 1});
    for (const std::uint8_t b : std::initializer_list<std::uint8_t>{2, 7, 9, 0}) bytes.push_back(b);
    const auto flat = ts2::terrain::parseByteLists(bytes);
    CHECK(flat.elementSize == 3 && (flat.elements == std::vector<std::uint8_t>{7, 9, 0xFF, 0xFF, 0xFF, 0xFF}));
    auto four = header(0x6B943B43, "c2DArray", {1, 1});
    for (const std::uint8_t b : std::initializer_list<std::uint8_t>{4, 1, 2, 3, 4}) four.push_back(b);
    CHECK(throws([&] { ts2::terrain::parseByteLists(four); }));

    auto lists = header(0x2A51171B, "c3DArray", {2, 1, 1});
    u32(lists, 2);
    u32(lists, 658);
    u32(lists, 660);
    u32(lists, 0);
    const auto grid = ts2::array3d::parseLists(lists);
    CHECK(grid.cells.size() == 2 && (grid.cells[0] == std::vector<std::uint32_t>{658, 660}) && grid.cells[1].empty());
    lists.pop_back();
    CHECK(throws([&] { ts2::array3d::parseLists(lists); }));

    const auto empty = ts2::array3d::parse(header(0x2A51171B, "c3DArray", {20, 30, 0}));
    CHECK(empty.levels == 0 && empty.elements.empty() && empty.at(0, 0, 0).empty());
    auto filledEmpty = header(0x2A51171B, "c3DArray", {20, 30, 0});
    filledEmpty.push_back(1);
    CHECK(throws([&] { ts2::array3d::parse(filledEmpty); }));
}

// ObjM instance 2: object id lists under a number. Version 1 ends with a list of u32s.
void objectGroupsParse() {
    for (const std::uint32_t version : {2u, 1u}) {
        std::vector<std::uint8_t> d(64, 0);
        auto u32 = [&d](std::uint32_t v) {
            for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        };
        auto u16 = [&d](std::uint16_t v) {
            d.push_back(static_cast<std::uint8_t>(v));
            d.push_back(static_cast<std::uint8_t>(v >> 8));
        };
        u32(version);
        u32(2);
        u32(0);
        u32(3);
        u16(0x8A);
        u16(0x8B);
        u16(0x89);
        u32(5);
        u32(0);
        if (version == 1) {
            u32(1);
            u32(7);
        }
        const auto groups = ts2::objm::parseGroups(d);
        CHECK(groups.version == version && groups.objectsByKey.size() == 2);
        CHECK((groups.objectsByKey.at(0) == std::vector<std::uint16_t>{0x8A, 0x8B, 0x89}) && groups.objectsByKey.at(5).empty());
        d.resize(d.size() - 2);
        bool threw = false;
        try {
            ts2::objm::parseGroups(d);
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }
}

// Object functions: the "fJBO" word isn't checked, as in the game. Files that leave it 0 (some
// with the word later, all zeros before it) read as no functions.
void objectFunctionsIgnoreTheMagic() {
    std::vector<std::uint8_t> d(64 + 8, 0);
    for (const std::uint8_t b : std::initializer_list<std::uint8_t>{'f', 'J', 'B', 'O', 1, 0, 0, 0, 0x34, 0x12, 0x78, 0x56}) {
        d.push_back(b);
    }
    const auto functions = ts2::objf::parse(d);
    CHECK(functions.size() == 1 && functions[0].check == 0x1234 && functions[0].action == 0x5678);
    std::vector<std::uint8_t> empty(144, 0);
    empty[136] = 'f';
    empty[137] = 'J';
    empty[138] = 'B';
    empty[139] = 'O';
    CHECK(ts2::objf::parse(empty).empty());
}

namespace slot_test {
// A SLOT resource of `version` holding `entries`, each 78 bytes.
std::vector<std::uint8_t> resource(std::uint32_t version, const std::vector<std::vector<std::uint8_t>>& entries) {
    std::vector<std::uint8_t> d(64, 0);
    for (const std::uint32_t v : {0x534C4F54u, version, 0u, static_cast<std::uint32_t>(entries.size())}) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    for (const auto& e : entries) d.insert(d.end(), e.begin(), e.end());
    return d;
}

void put(std::vector<std::uint8_t>& entry, std::size_t offset, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) entry[offset + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

void putFloat(std::vector<std::uint8_t>& entry, std::size_t offset, float v) { put(entry, offset, std::bit_cast<std::uint32_t>(v)); }
}

// A version-0x47 SLOT resource: one container slot (type 0, bone 5) and two routing slots (type 3).
void slotResourceCountsContainerSlots() {
    std::vector<std::vector<std::uint8_t>> entries;
    for (const std::uint8_t type : {std::uint8_t{3}, std::uint8_t{0}, std::uint8_t{3}}) {
        std::vector<std::uint8_t> entry(78, 0);
        entry[0] = type;
        slot_test::put(entry, 70, type == 0 ? 5 : 1);
        entries.push_back(entry);
    }
    const auto s = ts2::slot::parse(slot_test::resource(0x47, entries));
    CHECK(s.version == 0x47 && s.entries.size() == 3 && s.containerCount() == 1);
    CHECK(s.containerBone(0) == 5 && s.containerBone(1) == -1);
}

// Every field of a 78-byte routing entry. Before version 0x47 x, y and the step are in sixteenths,
// and before 0x43 the distances too.
void slotEntryReadsEveryField() {
    std::vector<std::uint8_t> entry(78, 0);
    entry[0] = 3;
    slot_test::putFloat(entry, 2, -0.5f);
    slot_test::putFloat(entry, 6, 0.75f);
    slot_test::putFloat(entry, 10, 2.5f);
    slot_test::put(entry, 14, 1);
    slot_test::put(entry, 18, 10);
    slot_test::put(entry, 22, 1000);
    slot_test::put(entry, 26, 0x155);
    slot_test::put(entry, 30, 0xFFFFFFFF);
    slot_test::putFloat(entry, 34, 0.5f);
    slot_test::putFloat(entry, 38, 4.0f);
    slot_test::putFloat(entry, 42, 2.0f);
    slot_test::put(entry, 46, 100);
    slot_test::put(entry, 50, 0x20000);
    slot_test::putFloat(entry, 54, 0.35f);
    slot_test::put(entry, 58, 7);
    slot_test::put(entry, 62, static_cast<std::uint32_t>(-3));
    slot_test::putFloat(entry, 66, 0.0625f);
    slot_test::put(entry, 70, 2);
    slot_test::put(entry, 74, 9);
    const auto e = ts2::slot::parse(slot_test::resource(0x47, {entry})).entries.at(0);
    CHECK(e.type == ts2::slot::kRoutingSlot && e.x == -0.5f && e.y == 0.75f && e.z == 2.5f);
    CHECK(e.routingInts[0] == 1 && e.routingInts[1] == 10 && e.routingInts[2] == 1000);
    CHECK(e.routingFlags == 0x155 && e.allowsFacing(0) && !e.allowsFacing(1) && e.allowsFacing(6) && !e.allowsFacing(8));
    CHECK(e.routingInt == -1 && e.minDistance == 0.5f && e.maxDistance == 4.0f && e.optimalDistance == 2.0f);
    CHECK(e.containerValue == 100 && e.objectFlags == 0x20000 && e.routingFloat == 0.35f && e.containerKind == 7);
    CHECK(e.facing == -3 && e.step == 0.0625f && e.bone == 2 && e.id == 9);

    slot_test::putFloat(entry, 2, 48.0f);
    slot_test::put(entry, 66, 9);
    const auto old = ts2::slot::parse(slot_test::resource(0x46, {entry})).entries.at(0);
    CHECK(old.x == 3.0f && old.step == 0.5625f && old.maxDistance == 4.0f);
    slot_test::put(entry, 38, 32);
    CHECK(ts2::slot::parse(slot_test::resource(0x42, {entry})).entries.at(0).maxDistance == 2.0f);

    slot_test::put(entry, 6, 0x7FC00000);  // NaN
    bool threw = false;
    try {
        ts2::slot::parse(slot_test::resource(0x47, {entry}));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

// An interaction table in the newest form (version 0x55): one interaction with its action and
// test trees, settings, one motive group and the table's name at the end.
void interactionTableParse() {
    std::vector<std::uint8_t> d(64, 0);
    const std::string name = "untitled tree table";
    std::copy(name.begin(), name.end(), d.begin());
    auto u16 = [&d](std::uint16_t v) { d.push_back(static_cast<std::uint8_t>(v)); d.push_back(static_cast<std::uint8_t>(v >> 8)); };
    auto u32 = [&d](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    u16(0xFFFF);
    u16(0xFFFF);
    u32(0x55);
    u32(0);
    u16(1);
    u16(0x2007);
    u16(0x2008);
    u32(0xC45);
    u32(0);
    u32(3);
    u32(0);
    u32(50);
    u32(0xFFFFFFFF);
    u16(0);
    u32(2);
    u32(0x3F000000);
    u32(0);
    u32(1);
    u32(1);  // one motive group
    u32(1);  // with one effect: type, minimum, range
    u16(3);
    u16(10);
    u16(0xFFFB);
    u32(0);  // no further motive lists
    u32(static_cast<std::uint32_t>(name.size()));
    d.insert(d.end(), name.begin(), name.end());
    const auto t = ts2::ttab::parse(d);
    CHECK(t.name == name && t.version == 0x55 && t.interactions.size() == 1 && t.tableName == name);
    const auto& in = t.interactions[0];
    CHECK(in.action == 0x2007 && in.test == 0x2008 && in.flags == 0xC45 && in.autonomy == 50 &&
          in.joiningIndex == -1 && in.value3 == 0.5f && in.iconIndex == 1);
    CHECK(in.motives.size() == 1 && in.motives[0].size() == 1 && in.motives[0][0].min == 10 &&
          in.motives[0][0].delta == -5 && in.motives[0][0].type == 3);
    CHECK(t.extraBytes == 0);
}

// Wants and fears, version 6 with version-10 wants: an extra want, the two lists with their
// maximums, the closing value and a one-group history. Bytes after the history are counted.
void wantsAndFearsParse() {
    std::vector<std::uint8_t> d;
    auto u8 = [&d](std::uint8_t v) { d.push_back(v); };
    auto u16 = [&d](std::uint16_t v) { d.push_back(static_cast<std::uint8_t>(v)); d.push_back(static_cast<std::uint8_t>(v >> 8)); };
    auto u32 = [&d](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    auto want = [&](std::uint32_t guid, std::uint8_t type, std::int32_t points) {
        u32(10);
        u16(27);
        u32(guid);
        u8(type);
        if (type == 1) {
            u16(15);
            u16(0);
        } else if (type == 5) {
            u32(0x12345678);
            u16(8);
        } else {
            u16(0);
        }
        u32(0);
        u32(static_cast<std::uint32_t>(points));
        u32(7);
        u8(6);
    };
    u32(6);
    u32(1);
    want(0xAEAAAAAA, 5, 10000);
    u32(4);
    u32(2);
    want(0xADA6814F, 0, 3000);
    want(0x8D9ACDCE, 1, 3500);
    u32(3);
    u32(1);
    want(0xED9ACDEC, 1, -5000);
    u32(0);
    u32(2);
    u32(1234567);
    u32(1);
    u32(0xADA6814F);
    u32(1);
    want(0xADA6814F, 0, 3000);
    u8(0xEE);
    u8(0xEE);
    const auto s = ts2::swaf::parse(d);
    CHECK(s.version == 6 && s.extra && s.extra->type == 5 && s.extra->argument == 0x12345678 && s.extra->value == 8);
    CHECK(s.maxWants == 4 && s.wants.size() == 2 && s.wants[1].argument == 15 && s.wants[1].tail.size() == 3);
    CHECK(s.wants[0].guid == 0xADA6814F && s.wants[0].id == 27 && s.wants[0].tail[1] == 3000 && s.wants[0].flags == 6);
    CHECK(s.maxFears == 3 && s.fears.size() == 1 && s.fears[0].tail[1] == -5000);
    CHECK(s.historyVersion == 2 && s.historyValue == 1234567 && s.history.at(0xADA6814F).size() == 1);
    CHECK(s.extraBytes == 2);
    const auto slots = ts2::swaf::slots(s);
    CHECK(slots.wants == 4 && slots.fears == 3 && slots.holds == 1);
}

// Old records have the game's 4 want and 3 fear slots and one hold. Newer ones have their saved slots and
// one hold more than the saved extra holds.
void wantSlotsFollowTheVersion() {
    ts2::swaf::WantsAndFears old;
    old.version = 1;
    old.lastValue = 5;
    const auto a = ts2::swaf::slots(old);
    CHECK(a.wants == 4 && a.fears == 3 && a.holds == 1);
    ts2::swaf::WantsAndFears graduate;
    graduate.version = 6;
    graduate.maxWants = 6;
    graduate.maxFears = 4;
    graduate.lastValue = 1;
    const auto b = ts2::swaf::slots(graduate);
    CHECK(b.wants == 6 && b.fears == 4 && b.holds == 2);
    graduate.maxWants = 0xFFFFFFFF;
    CHECK(ts2::swaf::slots(graduate).wants == 64);
}

// Scripts and the queue name an interaction by its string index, not its line: the first line with it.
void interactionsAreFoundByStringIndex() {
    ts2::ttab::InteractionTable table;
    table.interactions.resize(3);
    table.interactions[0].stringIndex = 4;
    table.interactions[1].stringIndex = 24;
    table.interactions[1].action = 0x1001;
    table.interactions[2].stringIndex = 24;
    CHECK(ts2::ttab::find(table, 24) == &table.interactions[1]);
    CHECK(ts2::ttab::find(table, 1) == nullptr);
}

// A version-0x55 family record with two members, read to its last byte.
void familyRecordIsReadByVersion() {
    const std::uint32_t words[] = {0x46414D49, 0x55, 0, 23, 23, 0, 6, 20000, 2, 9, 2, 0x0A000101, 0x0A000102,
                                   0xE0000301, 1, 20000};
    std::vector<std::uint8_t> d;
    for (const std::uint32_t w : words) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(w >> (8 * i)));
    }
    const auto f = ts2::fami::parse(d);
    CHECK(f.lot == 23 && f.currentLot == 23 && f.vacationLot == 0 && f.funds == 20000 && f.flags == 9 && f.funds2 == 20000);
    CHECK(f.members.size() == 2 && f.members[1] == 0x0A000102 && f.id7 == 0xE0000301 && f.extraBytes == 0);
}

// One Sim's entry in a family ties record: father 3, mother 5, children 20 and 21.
void familyTiesAreReadPerSim() {
    std::vector<std::uint8_t> d;
    auto u32 = [&d](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    auto u16 = [&d](std::uint16_t v) {
        d.push_back(static_cast<std::uint8_t>(v));
        d.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    u32(1);
    u32(1);
    u16(12);
    u32(1);
    u32(4);
    const std::pair<std::uint32_t, std::uint16_t> ties[] = {{0u, std::uint16_t{3}}, {1u, std::uint16_t{5}}, {4u, std::uint16_t{20}}, {4u, std::uint16_t{21}}};
    for (const auto& [type, other] : ties) {
        u32(type);
        u16(other);
    }
    const auto f = ts2::famt::parse(d);
    CHECK(f.version == 1 && f.sims.size() == 1 && f.extraBytes == 0);
    CHECK(f.sims[0].sim == 12 && f.sims[0].ties.size() == 4);
    CHECK(f.sims[0].ties[1].type == static_cast<std::uint32_t>(ts2::famt::TieType::Mother) &&
          f.sims[0].ties[1].other == 5);
}

// A Sim description stores as many person-data values as its person-data version allows.
void simDescriptionReadsPersonDataByVersion() {
    std::vector<std::uint8_t> d;
    auto u32 = [&d](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    u32(0x70);
    u32(0x20);
    u32(0x20);
    for (int i = 0; i < 170; ++i) {
        d.push_back(static_cast<std::uint8_t>(i));
        d.push_back(0);
    }
    d.push_back(14);
    d.push_back(0);
    u32(0x0A000101);
    d.push_back(0xAB);
    const auto s = ts2::sdsc::parse(d);
    CHECK(s.personData.size() == 170 && s.personData[169] == 169);
    CHECK(s.neighborId == 14 && s.guid == 0x0A000101 && s.rest.size() == 1);
    CHECK(ts2::sdsc::personDataCount(0x36) == 231 && ts2::sdsc::personDataCount(0x33) == 228 &&
          ts2::sdsc::personDataCount(0x2C) == 200 && ts2::sdsc::personDataCount(0x2F) == 204);
}

// A constant table's count takes 15 bits (tables of more than 255 constants exist). The top bit is a flag.
void constantTablesCountInFifteenBits() {
    std::vector<std::uint8_t> data(64, 0);
    const std::uint16_t count = 300;
    data.push_back(count & 0xFF);
    data.push_back(static_cast<std::uint8_t>((count >> 8) | 0x80));
    for (std::uint16_t i = 0; i < count; ++i) {
        data.push_back(static_cast<std::uint8_t>(i));
        data.push_back(0);
    }
    const auto c = ts2::bcon::parse(data);
    CHECK(c.values.size() == count && c.values[299] == 299 - 256 && c.flags == 0x80);
}

// A tuning list (FCNS): named floats, each with a byte after it. Autonomy reads its values by name
// and keeps its own for names the list lacks.
void tuningListsReadNamedValues() {
    std::vector<std::uint8_t> d(64, 0);
    auto u32 = [&](std::uint32_t v) {
        for (int b = 0; b < 4; ++b) d.push_back(static_cast<std::uint8_t>(v >> (8 * b)));
    };
    auto value = [&](std::string_view name, float v) {
        d.push_back(static_cast<std::uint8_t>(name.size()));
        d.insert(d.end(), name.begin(), name.end());
        u32(std::bit_cast<std::uint32_t>(v));
        d.push_back(0);
    };
    d.insert(d.end(), {'S', 'N', 'C', 'F'});
    u32(0x4F);
    u32(0);
    u32(3);
    value("random selection count", 1.0f);
    value("moderate attenuation", 0.3f);
    value("visitor high attenuation", 0.03f);
    const auto t = ts2::fcns::parse(d);
    CHECK(t.version == 0x4F && t.values.size() == 3 && t.find("moderate attenuation") == 0.3f && !t.find("nope"));
    const auto tunings = ts2::sim::AutonomyTunings::from(&t);
    CHECK(tunings.randomSelection == 1 && tunings.worstMotives == 3 && tunings.attenuation[1] == 0.3f);
    ts2::ttab::Interaction line;
    line.attenuationCode = 3;
    CHECK(ts2::sim::attenuationOf(line, tunings, false) == 0.3f);
    line.attenuationCode = 4;
    CHECK(ts2::sim::attenuationOf(line, tunings, true) == 0.03f && ts2::sim::attenuationOf(line, tunings, false) == 0.1f);
    line.attenuationCode = 0;
    line.attenuation = 0.5f;
    CHECK(ts2::sim::attenuationOf(line, tunings, false) == 0.5f);
    d.resize(d.size() - 3);
    bool threw = false;
    try {
        ts2::fcns::parse(d);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

// A neighborhood's weather record: version 3 names itself, then its values in order.
void weatherRecordReadsItsValues() {
    std::vector<std::uint8_t> d = {3, 0, 0, 0, 2, 0, 0, 0, 'h', 'i'};
    for (std::int32_t v = 1; v <= 11; ++v) {
        for (int b = 0; b < 4; ++b) d.push_back(static_cast<std::uint8_t>(v >> (8 * b)));
    }
    const auto w = ts2::weather::parse(d);
    CHECK(w.values[ts2::weather::Weather::kSeason] == 1 && w.values[ts2::weather::Weather::kSeasonDaysStandard] == 3 &&
          w.values[ts2::weather::Weather::kPondFrozenState] == 11);
    CHECK(ts2::weather::parse(std::vector<std::uint8_t>{0, 0, 0, 0}).values[ts2::weather::Weather::kSeason] == 1);
    bool threw = false;
    try {
        ts2::weather::parse(std::vector<std::uint8_t>{3, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF});
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

// A neighborhood's identity record: version 10 as Strangetown's university saves it, and version 7,
// which takes the rest from the type and gives the default seasons.
void neighborhoodIdIsRead() {
    const std::vector<std::uint8_t> university = {
        0x0a, 0, 0, 0, 4, 0, 0, 0, 'N', '0', '0', '2', 0x1b, 0, 0, 0, 2, 0, 0, 0, 4, 0, 0, 0, 'U', '0', '0', '1',
        0, 0, 0, 0, 0x11, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 3, 0, 2, 3};
    const auto n = ts2::nhoodid::parse(university);
    CHECK(n.prefix == "N002" && n.id == 0x1B && n.type == 2 && n.subPrefix == "U001" && n.requiredProducts == 0x11);
    CHECK(n.value2 == 1 && n.flags == 0 && (n.seasons == std::array<std::int32_t, 4>{3, 0, 2, 3}));
    const std::vector<std::uint8_t> older = {7, 0, 0, 0, 1, 0, 0, 0, 'D', 9, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0,
                                             5, 0, 0, 0, 0, 0, 0, 0};
    const auto o = ts2::nhoodid::parse(older);
    CHECK(o.id == 9 && o.type == 3 && o.value2 == 2 && (o.seasons == std::array<std::int32_t, 4>{1, 2, 3, 0}));
    bool threw = false;
    try {
        ts2::nhoodid::parse(std::vector<std::uint8_t>{0x0a, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0x7F});
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

// Instructions saved with an older node version get their operands rewritten as the game does on
// load. Manage Inventory repacks byte 0 and gives singular operations byte 9 = 0x0C. Add/Change
// the Action String moves byte 4 to bytes 14-15. A tree call keeps only bit 0 of byte 12.
void oldOperandsAreUpgraded() {
    Instruction counted;
    counted.opcode = 0x0033;
    counted.operands[0] = 0x0F;
    counted.operands[9] = 0x07;
    ts2::bhav::upgradeOperands(counted);
    CHECK(counted.operands[0] == 0x1B && counted.operands[9] == 0x07 && counted.nodeVersion == 2);

    Instruction singular;
    singular.opcode = 0x0033;
    singular.operands[0] = 0x03;
    ts2::bhav::upgradeOperands(singular);
    CHECK(singular.operands[0] == 0x03 && singular.operands[9] == 0x0C);

    Instruction current = singular;
    current.operands[9] = 0x01;
    ts2::bhav::upgradeOperands(current);  // already at version 2: left alone
    CHECK(current.operands[9] == 0x01);

    Instruction action;
    action.opcode = 0x0032;
    action.operands[3] = 0x31;
    action.operands[4] = 0x07;
    ts2::bhav::upgradeOperands(action);
    CHECK(action.operands[3] == 0x00 && action.operands[4] == 0 && action.operands[14] == 0x07 &&
          action.nodeVersion == 4);

    // The car portal's call to Idle: 0xFE left in byte 12 doesn't make it pass the caller's params.
    Instruction leftover;
    leftover.opcode = 0x0118;
    leftover.operands[0] = 0x08;
    leftover.operands[1] = 0x07;
    leftover.operands[12] = 0xFE;
    ts2::bhav::upgradeOperands(leftover);
    CHECK(leftover.operands[12] == 0x00 && leftover.operands[0] == 0x08 && leftover.nodeVersion == 1);
    Instruction pairs;
    pairs.opcode = 0x1001;
    pairs.operands[12] = 0x37;
    ts2::bhav::upgradeOperands(pairs);
    CHECK(pairs.operands[12] == 0x01);
    Instruction newer = leftover;
    newer.operands[12] = 0xFE;
    ts2::bhav::upgradeOperands(newer);  // already at version 1: left alone
    CHECK(newer.operands[12] == 0xFE);
}

void array3dRefusesElementIndexesPastTheElement() {
    ts2::array3d::Array3D grid;
    grid.width = 1;
    grid.height = 1;
    grid.levels = 1;
    grid.elementSize = 4;
    grid.elements = {1, 0, 2, 0};
    CHECK(grid.i16(0, 0, 0, 1) == 2);
    CHECK(grid.i16(0, 0, 0, 2) == 0);
    CHECK(grid.i16(0, 0, 0, std::numeric_limits<std::size_t>::max() / 2) == 0);
    CHECK(grid.i16(0, 0, 0, std::numeric_limits<std::size_t>::max()) == 0);
}

void packageMarksEveryCopyOfACompressedKey() {
    std::vector<std::uint8_t> file(96, 0);
    const auto put = [&](std::size_t at, std::uint32_t v) {
        if (file.size() < at + 4) {
            file.resize(at + 4);
        }
        for (int i = 0; i < 4; ++i) {
            file[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
        }
    };
    file[0] = 'D', file[1] = 'B', file[2] = 'P', file[3] = 'F';
    put(4, 1);     // major version
    put(32, 7);    // index major version
    put(36, 3);    // entries: the same key twice, then the compression directory
    put(40, 96);   // index offset
    put(44, 60);   // index size (20-byte entries)
    put(60, 1);    // index minor version: no high instance
    const std::uint32_t entries[3][5] = {
        {0x12345678, 1, 2, 156, 4}, {0x12345678, 1, 2, 160, 4}, {ts2::types::kDirectory, 0xE86B1EEF, 0x286B1F03, 164, 16}};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 5; ++j) {
            put(96 + i * 20 + j * 4, entries[i][j]);
        }
    }
    put(164, 0x12345678);
    put(168, 1);
    put(172, 2);
    put(176, 99);  // uncompressed size
    const ts2::dbpf::Package package{std::span<const std::uint8_t>(file)};
    CHECK(package.entries().size() == 3);
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(package.entries()[i].compressed && package.entries()[i].uncompressedSize == 99);
    }
    CHECK(!package.entries()[2].compressed);
}

// A written package reads back with its keys, stored bytes and compression directory. Its index
// has high instances only when a resource has one.
void writtenPackagesReadBack() {
    for (const std::uint32_t high : {0u, 7u}) {
        std::ostringstream out(std::ios::binary);
        ts2::dbpf::PackageWriter writer(out);
        const std::vector<std::uint8_t> plain{1, 2, 3};
        const std::vector<std::uint8_t> packed{4, 5};
        writer.add({0x10, 0xFFFFFFFF, 1, 0}, plain, false, 0);
        writer.add({0x10, 0xFFFFFFFF, 2, high}, packed, true, 40);
        writer.finish(0x00010000);
        const std::string text = out.str();
        const std::vector<std::uint8_t> bytes(text.begin(), text.end());
        ts2::dbpf::Package package{std::span<const std::uint8_t>(bytes)};
        const auto& entries = package.entries();
        CHECK(package.header().majorVersion == 1 && package.header().minorVersion == 2);
        CHECK(package.header().indexMinorVersion == (high ? 2u : 1u) && package.header().holeCount == 0);
        CHECK(bytes.size() > 0x43 && bytes[0x42] == 1);
        CHECK(entries.size() == 3 && entries[2].key.type == ts2::types::kDirectory);
        CHECK(package.readRaw(entries[0]) == plain && !entries[0].compressed);
        CHECK(package.readRaw(entries[1]) == packed && entries[1].compressed && entries[1].uncompressedSize == 40);
        CHECK(entries[1].key.instanceHigh == high);
    }
}

// A neighborhood's terrain: its size, sea level and type, then its corner heights and a byte per
// corner, each a c2DArray.
void neighborhoodTerrainParses() {
    std::vector<std::uint8_t> d;
    auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    auto f32 = [&](float v) { u32(std::bit_cast<std::uint32_t>(v)); };
    auto array = [&](std::uint32_t w, std::uint32_t h) {
        u32(0x6B943B43);
        u32(1);
        u32(8);
        for (const char c : std::string_view("c2DArray")) {
            d.push_back(static_cast<std::uint8_t>(c));
        }
        u32(w);
        u32(h);
    };
    u32(0xABCB5DA4);
    u32(4);
    u32(1);
    u32(1);
    f32(312.5f);
    u32(9);
    for (const char c : std::string_view("Temperate")) {
        d.push_back(static_cast<std::uint8_t>(c));
    }
    array(2, 2);
    for (const float h : {300.0f, 310.0f, 320.0f, 330.0f}) {
        f32(h);
    }
    array(2, 2);
    d.insert(d.end(), {0, 1, 2, 3});
    const auto t = ts2::nhood::parseTerrain(d);
    CHECK(t.version == 4 && t.width == 1 && t.height == 1 && t.seaLevel == 312.5f && t.type == "Temperate");
    CHECK(t.columns == 2 && t.rows == 2 && (t.heights == std::vector<float>{300, 310, 320, 330}));
    CHECK((t.extra == std::vector<std::uint8_t>{0, 1, 2, 3}));
}

// A small effects file: one camera block (its yaw curve 0, 10), one sequence, one visual effect
// whose component points at the camera, and the name it is started by.
void effectsFileReads() {
    std::vector<std::uint8_t> d;
    auto u8 = [&d](std::uint8_t v) { d.push_back(v); };
    auto u16 = [&d](std::uint16_t v) { d.push_back(static_cast<std::uint8_t>(v)); d.push_back(static_cast<std::uint8_t>(v >> 8)); };
    auto u32 = [&d](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    auto f32 = [&u32](float v) { u32(std::bit_cast<std::uint32_t>(v)); };
    auto text = [&d, &u32](const std::string& t) {
        u32(static_cast<std::uint32_t>(t.size()));
        d.insert(d.end(), t.begin(), t.end());
    };
    auto curve = [&u32, &f32](std::initializer_list<float> values) {
        u32(static_cast<std::uint32_t>(values.size()));
        for (float v : values) f32(v);
    };
    std::size_t cameraAt = 0;
    u16(0);
    for (int type = 1; type <= 13; ++type) {
        if (type == 3) {
            u32(1);
            u16(1);
            u32(0x40);
            text("flowerbed");
            u8(0);
            u8(0);
            f32(20);
            curve({1});
            curve({60});
            curve({0.8f});
            u32(1);
            f32(1);
            f32(1);
            f32(1);
            curve({1});
            for (const float v : {0.0f, 0.1f, 1.0f, 1.0f, 0.0f, 0.0f}) f32(v);
        } else if (type == 6) {
            u32(1);
            u16(1);
            u32(1);
            f32(3);
            f32(5);
            text("other");
            u32(0x12);
        } else if (type == 8) {
            u32(1);
            cameraAt = d.size();
            u16(1);
            u32(0x1860);
            f32(600);
            f32(1);
            curve({0});
            curve({0});
            f32(1);
            u8(0);
            for (int k = 0; k < 7; ++k) {
                k == 3 ? curve({0, 10}) : curve({2});
            }
            u8(0);
            u8(0);
            f32(0);
            text("");
        } else {
            u32(0);
        }
    }
    u16(3);
    u32(1);
    u32(0x10);
    u16(0);
    for (int k = 0; k < 5; ++k) u32(0);
    u32(1);
    text("motion");
    u8(8);
    u32(0);
    u8(0);
    f32(2);
    for (int k = 0; k < 9; ++k) f32(k % 4 == 0 ? 1.0f : 0.0f);
    for (int k = 0; k < 3; ++k) f32(static_cast<float>(k));
    u8(0);
    u8(0);
    for (int k = 0; k < 6; ++k) f32(0);
    u32(0x20);
    u32(0x60);
    u16(1);
    u16(32768);
    f32(0);
    u32(0);
    text("NhoodMotion");
    u32(0);
    text("end");
    u32(0xFFFFFFFF);
    u32(0);
    u16(0);
    u32(0);
    const auto f = ts2::effects::parse(d);
    CHECK(f.cameras.size() == 1 && f.cameras[0].flags == 0x1860 && f.cameras[0].length == 600);
    // Curves blend their samples linearly over 0..1 and hold their ends.
    const auto& yaw = f.cameras[0].curves[5];
    CHECK(yaw.at(0.5f) == 5 && yaw.at(-1) == 0 && yaw.at(2) == 10 && f.cameras[0].curves[8].at(0.3f) == 2 && ts2::effects::Curve{}.at(0.5f) == 0);
    CHECK(f.cameras[0].curves[5].values.size() == 2 && f.cameras[0].curves[5].values[1] == 10 && f.cameras[0].curves[8].values[0] == 2);
    CHECK(f.sequences.size() == 1 && f.sequences[0].items[0].effect == "other" && f.sequences[0].flags == 2);
    const auto* e = f.find("nhoodmotion");
    CHECK(e && e->flags == 0x10 && e->components.size() == 1);
    CHECK(e && e->components[0].type == 8 && e->components[0].index == 0 && e->components[0].offset[2] == 2);
    CHECK(e && e->components[0].state == 0x20 && e->components[0].stateMask == 0x60 && !f.find("missing"));
    CHECK(e && e->components[0].group == 1 && e->components[0].weight == 32768 && e->components[0].scale == 2);
    CHECK(f.decals.size() == 1 && f.decals[0].texture == "flowerbed" && f.decals[0].flags == 0x40 && f.decals[0].life == 20);
    CHECK(f.decals.size() == 1 && f.decals[0].size.at(0) == 60 && f.decals[0].alpha.at(0) == 0.8f && f.decals[0].sizeVary == 0.1f &&
          f.decals[0].rotationVary == 1 && f.decals[0].repeat == 1 && f.decals[0].colours.size() == 1);
    // A block version the game would not read, and a count larger than the data, are refused.
    auto throws = [](auto parse) {
        try {
            parse();
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    auto bad = d;
    bad[cameraAt] = 2;
    CHECK(throws([&] { ts2::effects::parse(bad); }));
    auto huge = d;
    huge[2] = huge[3] = huge[4] = huge[5] = 0xFF;
    CHECK(throws([&] { ts2::effects::parse(huge); }));
}

void neighborhoodCameraReads() {
    const auto configs = ts2::camera::parse(
        "# comment\ncamera \"Neighborhood\" Neighborhood\n"
        "zoom        1 (650 450 230)\nfov            (33  33  33)\nclip           (2  2  2) (2000 2000 2000)\n"
        "pitch          (40  32  25)   (40   32   25)\norientation 0 (0 45 90 135 180 225 270 315)\n");
    CHECK(configs.size() == 1);
    const auto& c = configs[0];
    CHECK(c.name == "Neighborhood" && c.controller == "Neighborhood" && c.defaultZoom == 1 && c.levels.size() == 3);
    CHECK(c.levels[0].distance == 650 && c.levels[2].distance == 230 && c.levels[1].fov == 33);
    CHECK(c.levels[0].nearClip == 2 && c.levels[0].farClip == 2000 && c.levels[1].pitchMin == 32 && c.levels[2].pitchMax == 25);
    CHECK(c.orientations.size() == 8 && c.orientations[2] == 90);
    // Pitch follows the distance between the zoom levels and holds beyond them.
    CHECK(ts2::camera::pitchAt(c, 650) == 40 && ts2::camera::pitchAt(c, 800) == 40 && ts2::camera::pitchAt(c, 450) == 32);
    CHECK(ts2::camera::pitchAt(c, 550) == 36 && ts2::camera::pitchAt(c, 100) == 25);
    // The blend goes from each level's minimum pitch to its maximum (the Live camera's lists).
    const auto live = ts2::camera::parse("camera \"Live\" Live\nzoom 1 (70 3)\npitch (30 2) (40 65)\nsetparam constraintWidth   -10\n");
    CHECK(live.size() == 1 && live[0].param("constraintwidth", 0) == -10 && c.param("constraintwidth", 0) == 0);
    // The free camera's one near and far clip.
    const auto free = ts2::camera::parse("camera \"CameraMan\" CameraMan\nfov (20 25)\nclip 0.15 2000\n");
    CHECK(free.size() == 1 && free[0].clip && (*free[0].clip)[0] == 0.15f && (*free[0].clip)[1] == 2000 && free[0].levels.size() == 2);
    CHECK(ts2::camera::pitchAt(live[0], 70, 1) == 40 && ts2::camera::pitchAt(live[0], 70, 0.5f) == 35);
    CHECK(ts2::camera::pitchAt(live[0], 3, 1) == 65 && ts2::camera::pitchAt(live[0], 3, 0) == 2);
    // The neighborhood's edge margins, and margins that would leave no room are dropped.
    const auto kept = ts2::camera::parse("camera \"N\" Neighborhood\nconstrain   .35 .35 .35 .35\n");
    CHECK(kept.size() == 1 && kept[0].constrain && (*kept[0].constrain)[3] == 0.35f && !c.constrain);
    CHECK(!ts2::camera::parse("camera \"N\" N\nconstrain .35 .6 .35 .35\n")[0].constrain);
    CHECK(!ts2::camera::parse("camera \"N\" N\nconstrain .35 .35\n")[0].constrain);

    // The saved camera record, laid out as the shipped Pleasantview's.
    std::vector<std::uint8_t> d(115, 0);
    auto put = [&](std::size_t at, float v) { std::memcpy(d.data() + at, &v, 4); };
    put(0x48, 230);
    put(0x51, 3.783f);
    put(0x59, 741.3f);
    put(0x5d, 625.2f);
    put(0x61, 312.5f);
    const auto saved = ts2::nhood::parseSavedCamera(d);
    CHECK(saved && saved->distance == 230 && saved->yaw == 3.783f && saved->target[0] == 741.3f && saved->target[2] == 312.5f);
    d.pop_back();
    CHECK(!ts2::nhood::parseSavedCamera(d));
}

void neighborhoodPaintRulesRead() {
    const auto rules = ts2::nhood::parsePaintRules(
        "[PaintInfos]\n0x10=1.0,nh-test-cliff,1, terrain-cliff, 0,1, 24,0\n0x01=0.9,nh-temperate-wet,2, lottexture-test-01, 0,1, 26,0\n"
        "0x04=0.9,nh-temperate-drydry,3, lottexture-test-01, 0,1, 26,0\n0x11=1.0,nh-test-beach,1, terrain-beach, 0,1, 24,0\n"
        "[TerrainTypeInfos]\nTemperate=0x10,0x11\n[PaintMapTable_Temperate]\n"
        "31=0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x04,\n"
        "[NHoodTerrainTunables]\nMaxBeachAltitude=4 \nMaxRoadProximityValue=20\n[NHoodTerrainRenderTunables]\nTextureVariantGridSize=13\n");
    CHECK(rules.paints.size() == 4 && rules.paints.at(0x01).texture == "nh-temperate-wet" && rules.paints.at(0x04).variants == 3);
    CHECK(rules.paints.at(0x01).maxStrength == 0.9f && rules.paints.at(0x01).spread.size() == 2 && rules.paints.at(0x01).spread[1].first == 26);
    CHECK(rules.types.at("Temperate").cliff == 0x10 && rules.types.at("Temperate").beach == 0x11);
    CHECK(rules.types.at("Temperate").table[31][0] == 0x01 && rules.types.at("Temperate").table[31][15] == 0x04);
    CHECK(rules.maxBeachAltitude == 4 && rules.textureVariantGridSize == 13);
    CHECK(ts2::nhood::spreadAt(rules.paints.at(0x01), 0) == 1 && std::fabs(ts2::nhood::spreadAt(rules.paints.at(0x01), 13) - 0.5) < 1e-6);
    CHECK(ts2::nhood::spreadAt(rules.paints.at(0x01), 30) == 0);

    // Out-of-range tunables are held to values the painter can use.
    const auto broken = ts2::nhood::parsePaintRules(
        "[PaintInfos]\n0x01=0.9,nh-x,100000, lottexture-test-01\n[NHoodTerrainTunables]\nMaxBeachWidthInCells=254\n"
        "HeightRangeForTemperatureDistribution=0\nMaxRoadProximityValue=0\n");
    CHECK(broken.paints.at(0x01).variants == 100 && broken.maxBeachWidthInCells == 253);
    CHECK(broken.heightRangeForTemperature == 500 && broken.maxRoadProximity == 1);

    // A flat strip: the first column under water, the rest just above the beach's top.
    ts2::nhood::Terrain t;
    t.type = "Temperate";
    t.seaLevel = 312.5f;
    t.columns = 8;
    t.rows = 3;
    for (std::uint32_t y = 0; y < t.rows; ++y) {
        for (std::uint32_t x = 0; x < t.columns; ++x) {
            t.heights.push_back(x == 0 ? 310.0f : 320.0f);
        }
    }
    const auto paints = ts2::scene::paintTerrain(t, rules, 10, {});
    const auto& under = paints.vertices[0];
    CHECK(under.id[0] == 0x11 || under.id[1] == 0x11 || under.id[2] == 0x11);  // beach under shallow water
    const auto& far = paints.vertices[7];
    CHECK(far.id[0] == 0x04 && far.id[1] == 0 && far.weight[0] == 250);  // far from roads: the table's last column
    for (const auto& v : paints.vertices) {
        unsigned sum = 0;
        for (int i = 0; i < 3 && v.id[i]; ++i) sum += v.weight[i];
        CHECK(sum <= 250 && sum >= 247);
    }
}

void neighborhoodRoadsParse() {
    std::vector<std::uint8_t> d;
    auto u8 = [&](std::uint8_t v) { d.push_back(v); };
    auto u16 = [&](std::uint16_t v) { d.insert(d.end(), {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8)}); };
    auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    auto f32 = [&](float v) { u32(std::bit_cast<std::uint32_t>(v)); };
    u32(0xCB);
    // One plant, then one road tile.
    u16(8);
    u32(1);
    u8(2);
    for (float v : {700.0f, 560.0f, 324.0f, 695.0f, 555.0f, 705.0f, 565.0f}) {
        f32(v);
    }
    u8(8);
    f32(270.0f);
    u32(65);
    u16(3);
    u32(1);
    u8(2);
    for (float v : {15.0f, 1025.0f, 365.0f, 10.0f, 1020.0f, 20.0f, 1030.0f}) {
        f32(v);
    }
    u8(3);
    const float corners[4][5] = {{10, 1020, 361.5f, 1, 0}, {10, 1030, 361.5f, 0, 0}, {20, 1030, 368.5f, 0, 1}, {20, 1020, 368.5f, 1, 1}};
    for (const auto& c : corners) {
        for (float v : c) {
            f32(v);
        }
    }
    u32(0x4b00);
    d.insert(d.end(), {0, 0, 0, 0, 1, 5, 0, 0, 0, 0});
    // One bridge piece (a road record, its version, a point, its turn, a point), then two decorations:
    // the guid comes before the turn, and a version 2 record has no guid or bounds and stands on the
    // older smaller tile.
    u16(3);
    u32(1);
    u8(2);
    for (float v : {410.0f, 845.0f, 402.7f, 400.0f, 840.0f, 420.0f, 850.0f}) {
        f32(v);
    }
    u8(3);
    d.insert(d.end(), 80, 0);
    u32(0x0ac40000);
    d.insert(d.end(), 10, 0);
    u8(3);
    d.insert(d.end(), 12, 0);
    for (float v : {0.0f, 0.0f, 0.707f, 0.707f}) {
        f32(v);
    }
    d.insert(d.end(), 12, 0);
    u16(8);
    u32(2);
    u8(2);
    for (float v : {705.0f, 505.0f, 325.0f, 701.0f, 501.0f, 708.0f, 508.0f}) {
        f32(v);
    }
    u8(8);
    u32(49);
    f32(90.0f);
    u8(2);
    for (float v : {80.0f, 40.0f, 300.0f, 1.0f, 2.0f, 3.0f, 4.0f}) {
        f32(v);
    }
    u8(2);
    f32(180.0f);
    const auto objects = ts2::nhood::parseObjects(d);
    CHECK(objects.bridges.size() == 1 && objects.bridges[0].road.piece == 0x0ac40000 && objects.bridges[0].road.center[0] == 410 &&
          objects.bridges[0].rotation[2] == 0.707f && objects.bridges[0].rotation[3] == 0.707f);
    CHECK(objects.decorations.size() == 2);
    CHECK(objects.decorations[0].type == 49 && objects.decorations[0].angle == 90 && objects.decorations[0].bounds[3] == 508);
    CHECK(objects.decorations[1].type == 0 && objects.decorations[1].angle == 180 && objects.decorations[1].position[0] == 100 &&
          objects.decorations[1].bounds[2] == 0);
    CHECK(objects.version == 0xCB && objects.roads.size() == 1 && objects.flora.size() == 1);
    CHECK(objects.flora[0].position[1] == 560 && objects.flora[0].bounds[2] == 705 && objects.flora[0].angle == 270 &&
          objects.flora[0].type == 65);
    const auto& road = objects.roads[0];
    CHECK(road.piece == 0x4b00 && road.bounds[0] == 10 && road.bounds[3] == 1030);
    CHECK(road.corners[2][0] == 20 && road.corners[2][1] == 1030 && road.corners[2][2] == 368.5f && road.corners[3][3] == 1);
}

// Binary property sets (magic 0xCBE750E0) read like the XML ones. Reference lists (3IDR) list keys,
// version 2 with a high instance.
void binaryPropertySetsAndReferenceLists() {
    std::vector<std::uint8_t> d = {0xE0, 0x50, 0xE7, 0xCB, 2, 0, 3, 0, 0, 0};
    auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    auto name = [&](const std::string& s) {
        u32(static_cast<std::uint32_t>(s.size()));
        d.insert(d.end(), s.begin(), s.end());
    };
    u32(0xEB61E4F7), name("lk0"), u32(0x10002);
    u32(0x0B8BEA18), name("type"), name("skin");
    u32(0xABC78708), name("stretch"), u32(0x3F800000);
    const auto p = ts2::propset::parse(d);
    CHECK(p.at("lk0") == "65538" && p.at("type") == "skin" && std::stof(p.at("stretch")) == 1.0f);

    d = {0xEF, 0xBE, 0xAD, 0xDE, 2, 0, 0, 0, 1, 0, 0, 0};
    u32(0xE519C933), u32(0x1C0532FA), u32(0xFF6B8A91), u32(0x12345678);
    const auto refs = ts2::reflist::parse(d);
    CHECK(refs.size() == 1 && refs[0].type == 0xE519C933 && refs[0].instance == 0xFF6B8A91 && refs[0].instanceHigh == 0x12345678);
}

void wallCoveringsFollowTheWallGraph() {
    // The room graph numbers its walls 1..3. The wall graph has the first two, numbered 10 and 11.
    ts2::wallgraph::WallGraph rooms, walls;
    rooms.points = {{1, 0, 0, 0}, {2, 1, 0, 0}, {3, 1, 1, 0}, {4, 0, 1, 0}};
    rooms.walls = {{1, 1, 1, 2, 0}, {2, 2, 1, 3, 0}, {3, 3, 1, 4, 0}};
    walls.points = {{7, 1, 1, 0}, {8, 1, 0, 0}, {9, 0, 0, 0}};
    walls.walls = {{10, 9, 0, 8, 0}, {11, 7, 0, 8, 0}};
    const std::vector<ts2::walllayer::Entry> coverings{{10, 0x17, 1, 2}, {11, 0x2D0EA907, 0, 0}};
    const auto out = ts2::content::coveringsOfRoomWalls(rooms, walls, coverings);
    CHECK(out.size() == 2 && out[0].wall == 1 && out[0].type == 0x17 && out[0].left == 1 && out[1].wall == 2 &&
          out[1].type == 0x2D0EA907);  // wall 3 only divides rooms
}

void lotDescriptionParse() {
    auto make = [](std::uint16_t version, std::uint16_t own) {
        std::vector<std::uint8_t> d;
        auto u8 = [&](std::uint8_t v) { d.push_back(v); };
        auto u16 = [&](std::uint16_t v) { d.insert(d.end(), {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8)}); };
        auto u32 = [&](std::uint32_t v) {
            for (int i = 0; i < 4; ++i) {
                d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
            }
        };
        auto text = [&](const std::string& s) {
            u32(static_cast<std::uint32_t>(s.size()));
            d.insert(d.end(), s.begin(), s.end());
        };
        u16(version);
        u16(own);
        u32(2);
        u32(3);
        u8(0);
        u8(8);
        u8(3);
        u32(3);
        text("Lot");
        text("");
        u32(2);
        u32(0);
        u32(0);
        if (own >= 11) {
            d.insert(d.end(), 22, 0);
        }
        u32(98);
        u32(80);
        u32(std::bit_cast<std::uint32_t>(324.1875f));
        u32(32);
        u8(1);
        text("lottexture");
        u8(0);
        if (version > 0x0D) {
            u32(7);  // the owner
        }
        return d;
    };
    const std::uint16_t versions[2][2] = {{13, 6}, {18, 11}};
    for (const auto& [version, own] : versions) {
        const auto lot = ts2::nhood::parseLotDescription(make(version, own));
        CHECK(lot.width == 2 && lot.depth == 3 && lot.roadSides == 8 && lot.ownFacing == 3 && lot.name == "Lot");
        CHECK(lot.x == 98 && lot.y == 80 && lot.elevation == 324.1875f && lot.id == 32 && lot.facing == 1);
        CHECK(lot.texture == "lottexture" && lot.owner == (version > 0x0D ? 7 : 0));
    }
    // From version 0x12 the description ends with the rooms' light percents.
    auto withRooms = make(18, 11);
    withRooms.insert(withRooms.end(), 6, 0);
    for (const std::uint16_t v : std::initializer_list<std::uint16_t>{3, 0, 2, 40, 6, 40, 9, 100}) {
        withRooms.insert(withRooms.end(), {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8)});
    }
    const auto lit = ts2::nhood::parseLotDescription(withRooms);
    CHECK(lit.roomLightPercents.size() == 3 && lit.roomLightPercent(2) == 40 && lit.roomLightPercent(9) == 100);
    CHECK(lit.roomLightPercent(5) == 100 && lit.roomLightPercent(0) == 100);
    CHECK(ts2::nhood::parseLotDescription(make(18, 11)).roomLightPercents.empty());
}

// A wall graph's point index finds what WallGraph::point finds: the first of a repeated id, and
// nothing for an id it lacks.
void wallGraphPointIndexFindsTheFirstPoint() {
    ts2::wallgraph::WallGraph graph;
    graph.points = {{4, 1, 2, 0}, {7, 3, 4, 0}, {4, 5, 6, 1}};
    const ts2::wallgraph::PointIndex index(graph);
    CHECK(index(4) == &graph.points[0] && graph.point(4) == &graph.points[0]);
    CHECK(index(7) == &graph.points[1]);
    CHECK(index(9) == nullptr && graph.point(9) == nullptr);
}

} // namespace

namespace {

// A 2 x 2 cursor with two palette colors: rows are stored bottom up and the mask hides one pixel.
std::vector<std::uint8_t> tinyCursor() {
    std::vector<std::uint8_t> d;
    auto u16 = [&d](std::uint32_t v) { d.push_back(static_cast<std::uint8_t>(v)); d.push_back(static_cast<std::uint8_t>(v >> 8)); };
    auto u32 = [&](std::uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); };
    u16(0); u16(2); u16(1);
    d.insert(d.end(), {2, 2, 0, 0});
    u16(1); u16(0);
    const std::uint32_t size = 40 + 8 + 8 + 8;
    u32(size); u32(22);
    u32(40); u32(2); u32(4); u16(1); u16(8); u32(0); u32(0); u32(0); u32(0); u32(2); u32(0);
    d.insert(d.end(), {0, 0, 255, 0, 255, 0, 0, 0});  // red, blue (BGRA)
    d.insert(d.end(), {1, 1, 0, 0, 0, 0, 0, 0});      // bottom row blue, top row red
    d.insert(d.end(), {0, 0, 0, 0, 0x40, 0, 0, 0});   // the top row's right pixel masked out
    return d;
}

} // namespace

void cursorReadsPictureMaskAndHotspot() {
    auto d = tinyCursor();
    const auto c = ts2::cursor::decode(d);
    CHECK(c && c->pixels.width == 2 && c->pixels.height == 2 && c->hotX == 1 && c->hotY == 0);
    CHECK(c->pixels.rgba[0] == 255 && c->pixels.rgba[2] == 0 && c->pixels.rgba[3] == 255);
    CHECK(c->pixels.rgba[7] == 0);
    CHECK(c->pixels.rgba[8] == 0 && c->pixels.rgba[10] == 255 && c->pixels.rgba[11] == 255);
    d[27] = 0x02;  // a picture 514 wide
    CHECK(!ts2::cursor::decode(d));
}

// Two pictures shown in three steps (0, 1, 0), the first step longer, as the game's hourglass does.
// An animated cursor shows each step for its own time and loops. A stall of over a second starts the
// step shown again, and steps of no time can't hang it.
void cursorPlaybackStepsAndLoops() {
    using Step = ts2::cursor::AnimatedCursor::Step;
    const std::vector<Step> steps{{0, 6}, {1, 3}, {2, 12}};  // 0.1 s, 0.05 s, 0.2 s
    ts2::cursor::CursorPlayback play;
    CHECK(play.step(steps, 10.0) == 0);
    CHECK(play.step(steps, 10.09) == 0);
    CHECK(play.step(steps, 10.11) == 1);
    CHECK(play.step(steps, 10.16) == 2);
    CHECK(play.step(steps, 10.36) == 0);  // looped
    CHECK(play.step(steps, 10.46) == 1);
    CHECK(play.step(steps, 15.0) == 1);   // after a stall the shown step starts again
    CHECK(play.step(steps, 15.04) == 1);
    CHECK(play.step(steps, 15.06) == 2);
    play.restart();
    CHECK(play.step(steps, 20.0) == 0);
    const std::vector<Step> empty;
    CHECK(play.step(empty, 21.0) == 0);
    const std::vector<Step> none{{0, 0}, {1, 0}};
    ts2::cursor::CursorPlayback quick;
    CHECK(quick.step(none, 0.0) == 0);
    CHECK(quick.step(none, 0.9) < 2);  // each counts as one jiffy, and the call returns
}

void animatedCursorReadsSequenceAndRates() {
    auto make = [](std::uint32_t steps, std::vector<std::uint32_t> sequence, std::vector<std::uint32_t> rates) {
        std::vector<std::uint8_t> body;
        auto u32 = [](std::vector<std::uint8_t>& to, std::uint32_t v) {
            for (int i = 0; i < 4; ++i) {
                to.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
            }
        };
        auto chunk = [&](std::vector<std::uint8_t>& to, const char* id, const std::vector<std::uint8_t>& data) {
            to.insert(to.end(), id, id + 4);
            u32(to, static_cast<std::uint32_t>(data.size()));
            to.insert(to.end(), data.begin(), data.end());
            if (data.size() % 2 != 0) {
                to.push_back(0);
            }
        };
        std::vector<std::uint8_t> header;
        for (const std::uint32_t v : {36u, 2u, steps, 0u, 0u, 0u, 0u, 10u, 3u}) {
            u32(header, v);
        }
        chunk(body, "anih", header);
        auto list = [&](const std::vector<std::uint32_t>& values) {
            std::vector<std::uint8_t> out;
            for (const std::uint32_t v : values) {
                u32(out, v);
            }
            return out;
        };
        if (!rates.empty()) {
            chunk(body, "rate", list(rates));
        }
        if (!sequence.empty()) {
            chunk(body, "seq ", list(sequence));
        }
        std::vector<std::uint8_t> frames{'f', 'r', 'a', 'm'};
        chunk(frames, "icon", tinyCursor());
        chunk(frames, "icon", tinyCursor());
        chunk(body, "LIST", frames);
        std::vector<std::uint8_t> file{'R', 'I', 'F', 'F'};
        u32(file, static_cast<std::uint32_t>(body.size() + 4));
        file.insert(file.end(), {'A', 'C', 'O', 'N'});
        file.insert(file.end(), body.begin(), body.end());
        return file;
    };
    const auto a = ts2::cursor::decodeAnimated(make(3, {0, 1, 0}, {40, 5, 5}));
    CHECK(a && a->frames.size() == 2 && a->steps.size() == 3);
    CHECK(a->steps[0].frame == 0 && a->steps[0].jiffies == 40 && a->steps[1].frame == 1 && a->steps[1].jiffies == 5);
    CHECK(a->frames[1].hotX == 1 && a->frames[1].pixels.width == 2);
    // A size that counts the 8 bytes before it, as the game's hourglass has, still reads.
    auto counted = make(3, {0, 1, 0}, {40, 5, 5});
    const std::uint32_t whole = static_cast<std::uint32_t>(counted.size());
    for (int i = 0; i < 4; ++i) {
        counted[4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(whole >> (8 * i));
    }
    CHECK(ts2::cursor::decodeAnimated(counted).has_value());
    // No sequence and no rates: each picture in turn, all at the header's rate.
    const auto plain = ts2::cursor::decodeAnimated(make(2, {}, {}));
    CHECK(plain && plain->steps.size() == 2 && plain->steps[1].frame == 1 && plain->steps[1].jiffies == 10);
    // A step of no time still lasts one sixtieth of a second.
    const auto still = ts2::cursor::decodeAnimated(make(2, {}, {0, 0}));
    CHECK(still && still->steps[0].jiffies == 1);
    CHECK(!ts2::cursor::decodeAnimated(make(3, {0, 2, 0}, {})));          // a step showing a picture that isn't there
    CHECK(!ts2::cursor::decodeAnimated(make(3, {0, 1}, {})));             // a sequence shorter than the steps
    CHECK(!ts2::cursor::decodeAnimated(make(3, {}, {})));                 // more steps than pictures, no sequence
    CHECK(!ts2::cursor::decodeAnimated(make(ts2::cursor::kMaxSteps + 1, {}, {})));
    auto cut = make(3, {0, 1, 0}, {40, 5, 5});
    cut.resize(cut.size() - 10);
    CHECK(!ts2::cursor::decodeAnimated(cut));
    CHECK(!ts2::cursor::decodeAnimated(tinyCursor()));
}

void packageRecognisesAppleDoubleMetadata() {
    const std::vector<std::uint8_t> appleDouble{0x00, 0x05, 0x16, 0x07};
    const ts2::dbpf::Package package{std::span<const std::uint8_t>(appleDouble)};
    CHECK(package.appleDouble() && package.entries().empty());
}

std::vector<Test> formatTests() {
    return {
        {"a 3D array refuses element indexes past the element", array3dRefusesElementIndexesPastTheElement},
        {"a package marks every copy of a compressed key", packageMarksEveryCopyOfACompressedKey},
        {"a package recognises AppleDouble metadata", packageRecognisesAppleDoubleMetadata},
        {"written packages read back", writtenPackagesReadBack},
        {"interactions are found by string index", interactionsAreFoundByStringIndex},
        {"Weather record reads its values", weatherRecordReadsItsValues},
        {"Neighborhood id is read", neighborhoodIdIsRead},
        {"old operands are upgraded", oldOperandsAreUpgraded},
        {"SLOT resource counts container slots", slotResourceCountsContainerSlots},
        {"SLOT entry reads every field", slotEntryReadsEveryField},
        {"cursor reads picture, mask and hotspot", cursorReadsPictureMaskAndHotspot},
        {"animated cursor reads sequence and rates", animatedCursorReadsSequenceAndRates},
        {"cursor playback steps and loops", cursorPlaybackStepsAndLoops},
        {"pattern map reads names by id", patternMapReadsNamesById},
        {"pattern map reads long names", patternMapReadsLongNames},
        {"name map reads names", nameMapReadsNames},
        {"terrain resources parse", terrainResourcesParse},
        {"lot grids with variable tiles parse", lotGridsWithVariableTilesParse},
        {"object groups parse", objectGroupsParse},
        {"object functions ignore the magic", objectFunctionsIgnoreTheMagic},
        {"roofs read every version", roofsReadEveryVersion},
        {"neighborhood terrain parses", neighborhoodTerrainParses},
        {"effects file reads", effectsFileReads},
        {"neighborhood camera reads", neighborhoodCameraReads},
        {"neighborhood paint rules read", neighborhoodPaintRulesRead},
        {"neighborhood roads parse", neighborhoodRoadsParse},
        {"wall coverings follow the wall graph", wallCoveringsFollowTheWallGraph},
        {"lot description parse", lotDescriptionParse},
        {"binary property sets and reference lists", binaryPropertySetsAndReferenceLists},
        {"fence post layer reads posts", fencePostLayerReadsPosts},
        {"neighbor remap keeps table 6", neighborRemapKeepsTableSix},
        {"wall layer reads coverings per side", wallLayerReadsCoveringsPerSide},
        {"wants and fears parse", wantsAndFearsParse},
        {"want slots follow the version", wantSlotsFollowTheVersion},
        {"interaction table parse", interactionTableParse},
        {"Sim description reads person data by version", simDescriptionReadsPersonDataByVersion},
        {"family record is read by version", familyRecordIsReadByVersion},
        {"family ties are read per Sim", familyTiesAreReadPerSim},
        {"Constant tables count in fifteen bits", constantTablesCountInFifteenBits},
        {"tuning lists read named values", tuningListsReadNamedValues},
        {"saved object reads playing animations", savedObjectReadsPlayingAnimations},
        {"wall graph point index finds the first point", wallGraphPointIndexFindsTheFirstPoint},
    };
}
