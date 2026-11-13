// File formats: packages, object data, lot and neighborhood resources.
#include "test_support.hpp"

#include "content/lot.hpp"
#include "formats/camera_config.hpp"
#include "formats/cobject.hpp"
#include "formats/cobject_record.hpp"
#include "formats/face_shape.hpp"
#include "formats/cursor.hpp"
#include "formats/dbpf_writer.hpp"
#include "formats/effects.hpp"
#include "formats/lot_description.hpp"
#include "formats/objm.hpp"
#include "formats/qfs.hpp"
#include "formats/nhood_objects.hpp"
#include "formats/nhood_terrain.hpp"
#include "formats/saved_camera.hpp"
#include "formats/simi.hpp"
#include "formats/srel.hpp"
#include "formats/lot_grid.hpp"
#include "formats/lot_records.hpp"
#include "formats/sdsc.hpp"
#include "formats/fami.hpp"
#include "formats/famt.hpp"
#include "formats/ngbh.hpp"
#include "formats/swaf.hpp"
#include "formats/str.hpp"
#include "formats/object_archive.hpp"
#include "formats/propset.hpp"
#include "formats/world_db.hpp"
#include "formats/objt.hpp"
#include "formats/roofs.hpp"
#include "formats/stringmap.hpp"
#include "formats/fencepostlayer.hpp"
#include "formats/walllayer.hpp"
#include "formats/wallgraph.hpp"
#include "formats/array3d.hpp"
#include "formats/terrain.hpp"

#include <bit>
#include <initializer_list>
#include <utility>
#include <limits>
#include <sstream>

namespace {

// A lot's floor pattern list (cStringMap): names by the id the floor grid stores.
void effectDetailFollowsZoomLevels() {
    ts2::camera::Config c;
    c.levels.resize(5);
    const float distances[5] = {60, 40, 30, 20, 10};
    for (std::size_t i = 0; i < 5; ++i) {
        c.levels[i].distance = distances[i];
    }
    CHECK(ts2::camera::effectDetailAt(c, 80) == 1);  // beyond the farthest it holds
    CHECK(ts2::camera::effectDetailAt(c, 60) == 1);
    CHECK(ts2::camera::effectDetailAt(c, 50) == 1.5f);
    CHECK(ts2::camera::effectDetailAt(c, 20) == 4);
    CHECK(ts2::camera::effectDetailAt(c, 10) == 5);
    c.levels.resize(1);  // the free camera has none to go by
    CHECK(ts2::camera::effectDetailAt(c, 10) == 1);
}

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

// A lot's simulator state: the ticks' random number state comes after the globals and three other
// values, in versions with sixty globals only.
void simulatorStateReadsTheRandomState() {
    std::vector<std::uint8_t> d(64, 0);
    auto put = [&d](std::uint32_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    put(0x53494D49, 4);
    put(0xD8, 4);
    put(0, 4);
    for (int g = 0; g < 60; ++g) put(static_cast<std::uint32_t>(g), 2);
    put(1, 4); put(2, 2); put(3, 4);
    put(0xBC598883, 4); put(0x4EE46106, 4);
    const auto state = ts2::simi::parse(d);
    CHECK(state.globals.size() == 60 && state.globals[59] == 59);
    CHECK(state.randomState == 0xBC598883u);
    d[0x44] = 0xD3;
    CHECK(!ts2::simi::parse(d).randomState);
    d[0x44] = 0xD8;
    d.resize(d.size() - 6);
    CHECK(!ts2::simi::parse(d).randomState);
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
    u32(0x09D0CE69), u32(11), u32(0x60000001), str("a-idle"), str("ovl"), u32(6), u32(4);
    d.push_back(43), d.push_back(0), d.push_back(0x00), d.push_back(0x20);  // event object 43, tree 0x2000
    u32(0), f32(-1.5f), f32(-1), f32(-1), u32(1), u32(1), u32(0x7FFFFF00), u32(0), floats(7), u32(0xFFFFFFFF), u32(1);
    u32(0x584BA0BB), u32(0), u32(0), str(""), u32(0), u32(0), u32(0), u32(0);
    u32(0xC9DE69B6), u32(1), u32(0x3AFF30DB), u32(7), u32(1), u32(0), u32(100), u32(0x7FFFFF00), f32(0.1f), f32(1);
    u32(5), u32(1), f32(0.5f), u32(1);                        // layer 1, weight 0.5, blend curve:
    u32(0x8B969D64), u32(1), u32(1), f32(0), u32(3);
    for (const auto& [kind, ms] : {std::pair{0u, 150.0f}, std::pair{1u, 3000.0f}, std::pair{2u, 250.0f}}) {  // rise, hold, fall
        u32(kind), f32(0), f32(1), f32(ms);
    }
    u32(0), u32(1), f32(1), f32(0), floats(24), u32(454), u32(1), u32(0), u32(0x7FFFFF00), f32(0), u32(1), f32(354);
    str("a-idle_anim"), u32(1);

    const auto saved = ts2::cobject::parse(d);
    CHECK(saved.classes == std::vector<std::string>({"cAnimatable", "cObject"}));
    CHECK(saved.outerTag == 0x7D4F2BB8);
    CHECK(saved.joints.size() == 1 && saved.joints[0].name == "slot_chairdining_trans" && saved.joints[0].translation[1] == 0.373f &&
          saved.joints[0].rotation[3] == 1.0f);
    CHECK(saved.animations.size() == 1);
    const auto& a = saved.animations.front();
    CHECK(a.name == "a-idle" && a.type == "ovl" && a.category == 4 && a.eventObject == 43 && a.eventTree == 0x2000 &&
          a.eventObjectGuid == 0x584BA0BB && a.rate == -1.5f);
    CHECK(a.state == 1 && a.layer == 1 && a.plays == 1 && a.weight == 0.5f && a.lengthMs == 3400.0f && a.startedMsAgo == 354);
    CHECK(a.fadeInMs == 150.0f && a.fadeOutMs == 250.0f && a.flipped);
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

// The object map: its objects, then each object id's wait as a Sleep left it (an s16 and a value
// not known yet). A wait table longer than its data is refused.
void objectMapReadsWaits() {
    std::vector<std::uint8_t> d(64, 0);
    auto u32 = [&d](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    u32(0);
    u32(0xD8);
    u32(0x4F626A4D);
    u32(1);
    u32(2);
    u32(7);
    u32(2);
    u32(0xFFFE | 9u << 16);
    u32(193 | 1u << 16);
    const auto map = ts2::objm::parse(d);
    CHECK(map.typeIndexByObject.at(2) == 7);
    CHECK((map.waitTicks == std::vector<std::int16_t>{-2, 193}));
    d.resize(d.size() - 2);
    bool threw = false;
    try {
        ts2::objm::parse(d);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
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
          in.joiningIndex == -1 && in.memoryMultiplier == 0.5f && in.iconIndex == 1);
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

// A want tree's nodes nest as the file nests them, each with its properties, factors and children.
// Nodes nested past the depth limit are left out with all they hold.
void wantTreesAreRead() {
    const std::string text = R"(<?xml version="1.0"?>
<tree version="8">
  <cGZPropertySetString>
    <AnyUint32 key="id" type="0xeb61e4f7">0x6dce563c</AnyUint32>
    <AnyString key="folder" type="0x0b8bea18">GU</AnyString>
  </cGZPropertySetString>
  <wantNode key="root">
    <cGZPropertySetString>
      <!--Want name is <odd> text-->
      <AnyUint32 key="wantId" type="0xeb61e4f7">0xcdc2132e</AnyUint32>
      <AnyBoolean key="visible" type="0xcba908e1">False</AnyBoolean>
      <AnySint32 key="lifetime" type="0x0c264712">48</AnySint32>
      <AnySint32 key="minScore" type="0x0c264712">-5</AnySint32>
    </cGZPropertySetString>
    <ads key="age" weight="17" superZero="True" locked="False">
      <bool key="Baby" value="False" />
      <bool key="Toddler" value="True" />
    </ads>
    <ads key="mood" weight="3" superZero="False" locked="False">
      <range key="mood" min="-1" max="-25" index="4" />
    </ads>
    <children>
      <wantNode key="node">
        <cGZPropertySetString>
          <AnyUint32 key="wantId" type="0xeb61e4f7">0x8da69c12</AnyUint32>
          <AnyBoolean key="regeneration" type="0xcba908e1">True</AnyBoolean>
          <AnySint32 key="scoreOverride" type="0x0c264712">-1000</AnySint32>
        </cGZPropertySetString>
        <children />
      </wantNode>
      <wantNode key="node">
        <cGZPropertySetString>
          <AnyUint32 key="wantId" type="0xeb61e4f7">0x2da69c01</AnyUint32>
        </cGZPropertySetString>
        <children />
      </wantNode>
    </children>
  </wantNode>
</tree>)";
    const auto tree = ts2::want_trees::parse(text);
    CHECK(tree.id == 0x6DCE563C && tree.folder == "GU" && tree.nodes.size() == 3);
    const auto& root = tree.nodes[0];
    CHECK(root.wantId == 0xCDC2132E && !root.visible && root.lifetime == 48 && root.minScore == -5 && root.parent == -1);
    const auto& age = root.ads[static_cast<int>(ts2::want_trees::AdKind::Age)];
    CHECK(age.weight == 17 && age.superZero && age.ticked == std::vector<std::string>{"Toddler"});
    const auto& mood = root.ads[static_cast<int>(ts2::want_trees::AdKind::Mood)];
    CHECK(mood.weight == 3 && mood.min == -1 && mood.max == -25 && mood.index == 4);
    CHECK(root.children == (std::vector<std::int32_t>{1, 2}) && tree.nodes[2].parent == 0 && tree.nodes[2].wantId == 0x2DA69C01);
    CHECK(tree.nodes[1].wantId == 0x8DA69C12 && tree.nodes[1].regeneration && tree.nodes[1].scoreOverride == -1000);

    std::string deep;
    for (std::size_t i = 0; i < ts2::want_trees::kMaxDepth + 5; ++i) {
        deep += "<wantNode key=\"node\">";
    }
    for (std::size_t i = 0; i < ts2::want_trees::kMaxDepth + 5; ++i) {
        deep += "</wantNode>";
    }
    deep += "<wantNode key=\"root\"></wantNode>";
    CHECK(ts2::want_trees::parse(deep).nodes.size() == ts2::want_trees::kMaxDepth);
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
    CHECK(n.pack == 1 && n.flags == 0 && (n.seasons == std::array<std::int32_t, 4>{3, 0, 2, 3}));
    const std::vector<std::uint8_t> older = {7, 0, 0, 0, 1, 0, 0, 0, 'D', 9, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0,
                                             5, 0, 0, 0, 0, 0, 0, 0};
    const auto o = ts2::nhoodid::parse(older);
    CHECK(o.id == 9 && o.type == 3 && o.pack == 2 && (o.seasons == std::array<std::int32_t, 4>{1, 2, 3, 0}));
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
void effectsFileReads();

// A particle block of the newest version, each float a new number, so a field read out of place shows.
void particleBlockFieldsRead() {
    std::vector<std::uint8_t> d;
    auto u8 = [&d](std::uint8_t v) { d.push_back(v); };
    auto u16 = [&d](std::uint16_t v) { d.push_back(static_cast<std::uint8_t>(v)); d.push_back(static_cast<std::uint8_t>(v >> 8)); };
    auto u32 = [&d](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    float number = 0;
    auto f32 = [&] { number += 1; u32(std::bit_cast<std::uint32_t>(number)); };
    auto floats = [&](int n) { for (int i = 0; i < n; ++i) f32(); };
    auto text = [&](const std::string& t) {
        u32(static_cast<std::uint32_t>(t.size()));
        d.insert(d.end(), t.begin(), t.end());
    };
    auto curve = [&](int n) {
        u32(static_cast<std::uint32_t>(n));
        floats(n);
    };
    auto vectors = [&](int n) {
        u32(static_cast<std::uint32_t>(n));
        floats(3 * n);
    };
    u16(0);
    u32(1);
    u16(11);
    u32(1);  // the flags' high half
    u32(0x80000002);
    floats(21);  // life 1-2, prewarm 3, delay 4-5, gap 6-7, direction 8-13, speed 14-15, spawn 16-21
    f32();       // ring 22
    curve(1);    // count 23
    f32();       // interval 24
    u16(9);
    f32();       // speedScale 25
    curve(1);    // size 26
    f32();       // sizeVary 27
    curve(1);    // aspect 28
    f32();       // aspectVary 29
    floats(6);   // axes 30-35
    u8(1);
    floats(4);   // rotation scales 36-39
    floats(2);   // offsets 40-41
    curve(1);    // rotation 42
    curve(1);    // second rotation 43
    vectors(1);  // pivots 44-46
    floats(3);   // pivotVary 47-49
    curve(1);    // alpha 50
    f32();       // alphaVary 51
    vectors(1);  // colours 52-54
    floats(3);   // colourVary 55-57
    text("smoke");
    u8(4);
    u8(2);
    u8(5);
    u8(7);
    f32();       // layer 58
    f32();       // framesPerSecond 59
    u8(3);
    u8(6);
    floats(11);  // gravity 60-62, windShare 63, blast 64, blastCentre 65-67, drag 68, stretch 69, screw 70
    u32(0);      // no path points
    for (const std::uint8_t b : std::array<std::uint8_t, 4>{11, 12, 13, 14}) u8(b);
    u32(0);      // no models
    text("");
    floats(3);   // pin offset 71-73
    floats(10);  // 74-83
    floats(6);   // walk 84-89
    u16(1);
    u32(0);      // no wiggles
    u32(0);
    u32(0);      // no emit list
    u32(0);
    text("poof");
    f32();       // 90
    curve(1);    // 91
    u8(0);
    floats(2);   // 92-93
    curve(1);    // 94
    u8(0);
    f32();       // 95
    for (int type = 2; type <= 13; ++type) u32(0);
    u16(3);
    u32(0);
    text("end");
    u32(0xFFFFFFFF);
    const auto f = ts2::effects::parse(d);
    CHECK(f.particles.size() == 1);
    if (f.particles.empty()) {
        return;
    }
    const auto& b = f.particles[0];
    CHECK(b.flags == 0x180000002 && b.life[1] == 2 && b.prewarm == 3 && b.delay[0] == 4 && b.gap[1] == 7);
    CHECK(b.directionLow[0] == 8 && b.directionHigh[2] == 13 && b.speed[1] == 15 && b.spawnLow[0] == 16 && b.spawnHigh[2] == 21);
    CHECK(b.ring == 22 && b.count.at(0) == 23 && b.interval == 24 && b.burstLimit == 9 && b.speedScale == 25);
    CHECK(b.size.at(0) == 26 && b.sizeVary == 27 && b.aspect.at(0) == 28 && b.aspectVary == 29 && b.rotationAxis[0] == 30);
    CHECK(b.rotationScale[0] == 36 && b.rotationOffset == 40 && b.rotation.at(0) == 42);
    CHECK(b.pivots.size() == 1 && b.pivots[0][0] == 44 && b.pivotVary[2] == 49 && b.alpha.at(0) == 50 && b.alphaVary == 51);
    CHECK(b.colours.size() == 1 && b.colours[0][2] == 54 && b.colourVary[0] == 55 && b.texture == "smoke");
    CHECK(b.atlasColumns == 4 && b.atlasRows == 2 && b.alignment == 5 && b.blend == 7 && b.layer == 58);
    CHECK(b.framesPerSecond == 59 && b.firstFrame == 3 && b.frameCycle == 6);
    CHECK(b.gravity[0] == 60 && b.windShare == 63 && b.blast == 64 && b.blastCentre[2] == 67 && b.drag == 68 && b.stretch == 69);
    CHECK((b.screenEdge == std::array<std::uint8_t, 4>{11, 12, 13, 14}));
    CHECK(b.walk.retime[0] == 84 && b.walk.speed[1] == 87 && b.walk.turn == 88 && b.walk.leastTurn == 89 && b.deathEffect == "poof");
}

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
    u16(2);
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
    // Curves blend their samples linearly over 0..1 and give 0 past either end.
    const auto& yaw = f.cameras[0].curves[5];
    CHECK(yaw.at(0.5f) == 5 && yaw.at(1) == 10 && yaw.at(-1) == 0 && yaw.at(2) == 0 && f.cameras[0].curves[8].at(0.3f) == 2 && ts2::effects::Curve{}.at(0.5f) == 0);
    // A packed curve is a sine, at any time.
    const ts2::effects::Curve sine{{2, 3, 0.5f, 1}, true};
    CHECK(std::abs(sine.at(0.5f) - 1) < 1e-5f && std::abs(sine.at(1.5f) - (2 * std::sin(3.0f) + 1)) < 1e-5f);
    // A record plays from its own level up, and a name with no record at or below the level plays nothing.
    CHECK(f.find("nhoodmotion", 2) && f.find("nhoodmotion", 3) && !f.find("nhoodmotion", 1));
    ts2::effects::File levels;
    levels.effects.resize(4);
    levels.effects[0].level = 1;
    levels.effects[1].level = 3;
    levels.effects[2].level = 0;
    levels.effects[3].level = 2;
    levels.names["fx"] = {0, 1, 2};
    levels.names["bad"] = {99};
    CHECK(levels.find("FX", 1) == &levels.effects[0] && levels.find("fx", 2) == &levels.effects[0] && levels.find("fx") == &levels.effects[1]);
    CHECK(!levels.find("fx", 0) && !levels.find("bad"));
    // A later file replaces the same name and level, keeps other levels, and its components keep their blocks.
    ts2::effects::File later;
    later.blockCounts[0] = 1;
    later.particles.resize(1);
    later.effects.resize(2);
    later.effects[0].level = 3;
    later.effects[0].components.resize(3);
    later.effects[0].components[0].type = 1;
    later.effects[0].components[1].type = 1;
    later.effects[0].components[1].index = -1;
    later.effects[0].components[2].type = 0;
    later.effects[0].components[2].index = 5;
    later.effects[1].level = 1;
    later.names["fx"] = {0};
    later.names["new"] = {1, 77};
    levels.blockCounts[0] = 7;
    levels.particles.resize(7);
    ts2::effects::merge(levels, std::move(later));
    CHECK(levels.effects.size() == 6 && levels.particles.size() == 8 && levels.blockCounts[0] == 8);
    CHECK(levels.find("fx") == &levels.effects[4] && levels.find("fx", 2) == &levels.effects[0] && levels.find("new", 1) == &levels.effects[5]);
    CHECK(levels.effects[4].components[0].index == 7 && levels.effects[4].components[1].index == -1 && levels.effects[4].components[2].index == 5);
    CHECK(levels.names["new"].size() == 1);
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
    // A distance's zoom level is the first level at or nearer than it, the nearest below them all.
    const auto levels = ts2::camera::parse("camera \"Live\" Live\nzoom 1 (70 40 25 15 8 3)\n");
    CHECK(levels.size() == 1 && ts2::camera::zoomLevelAt(levels[0], 70) == 0 && ts2::camera::zoomLevelAt(levels[0], 80) == 0);
    CHECK(ts2::camera::zoomLevelAt(levels[0], 13.96f) == 4 && ts2::camera::zoomLevelAt(levels[0], 7.28f) == 5);
    CHECK(ts2::camera::zoomLevelAt(levels[0], 2) == 5 && ts2::camera::zoomLevelAt(ts2::camera::Config{}, 10) == -1);
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

// A face shape (LxNR): points as (offset, texture coordinates) pairs of a shared pool, per mesh key.
// Baby meshes of versions before 5 are left out, and broken data throws.
void faceShapesRead() {
    auto make = [](std::uint32_t version, std::uint16_t badIndex) {
        std::vector<std::uint8_t> d;
        auto u16 = [&](std::uint16_t v) { d.insert(d.end(), {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8)}); };
        auto u32 = [&](std::uint32_t v) {
            for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        };
        auto f32 = [&](float f) { u32(std::bit_cast<std::uint32_t>(f)); };
        u32(version);
        u32(2);
        u32(0x00000005);  // baby, gender 0, face
        u32(0x00000003);  // adult, gender 0, face
        u32(0x6B657965);
        u16(2);
        f32(0.5f), f32(0), f32(-0.25f);
        f32(0.1f), f32(0.2f), f32(0);
        u32(0x6D737472);
        u16(2), u16(0), u16(badIndex);
        u16(2), u16(1), u16(1);
        u32(0x64617461);
        return d;
    };
    const auto shape = ts2::faceshape::parse(make(5, 1));
    const auto* baby = shape.find(5, 0, 0);
    CHECK(shape.meshes.size() == 2 && baby != nullptr && baby->points.size() == 1);
    CHECK(baby != nullptr && baby->points[0].offset[0] == 0.5f && baby->points[0].offset[2] == -0.25f && baby->points[0].uv[1] == 0.2f);
    CHECK(ts2::faceshape::parse(make(4, 1)).find(5, 0, 0) == nullptr);
    bool threw = false;
    try {
        ts2::faceshape::parse(make(5, 7));
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        auto cut = make(5, 1);
        cut.resize(cut.size() - 6);
        ts2::faceshape::parse(cut);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    // A point that isn't a finite number is broken data too.
    threw = false;
    try {
        auto bad = make(5, 1);
        const std::uint32_t nan = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
        // The pool's first float sits after the version, the key count, two keys, the tag and the size.
        for (int i = 0; i < 4; ++i) bad[4 * 5 + 2 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(nan >> (8 * i));
        ts2::faceshape::parse(bad);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
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

// The beach bands run from the beach side's edge, ends and the lot's far edges included.
void beachLocationsLieInBandsOffTheBeachSide() {
    const std::optional<std::uint8_t> side0{std::uint8_t{0}}, side1{std::uint8_t{1}};
    // Tiles 0 to 19 along x and 0 to 29 along y, so the far edges are at 20 and 30.
    const ts2::content::WorldBounds lot{0, 0, 0, 19, 29, 0};
    auto beach = [&](std::optional<std::uint8_t> side, double x, double y, int mask) {
        return ts2::content::isBeachLocation(lot, side, x, y, mask);
    };
    // Side 1: bands along y from 0.
    CHECK(beach(side1, 5, 0, 1) && beach(side1, 5, 8, 1) && !beach(side1, 5, 8.5, 1));
    CHECK(beach(side1, 5, 10, 2) && !beach(side1, 5, 10, 1) && beach(side1, 5, 20, 7));
    CHECK(!beach(side1, 5, 20.5, 7));
    // The far edge x = 20 is on the lot, past it is not.
    CHECK(beach(side1, 20, 3, 1) && !beach(side1, 20.5, 3, 1));
    // Side 0: bands along x back from the far edge.
    CHECK(beach(side0, 20, 3, 1) && beach(side0, 12, 3, 1) && !beach(side0, 11, 3, 1));
    // Not a beach lot.
    CHECK(!beach(std::nullopt, 5, 0, 7));
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

// Data for the QFS tests: words with stray bytes, repeating at many distances as saved records do.
std::vector<std::uint8_t> sampleData(std::size_t size, std::uint32_t seed) {
    static const std::vector<std::string> words = {"the ", "sim ", "lot ", "house ", "garden ", "family ", "plumbob ",
                                                   "neighborhood ", std::string("\x00\x01\x02\x03", 4), "\xff\xfe"};
    std::vector<std::uint8_t> out;
    std::uint32_t x = seed;
    while (out.size() < size) {
        x = x * 1103515245u + 12345u;
        if ((x >> 24) < 16) {
            out.push_back(static_cast<std::uint8_t>(x >> 8));
        } else {
            const std::string& word = words[(x >> 16) % words.size()];
            out.insert(out.end(), word.begin(), word.end());
        }
    }
    out.resize(size);
    return out;
}

std::uint64_t fnv1a(std::span<const std::uint8_t> bytes) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const std::uint8_t b : bytes) {
        h = (h ^ b) * 0x100000001B3ull;
    }
    return h;
}

std::vector<std::uint8_t> fromHex(std::string_view hex) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    }
    return out;
}

// The expected bytes come from the game's coder (re-encoding 8863 resources of real saves gave
// their stored bytes exactly). Over 250,000 bytes the faster search gives a different stream.
void qfsCompressesAsTheGameDoes() {
    const std::pair<std::string, std::string_view> small[] = {
        {"", "0a00000010fb000000fc"},
        {"a", "0b00000010fb000001fd61"},
        {"ab", "0c00000010fb000002fe6162"},
        {"abc", "0d00000010fb000003ff616263"},
        {"aaaa", "0d00000010fb000004010061fc"},
        {"abcdabcdabcdabcd", "1200000010fb000010e061626364880003fc"},
    };
    for (const auto& [text, hex] : small) {
        const std::vector<std::uint8_t> in(text.begin(), text.end());
        const std::vector<std::uint8_t> packed = ts2::qfs::compress(in);
        CHECK(packed == fromHex(hex));
        CHECK(ts2::qfs::decompress(packed) == in);
    }
    const std::tuple<std::size_t, std::uint32_t, std::size_t, std::uint64_t> large[] = {
        {2000, 1, 547, 0xf02b183306782e73ull},
        {200000, 3, 35961, 0x17d6041ee4d6c9cbull},  // longer than the 0x1FFFF window
        {300000, 5, 60368, 0x9d6c992468ea4210ull},  // the faster search
    };
    for (const auto& [size, seed, packedSize, hash] : large) {
        const std::vector<std::uint8_t> in = sampleData(size, seed);
        const std::vector<std::uint8_t> packed = ts2::qfs::compress(in);
        CHECK(packed.size() == packedSize);
        CHECK(fnv1a(packed) == hash);
        CHECK(ts2::qfs::decompress(packed) == in);
    }
}

// Every input packs and unpacks to itself, whatever its bytes: runs, noise, long matches.
void qfsRoundTripsAnyData() {
    std::uint32_t x = 99;
    for (int round = 0; round < 60; ++round) {
        const std::size_t size = static_cast<std::size_t>(round) * round * 37 % 9000;
        std::vector<std::uint8_t> in(size);
        for (std::size_t i = 0; i < size; ++i) {
            x = x * 1664525u + 1013904223u;
            const int kind = round % 4;
            in[i] = kind == 0 ? static_cast<std::uint8_t>(x >> 24) : kind == 1 ? static_cast<std::uint8_t>(i % 3) :
                    kind == 2 ? static_cast<std::uint8_t>((x >> 28) & 1) : static_cast<std::uint8_t>(i < size / 2 ? 7 : x >> 24);
        }
        CHECK(ts2::qfs::decompress(ts2::qfs::compress(in)) == in);
    }
    const std::vector<std::uint8_t> run(5000, 0x41);  // matches longer than the 0x404 limit
    CHECK(ts2::qfs::decompress(ts2::qfs::compress(run)) == run);
    bool refused = false;
    try {
        ts2::qfs::compress(std::vector<std::uint8_t>(ts2::qfs::kMaxInput + 1, 0));
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);
}

// The game keeps a resource compressed only from 50 bytes up and when it shrinks below 0.8.
void resourcesAreStoredAsTheGameStoresThem() {
    const ts2::dbpf::ResourceKey key{0x584F424A, 0xFFFFFFFF, 7, 0};
    const auto shortOne = ts2::dbpf::storeResource(key, std::vector<std::uint8_t>(49, 0));
    CHECK(!shortOne.compressed && shortOne.bytes.size() == 49 && shortOne.uncompressedSize == 49);
    const auto fifty = ts2::dbpf::storeResource(key, std::vector<std::uint8_t>(50, 0));
    CHECK(fifty.compressed && ts2::qfs::decompress(fifty.bytes) == std::vector<std::uint8_t>(50, 0));
    std::vector<std::uint8_t> noise(4000);
    std::uint32_t x = 5;
    for (std::uint8_t& b : noise) {
        x = x * 1664525u + 1013904223u;
        b = static_cast<std::uint8_t>(x >> 24);
    }
    const auto noisy = ts2::dbpf::storeResource(key, noise);
    CHECK(!noisy.compressed && noisy.bytes == noise);
    const auto words = ts2::dbpf::storeResource(key, sampleData(3000, 2));
    CHECK(words.compressed && words.uncompressedSize == 3000);
    // Packing to exactly 0.8 of the size is not smaller, as the game takes the share in single
    // precision.
    std::vector<std::uint8_t> edge;
    for (int rep = 0; rep < 6; ++rep) {
        for (char c = 'a'; c < 'a' + 15; ++c) {
            edge.push_back(static_cast<std::uint8_t>(c));
        }
    }
    for (std::size_t i = 0; i < edge.size(); i += 7) {
        edge[i] ^= 1;
    }
    CHECK(edge.size() == 90 && ts2::qfs::compress(edge).size() == 72);
    CHECK(!ts2::dbpf::storeResource(key, edge).compressed);
}

ts2::dbpf::PackageImage sampleImage(bool instanceHigh) {
    ts2::dbpf::PackageImage image;
    image.header[0x40] = 0x00;
    image.header[0x42] = 0x01;  // a word the game keeps, as family thumbnails carry
    image.header[0x18] = 0x2A;  // a date field, kept as it was
    for (std::uint32_t i = 0; i < 5; ++i) {
        const ts2::dbpf::ResourceKey key{0x584F424A + i % 2, 0xFFFFFFFF, 100 - i, instanceHigh && i == 3 ? 0xFF00u : 0u};
        image.resources.push_back(ts2::dbpf::storeResource(key, sampleData(40 + 300 * i, i + 1)));
    }
    image.dataOrder = {3, 0, 4, 1, 2};
    image.directoryIndexPosition = 2;
    image.directoryDataPosition = 1;
    return image;
}

// A package written from an image reads back as the same resources and writes back to the same
// bytes, as the game's own packed saves do when they are repacked.
void packageImagesWriteBackExactly() {
    for (const bool instanceHigh : {false, true}) {
        const ts2::dbpf::PackageImage image = sampleImage(instanceHigh);
        const std::vector<std::uint8_t> bytes = ts2::dbpf::writeImage(image);
        ts2::dbpf::verifyImage(bytes, image);
        ts2::dbpf::Package package{std::span<const std::uint8_t>(bytes)};
        const auto& h = package.header();
        CHECK(h.majorVersion == 1 && h.minorVersion == 2 && h.indexMajorVersion == 7 && h.holeCount == 0);
        CHECK(h.indexMinorVersion == (instanceHigh ? 2u : 1u));
        CHECK(bytes[0x42] == 0x01 && bytes[0x18] == 0x2A);
        CHECK(package.entries().size() == 6 && package.entries()[2].key.type == ts2::types::kDirectory);
        CHECK(package.entries()[0].offset > package.entries()[4].offset);  // data in the image's order
        CHECK(package.entries()[4].offset == 96);
        const ts2::dbpf::PackageImage again = ts2::dbpf::readImage(bytes);
        CHECK(ts2::dbpf::writeImage(again) == bytes);
        for (std::size_t i = 0; i < image.resources.size(); ++i) {
            CHECK(package.read(package.entries()[i < 2 ? i : i + 1]) ==
                  (image.resources[i].compressed ? ts2::qfs::decompress(image.resources[i].bytes) : image.resources[i].bytes));
        }
    }
    // Without compressed resources there is no directory.
    ts2::dbpf::PackageImage plain;
    plain.resources.push_back(ts2::dbpf::storeResource({1, 2, 3, 0}, std::vector<std::uint8_t>(10, 1)));
    plain.dataOrder = {0};
    const std::vector<std::uint8_t> bytes = ts2::dbpf::writeImage(plain);
    ts2::dbpf::verifyImage(bytes, plain);
    CHECK(ts2::dbpf::Package{std::span<const std::uint8_t>(bytes)}.entries().size() == 1);
}

// Broken images are refused, and a written package that differs from its image fails the check.
void packageImagesRefuseMistakes() {
    auto refused = [](auto&& action) {
        try {
            action();
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    ts2::dbpf::PackageImage twice = sampleImage(false);
    twice.resources[1].key = twice.resources[0].key;
    CHECK(refused([&] { ts2::dbpf::writeImage(twice); }));
    ts2::dbpf::PackageImage badOrder = sampleImage(false);
    badOrder.dataOrder = {0, 0, 1, 2, 3};
    CHECK(refused([&] { ts2::dbpf::writeImage(badOrder); }));
    ts2::dbpf::PackageImage notPacked = sampleImage(false);
    notPacked.resources[4].compressed = true;
    notPacked.resources[4].bytes = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    CHECK(refused([&] { ts2::dbpf::writeImage(notPacked); }));

    const ts2::dbpf::PackageImage image = sampleImage(false);
    std::vector<std::uint8_t> bytes = ts2::dbpf::writeImage(image);
    ts2::dbpf::PackageImage other = image;
    other.resources[2].bytes.back() ^= 1;
    CHECK(refused([&] { ts2::dbpf::verifyImage(bytes, other); }));
    bytes[bytes.size() / 2] ^= 0x55;  // a byte damaged on the way to the disk
    CHECK(refused([&] { ts2::dbpf::verifyImage(bytes, image); }));
    CHECK(refused([&] { ts2::dbpf::readImage(std::vector<std::uint8_t>(40, 0)); }));
}

// A simulator state record of the version the game writes reads back to every byte, and its fields
// sit where the reader of older saves finds them.
void simulatorStateRecordsWriteBack() {
    std::vector<std::uint8_t> bytes(ts2::simi::kRecordSize, 0);
    for (std::size_t i = 0x4C; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(i * 7 + 3);
    }
    auto put = [&bytes](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    put(0x40, ts2::simi::kType);
    put(0x44, ts2::simi::kWrittenVersion);
    put(0xCE, 0xBC598883);
    put(0xD2, 0x4EE46106);
    const auto record = ts2::simi::readRecord(bytes);
    CHECK(record && ts2::simi::writeRecord(*record) == bytes);
    CHECK(record->tickRandomState == 0xBC598883 && record->otherRandomState == 0x4EE46106);
    const ts2::simi::SimulatorState state = ts2::simi::parse(bytes);
    CHECK(state.randomState == record->tickRandomState);
    CHECK(std::equal(state.globals.begin(), state.globals.end(), record->globals.begin()));

    auto changed = bytes;
    put(0x44, 0xD2);  // an older version, written by the game only after its upgrade
    CHECK(!ts2::simi::readRecord(bytes));
    bytes = changed;
    CHECK(!ts2::simi::readRecord(std::span(bytes).first(ts2::simi::kRecordSize - 1)));
    changed[0x48] = 1;  // the reserved word the game always writes as 0
    CHECK(!ts2::simi::readRecord(changed));
}

// A lot info record of the version the game writes reads back to every byte. Texts take a 7-bit
// length, two bytes from 128 characters.
void lotInfoRecordsWriteBack() {
    const std::string description(200, 'd');
    std::vector<std::uint8_t> bytes(64, 0);
    auto u32 = [&bytes](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    bytes.push_back(11);
    bytes.push_back(0);
    u32(3);
    u32(5);
    bytes.insert(bytes.end(), {0, 2, 1});
    u32(0x2023);
    bytes.push_back(8);
    for (const char c : std::string("Test Lot")) bytes.push_back(static_cast<std::uint8_t>(c));
    bytes.insert(bytes.end(), {0xC8, 0x01});  // 200
    bytes.insert(bytes.end(), description.begin(), description.end());
    u32(2);
    u32(std::bit_cast<std::uint32_t>(1.3125f));
    u32(std::bit_cast<std::uint32_t>(0.9375f));
    for (int i = 0; i < 22; ++i) bytes.push_back(static_cast<std::uint8_t>(i + 1));

    const auto record = ts2::nhood::readLotInfo(bytes);
    CHECK(record && ts2::nhood::writeLotInfo(*record) == bytes);
    CHECK(record->width == 3 && record->depth == 5 && record->roadSides == 2 && record->ownFacing == 1 && record->flags == 0x2023);
    CHECK(record->name == "Test Lot" && record->description == description && record->values.size() == 2);

    auto older = bytes;
    older[64] = 8;
    CHECK(!ts2::nhood::readLotInfo(older));
    auto longer = bytes;
    longer.push_back(0);
    CHECK(!ts2::nhood::readLotInfo(longer));
    CHECK(!ts2::nhood::readLotInfo(std::span(bytes).first(bytes.size() - 1)));
    auto padded = bytes;  // the name's length 8 written in two bytes
    padded[81] = 0x88;
    padded.insert(padded.begin() + 82, 0x00);
    CHECK(!ts2::nhood::readLotInfo(padded));
}

// The neighborhood's lot descriptions and Sim descriptions write back as the game writes them, and
// the readers find their fields.
void neighborhoodRecordsWriteBack() {
    ts2::nhood::LotDescriptionRecord lot;
    lot.own.width = 3;
    lot.own.depth = 4;
    lot.own.name = "13 Dead End Lane";
    lot.own.values = {1, 2};
    lot.x = 82;
    lot.y = 47;
    lot.elevation = std::bit_cast<std::uint32_t>(356.625f);
    lot.id = 2;
    lot.facing = 1;
    lot.texture = "lottexture-canvas-desert";
    lot.owner = 5;
    lot.entries = {{1, 2, 3, 4}};
    lot.roomLights = {{std::uint16_t{3}, std::uint16_t{50}}, {std::uint16_t{7}, std::uint16_t{25}}};
    const std::vector<std::uint8_t> lotBytes = ts2::nhood::writeLotDescriptionRecord(lot);
    const ts2::nhood::LotDescription seen = ts2::nhood::parseLotDescription(lotBytes);
    CHECK(seen.version == 0x12 && seen.name == "13 Dead End Lane" && seen.x == 82 && seen.elevation == 356.625f);
    CHECK(seen.id == 2 && seen.texture == "lottexture-canvas-desert" && seen.owner == 5 && seen.roomLightPercent(7) == 25);
    const auto lotAgain = ts2::nhood::readLotDescriptionRecord(lotBytes);
    CHECK(lotAgain && ts2::nhood::writeLotDescriptionRecord(*lotAgain) == lotBytes);

    ts2::sdsc::Record sim;
    sim.personData[0] = 7;
    sim.personData[230] = -3;
    sim.neighborId = 12;
    sim.guid = 0xDEADBEEF;
    sim.relationships = {0x5A, 0x1F7};
    sim.value2 = 9;
    const std::vector<std::uint8_t> simBytes = ts2::sdsc::writeRecord(sim);
    const ts2::sdsc::SimDescription simSeen = ts2::sdsc::parse(simBytes);
    CHECK(simSeen.version == 0x71 && simSeen.personData.size() == 231 && simSeen.personData[230] == -3);
    CHECK(simSeen.neighborId == 12 && simSeen.guid == 0xDEADBEEF);
    const auto simAgain = ts2::sdsc::readRecord(simBytes);
    CHECK(simAgain && simAgain->relationships.size() == 2 && ts2::sdsc::writeRecord(*simAgain) == simBytes);
    auto broken = simBytes;
    broken[simBytes.size() - 9] = 1;  // the byte the game writes as 0
    CHECK(!ts2::sdsc::readRecord(broken));
    CHECK(!ts2::sdsc::readRecord(std::span(simBytes).first(simBytes.size() - 1)));
}

// The small lot records write back as the game writes them.
void smallLotRecordsWriteBack() {
    using namespace ts2::lotrecords;
    const std::vector<std::uint8_t> house = writeHouse({0, ""});
    CHECK(house.size() == 77 && house[64] == 'S' && house[68] == 0x55);
    const auto houseSeen = readHouse(house);
    CHECK(houseSeen && writeHouse(*houseSeen) == house);

    PairTable numbers;
    numbers.value = 0x59D1;
    numbers.numbers = {{1, 0x6D0B2B38}, {3, 0x6D0B2B38}};
    const std::vector<std::uint8_t> numberBytes = writePairTable(numbers);
    const auto numbersSeen = readPairTable(numberBytes, false);
    CHECK(numbersSeen && numbersSeen->numbers.size() == 2 && writePairTable(*numbersSeen) == numberBytes);
    PairTable texts;
    texts.texts = true;
    texts.textPairs = {{"156eb748", "floor_tile"}};
    const std::vector<std::uint8_t> textBytes = writePairTable(texts);
    const auto textsSeen = readPairTable(textBytes, true);
    CHECK(textsSeen && textsSeen->textPairs[0].second == "floor_tile" && writePairTable(*textsSeen) == textBytes);
    CHECK(!readPairTable(textBytes, false));

    WordTable table;
    table.type = kNineWordTableType;
    table.words = {5, 0x41500000, 0x40A00000, 0, 11, 0, 0, 0, 0};
    const std::vector<std::uint8_t> tableBytes = writeWordTable(table);
    CHECK(tableBytes.size() == 112);
    const auto tableSeen = readWordTable(tableBytes);
    CHECK(tableSeen && tableSeen->words == table.words && writeWordTable(*tableSeen) == tableBytes);

    const std::vector<Vertex> vertices = {{1, 0x41200000, 0x41300000, 7}};
    const std::vector<std::uint8_t> layer = writeVertexLayer(vertices);
    const auto layerSeen = readVertexLayer(layer);
    CHECK(layerSeen && layerSeen->size() == 1 && (*layerSeen)[0].value == 7 && writeVertexLayer(*layerSeen) == layer);

    const std::vector<std::uint8_t> flag = writeFlagRecord({true, 1, 1});
    CHECK(flag.size() == 80 && flag[64] == 3);
    const auto flagSeen = readFlagRecord(flag);
    CHECK(flagSeen && flagSeen->flag && writeFlagRecord(*flagSeen) == flag);
    auto broken = flag;
    broken[68] = 2;  // a flag the game never writes
    CHECK(!readFlagRecord(broken));
    broken = house;
    broken.push_back(0);
    CHECK(!readHouse(broken));

    RoutingAggregate aggregate;
    aggregate.packed = 0x10015;
    aggregate.list = {0x15};
    const std::vector<std::uint8_t> aggregateBytes = writeRoutingAggregate(aggregate);
    const auto aggregateSeen = readRoutingAggregate(aggregateBytes);
    CHECK(aggregateSeen && aggregateSeen->list.size() == 1 && writeRoutingAggregate(*aggregateSeen) == aggregateBytes);
    aggregate.objects = 1;
    aggregate.objectBytes = {3, 0, 0, 0, 0x72, 0};
    aggregate.extra = std::array<std::uint16_t, 4>{1, 2, 3, 4};
    const std::vector<std::uint8_t> withObject = writeRoutingAggregate(aggregate);
    const auto withObjectSeen = readRoutingAggregate(withObject);
    CHECK(withObjectSeen && withObjectSeen->extra && (*withObjectSeen->extra)[3] == 4 && withObjectSeen->objectBytes.size() == 6);
    CHECK(writeRoutingAggregate(*withObjectSeen) == withObject);
    broken = aggregateBytes;
    broken.push_back(0);  // bytes with no object to hold them
    CHECK(!readRoutingAggregate(broken));

    SgSystem system;
    system.values = {0x2BBB28, 0x17FF, 2};
    system.pairs = {{1, 2}, {3, 4}};
    const std::vector<std::uint8_t> systemBytes = writeSgSystem(system);
    const auto systemSeen = readSgSystem(systemBytes);
    CHECK(systemSeen && systemSeen->pairs.size() == 2 && writeSgSystem(*systemSeen) == systemBytes);
    CHECK(!readSgSystem(std::span(systemBytes).first(systemBytes.size() - 4)));

    const std::vector<std::uint8_t> counter = writeArchiveCounter(1002);
    CHECK(counter.size() == 8 && counter[0] == 4 && readArchiveCounter(counter) == 1002u);
    NeighborhoodInfo info;
    info.id = "E001";
    info.value1 = 6;
    info.value2 = 1;
    info.products = 0x11;
    info.value4 = 0x10;
    info.bytes = {1, 2, 3, 0};
    const std::vector<std::uint8_t> infoBytes = writeNeighborhoodInfo(info);
    CHECK(infoBytes.size() == 44 && infoBytes[0] == 10 && infoBytes[4] == 4 && infoBytes[8] == 'E');
    const auto infoSeen = readNeighborhoodInfo(infoBytes);
    CHECK(infoSeen && infoSeen->id == "E001" && infoSeen->products == 0x11 && writeNeighborhoodInfo(*infoSeen) == infoBytes);
    const std::vector<std::uint8_t> shortRecord = writeShortRecord(0x300);
    CHECK(shortRecord.size() == 14 && readShortRecord(shortRecord) == std::uint16_t{0x300});
    KeyedLists keyed;
    keyed.lists = {{0x02596F39, {1, 2, 3}}, {7, {}}};
    const std::vector<std::uint8_t> keyedBytes = writeKeyedLists(keyed);
    const auto keyedSeen = readKeyedLists(keyedBytes);
    CHECK(keyedSeen && keyedSeen->lists.size() == 2 && keyedSeen->lists[0].second.size() == 3 && writeKeyedLists(*keyedSeen) == keyedBytes);
    ts2::fami::Family family;
    family.lot = 10;
    family.currentLot = 10;
    family.funds = 20000;
    family.flags = 0x10;
    family.members = {0xAABBCCDD, 0x11223344};
    family.funds2 = 20000;
    const std::vector<std::uint8_t> familyBytes = ts2::fami::write(family);
    const ts2::fami::Family familySeen = ts2::fami::parse(familyBytes);
    CHECK(familySeen.version == 0x55 && familySeen.funds == 20000 && familySeen.members.size() == 2 && familySeen.extraBytes == 0);
    CHECK(ts2::fami::write(familySeen) == familyBytes);
    ts2::fami::History history;
    ts2::fami::HistoryItem item;
    item.shorts1[0] = 3;
    item.values2[7] = -1;
    item.flags[1] = true;
    item.texts = {"Married", ""};
    history.items = {item, item};
    const std::vector<std::uint8_t> historyBytes = ts2::fami::writeHistory(history);
    const auto historySeen = ts2::fami::readHistory(historyBytes);
    CHECK(historySeen && historySeen->items.size() == 2 && historySeen->items[1].texts[0] == "Married");
    CHECK(historySeen && ts2::fami::writeHistory(*historySeen) == historyBytes);
    ts2::famt::FamilyTies ties;
    ties.version = 1;
    ts2::famt::SimTies simTies;
    simTies.sim = 12;
    simTies.ties = {{2, 13}, {4, -1}};
    ties.sims = {simTies};
    const std::vector<std::uint8_t> tiesBytes = ts2::famt::write(ties);
    const ts2::famt::FamilyTies tiesSeen = ts2::famt::parse(tiesBytes);
    CHECK(tiesSeen.version == 1 && tiesSeen.sims.size() == 1 && tiesSeen.sims[0].ties[1].other == -1 && tiesSeen.extraBytes == 0);
    CHECK(ts2::famt::write(tiesSeen) == tiesBytes);
    ts2::ngbh::Record hood;
    hood.terrain = "Desert";
    hood.values[15] = 7;
    ts2::xobj::Token counted;
    counted.guid = 0xF0A9D3DD;
    counted.properties = {1, 0};
    ts2::xobj::Token single;
    single.guid = 0x1438C1DF;
    hood.global.version = ts2::ngbh::kInventoryVersion;
    hood.global.counted = {counted};
    hood.global.singular = {single};
    hood.neighbors = {{std::int16_t{11}, hood.global}, {std::int16_t{-2}, hood.global}};
    hood.flag = 1;
    hood.value3 = 9;
    const std::vector<std::uint8_t> hoodBytes = ts2::ngbh::writeRecord(hood);
    const ts2::ngbh::Neighborhood hoodSeen = ts2::ngbh::parse(hoodBytes);
    CHECK(hoodSeen.version == 0xCB && hoodSeen.terrain == "Desert" && hoodSeen.values.size() == 16 && hoodSeen.extraBytes == 0);
    CHECK(hoodSeen.global.counted.size() == 1 && hoodSeen.neighbors.size() == 2 && hoodSeen.neighbors.at(-2).singular[0].guid == 0x1438C1DF);
    const auto hoodAgain = ts2::ngbh::readRecord(hoodBytes);
    CHECK(hoodAgain && hoodAgain->neighbors[1].first == -2 && ts2::ngbh::writeRecord(*hoodAgain) == hoodBytes);
    ts2::swaf::Record wantsRecord;
    ts2::swaf::Want want;
    want.version = 10;
    want.id = 12;
    want.guid = 0x0F6FDE39;
    want.type = 4;
    want.argument = -3;
    want.value = 250;
    want.tail = {1, 2, 3};
    want.flags = 1;
    ts2::swaf::Want plain = want;
    plain.type = 0;
    plain.argument = 0;
    wantsRecord.maxWants = 4;
    wantsRecord.wants = {want, plain};
    wantsRecord.maxFears = 3;
    wantsRecord.lastValue = -1;
    wantsRecord.history = {{0x0F6FDE39, {want}}};
    const std::vector<std::uint8_t> wantsBytes = ts2::swaf::writeRecord(wantsRecord);
    const ts2::swaf::WantsAndFears wantsSeen = ts2::swaf::parse(wantsBytes);
    CHECK(wantsSeen.version == 6 && wantsSeen.wants.size() == 2 && wantsSeen.wants[0].argument == -3 && wantsSeen.wants[1].value == 250);
    CHECK(wantsSeen.history.size() == 1 && wantsSeen.extraBytes == 0);
    const auto wantsAgain = ts2::swaf::readRecord(wantsBytes);
    CHECK(wantsAgain && wantsAgain->lastValue == -1 && ts2::swaf::writeRecord(*wantsAgain) == wantsBytes);
    std::vector<ts2::propset::Property> props(5);
    props[0] = {ts2::propset::kUintType, "binidx", 1, {}, {}};
    props[1] = {ts2::propset::kStringType, "creatorid", 0, "00000000-0000-0000-0000-000000000000", {}};
    props[2] = {ts2::propset::kBoolType, "flag", 1, {}, {}};
    props[3] = {ts2::propset::kFloatType, "scale", std::bit_cast<std::uint32_t>(0.5f), {}, {}};
    props[4] = {ts2::propset::kStringListType, "names", 0, {}, {"a", "bc"}};
    const std::vector<std::uint8_t> propBytes = ts2::propset::writeBinary(props);
    const auto propText = ts2::propset::parse(propBytes);
    CHECK(propText.at("binidx") == "1" && propText.at("creatorid").size() == 36 && propText.at("flag") == "true");
    const auto propAgain = ts2::propset::readBinary(propBytes);
    CHECK(propAgain && propAgain->size() == 5 && (*propAgain)[4].texts[1] == "bc" && ts2::propset::writeBinary(*propAgain) == propBytes);
    const std::vector<std::uint8_t> dnaIndex = ts2::lotrecords::writeDnaIndex({0x18, 0x1C, 0x10000006});
    CHECK(dnaIndex.size() == 20 && dnaIndex[0] == 1 && dnaIndex[4] == 3 && dnaIndex[8] == 0x18);
    CHECK(ts2::lotrecords::readDnaIndex(dnaIndex) == std::vector<std::uint32_t>({0x18, 0x1C, 0x10000006}));
    auto dnaUnsorted = dnaIndex;
    std::swap(dnaUnsorted[8], dnaUnsorted[12]);
    CHECK(!ts2::lotrecords::readDnaIndex(dnaUnsorted));

    ts2::nhood::TerrainRecord terrain;
    terrain.width = terrain.height = 1;
    terrain.seaLevel = 312.5f;
    terrain.type = "Temperate";
    terrain.heightColumns = terrain.heightRows = 2;
    terrain.heights = {310.0f, 311.0f, 312.0f, 313.0f};
    terrain.extraColumns = terrain.extraRows = 2;
    terrain.extra = {0, 1, 2, 3};
    const std::vector<std::uint8_t> terrainBytes = ts2::nhood::writeTerrainRecord(terrain);
    CHECK(terrainBytes.size() == 33 + (28 + 16) + (28 + 4));
    const ts2::nhood::Terrain terrainSeen = ts2::nhood::parseTerrain(terrainBytes);
    CHECK(terrainSeen.version == 4 && terrainSeen.type == "Temperate" && terrainSeen.heights[3] == 313.0f && terrainSeen.extra[2] == 2);
    const auto terrainAgain = ts2::nhood::readTerrainRecord(terrainBytes);
    CHECK(terrainAgain && ts2::nhood::writeTerrainRecord(*terrainAgain) == terrainBytes);

    ts2::nhood::ObjectsRecord hoodObjects;
    hoodObjects.flora = {{{1, 2, 3}, {0, 0, 4, 4}, 90.0f, 0x1234}};
    ts2::nhood::RoadRecord road;
    road.center = {5, 6, 7};
    road.piece = 0x00010002;
    // The older reader skips road parts, so the first write has none.
    hoodObjects.roads = {road};
    hoodObjects.bridges = {ts2::nhood::BridgeRecord{road, {1, 1, 1}, {0, 0, 0.5f, 0.5f}, {2, 2, 2}}};
    hoodObjects.decorations = {{{8, 9, 10}, {}, 45.0f, 0x5678}};
    const std::vector<std::uint8_t> objectsBytes = ts2::nhood::writeObjectsRecord(hoodObjects);
    const ts2::nhood::Objects objectsSeen = ts2::nhood::parseObjects(objectsBytes);
    CHECK(objectsSeen.version == 0xCB && objectsSeen.flora.size() == 1 && objectsSeen.flora[0].type == 0x1234 && objectsSeen.flora[0].angle == 90.0f);
    CHECK(objectsSeen.decorations.size() == 1 && objectsSeen.decorations[0].type == 0x5678 && objectsSeen.decorations[0].angle == 45.0f);
    hoodObjects.roads[0].parts = {ts2::nhood::RoadPart{9, {}}};
    const auto objectsAgain = ts2::nhood::readObjectsRecord(ts2::nhood::writeObjectsRecord(hoodObjects));
    CHECK(objectsAgain && objectsAgain->roads[0].parts.size() == 1 && objectsAgain->bridges[0].rotation[3] == 0.5f &&
          ts2::nhood::writeObjectsRecord(*objectsAgain) == ts2::nhood::writeObjectsRecord(hoodObjects));

    ts2::archive::Record archive;
    archive.keys = {{0x49596978, 2, 0}};
    ts2::archive::Object archived;
    archived.guid = 0x2C258FE8;
    archived.value = 7;
    ts2::archive::Part archivedPart;
    archivedPart.state.version = ts2::xobj::kWrittenVersion;
    archivedPart.state.tail.emplace();
    archivedPart.state.tail->relationshipVersion = 3;
    ts2::archive::NamedValues sales;
    sales.key = 1;
    sales.name = "Sales Info";
    ts2::archive::Value nested;
    nested.type = 5;
    nested.pairs = {ts2::archive::Value{2, 9, {}, {}}, ts2::archive::Value{3, 1, {}, {}}};
    sales.pairs = {ts2::archive::Value{1, 3, {}, {}}, ts2::archive::Value{4, 0, "Grimoire", {}}, ts2::archive::Value{0, 0, {}, {}}, nested};
    archivedPart.values = {sales};
    archived.parts = {archivedPart, archivedPart};
    archive.objects = {archived, archived};
    const std::vector<std::uint8_t> archiveBytes = ts2::archive::writeRecord(archive);
    CHECK(archiveBytes.size() > 40 && archiveBytes[0] == 4 && archiveBytes[4] == 0xD8 && archiveBytes[8] == 1);
    const auto archiveAgain = ts2::archive::readRecord(archiveBytes);
    CHECK(archiveAgain && archiveAgain->objects.size() == 2 && archiveAgain->objects[1].parts.size() == 2);
    CHECK(archiveAgain && archiveAgain->objects[0].parts[1].values[0].pairs[1].text == "Grimoire" &&
          archiveAgain->objects[0].parts[0].values[0].pairs[3].pairs[0].word == 9);
    auto archiveGuid = archiveBytes;
    archiveGuid[24] ^= 1;  // the first object's GUID in the list no longer matches
    CHECK(!ts2::archive::readRecord(archiveGuid));
    ts2::str::Record strRecord;
    strRecord.name = "Sim";
    strRecord.entries = {{0xC8, "z", ""}, {0, "none", ""}, {1, "Ilia", "d"}, {1, "", ""}, {2, "Ilia", ""}};
    const std::vector<std::uint8_t> strBytes = ts2::str::writeRecord(strRecord);
    CHECK(strBytes.size() == 68 + 4 + 7 + 8 + 3 + 7 && strBytes[3] == 0 && strBytes[64] == 0xFD && strBytes[65] == 0xFF && strBytes[66] == 5);
    const auto strAgain = ts2::str::readRecord(strBytes);
    CHECK(strAgain && strAgain->name == "Sim" && strAgain->entries.size() == 5 && strAgain->entries[2].description == "d");
    CHECK(ts2::str::parse(strBytes).forLanguage(1).size() == 2);
    auto strOutOfOrder = strRecord;
    std::swap(strOutOfOrder.entries[0], strOutOfOrder.entries[4]);
    bool strRefused = false;
    try {
        ts2::str::writeRecord(strOutOfOrder);
    } catch (const std::runtime_error&) {
        strRefused = true;
    }
    CHECK(strRefused);
    auto strTail = strBytes;
    strTail.push_back(0);
    CHECK(!ts2::str::readRecord(strTail));
    std::vector<ts2::propset::Property> xmlProps(6);
    xmlProps[0] = {ts2::propset::kStringType, "a<&>\"'", 0, "x&y", {}};
    xmlProps[1] = {ts2::propset::kUintType, "u", 4000000000u, {}, {}};
    xmlProps[2] = {ts2::propset::kIntType, "i", static_cast<std::uint32_t>(-7), {}, {}};
    xmlProps[3] = {ts2::propset::kFloatType, "f", std::bit_cast<std::uint32_t>(0.94f), {}, {}};
    xmlProps[4] = {ts2::propset::kBoolType, "b", 1, {}, {}};
    xmlProps[5] = {ts2::propset::kStringListType, "s", 0, {}, {"p", ""}};
    const std::vector<std::uint8_t> xmlBytes = ts2::propset::writeXml(xmlProps);
    const std::string xmlText(xmlBytes.begin(), xmlBytes.end());
    CHECK(xmlText == "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<cGZPropertySetString>"
                     "\n  <AnyString key=\"a&lt;&amp;&gt;&quot;&apos;\" type=\"0x0b8bea18\">x&amp;y</AnyString>"
                     "\n  <AnyUint32 key=\"u\" type=\"0xeb61e4f7\">4000000000</AnyUint32>"
                     "\n  <AnySint32 key=\"i\" type=\"0x0c264712\">-7</AnySint32>"
                     "\n  <AnyFloat32 key=\"f\" type=\"0xabc78708\">0.9399999976</AnyFloat32>"
                     "\n  <AnyBoolean key=\"b\" type=\"0xcba908e1\">true</AnyBoolean>"
                     "\n  <AnyStringSet key=\"s\" type=\"0xad9a861e\">\n    <Element>p</Element>\n    <Element></Element>\n  </AnyStringSet>"
                     "\n</cGZPropertySetString>");
    const auto xmlAgain = ts2::propset::readXml(xmlBytes);
    CHECK(xmlAgain && xmlAgain->size() == 6 && (*xmlAgain)[0].name == "a<&>\"'" && (*xmlAgain)[2].word == static_cast<std::uint32_t>(-7));
    CHECK(ts2::propset::parse(xmlBytes).at("f") == "0.9399999976");
    const std::vector<std::uint8_t> xmlEmpty = ts2::propset::writeXml({});
    CHECK(std::string(xmlEmpty.begin(), xmlEmpty.end()).ends_with("<cGZPropertySetString/>") && ts2::propset::readXml(xmlEmpty));
    auto xmlSpaced = xmlBytes;
    xmlSpaced.insert(xmlSpaced.end() - 24, ' ');
    CHECK(!ts2::propset::readXml(xmlSpaced));
    std::vector<ts2::propset::Property> floats(1, {ts2::propset::kFloatType, "f", 0, {}, {}});
    for (const auto& [value, text] : std::initializer_list<std::pair<float, const char*>>{
             {0.0f, "0"}, {1.0f, "1"}, {-0.5f, "-0.5"}, {0.1f, "0.1000000014"}, {1e-12f, "0.0000000000009999999960"},
             {123456.5f, "123456.5"}, {-2.25f, "-2.25"}}) {
        floats[0].word = std::bit_cast<std::uint32_t>(value);
        const auto bytes = ts2::propset::writeXml(floats);
        CHECK(ts2::propset::parse(bytes).at("f") == text);
    }
    floats[0].word = std::bit_cast<std::uint32_t>(1e7f);
    bool floatRefused = false;
    try {
        ts2::propset::writeXml(floats);
    } catch (const std::runtime_error&) {
        floatRefused = true;
    }
    CHECK(floatRefused);
    auto historyBroken = historyBytes;
    historyBroken[12] = 1;  // the byte the game writes as 0
    CHECK(!ts2::fami::readHistory(historyBroken));
    auto counterBroken = counter;
    counterBroken[0] = 3;
    CHECK(!readArchiveCounter(counterBroken));
}

// String maps, roofs, the lot texture, the object type table and the world database write back as
// the game writes them, and their readers find the fields.
void lotWideRecordsWriteBack() {
    std::map<std::uint16_t, ts2::stringmap::Entry> patterns;
    patterns[3] = {"15cbf1f5", 4};
    patterns[1] = {"blank", 0};
    const std::vector<std::uint8_t> map = ts2::stringmap::write(patterns);
    const auto mapSeen = ts2::stringmap::parse(map);
    CHECK(mapSeen.size() == 2 && mapSeen.at(3).name == "15cbf1f5" && mapSeen.at(3).uses == 4);
    CHECK(ts2::stringmap::write(mapSeen) == map);
    // The game orders them by name ignoring case, and keeps names that differ only in case as one.
    patterns[0] = {"Zeta", 1};
    patterns[2] = {"alpha", 1};
    const std::vector<std::uint8_t> sorted = ts2::stringmap::write(patterns);
    CHECK(sorted[64 + 8 + 11 + 4 + 1] == '1' && sorted[64 + 8 + 11 + 4 + 1 + 8 + 6 + 1] == 'a');
    patterns[4] = {"ZETA", 1};
    bool refused = false;
    try {
        ts2::stringmap::write(patterns);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    CHECK(refused);

    ts2::roofs::Roof roof;
    roof.id = 9;
    roof.x0 = 21.0f;
    roof.level1 = 2;
    roof.slope = 0.5f;
    roof.extra = 7;
    const std::vector<std::uint8_t> roofs = ts2::roofs::write({roof});
    const auto roofsSeen = ts2::roofs::parse(roofs);
    CHECK(roofs.size() == 64 + 12 + 44 && roofsSeen.size() == 1 && roofsSeen[0].x0 == 21.0f && roofsSeen[0].extra == 7);
    CHECK(ts2::roofs::write(roofsSeen) == roofs);

    ts2::terrain::LotTexture texture;
    texture.width = 30;
    texture.height = 50;
    texture.canvas = "lottexture-canvas-grass";
    texture.paints = {"lottexture-dirt", "lottexture-sand"};
    const std::vector<std::uint8_t> textureBytes = ts2::terrain::writeLotTexture(texture);
    const auto textureSeen = ts2::terrain::parseLotTexture(textureBytes);
    CHECK(textureSeen.version == 7 && textureSeen.height == 50 && textureSeen.paints.size() == 2 && textureSeen.paints[1] == "lottexture-sand");
    CHECK(ts2::terrain::writeLotTexture(textureSeen) == textureBytes);

    ts2::objt::ObjectTypeTable table;
    ts2::objt::Entry entry;
    entry.guid = 0xB03F156F;
    entry.index = 5;
    entry.kind = ts2::objt::kPersonKind;
    entry.name = "Person";
    entry.flag = 1;
    entry.secondGuid = entry.guid;
    table.entries.push_back(entry);
    const std::vector<std::uint8_t> tableBytes = ts2::objt::write(table);
    const auto tableSeen = ts2::objt::parse(tableBytes);
    CHECK(tableSeen.version == 0x55 && tableSeen.entries.size() == 1 && tableSeen.find(5) && tableSeen.find(5)->name == "Person");
    CHECK(ts2::objt::write(tableSeen) == tableBytes);

    ts2::worlddb::Record world;
    world.values = {0, 0, -1, 29, 3, 4};
    const std::vector<std::uint8_t> worldBytes = ts2::worlddb::writeRecord(world);
    const auto worldSeen = ts2::worlddb::readRecord(worldBytes);
    CHECK(worldSeen && worldSeen->values[3] == 29 && !worldSeen->text && ts2::worlddb::writeRecord(*worldSeen) == worldBytes);
    world.text = "name";
    const std::vector<std::uint8_t> withText = ts2::worlddb::writeRecord(world);
    const auto withTextSeen = ts2::worlddb::readRecord(withText);
    CHECK(withTextSeen && withTextSeen->text == "name" && ts2::worlddb::writeRecord(*withTextSeen) == withText);
    auto broken = worldBytes;
    broken[68] = 6;  // an older version
    CHECK(!ts2::worlddb::readRecord(broken));
    broken = worldBytes;
    broken.back() = 2;  // a flag the game never writes
    CHECK(!ts2::worlddb::readRecord(broken));
}

// The wall graph, wall layers and fence posts write back as the game writes them.
void wallRecordsWriteBack() {
    ts2::wallgraph::WallGraph graph;
    graph.unknown = {1, 2};
    graph.firstLevel = -1;
    graph.width = 64;
    graph.height = 64;
    graph.levels = 3;
    graph.counters = {5, 6};
    graph.points = {{1, 10.0f, 12.5f, 0}, {2, 14.0f, 12.5f, 0}};
    graph.rooms = {0, 3};
    graph.walls = {{7, 1, 3, 2, 0}};
    const std::vector<std::uint8_t> bytes = ts2::wallgraph::write(graph);
    const ts2::wallgraph::WallGraph seen = ts2::wallgraph::parse(bytes);
    CHECK(seen.classId == ts2::wallgraph::kType && seen.version == 3 && seen.firstLevel == -1 && seen.counters[1] == 6);
    CHECK(seen.points.size() == 2 && seen.points[1].x == 14.0f && seen.walls.size() == 1 && seen.walls[0].leftRoom == 3);
    CHECK(ts2::wallgraph::write(seen) == bytes);

    const std::vector<ts2::walllayer::Entry> coverings = {{7, 0x12345678, 2, 9}};
    const std::vector<std::uint8_t> layer = ts2::walllayer::write(coverings);
    const auto coveringsSeen = ts2::walllayer::parse(layer);
    CHECK(layer.size() == 64 + 8 + 11 + 4 + 12 && coveringsSeen.size() == 1 && coveringsSeen[0].right == 9);
    CHECK(ts2::walllayer::write(coveringsSeen) == layer);

    const std::vector<ts2::fencepostlayer::Post> posts = {{1, 3.0f, 4.0f, 0xABCDEF01}};
    const std::vector<std::uint8_t> postBytes = ts2::fencepostlayer::write(posts);
    const auto postsSeen = ts2::fencepostlayer::parse(postBytes);
    CHECK(postsSeen.size() == 1 && postsSeen[0].y == 4.0f && postsSeen[0].fence == 0xABCDEF01);
    CHECK(ts2::fencepostlayer::write(postsSeen) == postBytes);
}

// Lot grids write back as the game writes them, and the grid readers find their cells.
void lotGridsWriteBack() {
    using namespace ts2::lotgrid;
    Grid grid;
    grid.width = 2;
    grid.height = 3;
    grid.levels = 2;
    for (std::uint8_t i = 0; i < 2 * 3 * 2 * 2; ++i) {
        grid.cells.push_back(i);
    }
    const std::vector<std::uint8_t> bytes = writeGrid(grid);
    const ts2::array3d::Array3D seen = ts2::array3d::parse(bytes);
    CHECK(seen.classId == k3DType && seen.version == 1 && seen.levels == 2 && seen.elementSize == 2);
    CHECK(seen.i16(1, 1, 2, 0) == (22 | 23 << 8));
    const auto again = readGrid(bytes);
    CHECK(again && again->threeD && writeGrid(*again) == bytes);

    Grid flat;
    flat.threeD = false;
    flat.width = 2;
    flat.height = 2;
    flat.cells = {1, 2, 3, 4};
    const std::vector<std::uint8_t> flatBytes = writeGrid(flat);
    const ts2::terrain::Array2D flatSeen = ts2::terrain::parseArray2D(flatBytes);
    CHECK(flatSeen.width == 2 && flatSeen.elementSize == 1);
    const auto flatAgain = readGrid(flatBytes);
    CHECK(flatAgain && !flatAgain->threeD && flatAgain->levels == 1 && writeGrid(*flatAgain) == flatBytes);

    auto broken = bytes;
    broken[68] = 2;  // a version the game does not write
    CHECK(!readGrid(broken));
    broken = bytes;
    broken[10] = 1;  // the name the game writes as zeros
    CHECK(!readGrid(broken));
    broken = bytes;
    broken[73] = 'C';  // another class name
    CHECK(!readGrid(broken));
    CHECK(!readGrid(std::span(bytes).first(80)));
}

// Relationships write back as the game writes them: version 2, the count, then the values.
void relationshipRecordsWriteBack() {
    using namespace ts2::srel;
    const Record record{{48, -20, 3}};
    const std::vector<std::uint8_t> bytes = writeRecord(record);
    CHECK(bytes.size() == 20 && bytes[0] == 2 && bytes[4] == 3);
    const Relationship seen = parse(bytes);
    CHECK(seen.version == 2 && seen.values == record.values && seen.extraBytes == 0);
    const auto again = readRecord(bytes);
    CHECK(again && writeRecord(*again) == bytes);
    auto broken = bytes;
    broken[0] = 3;  // an older game's version
    CHECK(!readRecord(broken));
    broken = bytes;
    broken.push_back(0);  // bytes after the values, which the game never writes
    CHECK(!readRecord(broken));
    CHECK(!readRecord(std::span(bytes).first(bytes.size() - 1)));
}

// Saved scene records write back as the game writes them, and the scene reader finds the joints and
// animations in what the writer gives.
void sceneRecordsWriteBack() {
    using namespace ts2::cobject;
    Record record;
    record.chain = Chain::Person;
    record.object.model = "afbodyhairup_cres";
    record.object.materials.push_back({"body", 7, {{"top", "red"}, {"bottom", "blue"}}});
    record.object.place[6] = std::bit_cast<std::uint32_t>(1.0f);
    PosedJoint joint;
    joint.name = "seat";
    joint.place = {std::bit_cast<std::uint32_t>(0.5f), 0, 0, 0, 0, 0, std::bit_cast<std::uint32_t>(1.0f)};
    record.object.joints.push_back(joint);
    record.object.blends.push_back({"smile", "face", std::bit_cast<std::uint32_t>(0.25f)});
    record.animatable.scale = std::bit_cast<std::uint32_t>(1.0f);
    record.animatable.places.emplace();

    Animation clip;
    clip.kind = Animation::Kind::Clip;
    clip.request.flags = 1;
    clip.request.name = "a-idle-neutral";
    clip.request.type = "ovl";
    clip.request.category = 3;
    clip.request.eventObject = -2;
    clip.request.eventTree = 0x2000;
    clip.request.rate = std::bit_cast<std::uint32_t>(1.0f);
    clip.request.eventObjectGuid = 0x7F05CC2C;
    clip.request.part.emplace(-5, 9);
    clip.instance.state = 1;
    clip.instance.startTick = 100;
    clip.instance.savedTick = 350;
    clip.instance.layer = 2;
    clip.instance.weight = std::bit_cast<std::uint32_t>(1.0f);
    Curve curve;
    curve.segments.push_back({0, {}, std::bit_cast<std::uint32_t>(200.0f)});
    curve.segments.push_back({1, {}, std::bit_cast<std::uint32_t>(600.0f)});
    clip.instance.curve = curve;
    clip.resource = "a-idle-neutral_anim";
    record.animatable.animations.push_back(clip);
    record.animatable.animations.push_back(Animation{});
    Animation attached = clip;
    attached.kind = Animation::Kind::Attached;
    attached.request.part.reset();
    attached.instance.curve.reset();
    attached.resource = "hand";
    attached.attached[11] = 4;
    record.animatable.animations.push_back(attached);
    record.locomotable.sets.push_back({3, {{"walk", std::bit_cast<std::uint32_t>(0.5f)}}});

    const std::vector<std::uint8_t> bytes = writeRecord(record);
    const SavedObject seen = parse(bytes);
    CHECK(seen.outerTag == kPersonTag && seen.classes.size() == 4 && seen.classes[3] == "cObject");
    CHECK(seen.joints.size() == 1 && seen.joints[0].name == "seat" && seen.joints[0].translation[0] == 0.5f);
    CHECK(seen.animations.size() == 2 && seen.animations[0].name == "a-idle-neutral" && seen.animations[0].flipped);
    CHECK(seen.animations[0].type == "ovl" && seen.animations[0].category == 3 && seen.animations[0].eventObject == -2);
    CHECK(seen.animations[0].eventTree == 0x2000 && seen.animations[0].eventObjectGuid == 0x7F05CC2C);
    CHECK(seen.animations[0].fadeInMs == 200.0f && seen.animations[0].lengthMs == 800.0f && seen.animations[0].startedMsAgo == 250);
    CHECK(seen.animations[1].kind == SavedAnimation::Kind::Attached && !seen.animations[1].lengthMs);
    const auto again = readRecord(bytes);
    CHECK(again && writeRecord(*again) == bytes && again->animatable.animations[1].kind == Animation::Kind::None);

    Record plain;
    plain.object.model = "chair";
    const std::vector<std::uint8_t> plainBytes = writeRecord(plain);
    CHECK(parse(plainBytes).classes.size() == 1);
    const auto plainAgain = readRecord(plainBytes);
    CHECK(plainAgain && plainAgain->chain == Chain::Object && writeRecord(*plainAgain) == plainBytes);

    auto broken = plainBytes;
    broken[68] = 0x10;  // a cObject version the game no longer writes
    CHECK(!readRecord(broken));
    broken = plainBytes;
    broken[3] = 1;  // the name the game writes as zeros
    CHECK(!readRecord(broken));
    broken = plainBytes;
    broken.push_back(0);
    CHECK(!readRecord(broken));
    CHECK(!readRecord(std::span(plainBytes).first(plainBytes.size() - 1)));
    // The model's name length written in two bytes, which the game never does. It follows the
    // four class headers.
    constexpr std::size_t modelAt = 64 + 16 + 21 + 20 + 16;
    CHECK(bytes[modelAt] == record.object.model.size());
    std::vector<std::uint8_t> longLength = bytes;
    longLength[modelAt] |= 0x80;
    longLength.insert(longLength.begin() + static_cast<std::ptrdiff_t>(modelAt) + 1, 0);
    CHECK(!readRecord(longLength));
}

// Saved camera views write back as the game writes them: a used slot of an orbiting camera takes 43
// bytes, of the free camera 41, an empty one 1. The neighborhood's reader finds its fields there.
void savedCameraRecordsWriteBack() {
    using namespace ts2::savedcamera;
    Record orbit;
    OrbitView view;
    view.zoomStep = 2;
    view.byte1 = 1;
    view.distance = std::bit_cast<std::uint32_t>(650.0f);
    view.orientation = 3;
    view.byte2 = 1;
    view.yaw = std::bit_cast<std::uint32_t>(1.5f);
    view.angle = std::bit_cast<std::uint32_t>(0.25f);
    view.target = {std::bit_cast<std::uint32_t>(10.0f), std::bit_cast<std::uint32_t>(20.0f), std::bit_cast<std::uint32_t>(3.0f)};
    view.value1 = 1;
    view.value2 = -4;
    orbit.orbit[0] = view;
    std::vector<std::uint8_t> bytes = writeRecord(orbit);
    CHECK(bytes.size() == 115 && bytes[64] == kOrbitKind && bytes[65] == kSlots && bytes[66] == 1);
    const auto hood = ts2::nhood::parseSavedCamera(bytes);
    CHECK(hood && hood->distance == 650.0f && hood->yaw == 1.5f && hood->target[1] == 20.0f);
    orbit.orbit[3] = view;
    bytes = writeRecord(orbit);
    CHECK(bytes.size() == 64 + 2 + 43 * 2 + 5);
    const auto again = readRecord(bytes);
    CHECK(again && writeRecord(*again) == bytes && again->orbit[3] && !again->orbit[1]);

    Record free;
    free.kind = kFreeKind;
    FreeView f;
    f.position = {1, 2, 3};
    f.rotation = {4, 5, 6, 7};
    f.value = 8;
    f.value1 = 9;
    f.value2 = 10;
    free.free[1] = f;
    const std::vector<std::uint8_t> freeBytes = writeRecord(free);
    CHECK(freeBytes.size() == 64 + 2 + 41 + 6);
    const auto freeAgain = readRecord(freeBytes);
    CHECK(freeAgain && writeRecord(*freeAgain) == freeBytes && freeAgain->free[1]->rotation[3] == 7);

    auto broken = bytes;
    broken[64] = 2;  // another kind
    CHECK(!readRecord(broken));
    broken = bytes;
    broken[65] = 6;
    CHECK(!readRecord(broken));
    broken = bytes;
    broken[66] = 2;  // a used byte the game never writes
    CHECK(!readRecord(broken));
    broken = bytes;
    broken[5] = 1;  // the name the game writes as zeros
    CHECK(!readRecord(broken));
    broken = bytes;
    broken.push_back(0);
    CHECK(!readRecord(broken));
    CHECK(!readRecord(std::span(bytes).first(bytes.size() - 1)));
}

// The object map's two records write back as the game writes them, and the reader of saves finds
// the same objects and waits in instance 1.
void objectMapRecordsWriteBack() {
    using namespace ts2::objm;
    MapRecord map;
    map.objects = {{3, 0}, {1, 2}, {7, 1}};
    map.waits = {5, 0, 0x00010009, 0, 0, 0, 2};
    map.waitsWithObject = 3;
    map.buildBuyBlockers = {7, 7};
    map.subscriptions = {{0xACFD5984, {{3, "Test - Tree"}}}, {0x709AE21B, {}}};
    const std::vector<std::uint8_t> bytes = writeMapRecord(map);
    const auto again = readMapRecord(bytes);
    CHECK(again && writeMapRecord(*again) == bytes);
    CHECK(again->subscriptions[0].subscribers[0].tree == "Test - Tree" && again->buildBuyBlockers.size() == 2);
    const ObjectMap parsed = parse(bytes);
    CHECK(parsed.version == kWrittenVersion && parsed.typeIndexByObject.at(1) == 2 && parsed.typeIndexByObject.at(7) == 1);
    CHECK(parsed.waitTicks.size() == 7 && parsed.waitTicks[0] == 5 && parsed.waitTicks[2] == 9);

    auto broken = bytes;
    broken[68] = 0xD2;  // an older version
    CHECK(!readMapRecord(broken));
    broken = bytes;
    broken[64] = 1;  // the first word the game writes as 0
    CHECK(!readMapRecord(broken));
    broken = bytes;
    broken.push_back(0);
    CHECK(!readMapRecord(broken));
    CHECK(!readMapRecord(std::span(bytes).first(bytes.size() - 1)));

    GroupsRecord groups;
    groups.groups = {{5, {1, 2, 3}}, {-1, {}}, {2, {9}}};
    const std::vector<std::uint8_t> groupBytes = writeGroupsRecord(groups);
    const auto groupsAgain = readGroupsRecord(groupBytes);
    CHECK(groupsAgain && writeGroupsRecord(*groupsAgain) == groupBytes);
    const ObjectGroups parsedGroups = parseGroups(groupBytes);
    CHECK(parsedGroups.version == 2 && parsedGroups.objectsByKey.at(5).size() == 3);
    auto oldGroups = groupBytes;
    oldGroups[64] = 1;
    CHECK(!readGroupsRecord(oldGroups));
}

// Saved objects of the version the game writes read back to the same record and the same bytes,
// for plain objects, objects with a material block and Sims.
void savedObjectsWriteBack() {
    using namespace ts2::xobj;
    SavedObject o;
    o.version = kWrittenVersion;
    o.header.location = {12.5f, 30.0f};
    o.header.value1 = 2;
    o.header.value2 = 1;
    o.header.value4 = 77;
    o.attributes = {1, -2, 3};
    o.semiAttributes = {4};
    o.temps = {1, 2, 3, 4, 5, 6, 7, 8};
    o.data[0x19] = 600;
    o.tokenSource = {9, 9, 9, 9, 9, 9, 9, 9};
    Tail t;
    t.token.guid = 0x12345678;
    t.token.properties = {5, 6};
    t.arrays = {{1, 2}, {}};
    t.shorts = {3};
    t.strings = {"one", std::string(130, 'x')};
    t.stackValue = 1;
    StackFrame f;
    f.objectId = 3;
    f.treeId = 0x1001;
    f.params = {1, 2, 3, 4};
    f.locals = {9};
    f.codeOwner = 2;
    t.stack = {f, f};
    t.relationshipVersion = 3;
    t.relationshipIds = {10, 11};
    t.slots.marker = -101;
    t.slots.values = {0, 5};
    t.effectFlag = 1;
    t.effectVersion = 2;
    t.effects = {{"glow", "slot", 1, 2, 3}};
    t.materials = {{"res", "subset", "mat"}};
    TimerEntry timer;
    timer.value3 = 4;
    t.timers = {timer};
    o.tail = t;
    for (const RecordKind kind : {RecordKind::Object, RecordKind::ObjectWithMaterial, RecordKind::Person}) {
        SavedObject k = o;
        if (kind == RecordKind::ObjectWithMaterial) {
            k.tail->materialBlock.assign(26, 0x5A);
        }
        if (kind == RecordKind::Person) {
            PersonPart p;
            p.version = 2;
            p.motives[0] = 50.0f;
            p.personData.assign(personDataCountForVersion(kWrittenVersion), 3);
            k.person = p;
            SavedInteraction running;
            running.number = 41;
            running.id = 7;
            running.name = "Eat";
            k.person->state.running = running;
            k.person->state.queue = {running, running};
            k.person->state.runs = {{12, 3, true}};
            k.person->state.entries = {{2, 0.5f, -1.0f}};
            k.person->state.runningSub = running;
            k.person->state.text = "text";
            k.person->state.value4 = 9;
        }
        const std::vector<std::uint8_t> bytes = write(k, kind);
        const SavedObject again = parse(bytes, kWrittenVersion, kind);
        CHECK(write(again, kind) == bytes);
        if (kind == RecordKind::Person) {
            const PersonState& s = again.person->state;
            CHECK(again.rest.empty() && s.queue.size() == 2 && s.queue[1].name == "Eat" && s.running.number == 41);
            CHECK(s.runs.size() == 1 && s.runs[0].object == 12 && s.runs[0].depth == 3 && s.runs[0].flag);
            CHECK(s.runningSub && s.runningSub->id == 7 && s.text == "text" && s.value4 == 9 && s.entries[0].value2 == -1.0f);
        }
        CHECK(again.tail->stack.size() == 2 && again.tail->stack[1].params.size() == 4 && again.tail->stackValue == 1);
        CHECK(again.tail->strings[1].size() == 130 && again.data[0x19] == 600 && again.header.location[1] == 30.0f);
    }
    bool refused = false;
    try {
        SavedObject older = o;
        older.version = 0xD2;
        write(older, RecordKind::Object);
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);
}

std::vector<Test> formatTests() {
    return {
        {"saved objects write back", savedObjectsWriteBack},
        {"object map records write back", objectMapRecordsWriteBack},
        {"saved camera records write back", savedCameraRecordsWriteBack},
        {"scene records write back", sceneRecordsWriteBack},
        {"relationship records write back", relationshipRecordsWriteBack},
        {"lot grids write back", lotGridsWriteBack},
        {"wall records write back", wallRecordsWriteBack},
        {"lot-wide records write back", lotWideRecordsWriteBack},
        {"small lot records write back", smallLotRecordsWriteBack},
        {"neighborhood records write back", neighborhoodRecordsWriteBack},
        {"simulator state records write back", simulatorStateRecordsWriteBack},
        {"lot info records write back", lotInfoRecordsWriteBack},
        {"QFS compresses as the game does", qfsCompressesAsTheGameDoes},
        {"QFS round-trips any data", qfsRoundTripsAnyData},
        {"resources are stored as the game stores them", resourcesAreStoredAsTheGameStoresThem},
        {"package images write back exactly", packageImagesWriteBackExactly},
        {"package images refuse mistakes", packageImagesRefuseMistakes},
        {"a 3D array refuses element indexes past the element", array3dRefusesElementIndexesPastTheElement},
        {"effect detail follows zoom levels", effectDetailFollowsZoomLevels},
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
        {"object map reads waits", objectMapReadsWaits},
        {"object groups parse", objectGroupsParse},
        {"object functions ignore the magic", objectFunctionsIgnoreTheMagic},
        {"roofs read every version", roofsReadEveryVersion},
        {"neighborhood terrain parses", neighborhoodTerrainParses},
        {"effects file reads", effectsFileReads},
        {"particle block fields read", particleBlockFieldsRead},
        {"neighborhood camera reads", neighborhoodCameraReads},
        {"neighborhood paint rules read", neighborhoodPaintRulesRead},
        {"neighborhood roads parse", neighborhoodRoadsParse},
        {"wall coverings follow the wall graph", wallCoveringsFollowTheWallGraph},
        {"lot description parse", lotDescriptionParse},
        {"beach locations lie in bands off the beach side", beachLocationsLieInBandsOffTheBeachSide},
        {"binary property sets and reference lists", binaryPropertySetsAndReferenceLists},
        {"face shapes read", faceShapesRead},
        {"fence post layer reads posts", fencePostLayerReadsPosts},
        {"neighbor remap keeps table 6", neighborRemapKeepsTableSix},
        {"wall layer reads coverings per side", wallLayerReadsCoveringsPerSide},
        {"wants and fears parse", wantsAndFearsParse},
        {"want slots follow the version", wantSlotsFollowTheVersion},
        {"want trees are read", wantTreesAreRead},
        {"interaction table parse", interactionTableParse},
        {"Sim description reads person data by version", simDescriptionReadsPersonDataByVersion},
        {"family record is read by version", familyRecordIsReadByVersion},
        {"family ties are read per Sim", familyTiesAreReadPerSim},
        {"Constant tables count in fifteen bits", constantTablesCountInFifteenBits},
        {"tuning lists read named values", tuningListsReadNamedValues},
        {"saved object reads playing animations", savedObjectReadsPlayingAnimations},
        {"simulator state reads the random state", simulatorStateReadsTheRandomState},
        {"wall graph point index finds the first point", wallGraphPointIndexFindsTheFirstPoint},
    };
}
