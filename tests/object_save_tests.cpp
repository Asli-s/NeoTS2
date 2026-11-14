// Saving objects: sold objects' records go, moved objects' records follow them, bought objects get
// records of their own from a saved object of their type, with their entries in the object map and
// type table.
#include "test_support.hpp"

#include "formats/cobject.hpp"
#include "formats/dbpf_writer.hpp"
#include "formats/objm.hpp"
#include "formats/resource_types.hpp"
#include "sim/object_save.hpp"

#include <cmath>
#include <cstring>
#include <sstream>

namespace {

namespace sim = ts2::sim;
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint32_t kVersion = 0xAD, kGroup = 0xFFFFFFFF;
constexpr std::uint32_t kChair = 0x11111111, kLamp = 0x22222222;

void u32(Bytes& b, std::uint32_t v) {
    for (int s = 0; s < 32; s += 8) b.push_back(static_cast<std::uint8_t>(v >> s));
}
void f32(Bytes& b, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, 4);
    u32(b, bits);
}
void str(Bytes& b, const std::string& s) {
    b.push_back(static_cast<std::uint8_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}
const Bytes kName(64, 0);

// An object's state as version 0xAD saves it, standing at (x, y) on the ground storey facing `facing`.
ts2::xobj::SavedObject state(float x, float y, std::int16_t facing) {
    ts2::xobj::SavedObject s;
    s.version = kVersion;
    s.header.location = {x, y};
    s.header.value1 = 1;
    s.attributes = {1, 2};
    s.semiAttributes = {3};
    s.data[1] = facing;
    s.tail.emplace();
    s.tail->relationshipVersion = 2;
    s.tail->slots.marker = -1;
    s.tail->stack.push_back({0, 0x1000, 3, 7, 7, {}, {0, 0}, 0, 1});
    return s;
}
// A scene record (cAnimatable and cObject, or cObject alone) placed at (y, x, z).
Bytes scene(float x, float y, float z, bool animatable) {
    Bytes b(64, 0);
    if (animatable) {
        u32(b, 0x7D4F2BB8), u32(b, 16), str(b, "cAnimatable");
    }
    u32(b, 0xFA1C39F7), u32(b, 17), str(b, "cObject");
    str(b, "model");
    u32(b, 0);  // groups
    for (const float v : {y, x, z, 0.0f, 0.0f, 0.0f, 1.0f}) f32(b, v);
    u32(b, 0), u32(b, 0);  // joints, blends
    if (animatable) {
        f32(b, 1);
        u32(b, 0);                         // no second pair of places
        for (int i = 0; i < 5; ++i) u32(b, 0);  // u32, u32, i32, i32, f32
        u32(b, 0), u32(b, 0);              // 8 bytes
        for (const float v : {y, x, z, 0.0f, 0.0f, 0.0f, 1.0f}) f32(b, v);
        u32(b, 1);                         // one animation slot, empty
        u32(b, 0);
    }
    return b;
}

struct SavedLot {
    std::string file;
    std::unique_ptr<ts2::dbpf::Package> package;
};

// A lot with two objects: chair 5 at (3.5, 4.5) and lamp 6 at (6.5, 2.5), each of its own type.
SavedLot twoObjects() {
    ts2::objt::ObjectTypeTable types;
    types.version = 78;
    for (const auto& [guid, index] : {std::pair{kChair, std::uint16_t{1}}, std::pair{kLamp, std::uint16_t{2}}}) {
        ts2::objt::Entry e;
        e.guid = e.secondGuid = guid;
        e.index = index;
        e.kind = 4;
        e.name = "type";
        types.entries.push_back(e);
    }
    ts2::objm::ObjectMap map;
    map.version = kVersion;
    map.order = {{5, 1}, {6, 2}};
    map.hasWaits = true;
    map.waitTicks.assign(6, 0);
    map.waitSecond.assign(6, 0);
    map.waitSecond[4] = map.waitSecond[5] = 9;
    map.rest = {2, 0, 0, 0, 0, 0, 0, 0, 0};
    std::ostringstream out(std::ios::binary);
    {
        ts2::dbpf::PackageWriter writer(out);
        auto add = [&](std::uint32_t type, std::uint32_t instance, const Bytes& b) { writer.add({type, kGroup, instance, 0}, b, false, 0); };
        add(ts2::types::kObjectTypeTable, 0, ts2::objt::write(types, kName));
        add(ts2::types::kObjectMap, 1, ts2::objm::write(map, kName));
        add(ts2::types::kSavedObject, 5, ts2::xobj::write(state(3.5f, 4.5f, 2), kName));
        add(ts2::types::kSavedObjectWithMaterial, 5, Bytes(90, 0));  // an old record of the other kind
        add(ts2::types::kSavedObject, 6, ts2::xobj::write(state(6.5f, 2.5f, 2), kName));
        add(ts2::types::kSceneObject, 5, scene(3.5f, 4.5f, 0.25f, true));  // a quarter tile above the floor
        add(ts2::types::kSceneObject, 6, scene(6.5f, 2.5f, 0, false));
        add(ts2::types::kSimRelationship, (5u << 16) | 6u, Bytes(70, 0));
        add(ts2::types::kDataTables, 6, Bytes(70, 0));
        // Left behind by the game for an id it let go: not in the object map.
        add(ts2::types::kSavedObject, 7, ts2::xobj::write(state(1.5f, 1.5f, 2), kName));
        add(ts2::types::kSceneObject, 7, scene(1.5f, 1.5f, 0, false));
        writer.finish(0);
    }
    SavedLot s;
    s.file = out.str();
    s.package = std::make_unique<ts2::dbpf::Package>(std::span(reinterpret_cast<const std::uint8_t*>(s.file.data()), s.file.size()));
    return s;
}

const ts2::content::ReplacedResource* changed(const sim::ObjectSave& c, std::uint32_t type, std::uint32_t instance) {
    for (const auto& r : c.resources)
        if (r.key.type == type && r.key.instance == instance) return &r;
    return nullptr;
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// A sold object's records go: its state, scene record, data tables and relationships, and its place
// in the object map, whose count after the waits follows.
void soldObjectsLeaveTheLot() {
    auto lot = twoObjects();
    sim::ObjectChanges changes;
    changes.sold = {6};
    const auto c = sim::objectChanges(*lot.package, changes);
    CHECK(c.problems.empty() && c.sold == 1);
    CHECK(c.removed.size() == 4);
    const auto* map = changed(c, ts2::types::kObjectMap, 1);
    CHECK(map != nullptr);
    if (map != nullptr) {
        const auto m = ts2::objm::parse(map->bytes);
        CHECK(m.order.size() == 1 && m.order[0].first == 5 && m.waitSecond[5] == 0 && m.rest[0] == 1);
    }
}

// A moved object's state and scene record follow it, as high above the floor as it was, turned to
// its facing; the rest of its state stays as saved.
void movedObjectsFollow() {
    auto lot = twoObjects();
    sim::ObjectChanges changes;
    auto now = state(10.5f, 8.5f, 0);
    now.attributes = {9, 9};  // not taken: only its place, data and slots
    changes.moved.push_back({5, kChair, now, 0});
    changes.floorHeight = [](std::int32_t, float x, float) { return x > 8 ? 3.0f : 0.0f; };
    const auto c = sim::objectChanges(*lot.package, changes);
    CHECK(c.problems.empty() && c.moved == 1);
    const auto* s = changed(c, ts2::types::kSavedObject, 5);
    const auto* sc = changed(c, ts2::types::kSceneObject, 5);
    CHECK(s != nullptr && sc != nullptr);
    if (s != nullptr && sc != nullptr) {
        const auto saved = ts2::xobj::parse(s->bytes, kVersion);
        CHECK(near(saved.header.location[0], 10.5f) && saved.data[1] == 0 && saved.attributes == std::vector<std::int16_t>({1, 2}));
        const auto placed = ts2::cobject::parse(sc->bytes);
        const auto& t = *placed.transform;
        CHECK(near(t[0], 8.5f) && near(t[1], 10.5f) && near(t[2], 3.25f) && near(t[5], 0.70710678f) && near(t[6], 0.70710678f));
        CHECK(placed.animatableTransform && near((*placed.animatableTransform)[2], 3.25f));
    }
}

// A bought object gets its own records from a saved object of its type (without what that one was
// playing), its type a table entry, and its place in the object map with flags like that one's.
void boughtObjectsJoinTheLot() {
    auto lot = twoObjects();
    sim::ObjectChanges changes;
    sim::BoughtObject b;
    b.now = {9, 0x33333333, state(1.5f, 1.5f, 4), 5};
    b.now.state.data[0x3C] = 1;
    b.scene.bytes = scene(20.5f, 20.5f, 0, true);
    b.scene.waitFlags = 9;
    b.frameOwners = {0x33333333};
    changes.bought.push_back(b);
    changes.newType = [](std::uint32_t guid) {
        ts2::objt::Entry e;
        e.guid = e.secondGuid = guid;
        e.kind = 4;
        e.name = "new";
        return std::optional(e);
    };
    const auto c = sim::objectChanges(*lot.package, changes);
    CHECK(c.problems.empty() && c.bought == 1);
    const auto* s = changed(c, ts2::types::kSavedObject, 9);
    const auto* sc = changed(c, ts2::types::kSceneObject, 9);
    const auto* map = changed(c, ts2::types::kObjectMap, 1);
    const auto* types = changed(c, ts2::types::kObjectTypeTable, 0);
    CHECK(s != nullptr && sc != nullptr && map != nullptr && types != nullptr);
    if (s != nullptr && sc != nullptr && map != nullptr && types != nullptr) {
        const auto saved = ts2::xobj::parse(s->bytes, kVersion);
        CHECK(saved.tail && saved.tail->stack.size() == 1 && saved.tail->stack[0].codeOwner == 3);
        const auto placed = ts2::cobject::parse(sc->bytes);
        CHECK(placed.animations.empty() && near((*placed.transform)[0], 1.5f) && near((*placed.transform)[5], -0.70710678f));
        const auto m = ts2::objm::parse(map->bytes);
        CHECK(m.typeIndexByObject.at(9) == 3 && m.waitTicks.size() == 9 && m.waitTicks[8] == 5 && m.waitSecond[8] == 11 && m.rest[0] == 3);
        const auto t = ts2::objt::parse(types->bytes);
        CHECK(t.entries.size() == 3 && t.entries[2].guid == 0x33333333 && t.entries[2].index == 3);
    }
    // An id the game let go, its records left behind, is taken again: they are replaced.
    changes.bought[0].now.id = 7;
    const auto again = sim::objectChanges(*lot.package, changes);
    CHECK(again.problems.empty() && again.bought == 1 && again.removed.empty() && changed(again, ts2::types::kSavedObject, 7) != nullptr);
    // A saved object's id isn't.
    changes.bought[0].now.id = 5;
    CHECK(sim::objectChanges(*lot.package, changes).bought == 0);
    // Unless that object is sold in the same save: an id given to an object of another type.
    changes.sold = {5};
    const auto replaced = sim::objectChanges(*lot.package, changes);
    CHECK(replaced.problems.empty() && replaced.sold == 1 && replaced.bought == 1 && replaced.removed.size() == 2);  // its relationship and its old record
    changes.sold.clear();
    changes.bought[0].now.id = 9;
    // One with no stack owners given, or taking a saved id, isn't saved, and adds no type.
    changes.bought[0].frameOwners.clear();
    const auto bad = sim::objectChanges(*lot.package, changes);
    CHECK(bad.bought == 0 && bad.problems.size() == 1 && changed(bad, ts2::types::kObjectTypeTable, 0) == nullptr);
}

void facingTurnsTheModel() {
    const auto q2 = sim::facingRotation(2), q0 = sim::facingRotation(0), q6 = sim::facingRotation(6);
    CHECK(near(q2[2], 0) && near(q2[3], 1));
    CHECK(near(q0[2], 0.70710678f) && near(q0[3], 0.70710678f));
    CHECK(near(std::fabs(q6[2]), 1) && near(q6[3], 0));
}

} // namespace

std::vector<Test> objectSaveTests() {
    return {
        {"sold objects leave the lot", soldObjectsLeaveTheLot},
        {"moved objects' records follow them", movedObjectsFollow},
        {"bought objects join the lot", boughtObjectsJoinTheLot},
        {"an object's facing turns its model", facingTurnsTheModel},
    };
}
