// The simulation around the VM: motives and mood, type signatures, resets, rooms, the clock and Lua limits.
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

void scoreCurvesInterpolateAndClamp() {
    using ts2::sim::ScoreCurve;
    // Out of order, with a repeated x (ignored) and a broken point (skipped).
    const ScoreCurve curve = ScoreCurve::parse("(10;20) (-10;0) (oops) (10;99) (30;20)");
    CHECK(curve.points().size() == 3);
    CHECK(curve(-50) == 0 && curve(0) == 10 && curve(20) == 20 && curve(100) == 20);
    CHECK(ScoreCurve::parse("")(5) == 0);
    CHECK(ScoreCurve::parse("(0;1) (nan;5) (2;3")(7) == 1);
    CHECK(curve(std::numeric_limits<float>::quiet_NaN()) == 0);  // a level that isn't a number
    CHECK(std::isfinite(ScoreCurve::parse("(-3e38;0) (3e38;3e38)")(0)));

    // Happiness is the mean over motives 0, 3, 5-9 and 13-15. The others don't count.
    ts2::sim::ScoreCurves curves;
    for (auto& c : curves) {
        c = ScoreCurve::parse("(-100;0) (100;20)");
    }
    std::array<float, ts2::sim::kMotiveCount> levels{};
    CHECK(ts2::sim::happiness(curves, levels) == 10);
    levels[1] = levels[2] = levels[4] = levels[10] = levels[11] = levels[12] = 100;
    CHECK(ts2::sim::happiness(curves, levels) == 10);
    levels[7] = 100;
    CHECK(std::abs(ts2::sim::happiness(curves, levels) - 11) < 1e-5f);
}

// Motives move by their daily rate / 720 each update. A decaying one is slowed by its modifier
// (here 50%), a rising one is not. All stay within -100..100.
void motivesDecayByTheirRates() {
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.personData[0x5D] = -720;  // hunger
    sim.personData[0xDB] = 50;    // hunger slowed by half
    sim.personData[0x60] = 1440;  // energy rises
    sim.personData[0xDE] = 50;    // no effect on a rise
    sim.personData[0x65] = -720;  // fun, already at the bottom
    sim.motives[ts2::sim::motive::kFun] = -100;
    sim.motives[2] = 150;         // any motive past 100 is brought back
    ts2::sim::decayMotives(sim);
    CHECK(sim.motives[ts2::sim::motive::kHunger] == -0.5f);
    CHECK(sim.motives[ts2::sim::motive::kEnergy] == 2.0f);
    CHECK(sim.motives[ts2::sim::motive::kFun] == -100.0f);
    CHECK(sim.motives[2] == 100.0f);
}

// A motive change moves its motive towards its limit and stops there. One already past the limit
// is left alone.
void motiveChangesStopAtTheirLimits() {
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.motives[5] = 79.5f;
    sim.motives[6] = 10;
    sim.motives[7] = -60;
    sim.motiveChanges = {{5, 1.0f, 80.0f}, {6, -2.0f, 0.0f}, {7, -1.0f, -50.0f}};
    ts2::sim::applyMotiveChanges(sim);
    CHECK(sim.motives[5] == 80.0f && sim.motives[6] == 8.0f && sim.motives[7] == -60.0f);
}

// Mood is the weighted average of the motives plus person data 0xA3, worked out every 60 ticks,
// and Seasons' temperature limits cap it.
void moodIsTheWeightedAverageOfTheMotives() {
    MemoryCodeSource code;
    std::vector<std::string> weights(16, "(-100;0) (100;0)");
    weights[7] = "(-100;3) (0;1) (100;1)";  // hunger counts three times as much when it is low
    weights[15] = "(-100;1) (100;1)";
    code.addStrings(ts2::sim::kGlobalGroup, 0x227, weights);  // adults
    code.addConstants(ts2::sim::kGlobalGroup, 0x17F, {85, 30, -30, -85, 0});
    code.addObjectType(0x7F000001, typed(0xD216EE01));
    code.addConstants(0x7F000001, 0x1017, {50, 80, 100, 80, 50});
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.personData[0x3A] = 30;
    sim.motives[ts2::sim::motive::kHunger] = -100;
    sim.motives[ts2::sim::motive::kFun] = 100;
    sim.personData[0xA3] = 10;
    ts2::sim::MotiveUpdate update(code, 0);
    update.tick(sim, 59);
    CHECK(sim.motives[ts2::sim::motive::kMood] == 0.0f);  // not yet 60 ticks
    update.tick(sim, 60);
    CHECK(sim.motives[ts2::sim::motive::kMood] == -40.0f);  // (-300 + 100) / 4 + 10
    CHECK(sim.motives[4] == -40.0f);
    CHECK(sim.personData[0x1B] == 2);  // the tick counter counts every tick

    sim.motives[ts2::sim::motive::kHunger] = 100;
    sim.personData[0x14] = 90;  // hot: mood at most 50
    ts2::sim::MotiveUpdate seasons(code, 0x80);
    seasons.tick(sim, 120);
    CHECK(sim.motives[ts2::sim::motive::kMood] == 50.0f);

    // Person data 0x1D stops the decay. At 2 and up the motive changes too.
    sim.personData[0x1D] = 2;
    sim.motiveChanges = {{5, 1.0f, 100.0f}};
    const float energy = sim.motives[5];
    seasons.tick(sim, 180);
    CHECK(sim.motives[5] == energy);
}

void advertisementsScaleByPersonality() {
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.personData[0x3A] = 27;   // adult
    sim.personData[2] = 600;     // read by types 1 and 2 (inverted)
    ts2::ttab::Interaction line;
    line.motives.resize(1);
    auto& adult = line.motives[0];
    adult.resize(10);
    adult[7] = {10, 30, 0};      // hunger: plain, (10 + 30) / 1000
    adult[8] = {10, 30, 1};      // hygiene: (10 + 30 * 600 / 1000) / 1000
    adult[9] = {10, 30, 2};      // bladder: inverted, (10 + 30 * 400 / 1000) / 1000
    adult[5] = {10, 30, 99};     // energy: a type past the table scales by 0
    adult[1] = {50, 50, 0};      // motive 1 never advertises
    auto near = [](float a, float b) { return std::abs(a - b) < 1e-6f; };
    CHECK(near(ts2::sim::advertisedChange(line, sim, 7), 0.040f));
    CHECK(near(ts2::sim::advertisedChange(line, sim, 8), 0.028f));
    CHECK(near(ts2::sim::advertisedChange(line, sim, 9), 0.022f));
    CHECK(near(ts2::sim::advertisedChange(line, sim, 5), 0.010f));
    CHECK(ts2::sim::advertisedChange(line, sim, 1) == 0);
    CHECK(ts2::sim::advertisedChange(line, sim, 15) == 0);  // no entry there
    sim.personData[0x3A] = 60;  // an elder reads group 4, which this line doesn't have
    CHECK(ts2::sim::advertisedChange(line, sim, 7) == 0);
}

void typeSignaturesSumTheCallGraph() {
    using namespace ts2::sim;
    MemoryCodeSource code;
    auto tree = [](std::uint32_t version, std::vector<std::uint16_t> calls) {
        Behavior b;
        b.treeVersion = version;
        for (const std::uint16_t c : calls) {
            Instruction in;
            in.opcode = c;
            b.instructions.push_back(in);
        }
        return b;
    };
    // Init (private 0x1000, version 5) calls a semi-global (version 3), a global (version 7), itself
    // and a primitive. The semi-global calls Init back and a tree that doesn't exist.
    code.add(kObjectGroup, 0x1000, tree(5, {0x2001, 0x0101, 0x1000, 0x0002}));
    code.add(kSemiGroup, 0x2001, tree(3, {0x1000, 0x1005}));
    code.add(kGlobalGroup, 0x0101, tree(7, {}));
    CHECK(treeSignature(code, kObjectGroup, 0x1000) == ((5u << 16) + 5 + 3 + 7));
    CHECK(treeSignature(code, kObjectGroup, 0) == 0);
    CHECK(treeSignature(code, kObjectGroup, 0x1005) == 0);

    ts2::objd::ObjectDefinition definition;
    definition.fields.assign(0x40, 0);
    definition.fields[0x3A] = 20;
    code.addObjectType(kObjectGroup, definition);
    code.addFunctions(kObjectGroup, {{0, 0x1000}, {0, 0x2001}});
    const ObjectType& type = *code.objectType(definition.guid());
    ts2::objt::Entry saved;
    const std::uint32_t words[4] = {(5u << 16) + 15, (3u << 16) + 15, 20, 0};
    for (std::size_t i = 0; i < 4; ++i) {
        saved.unknown[2 * i] = static_cast<std::uint16_t>(words[i]);
        saved.unknown[2 * i + 1] = static_cast<std::uint16_t>(words[i] >> 16);
    }
    CHECK(!typeChangedSinceSave(code, type, saved));
    saved.unknown[0] ^= 1;  // a script changed since the save
    CHECK(typeChangedSinceSave(code, type, saved));
}

void resetObjectsStartOver() {
    using namespace ts2::sim;
    MemoryCodeSource code;
    Behavior mainTree;
    mainTree.argCount = 4;
    mainTree.localCount = 2;
    mainTree.instructions.resize(1);
    code.add(kObjectGroup, 0x1001, mainTree);
    ts2::objd::ObjectDefinition definition;
    definition.fields.assign(0x70, 0);
    definition.fields[0x2D] = 77;  // becomes data 7
    definition.fields[0x6A] = 5;   // single-tile: data 0x2A
    code.addObjectType(kObjectGroup, definition);
    code.addFunctions(kObjectGroup, {{0, 0}, {0, 0x1001}});
    const ObjectType& type = *code.objectType(definition.guid());

    SimObject object;
    object.id = 42;
    object.group = kObjectGroup;
    object.data.fill(9);
    object.data[0x7F] = 123;  // beyond the game's fields: left alone
    object.thread.frames.resize(3);
    object.thread.temps.fill(6);
    object.tokenSource.kind = 3;
    object.token.guid = 4242;
    resetObject(object, type, code);
    CHECK(object.thread.temps[0] == 0 && object.thread.temps[7] == 0);
    CHECK(object.tokenSource.kind == 0 && object.token.guid == 4242);
    CHECK(object.data[0x2A] == 0);  // set only once Init has run
    CHECK(!isMultiTilePart(type));
    finishSingleTileReset(object, type);
    CHECK(object.data[0x01] == 9 && object.data[0x1D] == 9 && object.data[0x13] == 9);  // kept
    CHECK(object.data[0x08] == 0 && object.data[0x28] == 0);                            // cleared
    CHECK(object.data[0x0B] == 42 && object.data[0x17] == 2 && object.data[0x39] == 0xFF);
    CHECK(object.data[0x07] == 77 && object.data[0x2A] == 5 && object.data[0x7F] == 123);
    CHECK(object.thread.frames.size() == 1 && object.thread.frames[0].instance == 0x1001 &&
          object.thread.frames[0].pc == 0 && object.thread.frames[0].params.size() == 4);
}

// A model joint posed with the object's saved joints: a saved place replaces its node's own (names
// matched in any case) and moves the joints under it. With none saved the joint is where it rests.
void savedJointsPoseModelBones() {
    using namespace ts2::sim;
    RouteModel model;
    model.nodes = {{"", -1, {0, 0, 0}, {0, 0, 0, 1}},
                   {"slot_trans", 0, {0, 0, 0}, {0, 0, 0, 1}},
                   {"seat", 1, {0.5f, 0, 0}, {0, 0, 0.70710678f, 0.70710678f}}};
    model.bones = {{"seat", ModelBone{0.5f, 0, 1.5707963f}}};
    const auto rest = posedModelBone(model, "seat", {});
    CHECK(rest && rest->x == 0.5f && rest->y == 0);
    const std::vector<ts2::cobject::SavedJoint> saved{{"Slot_Trans", {0, 0.373f, 0}, {0, 0, 0, 1}}};
    const auto slid = posedModelBone(model, "seat", saved);
    CHECK(slid && std::fabs(slid->x - 0.5f) < 1e-5f && std::fabs(slid->y - 0.373f) < 1e-5f && std::fabs(slid->yaw - 1.5707963f) < 1e-4f);
    CHECK(!posedModelBone(model, "missing", saved));
}

void clockAdvancesAndReads() {
    std::array<std::int16_t, 60> g{};
    namespace gl = ts2::sim::global;
    g[gl::kHour] = 15;
    g[gl::kMinute] = 22;
    g[gl::kSecond] = 58;
    g[gl::kDayOfMonth] = 16;
    g[gl::kMonth] = 6;
    g[gl::kYear] = 1997;
    CHECK(ts2::sim::clockText(g) == "Mon 3:22 pm");
    // Time Of Day follows the hour on every step: night before 6, morning at 6, day 7 to 17,
    // evening at 18 and night from 19.
    using ts2::sim::TimeOfDay;
    CHECK(ts2::sim::timeOfDay(5) == TimeOfDay::Night && ts2::sim::timeOfDay(6) == TimeOfDay::Morning);
    CHECK(ts2::sim::timeOfDay(7) == TimeOfDay::Day && ts2::sim::timeOfDay(17) == TimeOfDay::Day);
    CHECK(ts2::sim::timeOfDay(18) == TimeOfDay::Evening && ts2::sim::timeOfDay(19) == TimeOfDay::Night);
    ts2::sim::advanceClock(g);
    CHECK(g[gl::kSecond] == 0 && g[gl::kMinute] == 23 && ts2::sim::clockText(g) == "Mon 3:23 pm");
    CHECK(g[gl::kTimeOfDay] == 0);
    g[gl::kHour] = 23;
    g[gl::kMinute] = 59;
    g[gl::kSecond] = 58;
    g[gl::kDayOfMonth] = 30;
    ts2::sim::advanceClock(g);
    CHECK(g[gl::kHour] == 0 && g[gl::kDayOfMonth] == 1 && g[gl::kMonth] == 7 && ts2::sim::clockText(g) == "Tue 12:00 am");
    CHECK(g[gl::kTimeOfDay] == 2);
    CHECK(g[gl::kDayOfWeek] == 1);
    // The game shows "Sat 11:41 am" on 27 June 1997 (a Friday) with Day Of Week 5.
    g[gl::kDayOfMonth] = 27;
    g[gl::kMonth] = 6;
    g[gl::kHour] = 11;
    g[gl::kMinute] = 41;
    g[gl::kDayOfWeek] = 5;
    CHECK(ts2::sim::clockText(g) == "Sat 11:41 am");
    g[gl::kDayOfWeek] = 9;
    CHECK(ts2::sim::weekday(g) == 4);  // out of range: the date's
    g[gl::kBudgetLow] = 968;
    g[gl::kBudgetHigh] = 2;
    CHECK(ts2::sim::funds(g) == 20968);
}

// A script stuck in an endless loop is stopped instead of freezing the game, and scripts run
// normally afterwards.
void luaEndlessLoopIsStopped() {
    ts2::sim::LuaRuntime runtime;
    runtime.load({}, {compileScript("Forever", "while true do end"), compileScript("Fine", "SetScriptReturnValue(true)")});
    CHECK(runtime.problems.empty());
    for (int round = 0; round < 2; ++round) {
        ts2::sim::ScriptCall stuck;
        CHECK(!runtime.run("Forever", stuck) && stuck.luaError.find("too long") != std::string::npos);
        ts2::sim::ScriptCall fine;
        CHECK(runtime.run("Fine", fine) && fine.returnValue);
    }
}

// A script that raises an error that isn't text fails with a readable message.
void luaErrorWithoutTextIsReported() {
    ts2::sim::LuaRuntime runtime;
    runtime.load({}, {compileScript("Table", "error({})")});
    ts2::sim::ScriptCall c;
    CHECK(!runtime.run("Table", c) && !c.luaError.empty());
}

// A script that asks for gigabytes of memory, at once or by growing a string, fails instead of
// using up the computer's memory, and scripts run normally afterwards.
void luaMemoryIsCapped() {
    ts2::sim::LuaRuntime runtime;
    runtime.load({}, {compileScript("Huge", "local s = string.rep('x', 1000000000)"),
                      compileScript("Growing", "local s = 'x' while true do s = s .. s end"),
                      compileScript("Fine", "SetScriptReturnValue(true)")});
    CHECK(runtime.problems.empty());
    for (const char* name : {"Huge", "Growing"}) {
        ts2::sim::ScriptCall greedy;
        CHECK(!runtime.run(name, greedy) && greedy.luaError.find("memory") != std::string::npos);
        ts2::sim::ScriptCall fine;
        CHECK(runtime.run("Fine", fine) && fine.returnValue);
    }
}

// Positions and levels from broken files are left out rather than converted out of range.
void brokenNumbersAreLeftOut() {
    const ts2::sim::RoomMap rooms(squareRoom(false));
    for (const float v : {std::nanf(""), std::numeric_limits<float>::infinity(), 1e30f, -1e30f}) {
        CHECK(rooms.roomAt(0, v, 2.5f) == ts2::sim::RoomMap::kNoRoom);
        CHECK(rooms.roomAt(0, 2.5f, v) == ts2::sim::RoomMap::kNoRoom);
    }
    auto farLevels = squareRoom(false);
    farLevels.points[0].level = std::numeric_limits<std::int32_t>::min();
    farLevels.points[1].level = std::numeric_limits<std::int32_t>::max();
    CHECK(ts2::sim::RoomMap(farLevels).roomAt(0, 2.5f, 2.5f) == ts2::sim::RoomMap::kNoRoom);

    ts2::sim::WallTiles walls;
    walls.add(0, 1e30f, 1, 2e30f, 1);
    walls.add(0, std::nanf(""), 1, 3, 1);
    walls.add(0, 1, 1, 3, 1);
    CHECK(walls.at(0, 1, 1) == ts2::sim::WallTiles::kLowY);

    using ts2::sim::DataValue;
    DataValue::Table table;
    const auto nan = DataValue::ofFloat(std::nanf(""));
    table[nan] = DataValue::ofInt(1);
    table[DataValue::ofFloat(1)] = DataValue::ofInt(3);
    table[nan] = DataValue::ofInt(2);
    CHECK(table.size() == 2 && table.at(nan).i == 2 && table.begin()->first.f == 1);
}

void roomsAreFilledBetweenWalls() {
    const ts2::sim::RoomMap rooms(squareRoom(false));
    CHECK(rooms.roomAt(0, 2.5f, 2.5f) == 5);
    CHECK(rooms.roomAt(0, 1.5f, 1.5f) == 5);
    CHECK(rooms.roomAt(0, 0.5f, 0.5f) == 0);
    CHECK(rooms.roomAt(0, 4.5f, 2.5f) == 0);
    CHECK(rooms.roomAt(1, 2.5f, 2.5f) == ts2::sim::RoomMap::kNoRoom);
    const auto* inside = rooms.room(5);
    const auto* yard = rooms.room(0);
    CHECK(inside != nullptr && inside->area() == 4 && !inside->touchesLotEdge && inside->level == 0);
    CHECK(yard != nullptr && yard->area() == 32 && yard->touchesLotEdge);
}

void diagonalWallsSplitTiles() {
    const ts2::sim::RoomMap rooms(squareRoom(true));
    CHECK(rooms.roomAt(0, 2.5f, 1.5f) == 5);
    CHECK(rooms.roomAt(0, 1.5f, 2.5f) == 6);
    CHECK(rooms.roomAt(0, 1.8f, 1.5f) == 5);
    CHECK(rooms.roomAt(0, 1.2f, 1.5f) == 6);
    const auto* upper = rooms.room(5);
    const auto* lower = rooms.room(6);
    CHECK(upper != nullptr && upper->quarterTiles == 8 && upper->area() == 2);
    CHECK(lower != nullptr && lower->quarterTiles == 8 && lower->area() == 2);
}

void savedRoomGridIsReadByQuarter() {
    ts2::array3d::Array3D grid;
    grid.width = 2;
    grid.height = 1;
    grid.levels = 2;
    grid.elementSize = 8;
    grid.elements.assign(2 * 1 * 2 * 8, 0);
    const std::int16_t leftBottomRightTop[4] = {7, 7, 8, 8};
    const std::size_t tile = (1 * 2 + 1) * 1 + 0;
    for (std::size_t i = 0; i < 4; ++i) {
        grid.elements[tile * 8 + i * 2] = static_cast<std::uint8_t>(leftBottomRightTop[i]);
    }
    const ts2::sim::RoomMap rooms(grid, -1);
    CHECK(rooms.roomAt(0, 1.5f, 0.1f) == 8);
    CHECK(rooms.roomAt(0, 1.9f, 0.5f) == 8);
    CHECK(rooms.roomAt(0, 1.1f, 0.5f) == 7);
    CHECK(rooms.roomAt(0, 1.5f, 0.9f) == 7);
    CHECK(rooms.roomAt(-1, 1.5f, 0.5f) == 0);
    const auto* room = rooms.room(7);
    CHECK(room != nullptr && room->quarterTiles == 2 && room->level == 0);
    // Sims' and objects' positions run x along the grid's second side: this grid is 1 tile along
    // a position's x and 2 along its y, so the room tile above is at position (0.5, 1.5).
    CHECK(rooms.positionWidth() == 1 && rooms.positionHeight() == 2);
    CHECK(rooms.roomAtPosition(0, 0.1f, 1.5f) == 8);
    CHECK(rooms.roomAtPosition(0, 0.5f, 1.1f) == 7);
    CHECK(rooms.roomAtPosition(0, 1.5f, 0.5f) == ts2::sim::RoomMap::kNoRoom);

    // A layer whose walls are all on level -1 encloses nothing on level 0: region 0 there.
    auto basementOnly = squareRoom(false);
    for (auto& p : basementOnly.points) {
        p.level = -1;
    }
    ts2::sim::RoomMap withLayer(grid, -1);
    withLayer.assignLayer(0, ts2::sim::RoomMap(basementOnly));
    CHECK(withLayer.room(7)->layerRegion[0] == 0 && withLayer.room(8)->layerRegion[0] == 0);
}

void vertexAliasesFollowKind() {
    namespace sg = ts2::scenegraph;
    auto element = [](std::uint32_t identity, std::uint32_t format, const std::vector<float>& values) {
        sg::Element e;
        e.identity = identity;
        e.blockFormat = format;
        e.count = static_cast<std::uint32_t>(values.size() / (format + 1));
        for (float v : values) {
            const auto bits = std::bit_cast<std::uint32_t>(v);
            for (int i = 0; i < 4; ++i) {
                e.data.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
            }
        }
        return e;
    };
    sg::Mesh mesh;
    mesh.elements.push_back(element(sg::kPositions, 2, {0, 0, 0, 1, 1, 1, 2, 2, 2}));
    mesh.elements.push_back(element(sg::kTextureCoordinates, 1, {0, 0, 0.25f, 0.25f, 0.5f, 0.5f, 0.75f, 0.75f}));
    mesh.elements.push_back(element(sg::kTextureCoordinates, 1, {1, 1, 2, 2, 3, 3, 4, 4}));
    // Positions and texture coordinates only: the coordinates take the third alias list, not the second.
    sg::ElementLink link;
    link.elements = {0, 1, 2};
    link.vertexCount = 2;
    link.aliases = {std::vector<std::uint16_t>{2, 0}, std::vector<std::uint16_t>{}, std::vector<std::uint16_t>{3, 1}};
    const auto positions = sg::vertexFloats(mesh, link, sg::kPositions);
    const auto uvs = sg::vertexFloats(mesh, link, sg::kTextureCoordinates);
    const auto second = sg::vertexFloats(mesh, link, sg::kTextureCoordinates, 1);
    CHECK(positions == std::vector<float>({2, 2, 2, 0, 0, 0}));
    CHECK(uvs == std::vector<float>({0.75f, 0.75f, 0.25f, 0.25f}));
    CHECK(second == std::vector<float>({4, 4, 2, 2}));
}

void motivesAreShownAsWholeNumbers() {
    ts2::sim::SimObject sim;
    sim.motives[ts2::sim::motive::kMood] = 45.83f;
    sim.motives[ts2::sim::motive::kHunger] = -45.8f;
    sim.motives[ts2::sim::motive::kFun] = std::numeric_limits<float>::quiet_NaN();
    sim.motives[ts2::sim::motive::kSocial] = 1e30f;
    sim.motives[ts2::sim::motive::kEnvironment] = 56.24f;
    CHECK(ts2::sim::wholeMotive(sim, ts2::sim::motive::kMood) == 45);
    CHECK(ts2::sim::wholeMotive(sim, ts2::sim::motive::kHunger) == -45);
    CHECK(ts2::sim::wholeMotive(sim, ts2::sim::motive::kFun) == 0);
    CHECK(ts2::sim::wholeMotive(sim, ts2::sim::motive::kSocial) == 100);
    CHECK(ts2::sim::wholeMotive(sim, ts2::sim::motive::kEnvironment) == 56);
    CHECK(ts2::sim::wholeMotive(sim, 99) == 0);
    sim.personData[ts2::sim::kEnvironmentScore] = 70;  // stands in for Environment
    CHECK(ts2::sim::wholeMotive(sim, ts2::sim::motive::kEnvironment) == 70);
}

} // namespace

std::vector<Test> simulationTests() {
    return {
        {"rooms are filled between walls", roomsAreFilledBetweenWalls},
        {"broken numbers are left out", brokenNumbersAreLeftOut},
        {"diagonal walls split tiles", diagonalWallsSplitTiles},
        {"saved room grid is read by quarter", savedRoomGridIsReadByQuarter},
        {"score curves interpolate and clamp", scoreCurvesInterpolateAndClamp},
        {"motives decay by their rates", motivesDecayByTheirRates},
        {"motive changes stop at their limits", motiveChangesStopAtTheirLimits},
        {"mood is the weighted average of the motives", moodIsTheWeightedAverageOfTheMotives},
        {"advertisements scale by personality", advertisementsScaleByPersonality},
        {"the Sim clock advances and reads", clockAdvancesAndReads},
        {"type signatures sum the call graph", typeSignaturesSumTheCallGraph},
        {"reset objects start over", resetObjectsStartOver},
        {"saved joints pose model bones", savedJointsPoseModelBones},
        {"Lua endless loop is stopped", luaEndlessLoopIsStopped},
        {"Lua memory is capped", luaMemoryIsCapped},
        {"Lua error without text is reported", luaErrorWithoutTextIsReported},
        {"vertex aliases follow kind", vertexAliasesFollowKind},
        {"motives are shown as whole numbers", motivesAreShownAsWholeNumbers},
    };
}
