// The simulation around the VM: motives and mood, type signatures, resets, rooms, the clock and Lua limits.
#include "test_support.hpp"

#include "sim/animation.hpp"
#include "sim/ground_click.hpp"
#include "sim/idle_spot.hpp"
#include "formats/qfs.hpp"
#include "formats/resource_types.hpp"
#include "sim/lot_save.hpp"
#include "sim/lot_save_detail.hpp"
#include "sim/route_goals.hpp"
#include "sim/simulator_ids.hpp"
#include "sim/linked_interactions.hpp"
#include "sim/hood_save.hpp"
#include "formats/ngbh.hpp"
#include "formats/sdsc.hpp"
#include "formats/fami.hpp"
#include "formats/famt.hpp"
#include "formats/lot_records.hpp"
#include "formats/simi.hpp"
#include "formats/srel.hpp"
#include "sim/clock.hpp"
#include "sim/relationship_table.hpp"
#include "sim/character_file.hpp"
#include "sim/character_look.hpp"
#include "formats/hash.hpp"
#include "formats/rcol.hpp"
#include "formats/cobject_record.hpp"
#include "sim/character_models.hpp"
#include "sim/newborn_file.hpp"
#include "scene/assets.hpp"
#include "scene/sims.hpp"
#include "formats/propset.hpp"
#include "formats/reference_list.hpp"
#include "formats/str.hpp"
#include "sim/vm_execution.hpp"
#include "sim/route_portals.hpp"
#include "sim/selection.hpp"
#include "sim/walking.hpp"
#include "sim/path_passes.hpp"
#include "sim/route_world.hpp"
#include "sim/speed_clock.hpp"
#include "sim/turn.hpp"
#include "sim/walk_path.hpp"
#include "sim/walk_plan.hpp"

#include <bit>
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

// With no Sim selected, as in a household just moved in, control goes to the first selectable Sim:
// humans before pets, then the oldest, then by catalog name. A selected Sim that can't be selected
// any more passes control on to the next one after it.
void selectionPassesToTheNextSelectableSim() {
    using ts2::sim::SimObject;
    MemoryCodeSource code;
    ts2::sim::World world;
    auto person = [&](std::uint32_t guid, std::int16_t stage, std::int16_t species, std::uint16_t names) -> SimObject& {
        auto d = typed(guid, ts2::sim::kPersonType);
        d.fields[ts2::objd::field::kCatalogStringsId] = names;
        SimObject& s = world.create(kObjectGroup, d);
        s.personData[0x3A] = stage;
        s.personData[0xBA] = species;
        s.personData[0x9E] = 1;
        return s;
    };
    code.addCatalogStrings(kObjectGroup, 1, {"Zoe"});
    code.addCatalogStrings(kObjectGroup, 2, {"adam"});
    world.create(kObjectGroup, typed(kFridgeGuid));
    const SimObject& child = person(0x100, 8, 0, 1);
    const SimObject& dog = person(0x101, 32, 1, 1);
    const SimObject& zoe = person(0x102, 32, 0, 1);
    const SimObject& adam = person(0x103, 32, 0, 2);
    SimObject& visitor = person(0x104, 64, 0, 1);
    visitor.personData[0x9E] = 0;
    CHECK((ts2::sim::selectionOrder(world, code) == std::vector<std::int16_t>{adam.id, zoe.id, child.id, dog.id}));

    std::vector<std::int16_t> globals(0x20, 0);
    ts2::sim::keepSelection(world, code, globals);
    CHECK(globals[3] == adam.id);
    ts2::sim::keepSelection(world, code, globals);
    CHECK(globals[3] == adam.id);
    // Selection Flags bit 1 is passed over, as is a Sim no longer selectable.
    world.find(adam.id)->personData[0x9E] = 3;
    world.find(zoe.id)->personData[0x9E] = 3;
    ts2::sim::keepSelection(world, code, globals);
    CHECK(globals[3] == child.id);
    world.find(child.id)->personData[0x9E] = 0;
    ts2::sim::keepSelection(world, code, globals);
    CHECK(globals[3] == dog.id);
    world.find(dog.id)->personData[0x9E] = 2;
    ts2::sim::keepSelection(world, code, globals);
    CHECK(globals[3] == 0);
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

// MultiplyAdValues scales a human's minimum in its age group, cut toward zero. Without a motive it
// leaves motives 2, 4 and 10-12 alone. A pet's value is the one its personality bits pick in each
// record of its species and age group.
void multipliedAdvertisementsFollowTheGame() {
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.personData[0x3A] = 27;  // adult
    ts2::ttab::Interaction line;
    line.motives.resize(1);
    line.motives[0].resize(16, {10, 30, 0});
    ts2::sim::multiplyAdvertisements(line, sim, 2.5f, -1);
    CHECK(line.motives[0][7].min == 25 && line.motives[0][7].delta == 30);
    CHECK(line.motives[0][2].min == 10 && line.motives[0][4].min == 10 && line.motives[0][11].min == 10);
    ts2::sim::multiplyAdvertisements(line, sim, -0.5f, 4);
    CHECK(line.motives[0][4].min == -5 && line.motives[0][7].min == 25);
    ts2::sim::multiplyAdvertisements(line, sim, 1.0e30f, 5);  // too large for the game's conversion
    CHECK(line.motives[0][5].min == 0);
    sim.personData[0x3A] = 60;  // an elder's group 4 is missing and stays missing
    ts2::sim::multiplyAdvertisements(line, sim, 3.0f, -1);
    CHECK(line.motives.size() == 1 && line.motives[0][7].min == 25);

    auto& dog = world.create(kObjectGroup, typed(0x17DDADE2, ts2::sim::kPersonType));
    dog.personData[0xBA] = 2;    // small dog
    dog.personData[0x3A] = 27;   // adult: group 2
    dog.personData[0xC7] = 0x9;  // first pair low bit, second pair high bit, third pair neither
    ts2::ttab::Interaction petLine;
    petLine.moreMotives.resize(3);
    petLine.moreMotives[2].resize(8);
    petLine.moreMotives[2][7] = {{10, 20, 30}, {10, 20, 30}, {10, 20, 30}};  // read as {min, range, type}
    ts2::sim::multiplyAdvertisements(petLine, dog, 2.0f, 7);
    const auto& r = petLine.moreMotives[2][7];
    // The saved order is the first, second, third value: type, min, range as read here.
    CHECK(r[0].type == 60 && r[0].min == 10 && r[0].delta == 20);
    CHECK(r[1].type == 30 && r[1].min == 20 && r[1].delta == 20);
    CHECK(r[2].type == 30 && r[2].min == 10 && r[2].delta == 40);
}

// A pet's advertisement adds up, over every record of its kind's list, the value its personality
// bits pick, in thousandths. Motive 13 advertises nothing to pets. Its happiness counts motive 1
// instead of 13.
void petsAdvertiseByPersonality() {
    ts2::sim::World world;
    auto& dog = world.create(kObjectGroup, typed(0x17DDADE2, ts2::sim::kPersonType));
    dog.personData[0xBA] = 2;    // small dog
    dog.personData[0x3A] = 60;   // elder: kind 8, the table's pet group 3
    dog.personData[0xC7] = 0x9;  // first record's low bit, second's high bit, the rest neither
    CHECK(ts2::sim::petKind(dog) == std::optional<std::size_t>{8});
    ts2::ttab::Interaction line;
    line.moreMotives.resize(4);
    line.moreMotives[3].resize(16);
    // Six records: more than MultiplyAdValues reaches. Read as {min, range, type}, saved type first.
    line.moreMotives[3][7].assign(6, ts2::ttab::MotiveEffect{20, 300, 1000});
    line.moreMotives[3][13].assign(1, ts2::ttab::MotiveEffect{50, 50, 50});
    line.moreMotives[3][1].assign(1, ts2::ttab::MotiveEffect{0, 5, 7});  // the low bit picks the first: 7
    auto near = [](float a, float b) { return std::abs(a - b) < 1e-5f; };
    CHECK(near(ts2::sim::advertisedChange(line, dog, 7), (1000 + 20 + 4 * 300) * 0.001f));
    CHECK(ts2::sim::advertisedChange(line, dog, 13) == 0);
    CHECK(near(ts2::sim::advertisedChange(line, dog, 1), 0.007f));
    dog.personData[0x3A] = 27;  // adult: group 2, which this line doesn't have
    CHECK(ts2::sim::advertisedChange(line, dog, 7) == 0);

    ts2::sim::ScoreCurves curves;
    curves.fill(ts2::sim::ScoreCurve::parse("(-100;-100) (100;100)"));
    std::array<float, ts2::sim::kMotiveCount> levels{};
    levels[1] = 50;
    levels[13] = 100;
    CHECK(near(ts2::sim::happiness(curves, levels, false), 5.0f));
    CHECK(near(ts2::sim::happiness(curves, levels, true), 10.0f));
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

// The room on each side of a wall, found in the room map beside the wall, is the room the wall
// graph names on that side: its left room walking from `from` to `to`, its right room the other way.
// The four walls of the square and its diagonal cover every direction a wall can run.
void roomOnSideOfWallMatchesTheWallGraph() {
    const auto graph = squareRoom(true);
    const ts2::sim::RoomMap rooms(graph);
    for (const auto& wall : graph.walls) {
        const auto& from = *graph.point(wall.from);
        const auto& to = *graph.point(wall.to);
        CHECK(ts2::sim::roomOnSideOfWall(rooms, from, to) == wall.leftRoom);
        CHECK(ts2::sim::roomOnSideOfWall(rooms, to, from) == wall.rightRoom);
    }
    // A wall along the lot's edge has nothing on its outer side.
    CHECK(ts2::sim::roomOnSideOfWall(rooms, {0, 0, 2, 0}, {0, 0, 0, 0}) == ts2::sim::RoomMap::kNoRoom);
    CHECK(ts2::sim::roomOnSideOfWall(rooms, {0, 0, 0, 0}, {0, 0, 2, 0}) == 0);
}

// A 2 by 2 room at (offset, offset) on a side by side lot, its outline in unit pieces, the first
// `greenhouse` of them of type kGreenhouseWall and the rest walls.
constexpr std::uint32_t kGreenhouseWall = 0x924E0F6B;
std::pair<ts2::wallgraph::WallGraph, std::vector<ts2::sim::WallPiece>> unitSquareRoom(int side, int offset, int greenhouse) {
    ts2::wallgraph::WallGraph g;
    g.width = side;
    g.height = side;
    g.levels = 1;
    const float o = static_cast<float>(offset);
    const std::vector<std::pair<float, float>> ring{{o, o}, {o + 1, o}, {o + 2, o}, {o + 2, o + 1}, {o + 2, o + 2},
                                                    {o + 1, o + 2}, {o, o + 2}, {o, o + 1}};
    for (std::int32_t i = 0; i < 8; ++i) {
        g.points.push_back({i, ring[static_cast<std::size_t>(i)].first, ring[static_cast<std::size_t>(i)].second, 0});
    }
    std::vector<ts2::sim::WallPiece> pieces;
    for (std::int32_t i = 0; i < 8; ++i) {
        g.walls.push_back({i, i, 0, (i + 1) % 8, 5});
        pieces.push_back({g.points[static_cast<std::size_t>(i)], g.points[static_cast<std::size_t>((i + 1) % 8)],
                          i < greenhouse ? kGreenhouseWall : 1u});
    }
    return {g, pieces};
}

// Room field 6 is 1 when more than half of the outline round the room is greenhouse walls. A room
// whose first tile has no piece on its edges or at its nearest corner gets 0.
void greenhouseRoomsAreMostlyGreenhouseWalls() {
    const std::set<std::uint32_t> greenhouse{kGreenhouseWall};
    const auto [graph, five] = unitSquareRoom(6, 1, 5);
    const ts2::sim::RoomMap rooms(graph);
    CHECK(rooms.roomAt(0, 1.5f, 1.5f) == 5);
    CHECK(ts2::sim::WallOutline(five).mostlyGreenhouse(rooms, 5, greenhouse));
    CHECK(!ts2::sim::WallOutline(unitSquareRoom(6, 1, 4).second).mostlyGreenhouse(rooms, 5, greenhouse));
    CHECK(!ts2::sim::WallOutline(five).mostlyGreenhouse(rooms, 5, {}));
    const auto [farGraph, far] = unitSquareRoom(8, 3, 8);
    const ts2::sim::RoomMap farRooms(farGraph);
    CHECK(ts2::sim::WallOutline(far).mostlyGreenhouse(farRooms, 5, greenhouse));
    CHECK(!ts2::sim::WallOutline(far).mostlyGreenhouse(farRooms, 0, greenhouse));
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

void speedClockMovesAtMost200MsAnUpdate() {
    ts2::sim::SpeedClock clock;
    clock.setRate(21);
    clock.update(16);  // 16 real ms at 21 times is 336, more than an update moves
    CHECK(clock.timeMs() == 200);
    clock.update(116);
    CHECK(clock.timeMs() == 400);
    clock.setRate(1);
    clock.update(416);  // a 300 ms frame takes in 200 of it
    CHECK(clock.timeMs() == 600);
    clock.update(426);
    CHECK(clock.timeMs() == 610);
}

// An object's saved record takes what the running object holds: its attributes, data, position,
// temporary registers, arrays and slots, and the frames its scripts wait in, which name the object
// whose code they run by its entry in the lot's object type table.
void theObjectRecordFollowsTheRunningObject() {
    ts2::sim::World world;
    ts2::sim::SimObject& object = world.create(kObjectGroup, typed(0x1000));
    object.attributes[1] = 9;
    object.semiAttributes[0] = 3;
    object.data[0x20] = 77;
    object.x = 5 * 65536;
    object.y = 7 * 65536 + 32768;
    object.level = 2;
    object.thread.temps[2] = -4;
    object.arrays = {{1, 2}, {3}};
    object.arrayIterators = {1, 0};
    object.slots = {11, 0};
    ts2::sim::Frame frame;
    frame.codeOwner = kObjectGroup;
    frame.instance = 0x1005;
    frame.pc = 6;
    frame.params = {2, 0, 0, 0};
    frame.locals = {5};
    frame.stackObjectId = 12;
    frame.runningOnId = object.id;
    object.thread.frames.push_back(frame);

    ts2::xobj::SavedObject loaded;
    loaded.attributes.assign(4, 0);
    loaded.semiAttributes.assign(2, 0);
    loaded.tail.emplace();
    loaded.tail->slots.marker = -1;
    ts2::xobj::StackFrame was;
    was.codeOwner = 41;  // an entry of its own group, which the frame keeps
    was.treeId = 0x1005;  // the same tree, carried on
    was.params = {0};
    was.value1 = 8;
    loaded.tail->stack.push_back(was);

    ts2::sim::TypeOwners owners;
    owners.indexByGroup[kObjectGroup] = 40;
    owners.groupByIndex[40] = kObjectGroup;
    owners.groupByIndex[41] = kObjectGroup;
    const ts2::xobj::SavedObject record = ts2::sim::objectRecord(loaded, object, owners);
    CHECK(record.attributes.size() == 4 && record.attributes[1] == 9 && record.semiAttributes[0] == 3);
    CHECK(record.data[0x20] == 77 && record.temps[2] == -4);
    CHECK(record.header.location[0] == 5.0f && record.header.location[1] == 7.5f && record.header.value1 == 2);
    CHECK((record.tail->arrays == std::vector<std::vector<std::int16_t>>{{1, 2}, {3}}));
    CHECK((record.tail->shorts == std::vector<std::int16_t>{1, 0} && record.tail->slots.values == std::vector<std::int16_t>{11, 0}));
    const ts2::xobj::StackFrame& out = record.tail->stack.at(0);
    // The entry it was saved with still holds its group, so it keeps it rather than taking another.
    CHECK(out.codeOwner == 41 && out.treeId == 0x1005 && out.node == 6);
    // The zeros the load padded the params with go, and the value the loader never reads stays.
    CHECK((out.params == std::vector<std::int16_t>{2} && out.locals == std::vector<std::int16_t>{5} && out.value1 == 8));
    CHECK(out.objectId == 12 && out.value2 == object.id);

    // A frame running another tree at that depth is a new one: it takes its own counts and the
    // object's id, not the shape of the frame saved there.
    loaded.tail->stack[0].treeId = 0x1004;
    const ts2::xobj::SavedObject freshRecord = ts2::sim::objectRecord(loaded, object, owners);
    const ts2::xobj::StackFrame& fresh = freshRecord.tail->stack.at(0);
    CHECK((fresh.params == std::vector<std::int16_t>{2, 0, 0, 0} && fresh.locals == std::vector<std::int16_t>{5} && fresh.value1 == object.id));
    loaded.tail->stack[0].treeId = 0x1005;

    // A frame whose code has no entry leaves the stack as it was read.
    ts2::sim::TypeOwners none;
    CHECK(ts2::sim::objectRecord(loaded, object, none).tail->stack.at(0).codeOwner == 41);
}

// A Sim's record keeps the interaction it is running and the ones waiting in its queue, with the
// fields the save holds that the engine does not work from left as they were read.
void theObjectRecordKeepsASimsInteractions() {
    ts2::sim::World world;
    ts2::sim::SimObject& sim = world.create(kObjectGroup, typed(0x1000));
    ts2::sim::QueuedInteraction running;
    running.object = 5;
    running.index = 3;
    running.action = 0x1234;
    running.priority = 9;
    running.args = {1, 2, 0, 0};
    running.name = "Play";
    running.iconObject = 5;
    running.linkId = 11;
    running.savedId = std::uint16_t{11};  // given back by the load
    running.linkedId = 12;
    running.nested = true;
    sim.running = ts2::sim::Boxed<ts2::sim::QueuedInteraction>(running);
    ts2::sim::QueuedInteraction waiting = running;
    waiting.name = "Play Later";
    waiting.nested = false;
    sim.queue.push_back(waiting);

    ts2::xobj::SavedObject loaded;
    loaded.tail.emplace();
    loaded.person.emplace();
    loaded.person->state.running.number = 7;
    loaded.person->state.running.id = 11;  // the interaction a load gives back with link 11
    loaded.person->state.running.flags = 0x55;
    loaded.person->state.running.lineFlag1 = 1;
    const ts2::xobj::SavedObject record = ts2::sim::objectRecord(loaded, sim, {});
    const ts2::xobj::SavedInteraction& out = record.person->state.running;
    CHECK(out.target == 5 && out.actor == sim.id && out.index == 3 && out.tree == 0x1234 && out.priority == 9);
    CHECK(out.name == "Play" && out.iconObject == 5 && out.id == 11 && out.linkId == 12 && out.lineFlag2 != 0);
    // What the save holds and the engine does not work from keeps what was read.
    CHECK(out.number == 7 && out.flags == 0x55 && out.lineFlag1 == 1);
    // A loaded one loses 0x80 once the queue has shown its name.
    loaded.person->state.running.flags = 0x8C;
    sim.running->nameShown = true;
    CHECK(ts2::sim::objectRecord(loaded, sim, {}).person->state.running.flags == 0x0C);
    sim.running->nameShown = false;
    loaded.person->state.running.flags = 0x55;
    CHECK(record.person->state.queue.size() == 1 && record.person->state.queue[0].name == "Play Later");
    CHECK(record.person->state.queue[0].lineFlag2 == 0);

    // One running a tree of its own, which a load gives line 0, is saved with line -1 again.
    sim.running->index = 0;
    sim.running->ownTree = true;
    CHECK(ts2::sim::objectRecord(loaded, sim, {}).person->state.running.index == -1);

    // An interaction keeps the number of the saved one it was loaded from, found by its id, and one
    // made since takes the next number of the game's counter. The interrupted one stays while the
    // Sim still runs what it was loaded running.
    loaded.person->state.running.id = 11;
    loaded.person->state.interrupted = loaded.person->state.running;
    loaded.person->state.interrupted.id = 30;
    loaded.person->state.interrupted.number = 6;
    sim.running->ownTree = false;
    sim.queue[0].linkId = 0;  // made since the load
    sim.queue[0].savedId.reset();
    ts2::sim::SaveCounters counters;
    counters.lastLinkId = 40;
    counters.lastNumber = 90;
    sim.queue[0].source = ts2::sim::QueuedInteraction::Source::Player;
    sim.queue[0].lineFlags = 0x100;
    sim.queue[0].attenuationCode = 3;  // moderate
    ts2::sim::QueuedInteraction pushed = sim.queue[0];
    pushed.source = ts2::sim::QueuedInteraction::Source::Script;
    pushed.markedAutonomous = true;
    pushed.pushedBy = 151;
    pushed.lineFlags = 0;
    pushed.attenuationCode = 0;  // the line's own value
    pushed.lineAttenuation = 0.5f;
    pushed.testFlags = 0x0C;  // as Push Interaction's test leaves it
    sim.queue.push_back(pushed);
    ts2::sim::TypeOwners owners;
    owners.tunings.attenuation[1] = 0.3f;
    owners.tunings.visitorAttenuation[1] = 0.02f;
    const ts2::xobj::SavedObject busy = ts2::sim::objectRecord(loaded, sim, owners, &counters);
    CHECK(busy.person->state.running.number == 7 && busy.person->state.queue.at(0).number == 91);
    CHECK(busy.person->state.interrupted.number == 6 && counters.lastNumber == 92);
    // A new interaction holds what the game gives one it has just made: flags by who made it, with
    // flag 1 for line flag 0x100, the object whose script pushed it, and the line's attenuation.
    const auto& picked = busy.person->state.queue.at(0);
    CHECK(picked.flags == 0x0D && picked.value2 == 0xFFFF && picked.value5 == 0.3f);
    const auto& byScript = busy.person->state.queue.at(1);
    CHECK(byScript.flags == 0x048C && byScript.value2 == 151 && byScript.number == 92 && byScript.value5 == 0.5f);
    // A push saves the object its picture shows: the Sim it is with, else its object.
    sim.queue.back().iconObject = 0;
    sim.queue.back().partner = 0;
    CHECK(ts2::sim::objectRecord(loaded, sim, owners).person->state.queue.at(1).iconObject == sim.queue.back().object);
    sim.queue.back().partner = 151;
    CHECK(ts2::sim::objectRecord(loaded, sim, owners).person->state.queue.at(1).iconObject == 151);
    sim.queue.back().noIconObject = true;
    CHECK(ts2::sim::objectRecord(loaded, sim, owners).person->state.queue.at(1).iconObject == -1);
    sim.queue.back() = pushed;
    sim.personData[0x20] = 1;  // a visitor
    CHECK(ts2::sim::objectRecord(loaded, sim, owners).person->state.queue.at(0).value5 == 0.02f);
    sim.personData[0x20] = 0;
    // A push gets 0x0C, with 0x400 when marked autonomous, 0x40 when the push marks it autonomous and
    // 2 for a continuation, and autonomy's own pick 0x0C00. Both have 0x80 until they have been the
    // selected Sim's.
    const auto flagsOf = [&](bool marked, bool autonomous, bool held, ts2::sim::QueuedInteraction::Source source) {
        sim.queue.back().markedAutonomous = marked;
        sim.queue.back().autonomous = autonomous;
        sim.queue.back().nameShown = held;
        sim.queue.back().source = source;
        sim.queue.back().testFlags = source == ts2::sim::QueuedInteraction::Source::Script ? std::optional<std::int32_t>(0x0C) : std::nullopt;
        return ts2::sim::objectRecord(loaded, sim, {}).person->state.queue.at(1).flags;
    };
    using Source = ts2::sim::QueuedInteraction::Source;
    CHECK(flagsOf(false, false, false, Source::Script) == 0x008C && flagsOf(true, false, false, Source::Script) == 0x048C);
    CHECK(flagsOf(false, true, false, Source::Script) == 0x00CC);
    sim.queue.back().continuation = true;
    CHECK(flagsOf(false, false, false, Source::Script) == 0x008E);
    sim.queue.back().continuation = false;
    sim.queue.back().holdsContinuations = true;
    CHECK(flagsOf(false, false, false, Source::Script) == 0x408C);
    sim.queue.back().holdsContinuations = false;
    CHECK(flagsOf(true, true, false, Source::Autonomy) == 0x0C80 && flagsOf(true, true, true, Source::Autonomy) == 0x0C00);
    CHECK(flagsOf(true, false, true, Source::Script) == 0x040C && flagsOf(false, false, true, Source::Player) == 0x000C);
    // Its last test's flags replace 0x10 and 0x2000, and a cancelled one has 0x100.
    sim.queue.back().markedAutonomous = false;
    sim.queue.back().autonomous = false;
    sim.queue.back().nameShown = false;
    sim.queue.back().source = Source::Script;
    sim.queue.back().testFlags = 0x1C;
    sim.queue.back().testValue = 4;
    sim.queue.back().cancelled = true;
    const auto written = ts2::sim::objectRecord(loaded, sim, {});
    const auto& tested = written.person->state.queue.at(1);
    CHECK(tested.flags == 0x019C && tested.value6 == 4);
    // One never tested, as the interaction a Sim gets to step out of the way, has 0x80 alone.
    sim.queue.back().testFlags.reset();
    sim.queue.back().cancelled = false;
    CHECK(ts2::sim::objectRecord(loaded, sim, {}).person->state.queue.at(1).flags == 0x0080);
    sim.queue.back().testFlags.reset();
    sim.queue.back().cancelled = false;
    // A tick marks what the selected Sim's queue shows: the running interaction by the Sim's action
    // picture, the others by their own.
    const std::int16_t shown = world.create(kObjectGroup, typed(0x1000)).id;
    sim.actionIconObject = shown;
    sim.queue.back().iconObject = shown;
    sim.queue.back().nameShown = false;
    std::array<std::int16_t, 4> globals{};
    globals[3] = static_cast<std::int16_t>(sim.id + 1);
    ts2::sim::markShownNames(world, globals);
    CHECK(!sim.queue.back().nameShown);
    globals[3] = sim.id;
    const std::int16_t icon = sim.queue.front().iconObject;
    sim.queue.front().iconObject = 0;
    sim.queue.front().noIconObject = true;  // no picture object: it keeps 0x80
    ts2::sim::markShownNames(world, globals);
    CHECK(sim.queue.back().nameShown && sim.running->nameShown && !sim.queue.front().nameShown);
    sim.queue.front().iconGuid = 0x2C85C27C;  // a picture by object type is shown too
    ts2::sim::markShownNames(world, globals);
    CHECK(sim.queue.front().nameShown);
    sim.queue.front().nameShown = false;
    sim.queue.front().iconGuid = 0;
    sim.queue.front().noIconObject = false;
    sim.queue.front().iconObject = 9999;  // an object no longer on the lot is not
    ts2::sim::markShownNames(world, globals);
    CHECK(!sim.queue.front().nameShown);
    sim.queue.front().iconObject = icon;
    sim.queue.pop_back();

    // A Sim that has finished what it was saved doing gets the blank interaction the game writes,
    // as does the interrupted place, each with a new id from the counter, and no sub-queue one.
    sim.running.reset();
    sim.queue.clear();
    loaded.person->state.runningSub = loaded.person->state.running;
    const ts2::xobj::SavedObject idle = ts2::sim::objectRecord(loaded, sim, {}, &counters);
    const auto& state = idle.person->state;
    CHECK(ts2::xobj::isBlank(state.interrupted) && state.interrupted.flags == 16 && state.interrupted.id == 41 &&
          state.interrupted.linkId == 41);
    CHECK(ts2::xobj::isBlank(state.running) && state.running.id == 42 && !state.runningSub && counters.lastLinkId == 42);
    // A blank the Sim still holds from the load stays as it was.
    ts2::xobj::SavedObject again = idle;
    sim.blankId = 42;
    CHECK(ts2::sim::objectRecord(again, sim, {}, &counters).person->state.running.id == 42 && counters.lastLinkId == 42);
    // One that has run an interaction since has a new blank, with an id of its own.
    sim.blankId = 0;
    CHECK(ts2::sim::objectRecord(again, sim, {}, &counters).person->state.running.id == 43);
    // A new interaction that drew the id of one saved before is not taken for it.
    ts2::sim::QueuedInteraction drawn;
    drawn.linkId = 11;
    drawn.linkedId = 11;
    drawn.name = "New";
    sim.queue = {drawn};
    loaded.person->state.queue = {loaded.person->state.running};
    loaded.person->state.queue[0].id = 11;
    loaded.person->state.queue[0].number = 7;
    const ts2::xobj::SavedObject redrawn = ts2::sim::objectRecord(loaded, sim, {}, &counters);
    CHECK(redrawn.person->state.queue.size() == 1 && redrawn.person->state.queue[0].number != 7);
    sim.queue.clear();
    loaded.person->state.queue.clear();
    // Nested interactions waiting go to the sub-queue, and one that has finished since the load is
    // gone from it.
    loaded.person->state.subQueue = {loaded.person->state.running};
    ts2::sim::QueuedInteraction nested;
    nested.nested = true;
    nested.name = "Nested";
    sim.queue = {nested};
    const ts2::xobj::SavedObject splitRecord = ts2::sim::objectRecord(loaded, sim, {}, &counters);
    const auto& split = splitRecord.person->state;
    CHECK(split.queue.empty() && split.subQueue.size() == 1 && split.subQueue[0].name == "Nested");
}

// An object made since the lot loaded is given a record of its own, started from what the lot
// holds for another object of its type.
void savingALotGivesNewObjectsARecord() {
    ts2::objt::ObjectTypeTable types;
    ts2::objt::Entry entry;
    entry.guid = 0x1000;
    entry.index = 40;
    types.entries.push_back(entry);
    ts2::sim::World world;
    ts2::sim::SimObject& was = world.create(kObjectGroup, typed(0x1000));
    ts2::sim::SimObject& made = world.create(kObjectGroup, typed(0x1000));
    made.data[0x20] = 55;
    const std::int32_t wasId = was.id, madeId = made.id;

    ts2::xobj::SavedObject record;
    record.version = ts2::xobj::kWrittenVersion;
    record.attributes.assign(3, 0);
    record.tail.emplace();
    record.tail->relationshipVersion = 2;
    ts2::objm::MapRecord map;
    map.objects = {{wasId, 40}};
    map.waits.assign(4, 0);

    ts2::dbpf::PackageImage package;
    package.resources = {
        ts2::dbpf::storeResource({ts2::types::kObjectMap, 0xFFFFFFFF, 1, 0}, ts2::objm::writeMapRecord(map)),
        ts2::dbpf::storeResource({ts2::types::kObjectTypeTable, 0xFFFFFFFF, 0, 0}, ts2::objt::write(types)),
        ts2::dbpf::storeResource({ts2::types::kSavedObject, 0xFFFFFFFF, static_cast<std::uint32_t>(wasId), 0},
                                 ts2::xobj::write(record, ts2::xobj::RecordKind::Object)),
    };
    package.dataOrder = {0, 1, 2};
    const std::array<std::int16_t, ts2::simi::kGlobalCount> globals{};
    const ts2::sim::LotSaveCounts counts = ts2::sim::fillLotPackage(package, world, globals, 0, {}, nullptr);
    CHECK(counts.objects == 1 && counts.objectsAdded == 1 && counts.objectsUnnamed == 0);
    CHECK(package.resources.size() == 4 && package.dataOrder.size() == 4);
    const auto& added = package.resources.back();
    CHECK(added.key.type == ts2::types::kSavedObject && added.key.instance == static_cast<std::uint32_t>(madeId));
    const auto body = added.compressed ? ts2::qfs::decompress(added.bytes) : added.bytes;
    const auto read = ts2::xobj::parse(body, ts2::xobj::kWrittenVersion, ts2::xobj::RecordKind::Object);
    // It takes the shape the lot gives that type and the state of the object it is for.
    CHECK(read.attributes.size() == 3 && read.data[0x20] == 55);
}

// A save writes the effects each object plays now in its record, in the list's second version as
// the game writes it: the plumbob's glow, kept by its saved byte. One whose effects have all ended
// keeps an empty list.
void savingALotWritesTheEffectsPlayingNow() {
    ts2::objt::ObjectTypeTable types;
    ts2::objt::Entry entry;
    entry.guid = 0x1000;
    entry.index = 40;
    types.entries.push_back(entry);
    ts2::sim::World world;
    ts2::sim::SimObject& glowing = world.create(kObjectGroup, typed(0x1000));
    ts2::sim::SimObject& ended = world.create(kObjectGroup, typed(0x1000));
    ts2::xobj::SavedObject record;
    record.version = ts2::xobj::kWrittenVersion;
    record.attributes.assign(3, 0);
    record.tail.emplace();
    record.tail->relationshipVersion = 2;
    ts2::xobj::SavedObject old = record;
    old.tail->effectFlag = 1;
    old.tail->effectVersion = 1;
    old.tail->effects = {{"pizzaSteam", "slot_pizzaBox_pizzaSteam", 0, 2, 0}};
    ts2::objm::MapRecord map;
    map.objects = {{glowing.id, 40}, {ended.id, 40}};
    map.waits.assign(4, 0);
    ts2::dbpf::PackageImage package;
    package.resources = {
        ts2::dbpf::storeResource({ts2::types::kObjectMap, 0xFFFFFFFF, 1, 0}, ts2::objm::writeMapRecord(map)),
        ts2::dbpf::storeResource({ts2::types::kObjectTypeTable, 0xFFFFFFFF, 0, 0}, ts2::objt::write(types)),
        ts2::dbpf::storeResource({ts2::types::kSavedObject, 0xFFFFFFFF, static_cast<std::uint32_t>(glowing.id), 0},
                                 ts2::xobj::write(record, ts2::xobj::RecordKind::Object)),
        ts2::dbpf::storeResource({ts2::types::kSavedObject, 0xFFFFFFFF, static_cast<std::uint32_t>(ended.id), 0},
                                 ts2::xobj::write(old, ts2::xobj::RecordKind::Object)),
    };
    package.dataOrder = {0, 1, 2, 3};
    ts2::sim::TypeOwners owners;
    owners.runningEffects = [&](std::int16_t id) {
        return id == glowing.id ? std::vector<ts2::xobj::Effect>{{"fxPlumbobGlow_75", "slot_plumbob_0_fx", 0, 2, 192}}
                                : std::vector<ts2::xobj::Effect>{};
    };
    const std::array<std::int16_t, ts2::simi::kGlobalCount> globals{};
    ts2::sim::fillLotPackage(package, world, globals, 0, owners, nullptr);
    const auto read = [&](std::int16_t id) {
        for (const auto& r : package.resources) {
            if (r.key.type == ts2::types::kSavedObject && r.key.instance == static_cast<std::uint32_t>(id)) {
                return ts2::xobj::parse(r.compressed ? ts2::qfs::decompress(r.bytes) : r.bytes, ts2::xobj::kWrittenVersion,
                                        ts2::xobj::RecordKind::Object);
            }
        }
        return ts2::xobj::SavedObject{};
    };
    const auto a = read(glowing.id), b = read(ended.id);
    CHECK(a.tail && a.tail->effectFlag == 1 && a.tail->effectVersion == 2 && a.tail->effects.size() == 1);
    CHECK(a.tail && !a.tail->effects.empty() && a.tail->effects[0].name == "fxPlumbobGlow_75" && a.tail->effects[0].value3 == 192);
    CHECK(b.tail && b.tail->effectFlag == 1 && b.tail->effects.empty());
}

// A save writes each installed type's values into the lot's type table, so the next load finds the
// type unchanged and keeps its objects running. Each entry keeps its place and its fifth value.
void savingALotWritesTheTypesValuesNow() {
    ts2::objt::ObjectTypeTable types;
    types.version = 0x4E;
    ts2::objt::Entry entry;
    entry.guid = 0x1000;
    entry.index = 40;
    entry.name = "Lamp";
    entry.unknown = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    types.entries.push_back(entry);
    entry.guid = 0x2000;  // not installed
    entry.index = 41;
    types.entries.push_back(entry);
    ts2::dbpf::PackageImage package;
    package.resources = {
        ts2::dbpf::storeResource({ts2::types::kObjectTypeTable, 0xFFFFFFFF, 0, 0}, ts2::objt::write(types)),
    };
    package.dataOrder = {0};
    ts2::sim::TypeOwners owners;
    owners.savedValues[0x1000] = {0x00120034, 0x00200040, 5, 6};
    const ts2::sim::World world;
    const std::array<std::int16_t, ts2::simi::kGlobalCount> globals{};
    const ts2::sim::LotSaveCounts counts = ts2::sim::fillLotPackage(package, world, globals, 0, owners, nullptr);
    CHECK(counts.typeTable == 1);
    const auto& stored = package.resources[0];
    const auto written = ts2::objt::parse(stored.compressed ? ts2::qfs::decompress(stored.bytes) : stored.bytes);
    CHECK(written.version == ts2::objt::kWrittenVersion && written.entries.size() == 2);
    const std::array<std::uint16_t, 10> now{0x34, 0x12, 0x40, 0x20, 5, 0, 6, 0, 9, 10};
    CHECK(written.entries[0].unknown == now && written.entries[0].index == 40 && written.entries[0].name == "Lamp");
    CHECK(written.entries[1].unknown == entry.unknown && written.entries[1].index == 41);
}

// A new object that gets no record, because no object of its type on the lot has one to start from
// and its type keeps a material block, stays out of the object map. Its id can hold a record left
// from an object deleted long ago, which the game would read as this object's.
void savingALotListsOnlyNewObjectsWithARecord() {
    ts2::objt::ObjectTypeTable types;
    ts2::objt::Entry entry;
    entry.guid = 0x1000;
    entry.index = 40;
    types.entries.push_back(entry);
    entry.guid = 0x3000;
    entry.index = 41;
    entry.flag = 1;
    types.entries.push_back(entry);
    ts2::sim::World world;
    const std::int32_t wasId = world.create(kObjectGroup, typed(0x1000)).id;
    const std::int32_t madeId = world.create(kObjectGroup, typed(0x3000)).id;

    ts2::xobj::SavedObject record;
    record.version = ts2::xobj::kWrittenVersion;
    record.tail.emplace();
    record.tail->relationshipVersion = 3;
    ts2::objm::MapRecord map;
    map.objects = {{wasId, 40}};
    map.waits.assign(4, 0);
    const std::vector<std::uint8_t> leftover{9, 9, 9, 9};

    ts2::dbpf::PackageImage package;
    package.resources = {
        ts2::dbpf::storeResource({ts2::types::kObjectMap, 0xFFFFFFFF, 1, 0}, ts2::objm::writeMapRecord(map)),
        ts2::dbpf::storeResource({ts2::types::kObjectTypeTable, 0xFFFFFFFF, 0, 0}, ts2::objt::write(types)),
        ts2::dbpf::storeResource({ts2::types::kSavedObject, 0xFFFFFFFF, static_cast<std::uint32_t>(wasId), 0},
                                 ts2::xobj::write(record, ts2::xobj::RecordKind::Object)),
        ts2::dbpf::storeResource({ts2::types::kSavedObject, 0xFFFFFFFF, static_cast<std::uint32_t>(madeId), 0}, leftover),
    };
    package.dataOrder = {0, 1, 2, 3};
    const std::array<std::int16_t, ts2::simi::kGlobalCount> globals{};
    const ts2::sim::LotSaveCounts counts = ts2::sim::fillLotPackage(package, world, globals, 0, {}, nullptr);
    CHECK(counts.objects == 1 && counts.objectsAdded == 0 && counts.objectsUnnamed == 1 && counts.objectMap == 1);
    const auto body = [](const ts2::dbpf::StoredResource& resource) {
        return resource.compressed ? ts2::qfs::decompress(resource.bytes) : resource.bytes;
    };
    const auto written = ts2::objm::readMapRecord(body(package.resources[0]));
    CHECK(written && written->objects.size() == 1 && written->objects[0].first == wasId);
    CHECK(written && written->waitsWithObject == 1);
    CHECK(body(package.resources[3]) == leftover);
}

// Ids are reused. An object made at the id of one the lot was saved with, of another type, is a new
// object: the old object's record is not written with its state, and it gets its own. New objects of
// a type that lost an object are saved too, unless the lot made them as it loaded, which is the lot
// making a saved controller again.
void savingALotTellsReusedIdsApart() {
    ts2::objt::ObjectTypeTable types;
    ts2::objt::Entry entry;
    entry.guid = 0x1000;  // a lamp
    entry.index = 40;
    types.entries.push_back(entry);
    entry.guid = 0x6000;  // a plate
    entry.index = 41;
    types.entries.push_back(entry);
    entry.guid = 0x7000;  // a controller
    entry.index = 42;
    types.entries.push_back(entry);
    ts2::sim::World world;
    ts2::sim::SimObject& lamp = world.create(kObjectGroup, typed(0x1000));
    const std::int32_t lampId = lamp.id;
    const std::int32_t plateId = world.create(kObjectGroup, typed(0x6000)).id;
    ts2::sim::SimObject& eaten = world.create(kObjectGroup, typed(0x6000));
    const std::int32_t eatenId = eaten.id;
    ts2::sim::SimObject& gone = world.create(kObjectGroup, typed(0x7000));
    const std::int32_t goneId = gone.id;
    world.remove(lamp);
    world.remove(eaten);
    world.remove(gone);
    world.purge();
    ts2::sim::SimObject& newPlate = world.create(kObjectGroup, typed(0x6000));  // takes the lamp's id
    newPlate.data[0x20] = 66;
    const std::int32_t controllerId = world.create(kObjectGroup, typed(0x7000)).id;
    const std::int32_t laterPlateId = world.create(kObjectGroup, typed(0x6000)).id;
    CHECK(newPlate.id == lampId);

    const auto recordWith = [](std::size_t attributes, std::int16_t data20) {
        ts2::xobj::SavedObject record;
        record.version = ts2::xobj::kWrittenVersion;
        record.attributes.assign(attributes, 0);
        record.data[0x20] = data20;
        record.tail.emplace();
        record.tail->relationshipVersion = 3;
        return ts2::xobj::write(record, ts2::xobj::RecordKind::Object);
    };
    ts2::objm::MapRecord map;
    map.objects = {{lampId, 40}, {plateId, 41}, {eatenId, 41}, {goneId, 42}};
    map.waits.assign(16, 0);
    ts2::dbpf::PackageImage package;
    package.resources = {
        ts2::dbpf::storeResource({ts2::types::kObjectMap, 0xFFFFFFFF, 1, 0}, ts2::objm::writeMapRecord(map)),
        ts2::dbpf::storeResource({ts2::types::kObjectTypeTable, 0xFFFFFFFF, 0, 0}, ts2::objt::write(types)),
        ts2::dbpf::storeResource({ts2::types::kSavedObject, 0xFFFFFFFF, static_cast<std::uint32_t>(lampId), 0}, recordWith(12, 11)),
        ts2::dbpf::storeResource({ts2::types::kSavedObject, 0xFFFFFFFF, static_cast<std::uint32_t>(plateId), 0}, recordWith(8, 22)),
        ts2::dbpf::storeResource({ts2::types::kSavedObject, 0xFFFFFFFF, static_cast<std::uint32_t>(eatenId), 0}, recordWith(8, 33)),
        ts2::dbpf::storeResource({ts2::types::kSavedObject, 0xFFFFFFFF, static_cast<std::uint32_t>(goneId), 0}, recordWith(8, 44)),
    };
    package.dataOrder = {0, 1, 2, 3, 4, 5};
    ts2::sim::TypeOwners owners;
    owners.madeAsLoaded = {static_cast<std::int16_t>(controllerId)};
    const std::array<std::int16_t, ts2::simi::kGlobalCount> globals{};
    const ts2::sim::LotSaveCounts counts = ts2::sim::fillLotPackage(package, world, globals, 0, owners, nullptr);
    CHECK(counts.objects == 1 && counts.objectsAdded == 2 && counts.objectsRemade == 1);
    const auto body = [](const ts2::dbpf::StoredResource& resource) {
        return resource.compressed ? ts2::qfs::decompress(resource.bytes) : resource.bytes;
    };
    const auto written = ts2::objm::readMapRecord(body(package.resources[0]));
    std::map<std::int32_t, std::int32_t> listed;
    if (written) {
        listed.insert(written->objects.begin(), written->objects.end());
    }
    CHECK(listed.size() == 3 && listed[lampId] == 41 && listed[plateId] == 41 && listed[laterPlateId] == 41);
    CHECK(!listed.contains(controllerId));  // freed ids are given out lowest first, so it took the eaten plate's
    // The lamp's id now holds the new plate's record, started from the other plate's.
    const auto atLampId = ts2::xobj::parse(body(package.resources[2]), ts2::xobj::kWrittenVersion, ts2::xobj::RecordKind::Object);
    CHECK(atLampId.attributes.size() == 8 && atLampId.data[0x20] == 66);
}

// A type with an object whose stack stays as it was loaded keeps the values it was loaded with in
// the type table, so the game resets that object rather than resume places in trees that changed.
void savingALotKeepsStaleTypesValues() {
    ts2::objt::ObjectTypeTable types;
    ts2::objt::Entry entry;
    entry.guid = 0x1000;
    entry.index = 40;
    entry.unknown = {1, 2, 3, 4, 5, 6, 7, 8, 0, 0};
    types.entries.push_back(entry);
    ts2::sim::World world;
    ts2::sim::SimObject& object = world.create(kObjectGroup, typed(0x1000));
    ts2::sim::Frame frame;
    frame.codeOwner = 0x7F999999;  // code whose group no entry of the table holds
    object.thread.frames.push_back(frame);
    ts2::xobj::SavedObject record;
    record.version = ts2::xobj::kWrittenVersion;
    record.attributes.assign(8, 0);
    record.tail.emplace();
    record.tail->relationshipVersion = 3;
    ts2::objm::MapRecord map;
    map.objects = {{object.id, 40}};
    map.waits.assign(4, 0);
    ts2::dbpf::PackageImage package;
    package.resources = {
        ts2::dbpf::storeResource({ts2::types::kObjectMap, 0xFFFFFFFF, 1, 0}, ts2::objm::writeMapRecord(map)),
        ts2::dbpf::storeResource({ts2::types::kObjectTypeTable, 0xFFFFFFFF, 0, 0}, ts2::objt::write(types)),
        ts2::dbpf::storeResource({ts2::types::kSavedObject, 0xFFFFFFFF, static_cast<std::uint32_t>(object.id), 0},
                                 ts2::xobj::write(record, ts2::xobj::RecordKind::Object)),
    };
    package.dataOrder = {0, 1, 2};
    ts2::sim::TypeOwners owners;
    owners.savedValues[0x1000] = {0x00120034, 0x00200040, 5, 6};
    const std::array<std::int16_t, ts2::simi::kGlobalCount> globals{};
    const ts2::sim::LotSaveCounts counts = ts2::sim::fillLotPackage(package, world, globals, 0, owners, nullptr);
    CHECK(counts.stacksKept == 1 && counts.typeTable == 1);
    const auto& stored = package.resources[1];
    const auto written = ts2::objt::parse(stored.compressed ? ts2::qfs::decompress(stored.bytes) : stored.bytes);
    CHECK(written.entries.size() == 1 && written.entries[0].unknown == entry.unknown);
}

// Ids a save has to make go on from the simulator's id counter (global 0x24), which is past every id
// it gave out, and the save leaves the counter as it is. A number at the highest there is, which only
// a broken file holds, starts the count again at 1.
void savingALotKeepsInteractionIdsApart() {
    ts2::objt::ObjectTypeTable types;
    ts2::objt::Entry entry;
    entry.guid = 0x51515151;
    entry.index = 40;
    entry.kind = ts2::objt::kPersonKind;
    types.entries.push_back(entry);
    ts2::sim::World world;
    ts2::sim::SimObject& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    ts2::sim::QueuedInteraction now;
    now.object = sim.id;
    now.name = "Dance";
    sim.running = ts2::sim::Boxed<ts2::sim::QueuedInteraction>(now);

    ts2::xobj::SavedObject record;
    record.version = ts2::xobj::kWrittenVersion;
    record.attributes.assign(8, 0);
    record.tail.emplace();
    record.tail->relationshipVersion = 3;
    record.person.emplace();
    record.person->version = 2;
    record.person->personData.assign(ts2::xobj::kPersonDataCount, 0);
    auto& state = record.person->state;
    state.running.index = -1;
    state.running.iconObject = -1;
    state.running.id = 500;  // a blank holding the lot's highest id
    state.running.linkId = 500;
    state.interrupted.number = INT32_MAX;
    state.interrupted.id = 7;
    ts2::objm::MapRecord map;
    map.objects = {{sim.id, 40}};
    map.waits.assign(4, 0);
    ts2::dbpf::PackageImage package;
    package.resources = {
        ts2::dbpf::storeResource({ts2::types::kObjectMap, 0xFFFFFFFF, 1, 0}, ts2::objm::writeMapRecord(map)),
        ts2::dbpf::storeResource({ts2::types::kObjectTypeTable, 0xFFFFFFFF, 0, 0}, ts2::objt::write(types)),
        ts2::dbpf::storeResource({ts2::types::kSavedPerson, 0xFFFFFFFF, static_cast<std::uint32_t>(sim.id), 0},
                                 ts2::xobj::write(record, ts2::xobj::RecordKind::Person)),
    };
    package.dataOrder = {0, 1, 2};
    std::array<std::int16_t, ts2::simi::kGlobalCount> globals{};
    globals[0x24] = 501;
    const ts2::sim::LotSaveCounts counts = ts2::sim::fillLotPackage(package, world, globals, 0, {}, nullptr);
    CHECK(counts.objects == 1 && counts.lastLinkId == 501 && globals[0x24] == 501);
    const auto& stored = package.resources[2];
    const auto written = ts2::xobj::parse(stored.compressed ? ts2::qfs::decompress(stored.bytes) : stored.bytes,
                                          ts2::xobj::kWrittenVersion, ts2::xobj::RecordKind::Person);
    CHECK(written.person && written.person->state.running.number == 1 && written.person->state.running.name == "Dance");
    CHECK(written.person && ts2::xobj::isBlank(written.person->state.interrupted) && written.person->state.interrupted.id == 501);
}

// Link numbers and the number an interaction takes as it starts come from the simulator's id
// counter, as the game's do, and a blank interaction keeps the one it was given when it was made.
void interactionIdsComeFromTheSimulatorCounter() {
    std::array<std::int16_t, ts2::simi::kGlobalCount> globals{};
    globals[0x24] = -1;  // 65535, the last before the counter wraps
    ts2::sim::QueuedInteraction pushed;
    CHECK(ts2::sim::ownLinkId(globals, pushed) == 65535 && pushed.linkedId == 65535 && globals[0x24] == 0);
    ts2::sim::QueuedInteraction other;
    CHECK(ts2::sim::ownLinkId(globals, other) == 1 && globals[0x24] == 2);  // 0 is never given out
    ts2::sim::World world;
    ts2::sim::SimObject& sim = world.create(kObjectGroup, typed(0x1000, ts2::sim::kPersonType));
    ts2::sim::QueuedInteraction alone;
    ts2::sim::vm_detail::startRunning(sim, alone, globals);
    CHECK(sim.running->linkId == 2 && sim.running->linkedId == 2 && globals[0x24] == 3);
    ts2::sim::vm_detail::startRunning(sim, other, globals);  // a linked one keeps its numbers
    CHECK(sim.running->linkId == 1 && globals[0x24] == 3);
}

// A save refuses while a Sim made this run is on the lot, as only the lot is written and the game
// would load that Sim with no neighbor behind it. A Sim the neighborhood had saves.
// A save writes each Sim on the lot into its description in the neighborhood: the person data the
// game's description takes from the Sim, and its own for the lot-only fields, as the neighbor's
// working copy holds them.
void savingALotWritesTheSimsDescriptions() {
    ts2::sim::World world;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {9};
    hood->neighbors[9] = {};
    hood->neighbors[9].guid = 0x51515151;
    hood->neighbors[9].personData.assign(231, 7);
    world.setNeighborhood(hood);
    ts2::sim::SimObject& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.personData[0x6D] = 1234;  // the current interaction's id, which the description takes
    sim.personData[0x1F] = 9;     // the neighbor id, which it does not
    (*world.neighborPersonData(9))[0x50] = 55;  // set by a script while the lot ran
    ts2::sdsc::Record record;
    record.personData.fill(7);
    record.neighborId = 9;
    record.guid = 0x51515151;
    ts2::dbpf::PackageImage neighborhood;
    neighborhood.resources = {ts2::dbpf::storeResource({ts2::sdsc::kType, 0xFFFFFFFF, 9, 0}, ts2::sdsc::writeRecord(record))};
    neighborhood.dataOrder = {0};
    const ts2::sim::HoodSaveCounts counts = ts2::sim::fillNeighborhoodPackage(neighborhood, {}, 1, world, {});
    const auto& stored = neighborhood.resources[0];
    const auto written = ts2::sdsc::readRecord(stored.compressed ? ts2::qfs::decompress(stored.bytes) : stored.bytes);
    CHECK(counts.descriptions == 1 && written && written->personData[0x6D] == 1234 && written->personData[0x1F] == 7 &&
          written->personData[0x50] == 55 && written->personData[0x02] == 0);
}

// A save writes the inventories scripts changed into the neighborhood record: the token a birth
// takes from the parent is gone, and a new Sim's inventory goes in by its id, as the game keeps each
// list in ascending order. Untouched inventories stay as they were.
void savingALotWritesTheChangedInventories() {
    ts2::ngbh::Record saved;
    saved.global.version = ts2::ngbh::kInventoryVersion;
    const ts2::xobj::Token pregnancy{0xED4CB1A4, 0, 0, 0, 0, {9, 361}};
    saved.neighbors = {{248, {ts2::ngbh::kInventoryVersion, {}, {pregnancy}}}, {300, {ts2::ngbh::kInventoryVersion, {}, {pregnancy}}}};
    const std::vector<std::uint8_t> bytes = ts2::ngbh::writeRecord(saved);
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->record = std::make_shared<ts2::ngbh::Neighborhood>(ts2::ngbh::parse(bytes));
    ts2::sim::World world;
    world.setNeighborhood(hood);
    world.inventory(ts2::sim::InventoryKind::Neighbor, 248)->singular.clear();
    world.inventory(ts2::sim::InventoryKind::Neighbor, 269)->singular.push_back(
        std::make_shared<ts2::sim::InventoryToken>(ts2::sim::InventoryToken{0x33F6E0FB, 0, 0, 0, 0, {1}}));
    ts2::dbpf::PackageImage neighborhood;
    neighborhood.resources = {ts2::dbpf::storeResource({ts2::ngbh::kType, 0xFFFFFFFF, 1, 0}, bytes)};
    neighborhood.dataOrder = {0};
    const ts2::sim::HoodSaveCounts counts = ts2::sim::fillNeighborhoodPackage(neighborhood, {}, 1, world, {});
    const auto& stored = neighborhood.resources[0];
    const auto written = ts2::ngbh::readRecord(stored.compressed ? ts2::qfs::decompress(stored.bytes) : stored.bytes);
    CHECK(counts.inventories == 2 && written && written->neighbors.size() == 3);
    CHECK(written->neighbors[0].first == 248 && written->neighbors[0].second.singular.empty());
    CHECK(written->neighbors[1].first == 269 && written->neighbors[1].second.singular.size() == 1 &&
          written->neighbors[1].second.singular[0].guid == 0x33F6E0FB && written->neighbors[1].second.version == ts2::ngbh::kInventoryVersion);
    CHECK(written->neighbors[2].first == 300 && written->neighbors[2].second.singular.size() == 1);
}

// The family living on the lot saves the lot's funds as both its funds and its last saved funds,
// and another family keeps its own. A relationship record takes the neighbor's values as they are
// now, grown ones included.
void savingALotWritesTheFamilyFundsAndRelationships() {
    ts2::sim::World world;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {9, 10};
    hood->familyLots = {{1, 4}, {2, 7}};
    hood->relationships[{std::int16_t{9}, std::int16_t{10}}] = {45, 49152, 20};
    world.setNeighborhood(hood);
    world.setLotNumber(4);
    world.neighborRelationship(9, 10).resize(10, 0);
    ts2::fami::Family family;
    family.version = ts2::fami::kWrittenVersion;
    family.lot = 4;
    family.funds = family.funds2 = 4242;
    ts2::fami::Family other = family;
    other.lot = 7;
    std::array<std::int16_t, ts2::simi::kGlobalCount> globals{};
    ts2::sim::setFunds(globals, 742);
    ts2::dbpf::PackageImage neighborhood;
    neighborhood.resources = {ts2::dbpf::storeResource({ts2::fami::kType, 0xFFFFFFFF, 1, 0}, ts2::fami::write(family)),
                              ts2::dbpf::storeResource({ts2::fami::kType, 0xFFFFFFFF, 2, 0}, ts2::fami::write(other)),
                              ts2::dbpf::storeResource({ts2::srel::kType, 0xFFFFFFFF, 0x9000A, 0},
                                                       ts2::srel::writeRecord({{45, 49152, 20}}))};
    neighborhood.dataOrder = {0, 1, 2};
    const ts2::sim::HoodSaveCounts counts = ts2::sim::fillNeighborhoodPackage(neighborhood, {}, 1, world, globals);
    const auto body = [&](std::size_t i) {
        const auto& stored = neighborhood.resources[i];
        return stored.compressed ? ts2::qfs::decompress(stored.bytes) : stored.bytes;
    };
    const ts2::fami::Family saved = ts2::fami::parse(body(0)), kept = ts2::fami::parse(body(1));
    const auto relationship = ts2::srel::readRecord(body(2));
    CHECK(counts.family && saved.funds == 742 && saved.funds2 == 742 && kept.funds == 4242 && kept.funds2 == 4242);
    const std::vector<std::int32_t> grown{45, 49152, 20, 0, 0, 0, 0, 0, 0, 0};
    CHECK(counts.relationships == 1 && relationship && relationship->values == grown);
}

// The order the game saves a description's relationship list in, from its own saves: Vidcund
// Curious's list after he met Armando Cox (0x1AE), and a list whose 0x37 and 0x7A share a bucket
// and swap when it is loaded and saved again.
void relationshipListsTakeTheGamesTableOrder() {
    const std::vector<std::uint32_t> vidcund{0xE4, 0x65, 0x13A, 0x72, 0xF9, 0x34, 0x71, 0xFA, 0x3B, 0xF8, 0xE3, 0xE5, 0x31, 0x2B, 0x32};
    const std::vector<std::uint32_t> met{0x1AE};
    const std::vector<std::uint32_t> withArmando{0xE4, 0x1AE, 0x65, 0x13A, 0x72, 0xF9, 0x34, 0x71, 0xFA, 0x3B, 0xF8, 0xE3, 0xE5, 0x31, 0x2B, 0x32};
    std::vector<std::uint32_t> partners = vidcund;
    partners.push_back(0x1AE);
    CHECK(ts2::sim::savedRelationshipList(ts2::sim::loadedRelationshipTable(vidcund), partners, met) == withArmando);
    const std::vector<std::uint32_t> saved{0x77, 0x79, 0xF9, 0x7E, 0x64, 0x2A, 0x34, 0x76, 0xF8, 0x36, 0x31, 0x38, 0x2B, 0x2D, 0x35, 0x37, 0x7A};
    const std::vector<std::uint32_t> savedAgain{0x77, 0x79, 0xF9, 0x7E, 0x64, 0x2A, 0x34, 0x76, 0xF8, 0x36, 0x31, 0x38, 0x2B, 0x2D, 0x35, 0x7A, 0x37};
    const std::vector<std::uint32_t> table = ts2::sim::loadedRelationshipTable(saved);
    CHECK(table == savedAgain && ts2::sim::savedRelationshipList(table, saved, {}) == savedAgain);
}

// A relationship made during play that holds values gets a record, and its neighbor's description
// lists it. One a failed lookup left empty is not saved.
void savingALotAddsNewRelationships() {
    ts2::sim::World world;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {0x2B, 0x1AE};
    hood->neighbors[0x2B] = {};
    hood->neighbors[0x2B].relationshipTable = {0xF9};
    hood->neighbors[0x1AE] = {};
    hood->relationships[{std::int16_t{0x2B}, std::int16_t{0xF9}}] = {1};
    world.setNeighborhood(hood);
    world.neighborRelationship(0x2B, 0x1AE).assign(10, 0);
    world.neighborRelationship(0x2B, 0x1AE)[0] = 8;
    world.neighborRelationship(0x1AE, 0x2B);  // looked up and left empty
    ts2::sdsc::Record record;
    record.neighborId = 0x2B;
    record.relationships = {0xF9};
    ts2::sdsc::Record other;
    other.neighborId = 0x1AE;
    ts2::dbpf::PackageImage neighborhood;
    neighborhood.resources = {ts2::dbpf::storeResource({ts2::sdsc::kType, 0xFFFFFFFF, 0x2B, 0}, ts2::sdsc::writeRecord(record)),
                              ts2::dbpf::storeResource({ts2::sdsc::kType, 0xFFFFFFFF, 0x1AE, 0}, ts2::sdsc::writeRecord(other)),
                              ts2::dbpf::storeResource({ts2::srel::kType, 0xFFFFFFFF, 0x2B00F9, 0}, ts2::srel::writeRecord({{1}}))};
    neighborhood.dataOrder = {0, 1, 2};
    const ts2::sim::HoodSaveCounts counts = ts2::sim::fillNeighborhoodPackage(neighborhood, {}, 1, world, {});
    const auto body = [&](std::size_t i) {
        const auto& stored = neighborhood.resources[i];
        return stored.compressed ? ts2::qfs::decompress(stored.bytes) : stored.bytes;
    };
    const auto vidcund = ts2::sdsc::readRecord(body(0));
    const std::vector<std::uint32_t> listed{0x1AE, 0xF9};
    CHECK(counts.relationships == 1 && counts.descriptions == 1 && neighborhood.resources.size() == 4 && vidcund &&
          vidcund->relationships == listed);
    const auto made = ts2::srel::readRecord(body(3));
    CHECK(neighborhood.resources[3].key.instance == 0x2B01AE && made && made->values.size() == 10 && made->values[0] == 8 &&
          neighborhood.dataOrder.size() == 4);
}

// A baby born this run gets its description and DNA in the neighborhood, joins its family's
// members after the others and the family ties, and the counter new neighbor ids start from moves
// past it. The ids come from that saved counter, as Star Curious's 0x301 did.
void savingALotWritesANewbornsNeighborhoodRecords() {
    ts2::sim::World world;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {0xF8};
    hood->neighbors[0xF8] = {};
    hood->neighbors[0xF8].guid = 0xCDBA17B7;
    hood->familyLots = {{1, 4}};
    hood->familyMembers[1] = {0xCDBA17B7};
    hood->familyTies[0xF8] = {{3, std::int16_t{0x2B}}};
    hood->nextNeighborId = std::int16_t{0x301};
    world.setNeighborhood(hood);
    ts2::sim::SavedNeighbor star;
    star.guid = 0xB805E5AA;
    star.personData.assign(231, 0);
    star.genes = {{ts2::sim::kHairGene, {"00000001-0000-0000-0000-000000000000", "00000001-0000-0000-0000-000000000000"}},
                  {ts2::sim::kFaceGene, {"nose,brow,jaw,", ""}}};
    const std::optional<std::int16_t> id = world.addNeighbor(std::move(star));
    CHECK(id == std::int16_t{0x301});
    world.setFamilyTie(0xF8, 0x301, ts2::famt::TieType::Father);
    world.addFamilyMember(1, 0xB805E5AA);
    ts2::fami::Family family;
    family.version = ts2::fami::kWrittenVersion;
    family.lot = 4;
    family.members = {0xCDBA17B7};
    ts2::famt::FamilyTies ties;
    ties.version = ts2::famt::kWrittenVersion;
    ties.sims = {{std::int16_t{0xF8}, 1, {{3, std::int16_t{0x2B}}}}};
    ts2::dbpf::PackageImage neighborhood;
    neighborhood.resources = {ts2::dbpf::storeResource({ts2::fami::kType, 0xFFFFFFFF, 1, 0}, ts2::fami::write(family)),
                              ts2::dbpf::storeResource({ts2::famt::kType, 0xFFFFFFFF, 0, 0}, ts2::famt::write(ties)),
                              ts2::dbpf::storeResource({ts2::lotrecords::kShortRecordType, 0xFFFFFFFF, 0, 0},
                                                       ts2::lotrecords::writeShortRecord(0x301)),
                              ts2::dbpf::storeResource({ts2::lotrecords::kDnaIndexType, 0xFFFFFFFF, 0, 0},
                                                       ts2::lotrecords::writeDnaIndex({2}))};
    neighborhood.dataOrder = {0, 1, 2, 3};
    // Until its character file is written a save leaves the baby out everywhere.
    ts2::dbpf::PackageImage leftOut = neighborhood;
    const ts2::sim::HoodSaveCounts without = ts2::sim::fillNeighborhoodPackage(leftOut, {}, 1, world, {});
    CHECK(without.newNeighbors == 0 && without.newDna == 0 && without.familyMembers == 0 && !without.familyTies &&
          !without.neighborCounter && leftOut.resources.size() == 4);
    const ts2::sim::HoodSaveCounts counts =
        ts2::sim::fillNeighborhoodPackage(neighborhood, {}, 1, world, {}, ts2::sim::NewNeighbors::Written);
    const auto body = [&](const ts2::dbpf::StoredResource& stored) {
        return stored.compressed ? ts2::qfs::decompress(stored.bytes) : stored.bytes;
    };
    const auto find = [&](std::uint32_t type, std::uint32_t instance) -> const ts2::dbpf::StoredResource* {
        for (const auto& r : neighborhood.resources) {
            if (r.key.type == type && r.key.instance == instance) {
                return &r;
            }
        }
        return nullptr;
    };
    CHECK(counts.newNeighbors == 1 && counts.newDna == 1 && counts.familyMembers == 1 && counts.familyTies &&
          counts.neighborCounter && neighborhood.dataOrder.size() == neighborhood.resources.size());
    const auto* description = find(ts2::sdsc::kType, 0x301);
    const auto record = description ? ts2::sdsc::readRecord(body(*description)) : std::nullopt;
    CHECK(record && record->neighborId == 0x301 && record->guid == 0xB805E5AA && record->value1 == 0 && record->value2 == 0);
    const auto* dna = find(ts2::types::kSimDna, 0x301);
    const std::string expected =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<cGZPropertySetString>\n"
        "  <AnyString key=\"1\" type=\"0x0b8bea18\">00000001-0000-0000-0000-000000000000</AnyString>\n"
        "  <AnyString key=\"268435457\" type=\"0x0b8bea18\">00000001-0000-0000-0000-000000000000</AnyString>\n"
        "  <AnyString key=\"5\" type=\"0x0b8bea18\">nose,brow,jaw,</AnyString>\n"
        "  <AnyString key=\"268435461\" type=\"0x0b8bea18\"></AnyString>\n</cGZPropertySetString>";
    const auto dnaBody = dna ? body(*dna) : std::vector<std::uint8_t>{};
    CHECK(std::string(dnaBody.begin(), dnaBody.end()) == expected);
    const std::vector<std::uint32_t> indexed{2, 0x301};
    CHECK(ts2::lotrecords::readDnaIndex(body(*find(ts2::lotrecords::kDnaIndexType, 0))) == indexed);
    const std::vector<std::uint32_t> members{0xCDBA17B7, 0xB805E5AA};
    CHECK(ts2::fami::parse(body(*find(ts2::fami::kType, 1))).members == members);
    const ts2::famt::FamilyTies saved = ts2::famt::parse(body(*find(ts2::famt::kType, 0)));
    CHECK(saved.sims.size() == 2 && saved.sims[0].ties.size() == 2 && saved.sims[0].ties[1].type == 4 &&
          saved.sims[0].ties[1].other == 0x301 && saved.sims[1].sim == 0x301 && saved.sims[1].value == 1 &&
          saved.sims[1].ties.size() == 1 && saved.sims[1].ties[0].type == 0 && saved.sims[1].ties[0].other == 0xF8);
    CHECK(ts2::lotrecords::readShortRecord(body(*find(ts2::lotrecords::kShortRecordType, 0))) == std::uint16_t{0x302});
}

// A new character file starts as its template's group without the name map, regrouped, the
// object definition a person under the new GUID named after the file and first name, and gets
// the name text and installed packs record. It takes the first free file number.
void newCharacterFilesStartFromTheirTemplate() {
    std::vector<std::uint8_t> definition(64 + 2 * 0x6C, 0);
    const std::string templateName = "TemplatePerson";
    std::copy(templateName.begin(), templateName.end(), definition.begin());
    definition[64 + 2 * 0x09] = 0x10;
    definition[64 + 2 * 0x29] = 0xD0;
    definition[64 + 2 * 0x29 + 1] = 0x07;
    definition.insert(definition.end(), {14, 0, 0, 0});
    definition.insert(definition.end(), templateName.begin(), templateName.end());
    const std::vector<ts2::sim::TemplateResource> templ{
        {{ts2::types::kObjectDefinition, 0x7FEDFE16, 0x80, 0}, definition},
        {{0x42484156, 0x7FEDFE16, 0x1000, 0}, {1, 2, 3}},
        {{0x4E524546, 0x7FEDFE16, 0x80, 0}, {9}}};
    ts2::sim::NewCharacter star;
    star.fileName = "N002_User00569";
    star.firstName = "Star";
    star.surname = "Curious";
    star.guid = 0xB805E5AA;
    star.templateGuid = 0x7FD96B54;
    const std::vector<std::uint8_t> packs{0xE0, 0x50};
    const ts2::dbpf::PackageImage made = ts2::sim::newCharacterPackage(templ, star, packs);
    CHECK(made.resources.size() == 4 && made.dataOrder.size() == 4);
    const auto body = [&](std::size_t i) {
        const auto& r = made.resources[i];
        return r.compressed ? ts2::qfs::decompress(r.bytes) : r.bytes;
    };
    const auto objd = body(0);
    const auto field = [&](std::size_t i) { return objd[64 + 2 * i] | objd[65 + 2 * i] << 8; };
    const std::string name = "N002_User00569 - Star";
    CHECK(made.resources[0].key.group == 0xFFFFFFFF && std::string(reinterpret_cast<const char*>(objd.data())) == name &&
          field(0x09) == 2 && field(0x0E) == 0xE5AA && field(0x0F) == 0xB805 && field(0x1D) == 0xE5AA &&
          field(0x1E) == 0xB805 && field(0x46) == 0x6B54 && field(0x47) == 0x7FD9 && objd.size() == 64 + 2 * 0x6C + 4 + name.size());
    const std::vector<std::uint8_t> copied{1, 2, 3};
    CHECK(made.resources[1].key.type == 0x42484156 && made.resources[1].key.group == 0xFFFFFFFF && body(1) == copied);
    const auto names = ts2::str::readRecord(body(2));
    CHECK(made.resources[2].key.type == ts2::types::kCatalogString && made.resources[2].key.instance == 0x7D0 && names &&
          names->entries.size() == 3 && names->entries[0].value == "Star" && names->entries[1].value.empty() &&
          names->entries[2].value == "Curious" && names->entries[2].description == ts2::sim::kNewNameComment);
    CHECK(made.resources[3].key.type == 0xEBFEE342 && body(3) == packs);
    const std::vector<std::string> existing{"N002_User00000.package", "N002_User00002.package"};
    CHECK(ts2::sim::newCharacterFileName("N002", existing) == "N002_User00001");
    // A file renamed in another case still holds its number.
    const std::vector<std::string> renamed{"n002_user00000.package"};
    CHECK(ts2::sim::newCharacterFileName("N002", renamed) == "N002_User00001");
}

// A made Sim's name is written in each language its names were drawn in, the line between its
// first name and surname with no comment, as in Brenda Barthelet's file.
void aMadeSimsNameTextHoldsEveryLanguage() {
    std::vector<std::uint8_t> definition(64 + 2 * 0x6C, 0);
    const std::string templateName = "TemplatePerson";
    std::copy(templateName.begin(), templateName.end(), definition.begin());
    definition[64 + 2 * 0x09] = 0x10;
    definition[64 + 2 * 0x29] = 0xD0;
    definition[64 + 2 * 0x29 + 1] = 0x07;
    definition.insert(definition.end(), {14, 0, 0, 0});
    definition.insert(definition.end(), templateName.begin(), templateName.end());
    const std::vector<ts2::sim::TemplateResource> templ{{{ts2::types::kObjectDefinition, 0x7FEDFE16, 0x80, 0}, definition}};
    ts2::sim::NewCharacter brenda;
    brenda.fileName = "N002_User00568";
    brenda.firstName = "Brenda";
    brenda.surname = "Barthelet";
    brenda.languageNames = {{"Brenda", "Barthelet"}, {"Britta", "Scheuer"}};
    const ts2::dbpf::PackageImage made = ts2::sim::newCharacterPackage(templ, brenda, std::vector<std::uint8_t>{1});
    const auto& r = made.resources[1];
    const auto names = ts2::str::readRecord(r.compressed ? ts2::qfs::decompress(r.bytes) : r.bytes);
    CHECK(r.key.type == ts2::types::kCatalogString && names && names->entries.size() == 6);
    CHECK(names->entries[0].language == 1 && names->entries[0].value == "Brenda" && names->entries[0].description == ts2::sim::kNewNameComment);
    CHECK(names->entries[1].value.empty() && names->entries[1].description.empty());
    CHECK(names->entries[3].language == 2 && names->entries[3].value == "Britta" && names->entries[5].value == "Scheuer");
}

// A name table's row in each of the 44 languages: the language's own, else US English's where the
// language is missing, shorter or empty there.
void aNameRowIsTakenInEveryLanguage() {
    const std::vector<std::vector<std::string>> lists{{"Brenda", "Ann"}, {"Brenda", "Anne"}, {}, {"Bea", ""}, {"Bea"}};
    const auto row = ts2::sim::inEveryLanguage(lists, 1);
    CHECK(row.size() == 44 && row[0] == "Ann" && row[1] == "Anne" && row[2] == "Ann" && row[3] == "Ann" && row[4] == "Ann" && row[43] == "Ann");
    CHECK(ts2::sim::inEveryLanguage(lists, 2).empty() && ts2::sim::inEveryLanguage({}, 0).empty());
}

// A made Sim's look records, as in Brenda's file: the age data's lists point into a resource list of
// its face model, an empty key, its skeleton, then each part once in the order the lists (in key
// order) first name it. Every face list takes the Sim's own face part, and an empty list is left out.
void madeLookRecordsListEachPartOnce() {
    ts2::sim::MadeAgeLook look;
    look.ageFlag = 0x10;
    look.gender = 1;
    look.ageNumber = 4;
    look.skinTone = "00000001-0000-0000-0000-000000000000";
    look.hairTone = "00000005-0000-0000-0000-000000000000";
    look.eyeColour = "32dee745-b6ce-419f-9e86-ae93802d2682";
    look.skeleton = {0xE519C933, 0x1C0532FA, 0xFF6B8A91, 0x3CC1ADAA};
    const ts2::dbpf::ResourceKey hair{0xEBCF3E27, 0x2C17B74A, 0x2F920455, 0}, body{0xEBCF3E27, 0x2C17B74A, 0x2F920450, 0},
        catalogFace{0xEBCF3E27, 0x2C17B74A, 0x11111111, 0};
    look.lists = {{0x10001, {hair}}, {0x10002, {catalogFace}}, {0x10008, {body}}, {0x80001, {hair, body}}, {0x80002, {catalogFace}}, {0x100001, {}}};
    ts2::dbpf::PackageImage package;
    ts2::sim::addMadeLookRecords(package, look);
    CHECK(package.resources.size() == 3);
    const auto body0 = [&](std::size_t i) {
        const auto& r = package.resources[i];
        return r.compressed ? ts2::qfs::decompress(r.bytes) : r.bytes;
    };
    const auto aged = ts2::propset::parse(body0(0));
    CHECK(ts2::propset::number(aged, "product") == 0xFFFF && ts2::propset::number(aged, "listcnt") == 5 && ts2::propset::number(aged, "age") == 0x10);
    CHECK(aged.at("lk1") == std::to_string(0x10002) && ts2::propset::number(aged, "le0_0") == 3 && ts2::propset::number(aged, "le1_0") == 4 &&
          ts2::propset::number(aged, "le2_0") == 5 && aged.at("ls3") == "2" && ts2::propset::number(aged, "le3_1") == 5 &&
          ts2::propset::number(aged, "le4_0") == 4 && !aged.contains("ls5"));
    const auto list = ts2::reflist::parse(body0(1));
    CHECK(list.size() == 6 && list[0].instance == ts2::sim::newFaceModelKey(4).instance && list[1].type == 0 && list[2].instance == 0xFF6B8A91 &&
          list[3].instance == hair.instance && list[4].group == 0xFFFFFFFF && list[4].instance == 1 && list[5].instance == body.instance);
    const auto face = ts2::propset::parse(body0(2));
    CHECK(package.resources[2].key.type == 0xEBCF3E27 && face.at("hairtone") == look.hairTone && ts2::propset::number(face, "age") == 0x10);
}

// A newborn's look records: its age data, and a face part and resource list for each age, the
// elder's with grey hair. The models are keyed by their names, as in Star Curious's file.
// A new Sim's face shape names both levels of its face's nodes in the file's own group, and a face
// node copy names the Sim's own mesh and writes its data lists in the opposite order.
void newFaceModelsNameTheSimsOwnParts() {
    CHECK(ts2::sim::ownGroupName(0x6F000193, "age5_0_gmnd") == "#0x6f000193!age5_0_gmnd");
    CHECK(ts2::sim::baseFaceAgeFlag(7) == ts2::sim::baseFaceAgeFlag(3) && ts2::sim::baseFaceAgeFlag(5) == 0x20 && ts2::sim::baseFaceAgeFlag(6) == 0);
    const auto shape = ts2::rcol::read(ts2::sim::newFaceShape(5, 0x6F000001, "ss_custompart_0x1_5_0_cmpm"));
    const auto* s = std::get_if<ts2::rcol::Shape>(&shape.blocks[0]);
    CHECK(s != nullptr && s->name.name == "#0x6f000001!age5_00_shpe" && s->items.size() == 2 && s->items[1].detailLevel == 15 &&
          s->items[1].node == "#0x6f000001!age5_0LOD15_gmnd" && s->parts[0].material == "ss_custompart_0x1_5_0_cmpm");
    ts2::rcol::Resource node;
    node.links.push_back({0xAC4F8687, 0x1C0532FA, 1, 2});
    ts2::rcol::GeometryNode g;
    g.header = {"cGeometryNode", 0x7BA3838C, 12};
    g.graph.header = {"cObjectGraphNode", 0, 4};
    g.name.header = {"cSGResource", 0, 2};
    g.name.name = "buFace_tslocator_gmnd";
    node.blocks.push_back(g);
    node.blockTypes.push_back(0x7BA3838C);
    for (const char* list : {"tsNoShadow", "face"}) {
        ts2::rcol::DataList d;
        d.header = {"cDataListExtension", 0x6A836D56, 1};
        d.extension = {"cExtension", 0, 3};
        d.value.type = 7;
        d.value.name = list;
        node.blocks.push_back(d);
        node.blockTypes.push_back(0x6A836D56);
    }
    const auto copy = ts2::rcol::read(ts2::sim::newFaceNode(ts2::rcol::write(node), 5, true, 0x6F000001));
    const auto mesh = ts2::hash::scenegraphKey("age5_0LOD15_gmdc");
    CHECK(copy.links[0].group == 0xFFFFFFFF && copy.links[0].instance == mesh.instance && copy.links[0].instanceHigh == mesh.instanceHigh);
    CHECK(std::get<ts2::rcol::GeometryNode>(copy.blocks[0]).name.name == "#0x6f000001!age5_0LOD15_gmnd");
    CHECK(std::get<ts2::rcol::DataList>(copy.blocks[1]).value.name == "face" && std::get<ts2::rcol::DataList>(copy.blocks[2]).value.name == "tsNoShadow");
    bool threw = false;
    try {
        ts2::sim::newFaceNode(ts2::sim::newFaceShape(5, 1, "m"), 5, false, 1);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

// A new face model is the skeleton with the grips' lists first among the model's extensions, every
// list in the opposite order, the shape reference among the top joint's children from head_ikctr
// on, and its blocks breadth first.
void newFaceModelsBuildOnTheSkeleton() {
    namespace rc = ts2::rcol;
    const auto joint = [](const char* name, std::vector<rc::Ref> children) {
        rc::TransformBlock t;
        t.transform.header = {"cTransformNode", 0x65246462, 7};
        t.transform.tree.header = {"cCompositionTreeNode", 0, 11};
        t.transform.tree.graph.header = {"cObjectGraphNode", 0, 4};
        t.transform.tree.graph.name = name;
        t.transform.tree.children = std::move(children);
        t.transform.joint = 0x7FFFFFFF;
        return t;
    };
    rc::Resource skeleton;
    rc::ResourceNode root;
    root.header = {"cResourceNode", 0xE519C933, 7};
    root.root = 1;
    root.name = {{"cSGResource", 0, 2}, "auSkel_cres"};
    root.tree.header = {"cCompositionTreeNode", 0, 11};
    root.tree.graph.header = {"cObjectGraphNode", 0, 4};
    root.tree.graph.name = "auSkel_cres";
    root.tree.children = {{1, 0, 1}};
    root.value = 2;
    skeleton.blocks = {root, joint("auskel", {{1, 0, 2}, {1, 0, 3}, {1, 0, 4}}), joint("root_trans", {}), joint("head_ikctr", {}), joint("l_hand_ikpole", {})};
    skeleton.blockTypes.assign(5, 0x65246462);
    skeleton.blockTypes[0] = 0xE519C933;
    const ts2::sim::GripAdjust grip{"glasses_grip", {0, 0.01f, 0.1f}, {0, 0.7f, 0.7f, 0}};
    const std::vector<std::string> morphs{"faceblends::happy"};
    const auto model = rc::read(ts2::sim::newFaceModel(rc::write(skeleton), 3, 0x6F000001, std::span(&grip, 1), morphs));
    const auto name = [&](std::uint32_t i) {
        const auto* t = std::get_if<rc::TransformBlock>(&model.blocks[i]);
        return t != nullptr ? t->transform.tree.graph.name : std::string(std::holds_alternative<rc::ShapeRef>(model.blocks[i]) ? "shape" : "list");
    };
    const auto& top = std::get<rc::ResourceNode>(model.blocks[0]);
    CHECK(top.name.name == "#0x6f000001!age3_0_cres" && top.value == 9 && model.links.size() == 1 && model.links[0].type == 0xFC6EB1F7);
    // Breadth first: the model, its grip list, the top joint, then the top joint's children.
    CHECK(model.blocks.size() == 7 && name(1) == "list" && name(2) == "auskel");
    CHECK(name(3) == "head_ikctr" && name(4) == "root_trans" && name(5) == "shape" && name(6) == "l_hand_ikpole");
    const auto& shape = std::get<rc::ShapeRef>(model.blocks[5]);
    CHECK(shape.names == morphs && shape.values.size() == 1 && shape.data.size() == 16 && shape.node.transform.joint == 0x7FFFFFFF);
    CHECK(std::get<rc::DataList>(model.blocks[1]).value.items[0].text == "glasses_grip");
}

// A new face mesh is the catalog mesh renamed, with its face's positions and normals replaced and
// everything else kept. Vertices that don't fit are refused.
void newFaceMeshesReplaceTheFace() {
    namespace rc = ts2::rcol;
    rc::Mesh m;
    m.header = {"cGeometryDataContainer", 0xAC4F8687, 4};
    m.name = {{"cSGResource", 0, 2}, "buFace_tslocator_gmdc"};
    const auto element = [](std::uint32_t identity, float value) {
        rc::Mesh::Element e;
        e.identity = identity;
        e.blockFormat = 2;
        std::vector<float> floats(6, value);
        e.data.resize(floats.size() * sizeof(float));
        std::memcpy(e.data.data(), floats.data(), e.data.size());
        return e;
    };
    m.elements = {element(0x5B830781, 1), element(0x3B83078B, 2), element(0x89D92BA0, 3)};
    m.links.push_back({{0, 1, 2}, 2, 3, {}});
    m.groups.push_back({2, 0, "face", {0, 1, 0}, 0, {}});
    rc::Resource catalog;
    catalog.blocks = {m};
    catalog.blockTypes = {0xAC4F8687};
    const std::vector<float> positions{0.5f, 0.5f, 0.5f, 0.25f, 0.25f, 0.25f}, normals{0, 0, 1, 0, 1, 0};
    const auto made = rc::read(ts2::sim::newFaceMesh(rc::write(catalog), 5, true, 0x6F000001, positions, normals));
    const auto& out = std::get<rc::Mesh>(made.blocks[0]);
    const auto floats = [](const rc::Mesh::Element& e) {
        std::vector<float> v(e.data.size() / sizeof(float));
        std::memcpy(v.data(), e.data.data(), e.data.size());
        return v;
    };
    CHECK(out.name.name == "#0x6f000001!age5_0LOD15_gmdc" && floats(out.elements[0]) == positions && floats(out.elements[1]) == normals);
    CHECK(floats(out.elements[2]) == std::vector<float>(6, 3.0f));
    bool threw = false;
    try {
        ts2::sim::newFaceMesh(rc::write(catalog), 5, false, 1, std::vector<float>(3, 0.0f), normals);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

// A new face material is the catalog face material composited with the eye and eyebrow overlays:
// SimSkin, its added properties in key order, and the composite then the base as its textures.
void newFaceMaterialsCompositeTheOverlays() {
    namespace rc = ts2::rcol;
    rc::Material m;
    m.header = {"cMaterialDefinition", 0x49596978, 11};
    m.name = {{"cSGResource", 0, 2}, "amface_s5_txmt"};
    m.description = "amface_s5";
    m.type = "SimStandardMaterial";
    m.properties = {{"reflectivity", "0"}, {"stdMatBaseTextureName", "amface-s5"}};
    m.textures = {"amface-s5"};
    rc::Resource catalog;
    catalog.blocks = {m};
    catalog.blockTypes = {0x49596978};
    ts2::sim::FaceMaterialLook look;
    look.name = ts2::sim::faceMaterialName(0x1A, 3, 0);
    look.eyeTexture = "uuface-eye-dkblue";
    look.browTexture = "uuface-browbushy-black";
    look.eyeColour = "e1";
    look.browFamily = "b1";
    look.hairTone = "h1";
    look.skinTone = "s1";
    look.age = 3;
    const auto made = rc::read(ts2::sim::newFaceMaterial(rc::write(catalog), 0x6F000001, look));
    const auto& out = std::get<rc::Material>(made.blocks[0]);
    const auto value = [&](const char* key) {
        const auto it = std::find_if(out.properties.begin(), out.properties.end(), [&](const auto& p) { return p.first == key; });
        return it != out.properties.end() ? it->second : std::string("-");
    };
    CHECK(look.name == "ss_customPart_0x1a_3_0_cmpm" && out.name.name == "#0x6f000001!ss_customPart_0x1a_3_0_cmpm_txmt");
    CHECK(ts2::sim::faceMaterialName(0x1A, 3, 1) == "ss_customPart_0x1a_3_1_cmpm");
    CHECK(out.type == "SimSkin" && out.description == "ss_custompart_0x1a_3_0_cmpm");
    CHECK(value("compositeBaseTextureName") == "_amface_s5_uuface_eye_dkblue_uuface_browbushy_black-diffuse");
    CHECK(value("baseTexture0") == "amface-s5" && value("cafOverlay1") == "b1," && value("cafNumOverlays") == "2" && value("numTexturesToComposite") == "3");
    CHECK(std::is_sorted(out.properties.begin(), out.properties.end()) && out.textures.size() == 2 && out.textures[1] == "amface-s5");
}

// The eyebrows a newborn's draw picks come from the baby eyebrow overlays of its gender, tone and
// hair tone, and the draw scales over them.
void newbornBrowFamilyFollowsTheDraw() {
    ts2::scene::BabyCatalog catalog;
    const auto brow = [&](const char* family, const char* gender, const char* hairTone, const char* age = "32") {
        catalog.overlays.push_back({{}, {{"subtype", "1"}, {"age", age}, {"gender", gender}, {"hairtone", hairTone}, {"family", family}}});
    };
    brow("adult", "3", "", "8");
    brow("first", "3", "");
    brow("wrong hair", "3", "h2");
    brow("boys", "2", "");
    brow("second", "1", "h1");
    ts2::scene::Newborn baby;
    baby.gender = 1;
    baby.hairTone = "H1";
    baby.browDraw = 0;
    CHECK(ts2::scene::newbornBrowFamily(catalog, baby) == "first");
    baby.browDraw = 0x80000000u;
    CHECK(ts2::scene::newbornBrowFamily(catalog, baby) == "second");
    CHECK(ts2::scene::newbornBrowFamily({}, baby).empty());
}

// A newborn's file can't be made without the catalog's faces, and says so rather than writing a
// file the game can't draw.
void newbornFileNeedsTheCatalog() {
    const auto empty = std::filesystem::temp_directory_path() / ("neots2-newborn-test-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(empty);
    {
        ts2::scene::Assets assets(empty);
        ts2::sim::NewCharacter character{"N001_User00000", "Baby", "Test", 0x1000, 0x7FD96B54};
        bool threw = false;
        try {
            (void)ts2::sim::newbornCharacterPackage(assets, {}, character, {}, ts2::sim::NewbornLookParts{});
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
    }
    std::filesystem::remove_all(empty);
}

void newbornLookRecordsCoverEveryAge() {
    ts2::sim::BabyLook look;
    look.gender = 2;
    look.skinTone = "6baf064a-85ad-4e37-8d81-a987e9f8da46";
    look.hairTone = "00000001-0000-0000-0000-000000000000";
    look.eyeColour = "2d6839c5-0b7c-48a1-9c55-4bd9cc873b0f";
    look.everydayBody = {0xEBCF3E27, 0x2C17B74A, 0x4C352F51, 0};
    ts2::dbpf::PackageImage package;
    ts2::sim::addBabyLookRecords(package, look);
    CHECK(package.resources.size() == 15 && package.dataOrder.size() == 15);
    const auto baby = ts2::sim::newFaceModelKey(5);
    CHECK(baby.type == 0xE519C933 && baby.group == 0xFFFFFFFF && baby.instance == 0xFF6B6530 && baby.instanceHigh == 0x21B3E6C2);
    std::size_t parts = 0;
    for (const auto& r : package.resources) {
        if (r.key.type != 0xEBCF3E27) {
            continue;
        }
        ++parts;
        const auto p = ts2::propset::parse(r.compressed ? ts2::qfs::decompress(r.bytes) : r.bytes);
        const bool elder = ts2::propset::number(p, "age") == 0x10;
        CHECK(p.at("hairtone") == (elder ? "00000005-0000-0000-0000-000000000000" : look.hairTone));
    }
    CHECK(parts == 7);
}

// A Sim's animations hang from the slot bone of the seat it sat in, by the hash of the bone's lower-case
// name, as the game's saves name "slot_barstool_0" (0x3CFD4451) for clips on a barstool. One started
// before the Sim sat (a blink) hangs from nothing, and one a save gave an anchor keeps it.
void animationsHangFromTheSeatTheyStartedIn() {
    MemoryCodeSource code;
    ts2::slot::Entry seat;
    seat.bone = 0;
    code.addObjectType(kObjectGroup, typed(0x52525252), {}, {seat});
    code.addStrings(kObjectGroup, 0x90, {"Slot_Barstool_0"});
    const auto* type = code.objectType(0x52525252);
    CHECK(type != nullptr && ts2::sim::slotAnchorName(code, *type, 0) == 0x3CFD4451);
    CHECK(type != nullptr && ts2::sim::slotAnchorName(code, *type, 1) == 0);

    ts2::sim::World world;
    auto& stool = world.create(kObjectGroup, typed(0x52525252));
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.data[2] = stool.id;
    sim.anchor = {stool.id, 0x3CFD4451};
    sim.anchoredAtMs = 1000;
    const auto overlay = [](const char* name, double startMs) {
        ts2::sim::PlayingOverlay o;
        o.name = name;
        o.startMs = startMs;
        o.lengthMs = 100000;
        return o;
    };
    sim.overlays = {overlay("blink", 500), overlay("idle", 1500), overlay("kept", 1500)};
    sim.overlays[2].savedAnchor = ts2::sim::AnimationAnchor{-1, 0};
    ts2::cobject::Record loaded;
    ts2::cobject::Animation clip;
    clip.kind = ts2::cobject::Animation::Kind::Clip;
    clip.request.type = "ovl";
    loaded.animatable.animations = {clip};
    const auto record = ts2::sim::lot_save_detail::sceneRecord(loaded, sim, world, 2000, nullptr);
    const auto anchorOf = [&](const char* name) {
        for (const auto& a : record.animatable.animations) {
            if (a.request.name == name) {
                return a.request.words3;
            }
        }
        return std::array<ts2::cobject::Word, 2>{7, 7};
    };
    CHECK((anchorOf("blink") == std::array<ts2::cobject::Word, 2>{0, 0}));
    CHECK((anchorOf("idle") == std::array<ts2::cobject::Word, 2>{static_cast<ts2::cobject::Word>(stool.id), 0x3CFD4451}));
    CHECK((anchorOf("kept") == std::array<ts2::cobject::Word, 2>{0xFFFFFFFF, 0}));
}

// An object's clip played until stopped is saved while it plays, and left out once Animate Stop has
// stopped it, as the game's saves keep no clip that has ended. A video game console stopped by its
// turn off came back from the save with its disc spinning.
void aStoppedLoopingClipIsNotSaved() {
    ts2::sim::World world;
    auto& console = world.create(kObjectGroup, typed(0x4D214140));
    ts2::sim::PlayingAnimation spin;
    spin.name = "o-electronicVideoGameSystem-CDSpin";
    spin.startMs = 1000;
    spin.loops = true;
    spin.lengthMs = std::numeric_limits<float>::infinity();
    console.animation = spin;
    const auto saved = [&] {
        ts2::cobject::Record loaded;
        loaded.animatable.animations = {ts2::cobject::Animation{}};
        const auto record = ts2::sim::lot_save_detail::sceneRecord(loaded, console, world, 5000, nullptr);
        return std::any_of(record.animatable.animations.begin(), record.animatable.animations.end(),
                           [&](const ts2::cobject::Animation& a) { return a.request.name == spin.name; });
    };
    CHECK(saved());
    console.animation->lengthMs = 3000;  // stopped 3 seconds in
    CHECK(!saved());
}

// A clip saved where the record held only an empty entry starts as the game makes a new clip, as
// every clip of the game's saves holds it: its places at no move with a scale of 1, the request's
// starting values, the started flag and the word after its resource name. One held until stopped
// holds its blend for the game's "until stopped" length. A load shows no clip from an entry of 0s.
void aClipBuiltFromAnEmptyEntryStartsAsTheGameMakesOne() {
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    ts2::sim::PlayingOverlay held;
    held.name = "a2o-pizza-carry";
    held.startMs = 1000;
    held.plays = -1;
    sim.overlays = {held};
    ts2::cobject::Record loaded;
    loaded.animatable.animations = {ts2::cobject::Animation{}};
    const auto record = ts2::sim::lot_save_detail::sceneRecord(loaded, sim, world, 2000, nullptr);
    const auto one = std::bit_cast<ts2::cobject::Word>(1.0f), minusOne = std::bit_cast<ts2::cobject::Word>(-1.0f);
    CHECK(record.animatable.animations.size() == 1);
    const auto& a = record.animatable.animations.front();
    CHECK(a.kind == ts2::cobject::Animation::Kind::Clip && a.request.name == "a2o-pizza-carry");
    CHECK(a.request.words1[0] == minusOne && a.request.words1[1] == minusOne && a.request.words1[4] == 0x7FFFFF00);
    CHECK(a.request.words1[12] == one && a.request.words1[13] == 0xFFFFFFFF && a.request.words1[14] == 1);
    CHECK((a.instance.words2 == std::array<ts2::cobject::Word, 3>{0, one, 5}));
    CHECK(a.instance.words4[0] == one && a.instance.words4[1] == 0);
    for (std::size_t place = 0; place < 3; ++place) {
        CHECK(a.instance.words4[2 + place * 8 + 6] == one && a.instance.words4[2 + place * 8 + 7] == one);
        CHECK(a.instance.words4[2 + place * 8] == 0 && a.instance.words4[2 + place * 8 + 5] == 0);
    }
    CHECK((a.instance.words5 == std::array<ts2::cobject::Word, 5>{1, 0, 0x7FFFFF00, 0x4EFFFFFE, 1}));
    CHECK(a.word == 1);
    CHECK(a.instance.curve && a.instance.curve->segments.size() == 3 && a.instance.curve->segments[1].length == 0x4EFFFFFE);
}

// A changed look keeps the Sim's skeleton: a file can name it twice, near the top without the
// key's high instance and far down with it, the age data naming the second. The game's rewrite keeps
// the top entry with the age data's key, and the age data names it there.
void aChangedLookKeepsTheSkeleton() {
    const ts2::dbpf::ResourceKey model{0xE519C933, 0xFFFFFFFF, 0xFFDF89DF, 0}, skeleton{0xE519C933, 0x1C0532FA, 0xFF6B8A91, 0x3CC1ADAA},
        hairA{0xEBCF3E27, 0x2C17B74A, 0xA, 0}, hairB{0xEBCF3E27, 0x2C17B74A, 0xB, 0}, top{0xEBCF3E27, 0x2C17B74A, 0xC, 0};
    const std::vector<ts2::propset::Property> props{{ts2::propset::kUintType, "age", 8, {}, {}},
                                                    {ts2::propset::kUintType, "skeletonkeyidx", 5, {}, {}},
                                                    {ts2::propset::kUintType, "listcnt", 2, {}, {}},
                                                    {ts2::propset::kStringType, "ls0", 0, "1", {}},
                                                    {ts2::propset::kStringType, "lk0", 0, "65537", {}},
                                                    {ts2::propset::kUintType, "le0_0", 3, {}, {}},
                                                    {ts2::propset::kStringType, "ls1", 0, "1", {}},
                                                    {ts2::propset::kStringType, "lk1", 0, "65540", {}},
                                                    {ts2::propset::kUintType, "le1_0", 4, {}, {}}};
    std::vector<std::uint8_t> list;
    const auto put = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            list.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    put(0xDEADBEEF);
    put(2);
    put(6);
    const ts2::dbpf::ResourceKey bareSkeleton{skeleton.type, skeleton.group, skeleton.instance, 0};
    for (const auto& k : {model, ts2::dbpf::ResourceKey{}, bareSkeleton, hairA, top, skeleton}) {
        put(k.type);
        put(k.group);
        put(k.instance);
        put(k.instanceHigh);
    }
    ts2::dbpf::PackageImage made;
    made.resources = {ts2::dbpf::storeResource({0xAC598EAC, 0xFFFFFFFF, 1, 0}, ts2::propset::writeBinary(props)),
                      ts2::dbpf::storeResource({0xAC506764, 0xFFFFFFFF, 1, 0}, list)};
    made.dataOrder = {0, 1};
    ts2::sim::OutfitLook look;
    look.lists[0x10001] = {hairB};
    const auto out = ts2::sim::rewriteOutfitLook(ts2::dbpf::writeImage(made), look);
    ts2::dbpf::Package package{std::span<const std::uint8_t>(out)};
    std::optional<std::vector<ts2::propset::Property>> rewritten;
    std::vector<ts2::scenegraph::Link> refs;
    for (const auto& e : package.entries()) {
        if (e.key.type == 0xAC598EAC) {
            rewritten = ts2::propset::readBinary(package.read(e));
        } else if (e.key.type == 0xAC506764) {
            refs = ts2::reflist::parse(package.read(e));
        }
    }
    std::uint32_t index = 99;
    for (const auto& p : rewritten.value_or(std::vector<ts2::propset::Property>{})) {
        if (p.name == "skeletonkeyidx") {
            index = p.word;
        }
    }
    CHECK(refs.size() == 4 && index == 2 && refs[index].instance == skeleton.instance && refs[index].group == skeleton.group &&
          refs[index].instanceHigh == skeleton.instanceHigh);
}

// A save rewrites a changed look in the age data and its resource list, as the game's did for a teen
// given a new hair: the model and skeleton entries stay first, then each part once, in the order the
// outfit lists (in key order) first name it. Other properties and records stay, but for the
// portrait of the Sim's age.
void aChangedLookRewritesTheAgeDataAndItsList() {
    ts2::sim::BabyLook baby;
    baby.gender = 2;
    baby.skinTone = "00000002-0000-0000-0000-000000000000";
    baby.hairTone = "00000003-0000-0000-0000-000000000000";
    baby.eyeColour = "00000001-0000-0000-0000-000000000000";
    baby.everydayBody = {0xEBCF3E27, 0x2C17B74A, 0x4C352F51, 0};
    ts2::dbpf::PackageImage made;
    ts2::sim::addBabyLookRecords(made, baby);
    // The baby's portrait and its shadow, and an older age's portrait.
    const std::vector<std::uint8_t> picture{1, 2, 3};
    for (const std::uint32_t instance : {0x20u, 0x2000u, 0x08u}) {
        made.resources.push_back(ts2::dbpf::storeResource({0x856DDBAC, 0xFFFFFFFF, instance, 0}, picture));
        made.dataOrder.push_back(made.resources.size() - 1);
    }
    const auto file = ts2::dbpf::writeImage(made);
    const ts2::dbpf::ResourceKey face{0xEBCF3E27, 0xFFFFFFFF, 1, 0}, hairA{0xEBCF3E27, 0x2C17B74A, 0xA, 0}, hairB{0xEBCF3E27, 0x2C17B74A, 0xB, 0};
    ts2::sim::OutfitLook look;
    look.properties = {{"haircolor", "00000004-0000-0000-0000-000000000000"}, {"fitness", "2"}};
    look.lists[0x10002] = {face};
    look.lists[0x10001] = {hairA};
    look.lists[0x4000001] = {hairB};
    look.lists[0x80001] = {hairA};
    const auto out = ts2::sim::rewriteOutfitLook(file, look);
    ts2::dbpf::Package package{std::span<const std::uint8_t>(out)};
    std::optional<std::vector<ts2::propset::Property>> props;
    std::vector<ts2::scenegraph::Link> refs;
    std::size_t records = 0;
    std::vector<std::uint32_t> portraits;
    // The compression directory isn't a record. The age data's resource list has its instance, beside
    // the outfit parts' own lists.
    constexpr std::uint32_t kDirectory = 0xE86B1EEF, kAgeData = 0xAC598EAC, kResourceList = 0xAC506764;
    std::optional<std::uint32_t> ageData;
    for (const auto& e : package.entries()) {
        if (e.key.type == kAgeData) {
            ageData = e.key.instance;
        }
    }
    for (const auto& e : package.entries()) {
        records += e.key.type != kDirectory;
        if (e.key.type == 0x856DDBAC) {
            portraits.push_back(e.key.instance);
        }
        if (e.key.type == kAgeData) {
            props = ts2::propset::readBinary(package.read(e));
        } else if (e.key.type == kResourceList && ageData && e.key.instance == *ageData) {
            refs = ts2::reflist::parse(package.read(e));
        }
    }
    // The portrait of the Sim's age and its shadow are left for the game to draw anew.
    CHECK(records == made.resources.size() - 2 && portraits == std::vector<std::uint32_t>{0x08});
    CHECK(props.has_value());
    if (!props) {
        return;
    }
    std::map<std::string, ts2::propset::Property> by;
    for (const auto& p : *props) {
        by[p.name] = p;
    }
    CHECK(by["haircolor"].text == "00000004-0000-0000-0000-000000000000" && by["fitness"].word == 2 && by["gender"].word == 2);
    CHECK(by["listcnt"].word == 4 && !by.contains("lk4"));
    // The baby's model, an empty entry and the skeleton stay, then the lists' parts in key order.
    CHECK(refs.size() == 6 && refs[3].instance == 0xA && refs[4].instance == 1 && refs[5].instance == 0xB);
    CHECK(by["lk0"].text == "65537" && by["le0_0"].word == 3 && by["lk1"].text == "65538" && by["le1_0"].word == 4);
    CHECK(by["lk2"].text == "524289" && by["le2_0"].word == 3 && by["lk3"].text == "67108865" && by["le3_0"].word == 5 && by["ls3"].text == "1");
}

void savingALotRefusesSimsMadeThisRun() {
    ts2::sim::World world;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {9};
    hood->neighbors[9] = {};
    hood->neighbors[9].guid = 0x51515151;
    world.setNeighborhood(hood);
    world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    ts2::dbpf::PackageImage package;
    const std::array<std::int16_t, ts2::simi::kGlobalCount> globals{};
    const auto refused = [&] {
        try {
            ts2::sim::fillLotPackage(package, world, globals, 0, {}, nullptr);
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    CHECK(!refused());
    ts2::sim::SavedNeighbor baby;
    baby.guid = 0x52525252;
    const auto babyId = world.addNeighbor(baby);
    CHECK(babyId.has_value());
    world.create(kObjectGroup, typed(0x52525252, ts2::sim::kPersonType));
    CHECK(refused());
    // Saved with its character file, it can be.
    ts2::sim::TypeOwners owners;
    owners.writtenNewSims = {*babyId};
    bool saved = true;
    try {
        ts2::sim::fillLotPackage(package, world, globals, 0, owners, nullptr);
    } catch (const std::runtime_error&) {
        saved = false;
    }
    CHECK(saved);
}

// A newborn saved with its character file starts its lot record from another Sim's, with what that
// Sim was doing cleared: no queue, runs, motive changes, text or pick, blank interactions with id 0.
void savingALotWritesANewbornFromAnotherSim() {
    ts2::sim::World world;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {9};
    hood->neighbors[9].guid = 0x51515151;
    world.setNeighborhood(hood);
    const std::int32_t parentId = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType)).id;
    ts2::sim::SavedNeighbor baby;
    baby.guid = 0x52525252;
    const auto babyId = world.addNeighbor(baby);
    ts2::sim::SimObject& newborn = world.create(kObjectGroup, typed(0x52525252, ts2::sim::kPersonType));
    newborn.x = 8 * 65536;
    newborn.y = 21 * 65536 + 32768;
    newborn.level = 1;
    const std::int32_t newbornId = newborn.id;

    ts2::objt::ObjectTypeTable types;
    ts2::objt::Entry parentType;
    parentType.guid = 0x51515151;
    parentType.index = 40;
    parentType.kind = ts2::objt::kPersonKind;
    types.entries.push_back(parentType);
    ts2::xobj::SavedObject parent;
    parent.version = ts2::xobj::kWrittenVersion;
    parent.attributes.assign(8, 0);
    parent.tail.emplace();
    parent.tail->relationshipVersion = 3;
    parent.tail->value1 = 1761673217;
    parent.person.emplace();
    parent.person->version = 2;
    parent.person->personData.assign(ts2::xobj::personDataCountForVersion(parent.version), 0);
    parent.person->state.text = "Use";
    parent.person->state.pickedIconObject = 174;
    parent.person->state.entries.push_back({14, 0.5f, 100.0f});
    parent.person->state.interrupted.number = 7;
    parent.person->state.interrupted.id = 1162;
    ts2::objm::MapRecord map;
    map.objects = {{parentId, 40}};
    map.waits.assign(4, 0);
    const std::uint32_t personType = 0x50455253;
    ts2::dbpf::PackageImage package;
    package.resources = {
        ts2::dbpf::storeResource({ts2::types::kObjectMap, 0xFFFFFFFF, 1, 0}, ts2::objm::writeMapRecord(map)),
        ts2::dbpf::storeResource({ts2::types::kObjectTypeTable, 0xFFFFFFFF, 0, 0}, ts2::objt::write(types)),
        ts2::dbpf::storeResource({personType, 0xFFFFFFFF, static_cast<std::uint32_t>(parentId), 0},
                                 ts2::xobj::write(parent, ts2::xobj::RecordKind::Person)),
    };
    package.dataOrder = {0, 1, 2};
    // The parent's scene record, posed and dressed as its own body is.
    ts2::cobject::Record parentScene;
    parentScene.chain = ts2::cobject::Chain::Person;
    parentScene.object.joints.push_back({"root_trans", {}});
    parentScene.object.materials.push_back({"-1681979", 10, {{"hair", "amhairbald_skin_s2"}}});
    parentScene.animatable.settings = {0x759, 0xA9, 0x7FFFFF00, 0x7FFFFF00, 0, 1, 2};
    package.resources.push_back(ts2::dbpf::storeResource({ts2::types::kSceneObject, 0xFFFFFFFF, static_cast<std::uint32_t>(parentId), 0},
                                                         ts2::cobject::writeRecord(parentScene)));
    package.dataOrder.push_back(3);
    ts2::sim::TypeOwners owners;
    ts2::objt::Entry babyType;
    babyType.guid = 0x52525252;
    babyType.index = 41;
    babyType.kind = ts2::objt::kPersonKind;
    babyType.name = "Star";
    owners.newTypes.push_back(babyType);
    owners.writtenNewSims = {*babyId};
    const std::array<std::int16_t, ts2::simi::kGlobalCount> globals{};
    const ts2::sim::LotSaveCounts counts = ts2::sim::fillLotPackage(package, world, globals, 0, owners, nullptr);
    CHECK(counts.objectsAdded == 1 && counts.objectsUnnamed == 0);
    const auto made = std::find_if(package.resources.begin(), package.resources.end(), [&](const auto& r) {
        return r.key.type == personType && r.key.instance == static_cast<std::uint32_t>(newbornId);
    });
    CHECK(made != package.resources.end());
    if (made != package.resources.end()) {
        const auto record = ts2::xobj::parse(made->compressed ? ts2::qfs::decompress(made->bytes) : made->bytes, ts2::xobj::kWrittenVersion,
                                             ts2::xobj::RecordKind::Person);
        CHECK(record.person && record.tail && record.tail->value1 == 0);
        if (record.person) {
            const auto& s = record.person->state;
            CHECK(s.text.empty() && s.pickedIconObject == 0 && s.entries.empty() && s.queue.empty());
            CHECK(ts2::xobj::isBlank(s.interrupted) && s.interrupted.id == 0);
        }
    }
    // Its scene record places it where it stands, with no pose or materials of the parent's.
    const auto scene = std::find_if(package.resources.begin(), package.resources.end(), [&](const auto& r) {
        return r.key.type == ts2::types::kSceneObject && r.key.instance == static_cast<std::uint32_t>(newbornId);
    });
    CHECK(scene != package.resources.end());
    if (scene != package.resources.end()) {
        const auto record = ts2::cobject::readRecord(scene->compressed ? ts2::qfs::decompress(scene->bytes) : scene->bytes);
        CHECK(record && record->chain == ts2::cobject::Chain::Person && record->object.joints.empty() && record->object.materials.empty());
        if (record) {
            CHECK(std::bit_cast<float>(record->object.place[0]) == 21.5f && std::bit_cast<float>(record->object.place[1]) == 8.0f);
            // Its first setting is the clip handle counter, past the Sims' saved one by a serial for
            // each of the lot's two Sims.
            CHECK(record->animatable.place == record->object.place && record->animatable.settings[0] == 0x759 + 2);
        }
    }
    // It gets an empty routing aggregate, as every idle Sim has.
    const auto routing = std::find_if(package.resources.begin(), package.resources.end(), [&](const auto& r) {
        return r.key.type == 0xBC66BAEC && r.key.instance == static_cast<std::uint32_t>(newbornId);
    });
    CHECK(routing != package.resources.end());
    if (routing != package.resources.end()) {
        const auto aggregate = ts2::lotrecords::readRoutingAggregate(routing->compressed ? ts2::qfs::decompress(routing->bytes) : routing->bytes);
        CHECK(aggregate && aggregate->objects == 0 && aggregate->list.empty());
    }
}

// A new object of a type the lot's table does not hold gets its type added at the end of the table
// and the record the game writes for an object just made, with the object's state over it.
void savingALotAddsNewTypes() {
    ts2::objt::ObjectTypeTable types;
    ts2::objt::Entry entry;
    entry.guid = 0x1000;
    entry.index = 40;
    types.entries.push_back(entry);
    ts2::sim::World world;
    const std::int32_t wasId = world.create(kObjectGroup, typed(0x1000)).id;
    ts2::sim::SimObject& made = world.create(kObjectGroup, typed(0x5000));
    made.data[0x20] = 44;
    const std::int32_t madeId = made.id;

    ts2::xobj::SavedObject record;
    record.version = ts2::xobj::kWrittenVersion;
    record.attributes.assign(8, 0);
    record.tail.emplace();
    record.tail->relationshipVersion = 3;
    ts2::objm::MapRecord map;
    map.objects = {{wasId, 40}};
    map.waits.assign(4, 0);
    ts2::dbpf::PackageImage package;
    package.resources = {
        ts2::dbpf::storeResource({ts2::types::kObjectMap, 0xFFFFFFFF, 1, 0}, ts2::objm::writeMapRecord(map)),
        ts2::dbpf::storeResource({ts2::types::kObjectTypeTable, 0xFFFFFFFF, 0, 0}, ts2::objt::write(types)),
        ts2::dbpf::storeResource({ts2::types::kSavedObject, 0xFFFFFFFF, static_cast<std::uint32_t>(wasId), 0},
                                 ts2::xobj::write(record, ts2::xobj::RecordKind::Object)),
    };
    package.dataOrder = {0, 1, 2};
    ts2::sim::TypeOwners owners;
    ts2::objt::Entry added;
    added.guid = 0x5000;
    added.index = 41;
    added.kind = 4;
    added.name = "*Disc";
    added.unknown[8] = 3;  // three semi-global attributes
    owners.newTypes.push_back(added);
    const std::array<std::int16_t, ts2::simi::kGlobalCount> globals{};
    const ts2::sim::LotSaveCounts counts = ts2::sim::fillLotPackage(package, world, globals, 0, owners, nullptr);
    CHECK(counts.typesAdded == 1 && counts.typeTable == 1 && counts.objectsAdded == 1 && counts.objectsFresh == 1);
    const auto body = [](const ts2::dbpf::StoredResource& resource) {
        return resource.compressed ? ts2::qfs::decompress(resource.bytes) : resource.bytes;
    };
    const auto table = ts2::objt::parse(body(package.resources[1]));
    CHECK(table.entries.size() == 2 && table.entries[1].guid == 0x5000 && table.entries[1].index == 41 &&
          table.entries[1].name == "*Disc");
    const auto written = ts2::objm::readMapRecord(body(package.resources[0]));
    CHECK(written && written->objects.size() == 2 && written->objects[0].first == madeId && written->objects[0].second == 41);
    // The new object's wait entry carries run flag 1, as the game's own new objects do, or the game
    // never runs its script after a load.
    CHECK(written && written->waits.size() >= static_cast<std::size_t>(madeId) && (written->waits[madeId - 1] & 0x10000) != 0 &&
          (written->waits[madeId - 1] & 0xFFFF) == 0xFFFF);
    CHECK(package.resources.size() == 4 && package.resources[3].key.instance == static_cast<std::uint32_t>(madeId));
    const auto fresh = ts2::xobj::parse(body(package.resources[3]), ts2::xobj::kWrittenVersion, ts2::xobj::RecordKind::Object);
    CHECK(fresh.header.value2 == 1 && fresh.header.value4 == -1 && fresh.attributes.size() == 8 &&
          fresh.semiAttributes.size() == 3);
    CHECK(fresh.tail && fresh.tail->relationshipVersion == 3 && fresh.tail->slots.marker == -101 && fresh.data[0x20] == 44);
}

namespace {

// An object's record as the lots the game ships save it (version 0xAD): no header value after the
// level's, a token without its three newer values, 0x58 data values and an empty tail.
std::vector<std::uint8_t> shippedObjectRecord(std::int16_t id, std::int16_t washValue) {
    std::vector<std::uint8_t> d(64, 0);
    const auto put = [&d](std::uint32_t v, int size) {
        for (int i = 0; i < size; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    for (int i = 0; i < 6; ++i) put(0, 4);  // reserved and location
    put(0, 4);                              // level
    put(1, 2);
    put(0, 4);
    put(0xFFFFFFFF, 4);
    put(0, 2);
    put(3, 2);  // three attributes
    for (int i = 0; i < 3; ++i) put(0, 2);
    put(0, 2);  // no semi-global attributes
    for (int i = 0; i < 8; ++i) put(0, 2);
    for (int i = 0; i < 0x58; ++i) {
        put(static_cast<std::uint16_t>(i == 0x0B ? id : i == 0x39 ? washValue : i == 0x20 ? 5 : 0), 2);
    }
    for (int i = 0; i < 8; ++i) put(0, 2);  // token source
    put(0x1000, 4);                          // token guid
    put(0, 2);
    put(0, 4);  // token properties
    put(0, 2);  // arrays
    put(0, 2);  // iterators
    put(0, 4);
    put(0, 2);  // strings
    put(0, 2);
    put(0, 4);  // stack frames
    put(0, 4);
    put(3, 4);  // relationships, none
    put(0, 4);
    put(0, 2);  // slots
    put(0, 2);  // effects
    put(0, 2);  // materials
    put(0, 4);  // timers
    return d;
}

} // namespace

// A record of the shipped version reads as the game reads it, with the wash clothes value set to
// 255 whatever was saved, and moves to the version the game writes.
void shippedObjectRecordsUpgrade() {
    const auto bytes = shippedObjectRecord(9, 7);
    const auto read = ts2::xobj::parse(bytes, ts2::xobj::kShippedVersion, ts2::xobj::RecordKind::Object);
    CHECK(read.data[0x0B] == 9 && read.data[0x20] == 5 && read.data[0x39] == 255);
    CHECK(read.attributes.size() == 3 && read.tail && read.tail->token.guid == 0x1000);
    const auto moved = ts2::xobj::upgraded(read);
    CHECK(moved && moved->version == ts2::xobj::kWrittenVersion);
    if (moved) {
        const auto written = ts2::xobj::write(*moved, ts2::xobj::RecordKind::Object);
        const auto again = ts2::xobj::parse(written, ts2::xobj::kWrittenVersion, ts2::xobj::RecordKind::Object);
        CHECK(again.data == read.data && again.attributes == read.attributes);
        CHECK(ts2::xobj::write(again, ts2::xobj::RecordKind::Object) == written);
    }
    // Only the shipped version is upgraded.
    auto other = read;
    other.version = 0xD2;
    CHECK(!ts2::xobj::upgraded(other));

    ts2::xobj::SavedObject sim = read;
    sim.person.emplace();
    sim.person->version = 2;
    sim.person->personData.assign(ts2::xobj::personDataCountForVersion(ts2::xobj::kShippedVersion), 4);
    const auto movedSim = ts2::xobj::upgraded(sim);
    CHECK(movedSim && movedSim->person->personData.size() == ts2::xobj::kPersonDataCount);
    CHECK(movedSim && movedSim->person->personData[0xA9] == 4 && movedSim->person->personData[0xAA] == 0);
}

// Saving a lot the game has never saved writes its objects and object map at the version the game
// writes. Its subscriptions come from the running lot, and only for objects the map lists. A
// listed object whose record cannot be moved stops the save.
void savingAShippedLotUpgradesIt() {
    ts2::objt::ObjectTypeTable types;
    ts2::objt::Entry entry;
    entry.guid = 0x1000;
    entry.index = 40;
    types.entries.push_back(entry);
    ts2::sim::World world;
    ts2::sim::SimObject& running = world.create(kObjectGroup, typed(0x1000));
    running.data[0x20] = 77;
    const std::int16_t id = running.id;
    const std::int16_t unsaved = world.create(kObjectGroup, typed(0x2000)).id;  // a type the table lacks
    world.messageSubscriptions().subscribe(0xAA, id, "CT - Listen");
    world.messageSubscriptions().subscribe(0xAA, unsaved, "CT - Other");
    world.messageSubscriptions().subscribe(0xBB, unsaved, "CT - Other");

    ts2::objm::MapRecord map;
    map.objects = {{id, 40}};
    map.waits.assign(4, 0);
    auto shippedMap = ts2::objm::writeMapRecord(map);
    shippedMap[68] = static_cast<std::uint8_t>(ts2::objm::kShippedVersion);
    shippedMap.resize(shippedMap.size() - 4);  // the shipped version ends before the subscriptions
    const auto loadedMap = ts2::objm::readMapRecord(shippedMap);
    CHECK(loadedMap && loadedMap->upgraded);

    const auto build = [&](std::vector<std::uint8_t> object) {
        ts2::dbpf::PackageImage package;
        package.resources = {
            ts2::dbpf::storeResource({ts2::types::kObjectMap, 0xFFFFFFFF, 1, 0}, shippedMap),
            ts2::dbpf::storeResource({ts2::types::kObjectTypeTable, 0xFFFFFFFF, 0, 0}, ts2::objt::write(types)),
            ts2::dbpf::storeResource({ts2::types::kSavedObject, 0xFFFFFFFF, static_cast<std::uint32_t>(id), 0},
                                     std::move(object)),
        };
        package.dataOrder = {0, 1, 2};
        return package;
    };
    const auto body = [](const ts2::dbpf::StoredResource& resource) {
        return resource.compressed ? ts2::qfs::decompress(resource.bytes) : resource.bytes;
    };
    const std::array<std::int16_t, ts2::simi::kGlobalCount> globals{};
    auto package = build(shippedObjectRecord(id, 0));
    const ts2::sim::LotSaveCounts counts = ts2::sim::fillLotPackage(package, world, globals, 0, {}, nullptr);
    CHECK(counts.objectMap == 1 && counts.objects == 1);
    const auto written = ts2::objm::readMapRecord(body(package.resources[0]));
    CHECK(written && !written->upgraded && written->objects.size() == 1 && written->waitsWithObject == 1);
    CHECK(written && written->subscriptions.size() == 1 && written->subscriptions[0].message == 0xAA &&
          written->subscriptions[0].subscribers.size() == 1 && written->subscriptions[0].subscribers[0].object == id);
    const auto object = ts2::xobj::parse(body(package.resources[2]), ts2::xobj::kWrittenVersion,
                                         ts2::xobj::RecordKind::Object);
    CHECK(object.data[0x20] == 77);  // the running object's state

    auto broken = shippedObjectRecord(id, 0);
    broken.resize(broken.size() - 10);
    auto cut = build(std::move(broken));
    bool refused = false;
    try {
        ts2::sim::fillLotPackage(cut, world, globals, 0, {}, nullptr);
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);
}

// Saving a lot puts the records built from the running lot back into its package and writes every
// other resource as the lot was loaded with it.
void savingALotKeepsWhatItDoesNotFill() {
    ts2::objt::ObjectTypeTable types;
    ts2::objt::Entry entry;
    entry.guid = 0x1000;
    entry.index = 40;
    types.entries.push_back(entry);
    ts2::sim::World world;
    const std::int32_t id = world.create(kObjectGroup, typed(0x1000)).id;

    ts2::simi::Record simulator;
    simulator.globals[7] = 11;
    ts2::objm::MapRecord map;
    map.objects = {{id, 40}};
    map.waits.assign(4, 0);
    const std::vector<std::uint8_t> other{1, 2, 3, 4, 5};

    ts2::dbpf::PackageImage package;
    package.resources = {
        ts2::dbpf::storeResource({ts2::simi::kType, 0xFFFFFFFF, 0, 0}, ts2::simi::writeRecord(simulator)),
        ts2::dbpf::storeResource({ts2::types::kObjectMap, 0xFFFFFFFF, 1, 0}, ts2::objm::writeMapRecord(map)),
        ts2::dbpf::storeResource({ts2::types::kObjectTypeTable, 0xFFFFFFFF, 0, 0}, ts2::objt::write(types)),
        ts2::dbpf::storeResource({ts2::types::kSavedObject, 0xFFFFFFFF, 2, 0}, other),
    };
    std::array<std::int16_t, ts2::simi::kGlobalCount> globals{};
    globals[7] = 25;
    const ts2::sim::LotSaveCounts counts = ts2::sim::fillLotPackage(package, world, globals, 0x1234, {}, nullptr);
    CHECK(counts.simulator == 1 && counts.objectMap == 1 && counts.camera == 0 && counts.keptAsLoaded == 2);

    const auto body = [](const ts2::dbpf::StoredResource& resource) {
        return resource.compressed ? ts2::qfs::decompress(resource.bytes) : resource.bytes;
    };
    const auto written = ts2::simi::readRecord(body(package.resources[0]));
    CHECK(written && written->globals[7] == 25 && written->tickRandomState == 0x1234);
    const auto writtenMap = ts2::objm::readMapRecord(body(package.resources[1]));
    CHECK(writtenMap && writtenMap->objects.size() == 1 && writtenMap->objects[0].first == id);
    // A resource the save does not fill keeps the bytes it was read with.
    CHECK(package.resources[3].bytes == other);
}

// Runs `frames` frames of `frameMs` real ms each, whose ticks take no real time.
int runFrames(ts2::sim::TickPacer& pacer, bool ultra, int frames, std::int64_t frameMs, std::int64_t& watch) {
    int ticks = 0;
    for (int i = 0; i < frames; ++i) {
        watch += frameMs;
        ticks += pacer.frame(ultra, [] {}, [&] { return watch; }, [] { return 0.0; });
    }
    return ticks;
}

void ticksKeepUpWithTheSpeedClock() {
    std::int64_t watch = 0;
    ts2::sim::TickPacer normal;
    const int atNormal = runFrames(normal, false, 100, 42, watch);
    CHECK(atNormal >= 97 && atNormal <= 101);  // 4200 clock ms, a tick each 42
    // At ultra speed 60 frames a second move the clock 200 ms a frame, not 336.
    ts2::sim::TickPacer ultra;
    ultra.setSpeed(21);
    watch = 0;
    const int atUltra = runFrames(ultra, true, 100, 16, watch);
    CHECK(atUltra >= 470 && atUltra <= 482);
    // Each frame's ticks run up to three ticks past the clock, as whether one runs is decided from
    // what was owed before the last ran.
    CHECK(ultra.owedMs() > -3 * ts2::sim::TickPacer::kTickMs && ultra.owedMs() < ts2::sim::TickPacer::kTickMs);
    ultra.setSpeed(1);
    CHECK(ultra.owedMs() == 0);
    // A slow frame's ticks stop after 250 real ms.
    ts2::sim::TickPacer slow;
    slow.setSpeed(21);
    int ran = 0;
    double taken = 0;
    for (int i = 0; i < 20; ++i) {
        watch += 200;
        ran = slow.frame(true, [&] { taken += 100; }, [&] { return watch; }, [&] { return taken; });
        taken = 0;
    }
    CHECK(ran == 3);
}

void simulatorIdsCountUpAndSkipZero() {
    std::array<std::int16_t, 256> globals{};
    CHECK(ts2::sim::nextSimulatorId(globals) == 1 && ts2::sim::nextSimulatorId(globals) == 2 && globals[0x24] == 3);
    globals[0x24] = -28498;  // as saved: the counter goes negative past 0x7FFF
    CHECK(ts2::sim::nextSimulatorId(globals) == 0x90AE && globals[0x24] == -28497);
    globals[0x24] = -1;
    CHECK(ts2::sim::nextSimulatorId(globals) == 0xFFFF && globals[0x24] == 0 && ts2::sim::nextSimulatorId(globals) == 1);
}

void simulatorRecordTakesTheSimulationsGlobalsAndTickState() {
    ts2::simi::Record loaded;
    loaded.globals[0] = 8;
    loaded.globals[0x2B] = 1;
    loaded.tickRandomState = 1;
    loaded.otherRandomState = 7;
    loaded.rest[3] = 9;
    std::array<std::int16_t, 256> globals{};
    globals[0] = 9;
    globals[0x3B] = 5;
    globals[0x3C] = 6;  // past the 60 the record saves
    const ts2::simi::Record saved = ts2::sim::simulatorRecord(loaded, globals, 0x1234);
    CHECK(saved.globals[0] == 9 && saved.globals[0x2B] == 0 && saved.globals[0x3B] == 5 && saved.tickRandomState == 0x1234);
    CHECK(saved.otherRandomState == 7 && saved.rest[3] == 9);
}

// The object map a save writes: objects that are gone drop out, objects the record already lists
// keep their place, new ones go in front with the newest first, and the wait table holds the ticks
// a sleeping script still waits, 0 for an id with no object. A new object carries run flag 1.
void objectMapRecordFollowsTheLotsObjects() {
    ts2::objt::ObjectTypeTable types;
    for (std::uint16_t i = 0; i < 3; ++i) {
        ts2::objt::Entry entry;
        entry.guid = 0x1000u + i;
        entry.index = static_cast<std::uint16_t>(40 + i);
        types.entries.push_back(entry);
    }
    ts2::sim::World world;
    ts2::sim::SimObject& kept = world.create(kObjectGroup, typed(0x1000));
    ts2::sim::SimObject& gone = world.create(kObjectGroup, typed(0x1001));
    ts2::sim::SimObject& sleeper = world.create(kObjectGroup, typed(0x1002));
    sleeper.thread.sleepTicks = 4;
    const std::int32_t keptId = kept.id, goneId = gone.id, sleeperId = sleeper.id;
    ts2::objm::MapRecord loaded;
    loaded.objects = {{goneId, 41}, {keptId, 40}};
    loaded.waits.assign(8, 0);
    loaded.waits[static_cast<std::size_t>(goneId) - 1] = 0x0009FFFE;
    loaded.waits[static_cast<std::size_t>(keptId) - 1] = 0x0002FFFE;  // kept, since it isn't sleeping
    loaded.buildBuyBlockers = {7};
    world.remove(gone);
    world.purge();
    const ts2::objm::MapRecord saved = ts2::sim::objectMapRecord(loaded, types, world);
    CHECK((saved.objects == std::vector<std::pair<std::int32_t, std::int32_t>>{{sleeperId, 42}, {keptId, 40}}));
    CHECK(saved.waits[static_cast<std::size_t>(goneId) - 1] == 0);
    CHECK(saved.waits[static_cast<std::size_t>(keptId) - 1] == 0x0002FFFE);
    CHECK(saved.waits[static_cast<std::size_t>(sleeperId) - 1] == 0x00010003);
    CHECK(saved.waitsWithObject == 2 && saved.buildBuyBlockers == std::vector<std::int32_t>{7});

    // The table grows to an object past its end, and an object new at a reused id starts as a new
    // object, not from the entry the gone object left.
    ts2::sim::SimObject& atGone = world.create(kObjectGroup, typed(0x1000));  // another type at the gone id
    CHECK(atGone.id == goneId);
    for (int i = 0; i < 8; ++i) {
        world.create(kObjectGroup, typed(0x1001));
    }
    const ts2::objm::MapRecord grown = ts2::sim::objectMapRecord(loaded, types, world);
    CHECK(grown.waits.size() == 11 && grown.waitsWithObject == 11);
    CHECK(grown.waits[static_cast<std::size_t>(goneId) - 1] == 0x0001FFFF);
}

} // namespace

std::vector<Test> simulationTests() {
    return {
        {"the simulator record takes the simulation's globals and tick state", simulatorRecordTakesTheSimulationsGlobalsAndTickState},
        {"the object map record follows the lot's objects", objectMapRecordFollowsTheLotsObjects},
        {"the object record follows the running object", theObjectRecordFollowsTheRunningObject},
        {"the object record keeps a Sim's interactions", theObjectRecordKeepsASimsInteractions},
        {"saving a lot keeps what it does not fill", savingALotKeepsWhatItDoesNotFill},
        {"saving a lot gives new objects a record", savingALotGivesNewObjectsARecord},
        {"saving a lot lists only new objects with a record", savingALotListsOnlyNewObjectsWithARecord},
        {"saving a lot adds new types", savingALotAddsNewTypes},
        {"saving a lot tells reused ids apart", savingALotTellsReusedIdsApart},
        {"saving a lot refuses Sims made this run", savingALotRefusesSimsMadeThisRun},
        {"saving a lot keeps interaction ids apart", savingALotKeepsInteractionIdsApart},
        {"interaction ids come from the simulator counter", interactionIdsComeFromTheSimulatorCounter},
        {"saving a lot writes the Sims' descriptions", savingALotWritesTheSimsDescriptions},
        {"saving a lot writes the changed inventories", savingALotWritesTheChangedInventories},
        {"saving a lot writes the family funds and relationships", savingALotWritesTheFamilyFundsAndRelationships},
        {"relationship lists take the game's table order", relationshipListsTakeTheGamesTableOrder},
        {"saving a lot adds new relationships", savingALotAddsNewRelationships},
        {"saving a lot writes a newborn's neighborhood records", savingALotWritesANewbornsNeighborhoodRecords},
        {"new character files start from their template", newCharacterFilesStartFromTheirTemplate},
        {"a made Sim's name text holds every language", aMadeSimsNameTextHoldsEveryLanguage},
        {"a name row is taken in every language", aNameRowIsTakenInEveryLanguage},
        {"made look records list each part once", madeLookRecordsListEachPartOnce},
        {"newborn look records cover every age", newbornLookRecordsCoverEveryAge},
        {"a changed look rewrites the age data and its list", aChangedLookRewritesTheAgeDataAndItsList},
        {"a changed look keeps the skeleton", aChangedLookKeepsTheSkeleton},
        {"animations hang from the seat they started in", animationsHangFromTheSeatTheyStartedIn},
        {"a clip built from an empty entry starts as the game makes one", aClipBuiltFromAnEmptyEntryStartsAsTheGameMakesOne},
        {"a stopped looping clip is not saved", aStoppedLoopingClipIsNotSaved},
        {"newborn brow family follows the draw", newbornBrowFamilyFollowsTheDraw},
        {"newborn file needs the catalog", newbornFileNeedsTheCatalog},
        {"saving a lot writes a newborn from another Sim", savingALotWritesANewbornFromAnotherSim},
        {"new face models name the Sim's own parts", newFaceModelsNameTheSimsOwnParts},
        {"new face models build on the skeleton", newFaceModelsBuildOnTheSkeleton},
        {"new face meshes replace the face", newFaceMeshesReplaceTheFace},
        {"new face materials composite the overlays", newFaceMaterialsCompositeTheOverlays},
        {"saving a lot keeps stale types' values", savingALotKeepsStaleTypesValues},
        {"saving a lot writes the effects playing now", savingALotWritesTheEffectsPlayingNow},
        {"saving a lot writes the types' values now", savingALotWritesTheTypesValuesNow},
        {"shipped object records upgrade", shippedObjectRecordsUpgrade},
        {"saving a shipped lot upgrades it", savingAShippedLotUpgradesIt},
        {"simulator ids count up and skip zero", simulatorIdsCountUpAndSkipZero},
        {"the speed clock moves at most 200 ms an update", speedClockMovesAtMost200MsAnUpdate},
        {"ticks keep up with the speed clock", ticksKeepUpWithTheSpeedClock},
        {"rooms are filled between walls", roomsAreFilledBetweenWalls},
        {"the room on a side of a wall matches the wall graph", roomOnSideOfWallMatchesTheWallGraph},
        {"greenhouse rooms are mostly greenhouse walls", greenhouseRoomsAreMostlyGreenhouseWalls},
        {"broken numbers are left out", brokenNumbersAreLeftOut},
        {"diagonal walls split tiles", diagonalWallsSplitTiles},
        {"saved room grid is read by quarter", savedRoomGridIsReadByQuarter},
        {"score curves interpolate and clamp", scoreCurvesInterpolateAndClamp},
        {"selection passes to the next selectable Sim", selectionPassesToTheNextSelectableSim},
        {"motives decay by their rates", motivesDecayByTheirRates},
        {"motive changes stop at their limits", motiveChangesStopAtTheirLimits},
        {"mood is the weighted average of the motives", moodIsTheWeightedAverageOfTheMotives},
        {"advertisements scale by personality", advertisementsScaleByPersonality},
        {"multiplied advertisements follow the game", multipliedAdvertisementsFollowTheGame},
        {"pets advertise by personality", petsAdvertiseByPersonality},
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
