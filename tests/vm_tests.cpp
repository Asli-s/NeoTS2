#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
#include <limits>
#include <cmath>
#include "sim/clock.hpp"
#include "sim/object_footprint.hpp"
#include "sim/object_reset.hpp"
#include "sim/route_grid.hpp"
#include "sim/route_items.hpp"
#include "sim/route_world.hpp"
#include "sim/type_signature.hpp"
#include "sim/vm.hpp"
#include "formats/datatables.hpp"
#include "formats/fami.hpp"
#include "formats/famt.hpp"
#include "formats/hash.hpp"
#include "formats/scenegraph.hpp"
#include "formats/sdsc.hpp"
#include "formats/slot.hpp"
#include "content/build_scripts.hpp"
#include "content/install.hpp"
#include "content/neighborhood.hpp"
#include "content/ui_meters.hpp"
#include "content/target_pictures.hpp"
#include "diag/log.hpp"
#include "diag/report.hpp"
#include "diag/upload.hpp"
#include "sim/motives.hpp"
#include "scene/rig.hpp"
#include "scene/sims.hpp"
#include "scene/portrait.hpp"
#include "formats/fencepostlayer.hpp"
#include "content/lot.hpp"
#include "scene/floor_light.hpp"
#include "scene/light_grid.hpp"
#include "scene/wall_runs.hpp"
#include "scene/lamp_lights.hpp"
#include "scene/portal_records.hpp"
#include "scene/room_samples.hpp"
#include "content/covering_colors.hpp"
#include "content/lighting.hpp"
#include "scene/portable_math.hpp"
#include <cfenv>
#include "scene/room_light.hpp"
#include "scene/wall_light.hpp"
#include "scene/light_corners.hpp"
#include "scene/lot_lighting.hpp"
#include "scene/light_map_texels.hpp"
#include "scene/lot_light_maps.hpp"
#include "scene/lot_shadow.hpp"
#include "scene/window_light.hpp"
#include "scene/neighborhood_lighting.hpp"
#include "scene/neighborhood_paint.hpp"
#include "scene/lot_parts.hpp"
#include "scene/lot_scene.hpp"
#include "scene/view.hpp"
#include "formats/namemap.hpp"
#include "formats/roofs.hpp"
#include "formats/terrain.hpp"
#include "formats/stringmap.hpp"
#include "formats/walllayer.hpp"
#include "formats/swaf.hpp"
#include "formats/ttab.hpp"
#include "formats/tga.hpp"
#include "formats/image.hpp"
#include "formats/jpeg.hpp"
#include "formats/reia.hpp"
#include "formats/camera_config.hpp"
#include "formats/lot_description.hpp"
#include "formats/nhood_objects.hpp"
#include "formats/nhood_paint_rules.hpp"
#include "formats/nhood_terrain.hpp"
#include "formats/anim.hpp"
#include "formats/propset.hpp"
#include "formats/reference_list.hpp"
#include "formats/ui_script.hpp"
#include "sim/lua_runtime.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using ts2::bhav::Behavior;
using ts2::bhav::Instruction;
using ts2::sim::Outcome;
using ts2::sim::RunResult;
using ts2::sim::ThreadState;
using ts2::sim::Vm;
namespace owner = ts2::sim::owner;
namespace op = ts2::sim::op;

int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("    FAILED line %d: %s\n", __LINE__, #cond);            \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

constexpr std::uint32_t kObjectGroup = 0x7F000001;
constexpr std::uint32_t kSemiGroup = 0x7F000002;
constexpr std::uint16_t T = ts2::bhav::kTargetTrue;
constexpr std::uint16_t F = ts2::bhav::kTargetFalse;
constexpr std::uint16_t E = ts2::bhav::kTargetError;

class MemoryCodeSource : public ts2::sim::CodeSource {
public:
    void add(std::uint32_t group, std::uint16_t instance, Behavior b) {
        behaviors_[{group, instance}] = std::make_shared<const Behavior>(std::move(b));
    }
    void addConstants(std::uint32_t group, std::uint16_t instance, std::vector<std::int16_t> v) {
        constants_[{group, instance}] = std::move(v);
    }

    std::shared_ptr<const Behavior> behavior(std::uint32_t group, std::uint16_t instance) override {
        const auto it = behaviors_.find({group, instance});
        return it != behaviors_.end() ? it->second : nullptr;
    }
    const std::vector<std::int16_t>* constants(std::uint32_t group, std::uint16_t instance) override {
        const auto it = constants_.find({group, instance});
        return it != constants_.end() ? &it->second : nullptr;
    }
    std::uint32_t semiGlobalGroup(std::uint32_t objectGroup) override {
        return objectGroup == kObjectGroup ? kSemiGroup : objectGroup;
    }
    void addObjectType(std::uint32_t group, ts2::objd::ObjectDefinition definition) {
        const std::uint32_t guid = definition.guid();
        objectTypes_[guid] = ts2::sim::ObjectType{group, std::move(definition)};
    }
    void addInteractionTable(std::uint32_t group, std::uint16_t instance, ts2::ttab::InteractionTable table) {
        tables_[{group, instance}] = std::make_shared<const ts2::ttab::InteractionTable>(std::move(table));
    }
    std::shared_ptr<const ts2::ttab::InteractionTable> interactionTable(std::uint32_t group,
                                                                       std::uint16_t instance) override {
        const auto it = tables_.find({group, instance});
        return it != tables_.end() ? it->second : nullptr;
    }
    const ts2::sim::ObjectType* objectType(std::uint32_t guid) override {
        const auto it = objectTypes_.find(guid);
        return it != objectTypes_.end() ? &it->second : nullptr;
    }
    void addFunctions(std::uint32_t group, std::vector<ts2::objf::Function> functions) {
        functions_[group] = std::move(functions);
    }
    const std::vector<ts2::objf::Function>* objectFunctions(const ts2::sim::ObjectType& type) override {
        const auto it = functions_.find(type.group);
        return it != functions_.end() ? &it->second : nullptr;
    }
    void addStrings(std::uint32_t group, std::uint16_t instance, std::vector<std::string> values) {
        strings_[{group, instance}] = std::move(values);
    }
    std::optional<std::string> string(std::uint32_t group, std::uint16_t instance,
                                      std::size_t index) override {
        const auto it = strings_.find({group, instance});
        if (it == strings_.end() || index >= it->second.size()) {
            return std::nullopt;
        }
        return it->second[index];
    }
    std::optional<std::uint16_t> behaviorByName(std::uint32_t group, const std::string& name) override {
        for (const auto& [key, b] : behaviors_) {
            if (key.first == group && b->name == name) {
                return key.second;
            }
        }
        return std::nullopt;
    }

private:
    std::map<std::uint32_t, std::vector<ts2::objf::Function>> functions_;
    std::map<std::pair<std::uint32_t, std::uint16_t>, std::vector<std::string>> strings_;
    std::map<std::uint32_t, ts2::sim::ObjectType> objectTypes_;
    std::map<std::pair<std::uint32_t, std::uint16_t>, std::shared_ptr<const ts2::ttab::InteractionTable>> tables_;
    std::map<std::pair<std::uint32_t, std::uint16_t>, std::shared_ptr<const Behavior>> behaviors_;
    std::map<std::pair<std::uint32_t, std::uint16_t>, std::vector<std::int16_t>> constants_;
};

Instruction expr(std::uint8_t lhsOwner, std::uint16_t lhsData, std::uint8_t operation,
                 std::uint8_t rhsOwner, std::uint16_t rhsData, std::uint16_t t, std::uint16_t f) {
    Instruction in;
    in.opcode = ts2::bhav::kExpressionOpcode;
    in.trueTarget = t;
    in.falseTarget = f;
    in.operands[0] = static_cast<std::uint8_t>(lhsData);
    in.operands[1] = static_cast<std::uint8_t>(lhsData >> 8);
    in.operands[2] = static_cast<std::uint8_t>(rhsData);
    in.operands[3] = static_cast<std::uint8_t>(rhsData >> 8);
    in.operands[5] = operation;
    in.operands[6] = lhsOwner;
    in.operands[7] = rhsOwner;
    return in;
}

Instruction call(std::uint16_t opcode, std::uint16_t t, std::uint16_t f,
                 std::array<std::int16_t, 4> args = {}) {
    Instruction in;
    in.opcode = opcode;
    in.trueTarget = t;
    in.falseTarget = f;
    for (std::size_t i = 0; i < 4; ++i) {
        in.operands[i * 2] = static_cast<std::uint8_t>(args[i]);
        in.operands[i * 2 + 1] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(args[i]) >> 8);
    }
    return in;
}

Instruction callWithTemps(std::uint16_t opcode, std::uint16_t t, std::uint16_t f) {
    Instruction in = call(opcode, t, f);
    std::fill_n(in.operands.begin(), 8, std::uint8_t{0xFF});
    return in;
}

Behavior bhav(std::vector<Instruction> code, std::uint8_t args = 0, std::uint8_t locals = 0) {
    Behavior b;
    b.format = 0x8009;
    b.argCount = args;
    b.localCount = locals;
    b.instructions = std::move(code);
    return b;
}

RunResult runPrivate(MemoryCodeSource& code, ThreadState& thread, std::uint16_t entry,
                     ts2::sim::Limits limits = {}) {
    Vm vm(code, limits);
    return vm.run(thread, kObjectGroup, entry);
}

void assignAndReturnTrue() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 5, T, E)}));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::True);
    CHECK(t.temps[0] == 5);
    CHECK(r.steps == 1);
}

void comparisonTakesFalseBranch() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kLiteral, 3, op::kGreater, owner::kLiteral, 5, T, F)}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::False);
}

void loopSumsOneToTen() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kLocal, 0, op::kAssign, owner::kLiteral, 0, 1, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 0, 2, E),
        expr(owner::kLocal, 0, op::kIncrementAndLess, owner::kLiteral, 11, 3, T),
        expr(owner::kTemp, 0, op::kAdd, owner::kLocal, 0, 2, E),
    }, 0, 1));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::True);
    CHECK(t.temps[0] == 55);
}

void callPassesLiteralArgumentsAndReturns() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 1, op::kAssign, owner::kParam, 0, 1, E),
        expr(owner::kTemp, 1, op::kAdd, owner::kParam, 1, F, E),
    }, 2));
    code.add(kObjectGroup, 0x1000, bhav({
        call(0x1001, E, 1, {7, 8}),
        expr(owner::kTemp, 2, op::kAssign, owner::kLiteral, 1, T, E),
    }));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::True);
    CHECK(t.temps[1] == 15);
    CHECK(t.temps[2] == 1);
}

void callPassesTempsWhenArgumentsAreAllFF() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 5, op::kAssign, owner::kParam, 0, 1, E),
        expr(owner::kTemp, 5, op::kMultiply, owner::kParam, 1, T, E),
    }, 2));
    code.add(kObjectGroup, 0x1000, bhav({callWithTemps(0x1001, T, E)}));
    ThreadState t;
    t.temps[0] = 6;
    t.temps[1] = 7;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
    CHECK(t.temps[5] == 42);
}

void localsArePerFrame() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1001, bhav({expr(owner::kLocal, 0, op::kAssign, owner::kLiteral, 99, T, E)}, 0, 1));
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kLocal, 0, op::kAssign, owner::kLiteral, 1, 1, E),
        call(0x1001, 2, E),
        expr(owner::kLocal, 0, op::kEquals, owner::kLiteral, 1, T, F),
    }, 0, 1));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
}

void callsResolveGlobalPrivateAndSemiGlobalGroups() {
    MemoryCodeSource code;
    code.add(ts2::sim::kGlobalGroup, 0x0100, bhav({expr(owner::kTemp, 0, op::kAdd, owner::kLiteral, 1, T, E)}));
    code.add(kSemiGroup, 0x2000, bhav({expr(owner::kTemp, 0, op::kAdd, owner::kLiteral, 10, T, E)}));
    code.add(kObjectGroup, 0x1001, bhav({expr(owner::kTemp, 0, op::kAdd, owner::kLiteral, 100, T, E)}));
    code.add(kObjectGroup, 0x1000, bhav({call(0x0100, 1, E), call(0x2000, 2, E), call(0x1001, T, E)}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
    CHECK(t.temps[0] == 111);
}

void constantsResolveByIdRange() {
    MemoryCodeSource code;
    code.addConstants(kObjectGroup, 0x1002, {0, 0, 7});
    code.addConstants(kSemiGroup, 0x2001, {30});
    code.addConstants(ts2::sim::kGlobalGroup, 0x0103, {0, 500});
    const std::uint16_t privateRef = (0x02 << 7) | 2;
    const std::uint16_t semiRef = ((0x40 + 1) << 7) | 0;
    const std::uint16_t globalRef = ((0x80 + 3) << 7) | 1;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kConstant, privateRef, 1, E),
        expr(owner::kTemp, 1, op::kAssign, owner::kConstant, semiRef, 2, E),
        expr(owner::kTemp, 2, op::kAssign, owner::kConstant, globalRef, T, E),
    }));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
    CHECK(t.temps[0] == 7);
    CHECK(t.temps[1] == 30);
    CHECK(t.temps[2] == 500);
}

// Bit 15 is bit 6 of the table number; bits 13-14 = 3 is an invalid constant.
void constantBit15PicksTheUpperTables() {
    MemoryCodeSource code;
    code.addConstants(kObjectGroup, 0x1041, {0, 9});
    code.addConstants(ts2::sim::kGlobalGroup, 0x0141, {0, 0, 0, 0, 44});
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kConstant, 0x8081, 1, E),
        expr(owner::kTemp, 1, op::kAssign, owner::kConstant, 0xC084, T, E),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kConstant, 0x6000, T, E),
    }));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
    CHECK(t.temps[0] == 9);
    CHECK(t.temps[1] == 44);
    const RunResult invalid = runPrivate(code, t, 0x1001);
    CHECK(invalid.outcome == Outcome::Error && invalid.reason == "invalid constant");
}

void flagsAreOneBased() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kSetFlag, owner::kLiteral, 3, 1, E),
        expr(owner::kTemp, 0, op::kIsFlagSet, owner::kLiteral, 3, 2, F),
        expr(owner::kTemp, 0, op::kClearFlag, owner::kLiteral, 3, 3, E),
        expr(owner::kTemp, 0, op::kIsFlagSet, owner::kLiteral, 3, F, T),
    }));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
    CHECK(t.temps[0] == 0);
}

void arithmeticWrapsAt16Bits() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 32767, 1, E),
        expr(owner::kTemp, 0, op::kAdd, owner::kLiteral, 1, T, E),
    }));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
    CHECK(t.temps[0] == -32768);
}

void divisionByZeroIsAnError() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kTemp, 0, op::kDivide, owner::kLiteral, 0, T, E)}));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::Error);
    CHECK(r.reason == "division by zero");
}

void writingALiteralIsAnError() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kLiteral, 0, op::kAssign, owner::kLiteral, 1, T, E)}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::Error);
}

void outOfRangeTempIsAnError() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kTemp, 500, op::kAssign, owner::kLiteral, 1, T, E)}));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::Error);
    CHECK(r.reason == "temp index out of range");
}

void infiniteLoopHitsStepLimit() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kLiteral, 1, op::kEquals, owner::kLiteral, 1, 0, E)}));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000, {1000, 64});
    CHECK(r.outcome == Outcome::StepLimit);
    CHECK(r.steps == 1000);
}

void infiniteRecursionHitsDepthLimit() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({call(0x1000, T, F)}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000, {100000, 32}).outcome == Outcome::DepthLimit);
}

void missingCallTargetIsReported() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({call(0x1234, T, F)}));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::MissingCode);
    CHECK(r.pc == 0);
}

void unimplementedPrimitiveStopsWithItsOpcode() {
    MemoryCodeSource code;
    Instruction locomotion;
    locomotion.opcode = 0x0072;
    locomotion.trueTarget = T;
    locomotion.falseTarget = F;
    code.add(kObjectGroup, 0x1000, bhav({locomotion}));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::Unsupported);
    CHECK(r.detail == "0x0072");
}

void jumpPastTheEndIsAnError() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 1, 7, E)}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::Error);
}

ts2::objd::ObjectDefinition definition(const char* name, std::uint16_t price) {
    ts2::objd::ObjectDefinition d;
    d.name = name;
    d.fields.assign(ts2::objd::kMaxFields, 0);
    d.fields[ts2::objd::field::kPrice] = price;
    return d;
}

void attributesBelongToEachObject() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kMyAttribute, 0, op::kAdd, owner::kLiteral, 5, T, E),
    }));
    ts2::sim::World world;
    auto& fridge = world.create(kObjectGroup, definition("Fridge", 600));
    auto& other = world.create(kObjectGroup, definition("Fridge", 600));
    Vm vm(code, {}, &world);
    CHECK(vm.run(fridge, 0x1000).outcome == Outcome::True);
    CHECK(vm.run(fridge, 0x1000).outcome == Outcome::True);
    CHECK(vm.run(other, 0x1000).outcome == Outcome::True);
    CHECK(fridge.attributes[0] == 10);
    CHECK(other.attributes[0] == 5);
}

void initObjectSetsDefaultsAndAttributesByParam() {
    MemoryCodeSource code;
    Instruction initObject = call(0x0001, 1, E);  // Generic Sims Call
    initObject.operands = {};
    initObject.operands[0] = 0x41;
    code.add(kObjectGroup, 0x1000, bhav({
        initObject,
        // The stack object's attribute picked by param 1 (here 3).
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kParam, 0, 2, E),
        expr(owner::kStackObjectAttributeByParam, 1, op::kAssign, owner::kLiteral, 7, T, E),
    }, 2));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kStackObjectAttributeByParam, 5, op::kAssign, owner::kLiteral, 7, T, E),  // no param 5
    }, 2));
    ts2::sim::World world;
    auto& me = world.create(kObjectGroup, definition("Me", 0));
    auto& other = world.create(kObjectGroup, definition("Other", 0));
    Vm vm(code, {}, &world);
    const std::int16_t args[] = {other.id, 3};
    CHECK(vm.run(me, 0x1000, args).outcome == Outcome::True);
    CHECK(me.data[0x04] == 1 && me.data[0x2A] == 3 && me.data[0x2B] == 0x0A && me.data[0x28] == 0x2020);
    CHECK(me.data[0x17] == 2 && me.data[0x31] == 1 && me.data[0x4E] == 5);
    CHECK(other.attributes[3] == 7);
    CHECK(vm.run(me, 0x1001, args).outcome == Outcome::Error);
}

void objectIdFieldMatchesTheObject() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kMyData, ts2::sim::kObjectIdField, T, E),
    }));
    ts2::sim::World world;
    world.create(kObjectGroup, definition("A", 1));
    auto& b = world.create(kObjectGroup, definition("B", 1));
    Vm vm(code, {}, &world);
    CHECK(vm.run(b, 0x1000).outcome == Outcome::True);
    CHECK(b.thread.temps[0] == b.id);
    CHECK(b.id == 2);
}

void stackObjectIsReachedThroughItsId() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kParam, 0, 1, E),
        expr(owner::kStackObjectAttribute, 3, op::kAssign, owner::kLiteral, 42, 2, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kStackObjectDefinition,
             ts2::objd::field::kPrice, T, E),
    }, 1));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, definition("Sim", 0));
    auto& fridge = world.create(kObjectGroup, definition("Fridge", 600));
    Vm vm(code, {}, &world);
    const std::int16_t args[] = {fridge.id};
    CHECK(vm.run(sim, 0x1000, args).outcome == Outcome::True);
    CHECK(fridge.attributes[3] == 42);
    CHECK(sim.attributes[3] == 0);
    CHECK(sim.thread.temps[0] == 600);
    CHECK(sim.thread.stackObjectId == fridge.id);
}

void eachFrameKeepsItsOwnStackObject() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 2, 1, E),
        call(0x1001, 2, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kStackObjectId, 0, T, E),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 1, op::kAssign, owner::kStackObjectId, 0, 1, E),
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 3, T, E),
    }));
    ts2::sim::World world;
    auto& me = world.create(kObjectGroup, definition("Me", 0));
    Vm vm(code, {}, &world);
    CHECK(vm.run(me, 0x1000).outcome == Outcome::True);
    CHECK(me.thread.temps[1] == 2);
    CHECK(me.thread.temps[0] == 2);
}

void missingStackObjectIsAnError() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectAttribute, 0, op::kAssign, owner::kLiteral, 1, T, E),
    }));
    ts2::sim::World world;
    auto& me = world.create(kObjectGroup, definition("Me", 0));
    me.thread.stackObjectId = 99;
    Vm vm(code, {}, &world);
    const RunResult r = vm.run(me, 0x1000);
    CHECK(r.outcome == Outcome::Error);
    CHECK(r.reason == "stack object does not exist");
}

void definitionWritesChangeOnlyThatObjectsCopy() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectDefinition, 0x3B, op::kAssign, owner::kLiteral, 1, T, E),
    }));
    ts2::sim::World world;
    auto& a = world.create(kObjectGroup, definition("A", 0));
    auto& b = world.create(kObjectGroup, definition("B", 0));
    a.thread.stackObjectId = a.id;
    Vm vm(code, {}, &world);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::True);
    CHECK(a.definition.field(0x3B) == 1);
    CHECK(b.definition.field(0x3B) == 0);
}

void scopedArgumentsBeyondCalleeArgCountAreIgnored() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kParam, 0, 1, E),
        expr(owner::kTemp, 0, op::kAdd, owner::kParam, 1, T, E),
    }, 2));
    Instruction c = call(0x1001, T, E);
    const std::uint8_t operands[13] = {owner::kLiteral, 3, 0, owner::kLiteral, 4, 0,
                                       0x00, 0x00, 0x02, 0x00, 0xFE, 0x0B, 0x01};
    std::copy(std::begin(operands), std::end(operands), c.operands.begin());
    code.add(kObjectGroup, 0x1000, bhav({c}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
    CHECK(t.temps[0] == 7);
}

void attributesNeedAMeObject() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kMyAttribute, 0, op::kAssign, owner::kLiteral, 1, T, E),
    }));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::Unsupported);
}

constexpr std::uint32_t kFridgeGuid = 0x6C50BA66;
constexpr std::uint32_t kChairGuid = 0x12345678;

ts2::objd::ObjectDefinition typed(std::uint32_t guid, std::uint16_t type = 4) {
    ts2::objd::ObjectDefinition d;
    d.fields.assign(ts2::objd::kMaxFields, 0);
    d.fields[ts2::objd::field::kGuid1] = static_cast<std::uint16_t>(guid);
    d.fields[ts2::objd::field::kGuid2] = static_cast<std::uint16_t>(guid >> 16);
    d.fields[ts2::objd::field::kType] = type;
    return d;
}

// The object types SmallWorld's searches look for, known to the code source like the game's catalog.
void addSmallWorldTypes(MemoryCodeSource& code) {
    code.addObjectType(kObjectGroup, typed(kFridgeGuid));
    code.addObjectType(kObjectGroup, typed(kChairGuid));
}

Instruction setToNext(std::uint8_t searchType, std::uint8_t targetOwner, std::uint8_t targetData,
                      std::uint32_t guid, std::uint16_t t, std::uint16_t f) {
    Instruction in;
    in.opcode = 0x001F;
    in.trueTarget = t;
    in.falseTarget = f;
    for (std::size_t i = 0; i < 4; ++i) {
        in.operands[i] = static_cast<std::uint8_t>(guid >> (8 * i));
    }
    in.operands[4] = static_cast<std::uint8_t>(0x80 | searchType);
    in.operands[5] = targetOwner;
    in.operands[7] = targetData;
    return in;
}

Instruction testObjectType(std::uint32_t guid, std::uint8_t idOwner, std::uint16_t idData,
                           std::uint16_t t, std::uint16_t f) {
    Instruction in;
    in.opcode = 0x0020;
    in.trueTarget = t;
    in.falseTarget = f;
    for (std::size_t i = 0; i < 4; ++i) {
        in.operands[i] = static_cast<std::uint8_t>(guid >> (8 * i));
    }
    in.operands[4] = static_cast<std::uint8_t>(idData);
    in.operands[5] = static_cast<std::uint8_t>(idData >> 8);
    in.operands[6] = idOwner;
    return in;
}

struct SmallWorld {
    ts2::sim::World world;
    ts2::sim::SimObject* controller;
    SmallWorld() {
        controller = &world.create(kObjectGroup, typed(0x0BADF00D));
        world.create(kObjectGroup, typed(kFridgeGuid));
        world.create(kObjectGroup, typed(kChairGuid));
        world.create(kObjectGroup, typed(kFridgeGuid));
        world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
        world.create(kObjectGroup, typed(kFridgeGuid));
        world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    }
};

int countWithLoop(Instruction search, const std::function<void(SmallWorld&)>& setup = {}) {
    MemoryCodeSource code;
    addSmallWorldTypes(code);
    search.trueTarget = 2;
    search.falseTarget = T;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 0, 1, E),
        search,
        expr(owner::kTemp, 0, op::kAdd, owner::kLiteral, 1, 1, E),
    }));
    SmallWorld w;
    if (setup) {
        setup(w);
    }
    Vm vm(code, {}, &w.world);
    const RunResult r = vm.run(*w.controller, 0x1000);
    return r.outcome == Outcome::True ? w.controller->thread.temps[0] : -1;
}

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

// The build catalog scripts: pattern materials and wall styles, keyed in lowercase.
void buildScriptsReadPatternsAndWallStyles() {
    ts2::content::BuildScripts b;
    b.addPatterns("# comment\r\nwallPattern \"deckredwood\"\r\nmaterial \"wall_deckRedwoodAlpha_base\"\r\nnotInCatalog\r\n", true);
    b.addPatterns("floorPattern \"sidewalk\"\nmaterial \"floor_sidewalk\"\n", false);
    b.addWallStyles("wall 16\ndefaultPattern \"deckRedwood\"\nwallThickness none\nverticalSpan floorToFloorBasedCeiling\n"
                    "wall 4\nnotRendered\nwall 1\nwallThickness standard\n");
    CHECK(b.wallPatterns.at("deckredwood") == "wall_deckRedwoodAlpha_base" && b.floorPatterns.at("sidewalk") == "floor_sidewalk");
    CHECK(!b.wallStyles.at(16).thick && b.wallStyles.at(16).belowFloor && b.wallStyles.at(16).defaultPattern == "deckredwood");
    CHECK(!b.wallStyles.at(4).rendered && b.wallStyles.at(1).thick && !b.wallStyles.at(1).belowFloor);
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

// Transforms: a quarter turn about z takes +x to +y, then the translation applies.
void sceneTransformsTurnAndMove() {
    using ts2::scene::Transform;
    float v[3] = {1, 0, 0};
    Transform::at(10, 20, 3, ts2::scene::kPi / 2).apply(v, true);
    CHECK(std::abs(v[0] - 10) < 1e-5f && std::abs(v[1] - 21) < 1e-5f && std::abs(v[2] - 3) < 1e-5f);
    float d[3] = {1, 0, 0};
    Transform::at(10, 20, 3, ts2::scene::kPi / 2).apply(d, false);
    CHECK(std::abs(d[0]) < 1e-5f && std::abs(d[1] - 1) < 1e-5f);
}

// A quad is wound to face the way it's asked to, whatever order its corners come in.
void sceneQuadsFaceTheirNormal() {
    ts2::scene::Primitive p;
    ts2::scene::addQuad(p, {{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}}, {0, 0, -1}, {});
    CHECK(p.indices.size() == 6 && p.positions.size() == 18);
    auto at = [&](std::size_t i, std::size_t k) { return p.positions[i * 3 + k]; };
    const float ux = at(1, 0) - at(0, 0), uy = at(1, 1) - at(0, 1), vx = at(2, 0) - at(0, 0), vy = at(2, 1) - at(0, 1);
    CHECK(ux * vy - uy * vx < 0);  // the first triangle's normal points down
}

// A hipped roof over a 2 x 4 rectangle: four sloped faces meeting at a ridge along the long side,
// eaves out by the overhang and the ridge as high as the slope allows.
void sceneHipRoofRisesToItsRidge() {
    ts2::roofs::Roof roof;
    roof.x0 = 0;
    roof.y0 = 0;
    roof.x1 = 2;
    roof.y1 = 4;
    roof.type = 5;
    ts2::scene::Surfaces surfaces;
    ts2::scene::RoofLook look;
    look.top = 0;
    ts2::scene::addRoof(surfaces, roof, 3.0f, 1.0f, look);
    const auto& top = surfaces[0];
    float lowest = 1e9f, highest = -1e9f, minX = 1e9f;
    for (std::size_t i = 0; i < top.positions.size(); i += 3) {
        minX = std::min(minX, top.positions[i]);
        lowest = std::min(lowest, top.positions[i + 2]);
        highest = std::max(highest, top.positions[i + 2]);
    }
    // Grown by half a tile each way: 3 wide, so the ridge is 1.5 above the eaves.
    CHECK(std::abs(minX + 0.5f) < 1e-4f && std::abs(lowest - 3.0f) < 1e-4f && std::abs(highest - 4.5f) < 1e-4f);
    CHECK(top.indices.size() % 3 == 0 && !top.indices.empty());
}

// Roofs on two lots: the pieces each roof's top shows, counted by texture cell, are those the game
// shows.
void sceneGridRoofsMatchTheGame() {
    using namespace ts2::scene;
    auto roof = [](float x0, float y0, float x1, float y1, std::int32_t level) {
        ts2::roofs::Roof r;
        r.level0 = r.level1 = level;
        r.x0 = x0;
        r.y0 = y0;
        r.x1 = x1;
        r.y1 = y1;
        r.type = 4;
        return r;
    };
    const auto main = RoofGrid::of(roof(23, 14, 11, 4, 2), 3.75f);
    const auto left = RoofGrid::of(roof(13, 10, 15, 14, 3), 6.75f);
    const auto right = RoofGrid::of(roof(19, 10, 21, 14, 3), 6.75f);
    CHECK(main && left && right);
    CHECK(main->nx == 27 && main->ny == 23 && main->minX == 10.5f && main->minY == 3.5f);
    CHECK(std::abs(main->z(0, 0, 0) - 3.33f) < 1e-4f && std::abs(main->z(1, 0, 0) - 3.83f) < 1e-4f);
    auto pieces = [](const RoofGrid& g, std::vector<const RoofGrid*> others) {
        Surfaces s;
        RoofLook look;
        look.top = 0;
        addRoofGrid(s, g, others, look);
        std::map<int, int> count;
        const auto& p = s[0];
        for (std::size_t t = 0; t + 2 < p.uvs.size() / 2; t += 3) {
            float u = 1, v = 1;
            for (std::size_t k = 0; k < 3; ++k) {
                u = std::min(u, p.uvs[(t + k) * 2]);
                v = std::min(v, p.uvs[(t + k) * 2 + 1]);
            }
            ++count[static_cast<int>(std::lround(v / 0.2f)) * 20 + static_cast<int>(std::lround(u / 0.2f)) * 2];
        }
        return count;
    };
    CHECK((pieces(*main, {&*left, &*right}) == std::map<int, int>{{4, 104}, {20, 32}, {22, 12}, {60, 884}}));
    CHECK((pieces(*right, {&*main, &*left}) == std::map<int, int>{{4, 28}, {20, 8}, {22, 4}, {26, 4}, {42, 2}, {60, 44}}));

    // The Dreamers' house (Pleasantview): a hipped roof over two hipped sheds that meet each other.
    auto ofType = [&](float x0, float y0, float x1, float y1, std::int32_t level, std::uint32_t type) {
        auto r = roof(x0, y0, x1, y1, level);
        r.type = type;
        return r;
    };
    const auto hip = RoofGrid::of(ofType(23, 18, 16, 5, 3, 5), 6.75f);
    const auto shed1 = RoofGrid::of(ofType(25, 5, 23, 15, 2, 2), 3.75f);
    const auto shed2 = RoofGrid::of(ofType(26, 8, 23, 12, 2, 2), 3.75f);
    CHECK(hip && shed1 && shed2);
    CHECK(hip->nx == 17 && hip->ny == 29 && hip->minX == 15.5f && hip->minY == 4.5f);
    CHECK(shed1->nx == 6 && shed1->ny == 23 && shed1->minX == 23 && shed1->minY == 4.5f);
    CHECK(shed2->nx == 8 && shed2->ny == 11 && shed2->minX == 23 && shed2->minY == 7.5f);
    CHECK((pieces(*hip, {&*shed1, &*shed2}) == std::map<int, int>{{0, 32}, {2, 112}, {4, 40}, {60, 672}, {66, 8}, {82, 32}}));
    CHECK((pieces(*shed1, {&*hip, &*shed2}) ==
           std::map<int, int>{{0, 10}, {2, 32}, {4, 12}, {20, 16}, {22, 8}, {26, 4}, {42, 2}, {60, 72}, {66, 4}, {82, 10}}));
    CHECK((pieces(*shed2, {&*hip, &*shed1}) ==
           std::map<int, int>{{0, 10}, {2, 16}, {4, 2}, {22, 8}, {60, 24}, {62, 16}, {64, 4}, {82, 10}}));

    // One roof of each kind on the ground (Cape Cod, rebuilt): a single slope, a gable across its
    // length, and a flat-topped hip meeting a small hip.
    const auto slope = RoofGrid::of(ofType(10, 1, 8, 3, 0, 1), 0);
    const auto across = RoofGrid::of(ofType(10, 5, 8, 7, 0, 3), 0);
    const auto flat = RoofGrid::of(ofType(10, 19, 1, 12, 0, 6), 0);
    const auto small = RoofGrid::of(ofType(9, 9, 7, 11, 0, 5), 0);
    CHECK(slope && across && flat && small);
    CHECK(slope->nx == 7 && slope->ny == 6 && slope->high == 3 && std::abs(slope->z(0, 0, 0) + 0.38f) < 1e-4f);
    CHECK(!across->ridgeAlongX && flat->cap == 7 && flat->peak == 7);
    CHECK((pieces(*slope, {}) == std::map<int, int>{{4, 12}, {60, 48}}));
    CHECK((pieces(*across, {}) == std::map<int, int>{{4, 24}, {60, 48}}));
    CHECK((pieces(*flat, {&*small}) ==
           std::map<int, int>{{0, 28}, {2, 94}, {4, 16}, {20, 2}, {46, 12}, {60, 418}, {62, 2}, {66, 16}, {82, 28}}));
    CHECK((pieces(*small, {&*flat}) == std::map<int, int>{{0, 10}, {2, 28}, {8, 2}, {24, 4}, {44, 2}, {46, 4}, {60, 12}, {82, 10}}));

    // A cone, a dome and an octagonal spire beside it: triangles per part (top, underside, edge
    // board bands) and how high each rises, as in the game.
    auto round = [&](ts2::roofs::Roof r, float slope) {
        r.slope = slope;
        Surfaces s;
        RoofLook look;
        look.top = look.atlas = 0;
        look.under = 1;
        look.edges = 2;
        look.trim = 3;
        CHECK(addRoundRoof(s, r, 0, look, 40, 40));
        std::vector<std::size_t> count;
        float top = -1e9f;
        for (int m = 0; m < 4; ++m) {
            count.push_back(s[m].indices.size() / 3);
            for (std::size_t i = 2; i < s[m].positions.size(); i += 3) {
                top = std::max(top, s[m].positions[i]);
            }
        }
        return std::pair{count, top};
    };
    const auto [cone, coneTop] = round(ofType(6, 11, 3, 9, 0, 7), 1);
    const auto [dome, domeTop] = round(ofType(25, 19, 24, 18, 0, 8), 0.5f);
    const auto [spire, spireTop] = round(ofType(28, 16, 29, 17, 0, 9), 1);
    CHECK((cone == std::vector<std::size_t>{1248, 48, 96, 96}) && std::abs(coneTop - 2.08f) < 1e-4f);
    CHECK((dome == std::vector<std::size_t>{768, 48, 96, 96}) && std::abs(domeTop - (std::sqrt(2.0f) - 0.42f)) < 1e-4f);
    CHECK((spire == std::vector<std::size_t>{528, 8, 32, 32}) && std::abs(spireTop - (2 * std::sqrt(2.0f) - 0.42f)) < 1e-4f);
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

// Cutaway cuts a wall on the storey looked at when a room lies behind it as the camera looks; a
// wall seen edge-on only between two rooms. Walls down cuts every wall that may be cut.
void sceneCutawayCutsWallsInFrontOfRooms() {
    using namespace ts2::scene;
    WallInfo outer;  // along +x; its left side, toward -y, is the room
    outer.x1 = 1;
    outer.level = 1;
    outer.roomLeft = true;
    View view;
    view.walls = WallView::Cutaway;
    view.level = 1;
    view.facing = {0, -1};  // looking from outside toward the room
    CHECK(cutDown(outer, view));
    view.facing = {0, 1};  // from inside toward the outdoors: the back wall stays up
    CHECK(!cutDown(outer, view));
    view.facing = {1, 0};  // edge-on
    CHECK(!cutDown(outer, view));
    WallInfo inner = outer;
    inner.roomRight = true;
    CHECK(cutDown(inner, view));
    view.walls = WallView::Down;
    CHECK(cutDown(outer, view));
    view.level = 2;
    CHECK(!cutDown(outer, view));
    view.level = 1;
    outer.cuttable = false;  // a foundation
    CHECK(!cutDown(outer, view));
    view.walls = WallView::Up;
    CHECK(!cutDown(inner, view));
}

// What a view shows: nothing above its storey, roofs only with walls up on the top storey, a wall's
// cut version in place of the full one, and a window gone with the wall it sits in.
void sceneViewsShowStoreysWallsAndRoofs() {
    using namespace ts2::scene;
    Scene scene;
    WallInfo wall;
    wall.x1 = 1;
    wall.level = 1;
    wall.roomLeft = wall.roomRight = true;
    scene.walls = {wall};
    auto add = [&](Part part, int level, bool cut = false, std::vector<int> walls = {}) {
        Instance i;
        i.mesh = 0;
        i.part = part;
        i.level = level;
        i.cut = cut;
        i.walls = std::move(walls);
        scene.instances.push_back(i);
    };
    add(Part::Other, 0);                   // 0 terrain
    add(Part::Object, 0);                  // 1 in the yard
    add(Part::Floor, 2);                   // 2 upstairs
    add(Part::Roof, 2);                    // 3
    add(Part::Wall, 1, false, {0});        // 4 full
    add(Part::Wall, 1, true, {0});         // 5 cut
    add(Part::Object, 1, false, {0});      // 6 a window in it
    CHECK(topLevel(scene) == 2 && groundLevel(scene) == 1);
    View view;
    view.level = 2;
    CHECK((visibleInstances(scene, view) == std::vector<std::uint8_t>{1, 1, 1, 1, 1, 0, 1}));
    view.level = 1;
    CHECK((visibleInstances(scene, view) == std::vector<std::uint8_t>{1, 1, 0, 0, 1, 0, 1}));
    view.walls = WallView::Down;
    CHECK((visibleInstances(scene, view) == std::vector<std::uint8_t>{1, 1, 0, 0, 0, 1, 0}));
    view.level = 2;
    CHECK((visibleInstances(scene, view) == std::vector<std::uint8_t>{1, 1, 1, 0, 1, 0, 1}));
}

// A version-0x47 SLOT resource: one container slot (type 0, bone 5) and two routing slots (type 3).
void slotResourceCountsContainerSlots() {
    std::vector<std::uint8_t> d(64, 0);
    auto u32 = [&d](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    u32(0x534C4F54);
    u32(0x47);
    u32(0);
    u32(3);
    for (const std::uint8_t type : {std::uint8_t{3}, std::uint8_t{0}, std::uint8_t{3}}) {
        std::vector<std::uint8_t> entry(78, 0);
        entry[0] = type;
        entry[70] = type == 0 ? 5 : 1;
        d.insert(d.end(), entry.begin(), entry.end());
    }
    const auto s = ts2::slot::parse(d);
    CHECK(s.version == 0x47 && s.types.size() == 3 && s.containerCount() == 1);
    CHECK(s.containerBone(0) == 5 && s.containerBone(1) == -1);
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
    u32(1);  // with one effect
    u16(10);
    u16(0xFFFB);
    u16(3);
    u32(0);  // no further motive lists
    u32(static_cast<std::uint32_t>(name.size()));
    d.insert(d.end(), name.begin(), name.end());
    const auto t = ts2::ttab::parse(d);
    CHECK(t.name == name && t.version == 0x55 && t.interactions.size() == 1 && t.tableName == name);
    const auto& in = t.interactions[0];
    CHECK(in.action == 0x2007 && in.test == 0x2008 && in.flags == 0xC45 && in.autonomy == 50 &&
          in.joiningIndex == -1 && in.value3 == 0.5f && in.value5 == 1);
    CHECK(in.motives.size() == 1 && in.motives[0].size() == 1 && in.motives[0][0].delta == -5 &&
          in.motives[0][0].type == 3);
    CHECK(t.extraBytes == 0);
}

// Wants and fears, version 6 with version-10 wants: an extra want, the two lists with their
// maximums, the closing value and a one-group history; bytes after the history are counted.
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
}

// "My slot" / "stack object's slot" (owners 0x14 / 0x10): the object held in a container slot.
void slotOwnersReadTheHeldObject() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, ts2::sim::owner::kMySlot, 1, 1, E),
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kMyData, 0x0B, 2, E),
        expr(owner::kTemp, 1, op::kAssign, ts2::sim::owner::kStackObjectSlot, 0, T, E),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 0, op::kAssign, ts2::sim::owner::kMySlot, 2, T, E),
    }));
    ts2::sim::World world;
    auto& counter = world.create(kObjectGroup, typed(kFridgeGuid), 2);
    auto& plate = world.create(kObjectGroup, typed(kChairGuid));
    counter.slots[1] = plate.id;
    CHECK(counter.slots.size() == 2 && plate.slots.empty());
    Vm vm(code, {}, &world);
    CHECK(vm.run(counter, 0x1000).outcome == Outcome::True);
    CHECK(counter.thread.temps[0] == plate.id && counter.thread.temps[1] == 0);
    const RunResult out = vm.run(counter, 0x1001);
    CHECK(out.outcome == Outcome::Error && out.reason == "slot number out of range");
}

// Search type 0x0C: objects whose object data field 0x3B ("category") equals temp 0.
void setToNextFindsObjectsByCategory() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 0, 1, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 5, 2, E),
        setToNext(0x0C, owner::kStackObjectId, 0, 0, 3, T),
        expr(owner::kTemp, 1, op::kAdd, owner::kLiteral, 1, 2, E),
    }));
    SmallWorld w;
    w.world.find(2)->data[0x3B] = 5;
    w.world.find(3)->data[0x3B] = 7;
    w.world.find(5)->data[0x3B] = 5;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(w.controller->thread.temps[1] == 2);
}

// Search type 0x08 "obj on same tile": the next object on the target's tile, wrapping around and
// ending with the target itself; a target that is not on the lot is false.
void setToNextFindsObjectsOnTheSameTile() {
    using ts2::sim::Tile;
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        setToNext(0x08, owner::kTemp, 0, 0, T, F),
    }));
    SmallWorld w;
    w.world.find(2)->tile = Tile{3, 4, 1};
    w.world.find(5)->tile = Tile{3, 4, 1};
    w.world.find(1)->tile = Tile{3, 4, 1};
    w.world.find(3)->tile = Tile{3, 4, 2};
    w.world.find(6)->tile = Tile{3, 5, 1};
    Vm vm(code, {}, &w.world);
    auto next = [&](std::int16_t from) {
        w.controller->thread.temps[0] = from;
        const RunResult r = vm.run(*w.controller, 0x1000);
        return r.outcome == Outcome::True ? w.controller->thread.temps[0] : std::int16_t{-1};
    };
    CHECK(next(2) == 5);
    CHECK(next(5) == 1);
    CHECK(next(1) == 2);
    CHECK(next(3) == 3);
    CHECK(next(4) == -1 && w.controller->thread.temps[0] == 4);
    CHECK(next(0) == -1);
    w.world.find(5)->data[0x38] = 0x04;
    CHECK(next(2) == 1);
    w.world.find(2)->data[0x2A] = 0x2000;
    w.controller->thread.temps[0] = 2;
    const RunResult refused = vm.run(*w.controller, 0x1000);
    CHECK(refused.outcome == Outcome::Error && refused.reason == "could not find object reference");
}

// Search type 0x03 "part of a multi-tile object": parts share object data 0x13 (the lead's id);
// the next part in falling id order, wrapping around; a single-tile target is false and becomes 0.
void setToNextWalksMultiTileParts() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        setToNext(0x03, owner::kTemp, 0, 0, T, F),
    }));
    SmallWorld w;
    for (std::int16_t id = 1; id <= 7; ++id) {
        w.world.find(id)->data[0x13] = id;
    }
    for (const std::int16_t id : {std::int16_t{2}, std::int16_t{4}, std::int16_t{6}}) {
        w.world.find(id)->definition.fields[ts2::objd::field::kMultiTileMasterId] = 0x55;
        w.world.find(id)->data[0x13] = 4;
    }
    w.world.find(3)->definition.fields[ts2::objd::field::kMultiTileMasterId] = 0x66;
    Vm vm(code, {}, &w.world);
    auto next = [&](std::int16_t from) {
        w.controller->thread.temps[0] = from;
        const RunResult r = vm.run(*w.controller, 0x1000);
        return r.outcome == Outcome::True ? w.controller->thread.temps[0] : std::int16_t{-1};
    };
    CHECK(next(6) == 4);
    CHECK(next(4) == 2);
    CHECK(next(2) == 6);
    CHECK(next(3) == 3);
    CHECK(next(5) == -1 && w.controller->thread.temps[0] == 0);
    CHECK(next(0) == -1);
}

Instruction setMotiveChange(std::uint8_t motive, std::uint8_t flags, std::int16_t perHour, std::int16_t limit,
                            std::uint16_t t) {
    Instruction in;
    in.opcode = 0x001D;
    in.trueTarget = t;
    in.falseTarget = E;
    in.operands[0] = owner::kLiteral;
    in.operands[1] = owner::kLiteral;
    in.operands[2] = motive;
    in.operands[3] = flags;
    in.operands[4] = static_cast<std::uint8_t>(perHour);
    in.operands[5] = static_cast<std::uint8_t>(perHour >> 8);
    in.operands[6] = static_cast<std::uint8_t>(limit);
    in.operands[7] = static_cast<std::uint8_t>(limit >> 8);
    return in;
}

// Set Motive Change adds a (motive, rate per tick, limit) entry; flag bit 0 clears them all;
// motives other than 0, 1, 5-9, 14, 15 are error 0x1A.
void setMotiveChangeKeepsChanges() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        setMotiveChange(5, 0, 900, 80, 1),
        setMotiveChange(5, 0, -1800, -50, T),
    }));
    code.add(kObjectGroup, 0x1001, bhav({setMotiveChange(0, 0x01, 0, 0, T)}));
    code.add(kObjectGroup, 0x1002, bhav({setMotiveChange(2, 0, 10, 10, T)}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& chair = world.create(kObjectGroup, typed(kChairGuid));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(sim.motiveChanges.size() == 2);
    CHECK(sim.motiveChanges[0].motive == 5 && sim.motiveChanges[0].perTick == 0.5f &&
          sim.motiveChanges[0].limit == 80.0f);
    CHECK(sim.motiveChanges[1].perTick == -1.0f && sim.motiveChanges[1].limit == -50.0f);
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::True && sim.motiveChanges.empty());
    const RunResult bad = vm.run(sim, 0x1002);
    CHECK(bad.outcome == Outcome::Error && bad.reason == "motive number out of range");
    CHECK(vm.run(chair, 0x1000).outcome == Outcome::Unsupported);
}

// Search 0x12 takes the type's GUID from temps 0 and 1; an unknown type is false and leaves the
// target alone.
void setToNextObjectOfTypeInTemps() {
    MemoryCodeSource code;
    addSmallWorldTypes(code);
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, static_cast<std::uint16_t>(kChairGuid & 0xFFFF), 1, E),
        expr(owner::kTemp, 1, op::kAssign, owner::kLiteral, static_cast<std::uint16_t>(kChairGuid >> 16), 2, E),
        setToNext(0x12, owner::kLocal, 0, 0, T, F),
    }, 0, 1));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kLocal, 0, op::kAssign, owner::kLiteral, 5, 1, E),
        expr(owner::kTemp, 1, op::kAssign, owner::kLiteral, 0x0BAD, 2, E),
        setToNext(0x12, owner::kLocal, 0, 0, T, 3),
        expr(owner::kTemp, 2, op::kAssign, owner::kLocal, 0, F, E),
    }, 0, 1));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(vm.run(*w.controller, 0x1001).outcome == Outcome::False && w.controller->thread.temps[2] == 5);
}

// Searches 0x05 and 0x2E-0x33 walk neighbor ids by species (person data 0xBA). A neighbor whose
// Sim is on the lot must pass the filters.
void setToNextWalksNeighborsBySpecies() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({setToNext(0x05, owner::kTemp, 0, 0, T, F)}));
    code.add(kObjectGroup, 0x1001, bhav({setToNext(0x2F, owner::kTemp, 0, 0, T, F)}));
    SmallWorld w;
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {3, 8, 12, 20};
    auto neighbor = [&](std::int16_t id, std::uint32_t guid, std::int16_t species) {
        hood->neighbors[id].guid = guid;
        hood->neighbors[id].personData.assign(231, 0);
        hood->neighbors[id].personData[0xBA] = species;
    };
    neighbor(3, 0x0D000003, 0);
    neighbor(8, 0x51515151, 0);  // the person on the lot (object 5)
    neighbor(12, 0x0D00000C, 2);
    neighbor(20, 0x0D000014, 0);
    w.world.setNeighborhood(hood);
    Vm vm(code, {}, &w.world);
    auto next = [&](std::uint16_t tree, std::int16_t from) {
        w.controller->thread.temps[0] = from;
        const RunResult r = vm.run(*w.controller, tree);
        return r.outcome == Outcome::True ? w.controller->thread.temps[0] : std::int16_t{-1};
    };
    CHECK(next(0x1000, 0) == 3);
    CHECK(next(0x1000, 3) == 8);
    w.world.find(5)->data[0x38] = 0x01;  // now its Sim is skipped
    CHECK(next(0x1000, 3) == 20);
    CHECK(next(0x1000, 20) == -1 && w.controller->thread.temps[0] == 0);
    CHECK(next(0x1001, 0) == 12);
}

// Search 0x09: objects beside the object in a local, in the same room, at +x, +y, -x, -y.
void setToNextFindsAdjacentObjects() {
    using ts2::sim::Tile;
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kLocal, 0, op::kAssign, owner::kLiteral, 2, 1, E),
        [] {
            Instruction in = setToNext(0x09, owner::kTemp, 0, 0, T, F);
            in.operands[6] = 0;
            return in;
        }(),
    }, 0, 1));
    SmallWorld w;
    w.world.find(2)->tile = Tile{10, 10, 1};
    w.world.find(4)->tile = Tile{10, 11, 1};  // +y
    w.world.find(6)->tile = Tile{9, 10, 1};   // -x
    w.world.find(7)->tile = Tile{11, 10, 1};  // +x, but another room
    w.world.find(3)->tile = Tile{10, 9, 2};   // -y on another level
    w.world.find(7)->data[0x1D] = 5;
    Vm vm(code, {}, &w.world);
    auto next = [&](std::int16_t from) {
        w.controller->thread.temps[0] = from;
        const RunResult r = vm.run(*w.controller, 0x1000);
        return r.outcome == Outcome::True ? w.controller->thread.temps[0] : std::int16_t{-1};
    };
    CHECK(next(0) == 4);
    CHECK(next(4) == 6);
    CHECK(next(6) == -1 && w.controller->thread.temps[0] == 6);
}

void setToNextVisitsEveryObjectOfAType() {
    CHECK(countWithLoop(setToNext(0x04, owner::kStackObjectId, 0, kFridgeGuid, 0, 0)) == 3);
    CHECK(countWithLoop(setToNext(0x04, owner::kStackObjectId, 0, kChairGuid, 0, 0)) == 1);
    CHECK(countWithLoop(setToNext(0x04, owner::kStackObjectId, 0, 0xDEADBEEF, 0, 0)) == 0);
}

void setToNextSeparatesPeopleFromObjects() {
    CHECK(countWithLoop(setToNext(0x01, owner::kStackObjectId, 0, 0, 0, 0)) == 2);
    CHECK(countWithLoop(setToNext(0x02, owner::kStackObjectId, 0, 0, 0, 0)) == 5);
    CHECK(countWithLoop(setToNext(0x00, owner::kStackObjectId, 0, 0, 0, 0)) == 7);
}

void setToNextCanTargetALocal() {
    MemoryCodeSource code;
    addSmallWorldTypes(code);
    code.add(kObjectGroup, 0x1000, bhav({
        setToNext(0x04, owner::kLocal, 1, kChairGuid, 1, F),
        expr(owner::kTemp, 0, op::kAssign, owner::kLocal, 1, T, E),
    }, 0, 2));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(w.controller->thread.temps[0] == 3);
    CHECK(w.controller->thread.stackObjectId == 0);
}

void setToNextWithoutTargetFlagUsesStackObject() {
    MemoryCodeSource code;
    addSmallWorldTypes(code);
    Instruction search = setToNext(0x04, owner::kLocal, 1, kChairGuid, T, F);
    search.operands[4] &= 0x7F;
    code.add(kObjectGroup, 0x1000, bhav({search}, 0, 2));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(w.controller->thread.stackObjectId == 3);
}

Instruction withFlags(Instruction in, std::uint8_t flags, std::uint16_t field = 0,
                      std::int16_t value = 0) {
    in.operands[8] = flags;
    in.operands[9] = static_cast<std::uint8_t>(field);
    in.operands[10] = static_cast<std::uint8_t>(field >> 8);
    in.operands[11] = static_cast<std::uint8_t>(value);
    in.operands[12] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) >> 8);
    return in;
}

void setToNextSkipsFlaggedObjectsUnlessAsked() {
    const Instruction fridges = setToNext(0x04, owner::kStackObjectId, 0, kFridgeGuid, 0, 0);
    auto flagSecondFridge = [](SmallWorld& w) { w.world.find(4)->data[0x38] = 0x04; };
    auto markThirdFridge = [](SmallWorld& w) { w.world.find(6)->data[0x6B] = 1; };
    CHECK(countWithLoop(fridges, flagSecondFridge) == 2);
    CHECK(countWithLoop(withFlags(fridges, 0x01), flagSecondFridge) == 3);
    CHECK(countWithLoop(fridges, markThirdFridge) == 2);
    CHECK(countWithLoop(withFlags(fridges, 0x04), markThirdFridge) == 3);
}

void setToNextCanRequireADataFieldValue() {
    const Instruction fridges = setToNext(0x04, owner::kStackObjectId, 0, kFridgeGuid, 0, 0);
    auto setField = [](SmallWorld& w) { w.world.find(6)->data[0x0E] = -3; };
    CHECK(countWithLoop(withFlags(fridges, 0x02, 0x0E, -3), setField) == 1);
    CHECK(countWithLoop(withFlags(fridges, 0x02, 0x0E, 0), setField) == 2);
}

void personSearchesFilterBySpecies() {
    auto oneIsAPet = [](SmallWorld& w) { w.world.find(7)->personData[0xBA] = 1; };
    CHECK(countWithLoop(setToNext(0x01, owner::kStackObjectId, 0, 0, 0, 0), oneIsAPet) == 1);
    CHECK(countWithLoop(setToNext(0x28, owner::kStackObjectId, 0, 0, 0, 0), oneIsAPet) == 2);
    CHECK(countWithLoop(setToNext(0x29, owner::kStackObjectId, 0, 0, 0, 0), oneIsAPet) == 1);
    CHECK(countWithLoop(setToNext(0x2B, owner::kStackObjectId, 0, 0, 0, 0), oneIsAPet) == 1);
    CHECK(countWithLoop(setToNext(0x2C, owner::kStackObjectId, 0, 0, 0, 0), oneIsAPet) == 0);
}

void verifySearchesCheckTheTargetWithoutMovingIt() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kParam, 0, 1, E),
        setToNext(0x1A, owner::kStackObjectId, 0, 0, 2, F),
        setToNext(0x1E, owner::kStackObjectId, 0, 0, T, F),
    }, 1));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    const std::int16_t sim[] = {5};
    const std::int16_t fridge[] = {2};
    const std::int16_t missing[] = {42};
    CHECK(vm.run(*w.controller, 0x1000, sim).outcome == Outcome::True);
    CHECK(w.controller->thread.stackObjectId == 5);
    CHECK(vm.run(*w.controller, 0x1000, fridge).outcome == Outcome::False);
    CHECK(w.controller->thread.stackObjectId == 2);
    CHECK(vm.run(*w.controller, 0x1000, missing).outcome == Outcome::False);
    CHECK(w.controller->thread.stackObjectId == 42);
}

// "Neighbor in stack object" (owner 0x18): fields computed from the neighbor whose id is in the
// stack object.
void neighborFieldsComeFromTheNeighbor() {
    constexpr std::uint32_t kNeighborGuid = 0x0A000101;
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kParam, 0, 1, E),
        expr(owner::kTemp, 0, op::kAssign, ts2::sim::owner::kNeighborInStackObject, 0, 2, E),
        expr(owner::kTemp, 1, op::kAssign, ts2::sim::owner::kNeighborInStackObject, 2, 3, E),
        expr(owner::kTemp, 2, op::kAssign, ts2::sim::owner::kNeighborInStackObject, 4, 4, E),
        expr(owner::kTemp, 3, op::kAssign, ts2::sim::owner::kNeighborInStackObject, 11, T, E),
    }, 1));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 14, 1, E),
        expr(owner::kTemp, 0, op::kAssign, ts2::sim::owner::kNeighborInStackObject, 6, T, E),
    }));
    ts2::sim::World world;
    auto& me = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& neighborSim = world.create(kObjectGroup, typed(kNeighborGuid, ts2::sim::kPersonType));
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {14, 15};
    std::vector<std::int16_t> personData(231, 0);
    personData[0x3A] = 19;
    hood->neighbors[14] = {kNeighborGuid, personData};
    hood->neighbors[15] = {0x0A000102, personData};
    world.setNeighborhood(hood);
    Vm vm(code, {}, &world);

    const std::int16_t onLot[] = {14};
    CHECK(vm.run(me, 0x1000, onLot).outcome == Outcome::True);
    CHECK(me.thread.temps[0] == neighborSim.id && me.thread.temps[1] == 19 && me.thread.temps[2] == 0 &&
          me.thread.temps[3] == 0);
    const std::int16_t elsewhere[] = {15};
    CHECK(vm.run(me, 0x1000, elsewhere).outcome == Outcome::True);
    CHECK(me.thread.temps[0] == 0 && me.thread.temps[1] == 19);
    const std::int16_t unknown[] = {99};
    CHECK(vm.run(me, 0x1000, unknown).outcome == Outcome::True);
    CHECK(me.thread.temps[0] == 0 && me.thread.temps[1] == 0);
    const RunResult family = vm.run(me, 0x1001);
    CHECK(family.outcome == Outcome::Unsupported && family.reason == "neighbor field not implemented");
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
    CHECK(f.lot == 23 && f.lot2 == 23 && f.funds == 20000 && f.flags == 9 && f.funds2 == 20000);
    CHECK(f.members.size() == 2 && f.members[1] == 0x0A000102 && f.id7 == 0xE0000301 && f.extraBytes == 0);
}

Instruction primitiveCall(std::uint16_t opcode, std::uint16_t t, std::uint16_t f);

Instruction arrayOp(std::uint8_t op, std::uint8_t whose, std::uint16_t array, std::uint8_t valueOwner,
                    std::uint16_t valueData, std::uint8_t iteratorOwner, std::uint16_t iteratorData,
                    std::uint16_t t, std::uint16_t f) {
    Instruction in = primitiveCall(0x0076, t, f);
    in.operands[1] = op;
    in.operands[2] = whose;
    in.operands[3] = static_cast<std::uint8_t>(array);
    in.operands[4] = static_cast<std::uint8_t>(array >> 8);
    in.operands[5] = valueOwner;
    in.operands[6] = static_cast<std::uint8_t>(valueData);
    in.operands[7] = static_cast<std::uint8_t>(valueData >> 8);
    in.operands[8] = iteratorOwner;
    in.operands[9] = static_cast<std::uint8_t>(iteratorData);
    in.operands[10] = static_cast<std::uint8_t>(iteratorData >> 8);
    return in;
}

// Array Operation on "my" object arrays; a new object has as many arrays as OBJD field 59.
void arrayOperationsWorkOnObjectArrays() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        arrayOp(5, 0, 1, owner::kLiteral, 30, 0, 0, 1, E),                // push back 30
        arrayOp(4, 0, 1, owner::kLiteral, 10, 0, 0, 2, E),                // push front 10
        arrayOp(5, 0, 1, owner::kLiteral, 20, 0, 0, 3, E),                // push back 20: 10 30 20
        arrayOp(13, 0, 1, 0, 0, 0, 0, 4, E),                              // sort ascending: 10 20 30
        arrayOp(1, 0, 1, owner::kTemp, 0, 0, 0, 5, E),                    // size -> temp 0
        expr(owner::kTemp, 1, op::kAssign, owner::kLiteral, 0, 6, E),
        arrayOp(10, 0, 1, owner::kLiteral, 30, owner::kTemp, 1, 7, E),    // find 30 -> temp 1
        arrayOp(12, 0, 1, 0, 0, 0, 0, 8, E),                              // sort descending: 30 20 10
        arrayOp(9, 0, 1, 0, 0, owner::kLiteral, 0, 9, E),                 // remove at 0: 20 10
        arrayOp(10, 0, 1, owner::kLiteral, 99, owner::kTemp, 1, E, T),    // find 99: false
    }));
    code.add(kObjectGroup, 0x1001, bhav({arrayOp(0, 0, 2, 0, 0, 0, 0, T, E)}));          // array 2: none
    code.add(kObjectGroup, 0x1002, bhav({arrayOp(7, 0, 0, 0, 0, 0, 0, T, E)}));          // remove from empty
    code.add(kObjectGroup, 0x1003, bhav({arrayOp(9, 0, 0, 0, 0, owner::kLiteral, 3, T, E)}));
    auto definition = typed(kFridgeGuid);
    definition.fields.resize(60, 0);
    definition.fields[59] = 2;
    ts2::sim::World world;
    auto& a = world.create(kObjectGroup, definition);
    CHECK(a.arrays.size() == 2);
    Vm vm(code, {}, &world);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::True);
    CHECK(a.thread.temps[0] == 3 && a.thread.temps[1] == 2);
    CHECK(a.arrays[1] == (std::vector<std::int16_t>{20, 10}));
    const RunResult bad = vm.run(a, 0x1001);
    CHECK(bad.outcome == Outcome::Error && bad.reason == "bad object array");
    CHECK(vm.run(a, 0x1002).reason == "array iterator does not exist");
    CHECK(vm.run(a, 0x1003).reason == "array iterator does not exist");

    // Owners 0x29 / 0x2B: array 1's iterator, and its element at the iterator (array 1 is 20 10).
    code.add(kObjectGroup, 0x1004, bhav({
        expr(ts2::sim::owner::kMyArrayIterator, 1, op::kAssign, owner::kLiteral, 1, 1, E),
        expr(owner::kTemp, 2, op::kAssign, ts2::sim::owner::kMyArrayAtIterator, 1, 2, E),
        expr(ts2::sim::owner::kMyArrayAtIterator, 1, op::kAssign, owner::kLiteral, 77, T, E),
    }));
    code.add(kObjectGroup, 0x1005, bhav({
        expr(ts2::sim::owner::kMyArrayIterator, 1, op::kAssign, owner::kLiteral, 5, 1, E),
        expr(owner::kTemp, 2, op::kAssign, ts2::sim::owner::kMyArrayAtIterator, 1, T, E),
    }));
    CHECK(vm.run(a, 0x1004).outcome == Outcome::True);
    CHECK(a.thread.temps[2] == 10 && a.arrays[1][1] == 77 && a.arrayIterators[1] == 1);
    CHECK(vm.run(a, 0x1005).reason == "array iterator does not exist");
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

// "Verify Neighbor ID in Stack Object": the stack object holds a neighbor id.
void verifyNeighborChecksTheNeighborhood() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kParam, 0, 1, E),
        setToNext(0x1D, owner::kStackObjectId, 0, 0, T, F),
    }, 1));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    const std::int16_t known[] = {7};
    const std::int16_t unknown[] = {8};
    const RunResult noHood = vm.run(*w.controller, 0x1000, known);
    CHECK(noHood.outcome == Outcome::Unsupported && noHood.reason == "Set to Next needs the neighborhood");
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {1, 7, 300};
    w.world.setNeighborhood(hood);
    CHECK(vm.run(*w.controller, 0x1000, known).outcome == Outcome::True);
    CHECK(vm.run(*w.controller, 0x1000, unknown).outcome == Outcome::False);
    CHECK(w.controller->thread.stackObjectId == 8);
}

void setToNextWrapsAroundLikeHideAll() {
    MemoryCodeSource code;
    addSmallWorldTypes(code);
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kLocal, 0, op::kAssign, owner::kStackObjectId, 0, 1, E),
        expr(owner::kStackObjectAttribute, 0, op::kAdd, owner::kLiteral, 1, 2, E),
        setToNext(0x04, owner::kStackObjectId, 0, kFridgeGuid, 3, 2),
        expr(owner::kStackObjectId, 0, op::kEquals, owner::kLocal, 0, T, 1),
    }, 0, 1));
    SmallWorld w;
    w.controller->thread.stackObjectId = 4;
    Vm vm(code, {1000, 64}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(w.world.find(2)->attributes[0] == 1);
    CHECK(w.world.find(4)->attributes[0] == 1);
    CHECK(w.world.find(6)->attributes[0] == 1);
    CHECK(w.controller->thread.stackObjectId == 4);
}

void testObjectTypeComparesGuids() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 2, 1, E),
        testObjectType(kFridgeGuid, owner::kStackObjectId, 0, 2, F),
        expr(owner::kLocal, 0, op::kAssign, owner::kLiteral, 3, 3, E),
        testObjectType(kFridgeGuid, owner::kLocal, 0, F, T),
    }, 0, 1));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
}

// Flag 0x01 compares the GUID the object's type was cloned from (OBJD fields 0x46-0x47), when
// that type exists, instead of its own.
void testObjectTypeComparesTheOriginalGuid() {
    constexpr std::uint32_t kRecolorGuid = 0x0D0D0D0D;
    auto original = [](std::uint16_t t, std::uint16_t f) {
        Instruction in = testObjectType(kFridgeGuid, owner::kStackObjectId, 0, t, f);
        in.operands[7] = 0x01;
        return in;
    };
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({original(T, F)}));
    code.add(kObjectGroup, 0x1001, bhav({testObjectType(kFridgeGuid, owner::kStackObjectId, 0, T, F)}));
    code.addObjectType(kObjectGroup, typed(kFridgeGuid));
    ts2::sim::World world;
    auto& me = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto recolor = typed(kRecolorGuid);
    recolor.fields[0x46] = static_cast<std::uint16_t>(kFridgeGuid);
    recolor.fields[0x47] = static_cast<std::uint16_t>(kFridgeGuid >> 16);
    auto& fridge = world.create(kObjectGroup, recolor);
    Vm vm(code, {}, &world);
    me.thread.stackObjectId = fridge.id;
    CHECK(vm.run(me, kObjectGroup, 0x1000).outcome == Outcome::True);
    me.thread.stackObjectId = fridge.id;
    CHECK(vm.run(me, kObjectGroup, 0x1001).outcome == Outcome::False);
}

void testObjectTypeOnMissingObjectIsFalse() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({testObjectType(kFridgeGuid, owner::kLiteral, 99, T, F)}));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::False);
}

void testObjectTypeCanAskWhetherAGuidExists() {
    MemoryCodeSource code;
    code.addObjectType(kObjectGroup, typed(kChairGuid));
    Instruction known = testObjectType(kChairGuid, owner::kLiteral, 0, T, F);
    known.operands[7] = 0x04;
    Instruction unknown = testObjectType(0xDEADBEEF, owner::kLiteral, 0, F, T);
    unknown.operands[7] = 0x04;
    code.add(kObjectGroup, 0x1000, bhav({known}));
    code.add(kObjectGroup, 0x1001, bhav({unknown}));
    SmallWorld w;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(static_cast<std::uint16_t>(w.controller->thread.temps[0]) == (kChairGuid & 0xFFFF));
    CHECK(static_cast<std::uint16_t>(w.controller->thread.temps[1]) == (kChairGuid >> 16));
    CHECK(vm.run(*w.controller, 0x1001).outcome == Outcome::True);
}

Instruction primitiveCall(std::uint16_t opcode, std::uint16_t t, std::uint16_t f) {
    Instruction in;
    in.opcode = opcode;
    in.trueTarget = t;
    in.falseTarget = f;
    return in;
}

Instruction sleepFor(std::uint16_t paramIndex, std::uint16_t t) {
    Instruction in = primitiveCall(0x0000, t, E);
    in.operands[0] = static_cast<std::uint8_t>(paramIndex);
    return in;
}

Instruction randomNumber(std::uint8_t destOwner, std::uint16_t destData, std::uint8_t rangeOwner,
                         std::uint16_t rangeData, std::uint16_t t) {
    Instruction in = primitiveCall(0x0008, t, E);
    in.operands[0] = static_cast<std::uint8_t>(destData);
    in.operands[2] = destOwner;
    in.operands[4] = static_cast<std::uint8_t>(rangeData);
    in.operands[5] = static_cast<std::uint8_t>(rangeData >> 8);
    in.operands[6] = rangeOwner;
    in.operands[8] = 1;
    return in;
}

void randomNumberStaysInRange() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({randomNumber(owner::kTemp, 0, owner::kLiteral, 10, T)}));
    Vm vm(code);
    std::array<int, 10> seen{};
    bool inRange = true;
    for (int i = 0; i < 500; ++i) {
        ThreadState t;
        CHECK(vm.run(t, kObjectGroup, 0x1000).outcome == Outcome::True);
        if (t.temps[0] < 0 || t.temps[0] > 9) {
            inRange = false;
        } else {
            ++seen[static_cast<std::size_t>(t.temps[0])];
        }
    }
    CHECK(inRange);
    CHECK(std::all_of(seen.begin(), seen.end(), [](int n) { return n > 0; }));
}

void randomNumberRangeCanComeFromALocal() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kLocal, 1, op::kAssign, owner::kLiteral, 1, 1, E),
        randomNumber(owner::kLocal, 0, owner::kLocal, 1, 2),
        expr(owner::kLocal, 0, op::kEquals, owner::kLiteral, 0, T, F),
    }, 0, 2));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
}

void randomNumberWithZeroRangeWritesNothing() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 5, 1, E),
        randomNumber(owner::kTemp, 0, owner::kLiteral, 0, T),
    }));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
    CHECK(t.temps[0] == 5);
}

Instruction breakPoint(std::uint16_t literalCondition, std::uint8_t flags) {
    Instruction in = primitiveCall(0x000F, T, E);
    in.operands[0] = static_cast<std::uint8_t>(literalCondition);
    in.operands[2] = owner::kLiteral;
    in.operands[4] = flags;
    return in;
}

void breakPointIgnoredInReleaseContinues() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({breakPoint(1, 0x01)}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);

    Instruction unconditional = primitiveCall(0x000F, T, E);
    std::fill_n(unconditional.operands.begin(), 8, std::uint8_t{0xFF});
    code.add(kObjectGroup, 0x1001, bhav({unconditional}));
    CHECK(runPrivate(code, t, 0x1001).outcome == Outcome::True);
}

void breakPointWithFalseConditionContinues() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({breakPoint(0, 0x00)}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
}

void breakPointWithTrueConditionIsAnError() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({breakPoint(1, 0x00)}));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::Error);
    CHECK(r.reason == "break point hit");
}

void expressionIgnoresStaleOperandBytes() {
    MemoryCodeSource code;
    Instruction assign = expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 9, T, E);
    assign.operands[4] = 0xFF;
    for (std::size_t k = 8; k < 16; ++k) {
        assign.operands[k] = static_cast<std::uint8_t>(0xA0 + k);
    }
    code.add(kObjectGroup, 0x1000, bhav({assign}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
    CHECK(t.temps[0] == 9);
}

void sleepYieldsInsideACallAndResumes() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1001, bhav({
        sleepFor(0, 1),
        expr(owner::kTemp, 0, op::kAdd, owner::kLiteral, 1, T, E),
    }, 1));
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 5, 1, E),
        call(0x1001, 2, E, {30}),
        expr(owner::kTemp, 1, op::kAssign, owner::kTemp, 0, T, E),
    }));
    ThreadState t;
    Vm vm(code);
    const RunResult first = vm.run(t, kObjectGroup, 0x1000);
    CHECK(first.outcome == Outcome::Sleeping);
    CHECK(t.sleepTicks == 30);
    CHECK(t.temps[0] == 5);
    CHECK(t.isRunning());
    CHECK(t.frames.size() == 2);

    const RunResult second = vm.resume(t, kObjectGroup);
    CHECK(second.outcome == Outcome::True);
    CHECK(t.temps[1] == 6);
    CHECK(!t.isRunning());
}

void sleepAsLastInstructionCompletesOnResume() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({sleepFor(0, T)}, 1));
    ThreadState t;
    Vm vm(code);
    const std::int16_t args[] = {3};
    CHECK(vm.run(t, kObjectGroup, 0x1000, args).outcome == Outcome::Sleeping);
    CHECK(vm.resume(t, kObjectGroup).outcome == Outcome::True);
    CHECK(!t.isRunning());
}

void runOnABusyThreadIsAnError() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({sleepFor(0, T)}, 1));
    ThreadState t;
    Vm vm(code);
    CHECK(vm.run(t, kObjectGroup, 0x1000).outcome == Outcome::Sleeping);
    CHECK(vm.run(t, kObjectGroup, 0x1000).outcome == Outcome::Error);
}

void resumeWithNothingToRunIsAnError() {
    MemoryCodeSource code;
    ThreadState t;
    Vm vm(code);
    CHECK(vm.resume(t, kObjectGroup).outcome == Outcome::Error);
}

void errorsLeaveTheThreadIdle() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1001, bhav({expr(owner::kTemp, 0, op::kDivide, owner::kLiteral, 0, T, E)}));
    code.add(kObjectGroup, 0x1000, bhav({call(0x1001, T, F)}));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::Error);
    CHECK(r.instance == 0x1001);
    CHECK(!t.isRunning());
}

void personDataBelongsToEachSim() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kMyPersonData, 0x0A, op::kAdd, owner::kLiteral, 100, 1, E),
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kParam, 0, 2, E),
        expr(owner::kStackObjectPersonData, 0x0A, op::kAssign, owner::kMyPersonData, 0x0A, T, E),
    }, 1));
    ts2::sim::World world;
    auto& cook = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& learner = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    const std::int16_t args[] = {learner.id};
    CHECK(vm.run(cook, 0x1000, args).outcome == Outcome::True);
    CHECK(cook.personData[0x0A] == 100);
    CHECK(learner.personData[0x0A] == 100);
}

void personDataByTempUsesATempAsTheIndex() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 1, op::kAssign, owner::kLiteral, 200, 1, E),
        expr(owner::kMyPersonDataByTemp, 1, op::kAssign, owner::kLiteral, 7, T, E),
    }));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(sim.personData[200] == 7);
}

void personDataOnANonPersonIsAnError() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kMyPersonData, 0, T, E),
    }));
    ts2::sim::World world;
    auto& fridge = world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);
    const RunResult r = vm.run(fridge, 0x1000);
    CHECK(r.outcome == Outcome::Error);
    CHECK(r.reason == "person data on an object that is not a person");
}

void interactionRunsObjectCodeInTheSimsThread() {
    constexpr std::uint32_t kSimGroup = 0x7F000099;
    MemoryCodeSource code;
    code.addConstants(kObjectGroup, 0x1002, {4});
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kStackObjectAttribute, 0, op::kSubtract, owner::kConstant, (0x02 << 7) | 0, 1, E),
        expr(owner::kMyPersonData, 0x0A, op::kAdd, owner::kLiteral, 1, T, E),
    }));
    code.add(kObjectGroup, 0x1000, bhav({call(0x1001, T, E)}));

    ts2::sim::World world;
    auto& sim = world.create(kSimGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& fridge = world.create(kObjectGroup, typed(kFridgeGuid));
    fridge.attributes[0] = 20;
    sim.thread.stackObjectId = fridge.id;

    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True);
    CHECK(fridge.attributes[0] == 16);
    CHECK(sim.personData[0x0A] == 1);
    CHECK(fridge.thread.frames.empty());
}

void presentationOnlyGenericCallsContinue() {
    MemoryCodeSource code;
    Instruction centerView = primitiveCall(0x0001, 1, F);
    centerView.operands[0] = 0x01;
    Instruction preloadClothing = primitiveCall(0x0001, T, F);
    preloadClothing.operands[0] = 0x1E;
    code.add(kObjectGroup, 0x1000, bhav({centerView, preloadClothing}));
    ThreadState t;
    CHECK(runPrivate(code, t, 0x1000).outcome == Outcome::True);
}

void otherGenericCallsReportTheirId() {
    MemoryCodeSource code;
    Instruction addToFamily = primitiveCall(0x0001, T, F);
    addToFamily.operands[0] = 0x04;
    code.add(kObjectGroup, 0x1000, bhav({addToFamily}));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::Unsupported);
    CHECK(r.detail == "0x04");
}

Instruction createObject(std::uint32_t guid, std::uint8_t where, std::uint8_t flags,
                         std::uint16_t t, std::uint16_t f) {
    Instruction in = primitiveCall(0x002A, t, f);
    for (std::size_t i = 0; i < 4; ++i) {
        in.operands[i] = static_cast<std::uint8_t>(guid >> (8 * i));
    }
    in.operands[4] = where;
    in.operands[5] = flags;
    return in;
}

constexpr std::uint32_t kLampGroup = 0x7F000077;
constexpr std::uint32_t kLampGuid = 0x1A2B3C4D;

void createObjectMakesItTheStackObject() {
    MemoryCodeSource code;
    code.addObjectType(kLampGroup, typed(kLampGuid));
    code.add(kObjectGroup, 0x1000, bhav({
        createObject(kLampGuid, 0x06, 0x00, 1, F),
        expr(owner::kStackObjectAttribute, 2, op::kAssign, owner::kLiteral, 9, T, E),
    }));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(world.size() == 2);
    const ts2::sim::SimObject* lamp = world.find(sim.thread.stackObjectId);
    CHECK(lamp != nullptr && lamp != &sim);
    CHECK(lamp != nullptr && lamp->group == kLampGroup && lamp->attributes[2] == 9);
}

void createObjectCanTakeItsGuidFromTemps() {
    MemoryCodeSource code;
    code.addObjectType(kLampGroup, typed(kLampGuid));
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, static_cast<std::uint16_t>(kLampGuid), 1, E),
        expr(owner::kTemp, 1, op::kAssign, owner::kLiteral, static_cast<std::uint16_t>(kLampGuid >> 16), 2, E),
        createObject(0, 0x06, 0x80, T, F),
    }));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(world.size() == 2);
}

void createObjectWithUnknownGuidReturnsFalse() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({createObject(0xDEADBEEF, 0x06, 0x00, T, F)}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::False);
    CHECK(world.size() == 1);
}

void createObjectRefusesTemplates() {
    MemoryCodeSource code;
    code.addObjectType(kLampGroup, typed(kLampGuid, 0x10));
    code.add(kObjectGroup, 0x1000, bhav({createObject(kLampGuid, 0x06, 0x00, T, F)}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::Error);
    CHECK(world.size() == 1);
}

void createObjectRejectsTwoPassingValues() {
    MemoryCodeSource code;
    code.addObjectType(kLampGroup, typed(kLampGuid));
    code.add(kObjectGroup, 0x1000, bhav({createObject(kLampGuid, 0x06, 0x12, T, F)}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::Error);
}

Behavior named(const char* name, std::vector<Instruction> code, std::uint8_t args = 0,
               std::uint8_t locals = 0) {
    Behavior b = bhav(std::move(code), args, locals);
    b.name = name;
    return b;
}

Instruction runTreeByName(std::uint8_t nameIndex, std::uint8_t flags, std::uint8_t how,
                          std::uint16_t t, std::uint16_t f) {
    Instruction in = primitiveCall(0x001C, t, f);
    in.operands[2] = flags;
    in.operands[4] = nameIndex;
    in.operands[5] = how;
    return in;
}

constexpr std::uint32_t kSimGroup = 0x7F000099;

struct NamedTreeSetup {
    MemoryCodeSource code;
    ts2::sim::World world;
    ts2::sim::SimObject* sim;
    ts2::sim::SimObject* fridge;
    NamedTreeSetup() {
        code.addStrings(kSimGroup, 0x12F, {"Restock", "Missing Tree"});
        code.addConstants(kObjectGroup, 0x1002, {10});
        code.addConstants(kSimGroup, 0x1002, {1000});
        code.add(kObjectGroup, 0x1005, named("Restock", {
            expr(owner::kStackObjectAttribute, 0, op::kAdd, owner::kParam, 0, 1, E),
            expr(owner::kStackObjectAttribute, 0, op::kAdd, owner::kConstant, (0x02 << 7) | 0, T, E),
        }, 1));
        sim = &world.create(kSimGroup, typed(0x51515151, ts2::sim::kPersonType));
        fridge = &world.create(kObjectGroup, typed(kFridgeGuid));
        sim->thread.stackObjectId = fridge->id;
    }
};

void runTreeByNameRunsTheStackObjectsTree() {
    NamedTreeSetup s;
    Instruction call = runTreeByName(0, 0x10, 0x00, 1, F);
    call.operands[6] = owner::kLiteral;
    call.operands[7] = 3;
    s.code.add(kSimGroup, 0x1000, named("Caller", {
        call,
        expr(owner::kTemp, 0, op::kAssign, owner::kConstant, (0x02 << 7) | 0, T, E),
    }));
    Vm vm(s.code, {}, &s.world);
    CHECK(vm.run(*s.sim, 0x1000).outcome == Outcome::True);
    CHECK(s.fridge->attributes[0] == 13);
    CHECK(s.sim->thread.temps[0] == 1000);
}

void runTreeByNameTakesFalseWhenTheTreeIsMissing() {
    NamedTreeSetup s;
    s.code.add(kSimGroup, 0x1000, named("Caller", {runTreeByName(1, 0x00, 0x00, T, F)}));
    Vm vm(s.code, {}, &s.world);
    CHECK(vm.run(*s.sim, 0x1000).outcome == Outcome::False);
}

void runTreeByNameCanPassTheCallersParams() {
    NamedTreeSetup s;
    s.code.add(kSimGroup, 0x1000, named("Caller", {runTreeByName(0, 0x20, 0x00, T, F)}, 1));
    Vm vm(s.code, {}, &s.world);
    const std::int16_t args[] = {5};
    CHECK(vm.run(*s.sim, 0x1000, args).outcome == Outcome::True);
    CHECK(s.fridge->attributes[0] == 15);
}

void runTreeByNameOtherModesReportThemselves() {
    NamedTreeSetup s;
    s.code.add(kSimGroup, 0x1000, named("Caller", {runTreeByName(0, 0x00, 0x01, T, F)}));
    Vm vm(s.code, {}, &s.world);
    const RunResult r = vm.run(*s.sim, 0x1000);
    CHECK(r.outcome == Outcome::Unsupported);
    CHECK(r.reason == "Run Tree by Name mode 0x01 not implemented");
}

// Mode 2 pushes the stack object's tree onto my stack like mode 0; mode 3 runs a tree of the
// object type whose GUID is in temps 0 and 1, with the stack object as its stack object.
void runTreeByNamePushesOntoMyStack() {
    NamedTreeSetup s;
    s.code.addObjectType(kObjectGroup, typed(kFridgeGuid));
    Instruction push = runTreeByName(0, 0x10, 0x02, 1, F);
    push.operands[6] = owner::kLiteral;
    push.operands[7] = 4;
    s.code.add(kSimGroup, 0x1000, named("Caller", {push, expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 1, T, E)}));
    s.code.add(kSimGroup, 0x1001, named("Typed caller", {
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, static_cast<std::uint16_t>(kFridgeGuid & 0xFFFF), 1, E),
        expr(owner::kTemp, 1, op::kAssign, owner::kLiteral, static_cast<std::uint16_t>(kFridgeGuid >> 16), 2, E),
        [] {
            Instruction in = runTreeByName(0, 0x10, 0x03, T, F);
            in.operands[6] = owner::kLiteral;
            in.operands[7] = 2;
            return in;
        }(),
    }));
    s.code.add(kSimGroup, 0x1002, named("Bad GUID", {
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 0x1234, 1, E),
        runTreeByName(0, 0x10, 0x03, T, F),
    }));
    Vm vm(s.code, {}, &s.world);
    CHECK(vm.run(*s.sim, 0x1000).outcome == Outcome::True);
    CHECK(s.fridge->attributes[0] == 14 && s.sim->thread.temps[0] == 1);
    CHECK(vm.run(*s.sim, 0x1001).outcome == Outcome::True);
    CHECK(s.fridge->attributes[0] == 26);
    const RunResult bad = vm.run(*s.sim, 0x1002);
    CHECK(bad.outcome == Outcome::Error && bad.reason == "you sent me a crappy GUID, please fix it");
}

Instruction relationship(std::uint8_t variable, std::uint8_t flags, std::uint8_t aOwner,
                         std::uint16_t aData, std::uint8_t bOwner, std::uint16_t bData,
                         std::uint8_t valueOwner, std::uint16_t valueData, std::uint16_t t,
                         std::uint16_t f) {
    Instruction in = primitiveCall(0x001A, t, f);
    in.operands[0] = variable;
    in.operands[1] = flags;
    in.operands[2] = aOwner;
    in.operands[3] = static_cast<std::uint8_t>(aData);
    in.operands[4] = static_cast<std::uint8_t>(aData >> 8);
    in.operands[5] = bOwner;
    in.operands[6] = static_cast<std::uint8_t>(bData);
    in.operands[7] = static_cast<std::uint8_t>(bData >> 8);
    in.operands[8] = valueOwner;
    in.operands[9] = static_cast<std::uint8_t>(valueData);
    in.operands[10] = static_cast<std::uint8_t>(valueData >> 8);
    return in;
}

void relationshipWritesAndReadsBack() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        relationship(2, 0x04, owner::kLiteral, 1, owner::kLiteral, 2, owner::kLiteral, 75, 1, E),
        relationship(2, 0x01, owner::kLiteral, 1, owner::kLiteral, 2, owner::kTemp, 0, 2, E),
        relationship(2, 0x01, owner::kLiteral, 2, owner::kLiteral, 1, owner::kTemp, 1, E, T),
    }));
    ts2::sim::World world;
    auto& a = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::True);
    CHECK(a.thread.temps[0] == 75);
    CHECK(world.relationship(1, 2).size() == 3);
    CHECK(world.relationship(2, 1).empty());
}

void relationshipReadCreatesTheVariableUnlessAsked() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 9, 1, E),
        relationship(4, 0x00, owner::kLiteral, 1, owner::kLiteral, 2, owner::kTemp, 0, T, E),
    }));
    ts2::sim::World world;
    auto& a = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::True);
    CHECK(a.thread.temps[0] == 0);
    CHECK(world.relationship(1, 2).size() == 5);
}

void relationshipWithMissingObjectIsAnError() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        relationship(0, 0x00, owner::kLiteral, 1, owner::kLiteral, 9, owner::kTemp, 0, T, F),
    }));
    ts2::sim::World world;
    auto& a = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::Error);
}

// With flag 0x02, A and B are neighbor ids and the variables are A's saved list towards B.
void relationshipWithNeighborIdsUsesTheNeighborhood() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        relationship(2, 0x02, owner::kLiteral, 1, owner::kLiteral, 9, owner::kTemp, 0, T, F),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        relationship(0, 0x06, owner::kLiteral, 9, owner::kLiteral, 1, owner::kLiteral, 55, 1, F),
        relationship(0, 0x02, owner::kLiteral, 9, owner::kLiteral, 1, owner::kTemp, 1, T, F),
    }));
    code.add(kObjectGroup, 0x1002, bhav({
        relationship(0, 0x02, owner::kLiteral, 1, owner::kLiteral, 4, owner::kTemp, 0, T, F),
    }));
    ts2::sim::World world;
    auto& a = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::Unsupported);

    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {1, 9};
    hood->relationships[{std::int16_t{1}, std::int16_t{9}}] = {10, 0, 42};
    world.setNeighborhood(hood);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::True);
    CHECK(a.thread.temps[0] == 42);
    CHECK(vm.run(a, 0x1001).outcome == Outcome::True);  // 9 -> 1 had no list: it grows
    CHECK(a.thread.temps[1] == 55);
    CHECK(hood->relationships.size() == 1);              // the saved neighborhood is not changed
    const RunResult missing = vm.run(a, 0x1002);
    CHECK(missing.outcome == Outcome::Error && missing.reason == "neighbor not found");
}

// Owner 0x26 "neighbor's object definition": a field of the object type of the neighbor in the
// stack object.
void neighborObjectDefinitionReads() {
    constexpr std::uint32_t kNeighborGuid = 0x0A000101;
    MemoryCodeSource code;
    auto definition = typed(kNeighborGuid, ts2::sim::kPersonType);
    definition.fields[0x2F] = 42;
    code.addObjectType(kObjectGroup, definition);
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 14, 1, E),
        expr(owner::kTemp, 0, op::kAssign, ts2::sim::owner::kNeighborObjectDefinition, 0x2F, T, E),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 14, 1, E),
        expr(owner::kTemp, 0, op::kAssign, ts2::sim::owner::kNeighborObjectDefinition, 500, T, E),
    }));
    ts2::sim::World world;
    auto& a = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {14};
    hood->neighbors[14].guid = kNeighborGuid;
    world.setNeighborhood(hood);
    Vm vm(code, {}, &world);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::True && a.thread.temps[0] == 42);
    const RunResult bad = vm.run(a, 0x1001);
    CHECK(bad.outcome == Outcome::Error && bad.reason == "index into object definition out of range");
}

// Get Direction To: from the object in temp 1 to the stack object, as 8 directions or in degrees.
void getDirectionToMeasuresTowardsStackObject() {
    auto direction = [](bool degrees) {
        Instruction in;
        in.opcode = 0x000C;
        in.trueTarget = T;
        in.falseTarget = E;
        in.operands[0] = 0;  // result: temp 0
        in.operands[2] = owner::kTemp;
        in.operands[4] = 0x01;
        in.operands[5] = owner::kTemp;
        in.operands[6] = 1;
        in.operands[8] = degrees ? 0x02 : 0x00;
        return in;
    };
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({direction(false)}));
    code.add(kObjectGroup, 0x1001, bhav({direction(true)}));
    SmallWorld w;
    auto place = [&](std::int16_t id, float x, float y) {
        w.world.find(id)->x = static_cast<std::int32_t>(x * 65536);
        w.world.find(id)->y = static_cast<std::int32_t>(y * 65536);
    };
    place(2, 10.5f, 10.5f);
    Vm vm(code, {}, &w.world);
    auto measure = [&](std::uint16_t tree, float x, float y) {
        place(3, x, y);
        w.controller->thread.stackObjectId = 3;
        w.controller->thread.temps[1] = 2;
        CHECK(vm.run(*w.controller, tree).outcome == Outcome::True);
        return w.controller->thread.temps[0];
    };
    CHECK(measure(0x1000, 12.5f, 10.5f) == 2);
    CHECK(measure(0x1000, 10.5f, 12.5f) == 4);
    CHECK(measure(0x1000, 8.5f, 8.5f) == 7);
    CHECK(measure(0x1000, 10.5f, 8.5f) == 0);
    CHECK(measure(0x1001, 12.5f, 10.5f) == 90);
    CHECK(measure(0x1001, 10.5f, 12.5f) == 180);
    CHECK(measure(0x1001, 8.5f, 8.5f) == 315);
}

// Get Distance To: from the object in temp 1 to the stack object, in tiles or hundredths; a level
// counts as 10 tiles.
void getDistanceToMeasuresInTiles() {
    auto distance = [](bool hundredths) {
        Instruction in;
        in.opcode = 0x000B;
        in.trueTarget = T;
        in.falseTarget = E;
        in.operands[0] = 0;  // into temp 0
        in.operands[2] = 0x01;
        in.operands[3] = owner::kTemp;
        in.operands[4] = 1;
        in.operands[6] = hundredths ? 0x02 : 0x00;
        return in;
    };
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({distance(false)}));
    code.add(kObjectGroup, 0x1001, bhav({distance(true)}));
    SmallWorld w;
    auto place = [&](std::int16_t id, float x, float y, std::int32_t level) {
        auto* o = w.world.find(id);
        o->x = static_cast<std::int32_t>(x * 65536);
        o->y = static_cast<std::int32_t>(y * 65536);
        o->level = level;
    };
    place(2, 10.5f, 10.5f, 1);
    place(3, 13.5f, 14.5f, 1);
    place(4, 10.5f, 10.5f, 2);
    Vm vm(code, {}, &w.world);
    auto measure = [&](std::uint16_t tree, std::int16_t to) {
        w.controller->thread.stackObjectId = to;
        w.controller->thread.temps[1] = 2;
        CHECK(vm.run(*w.controller, tree).outcome == Outcome::True);
        return w.controller->thread.temps[0];
    };
    CHECK(measure(0x1000, 3) == 5);
    CHECK(measure(0x1001, 3) == 500);
    CHECK(measure(0x1000, 4) == 10);
}

Instruction pushInteraction(std::uint8_t index, std::uint8_t objectTemp, std::uint8_t priority,
                            std::uint16_t t, std::uint16_t f) {
    Instruction in;
    in.opcode = 0x000D;
    in.trueTarget = t;
    in.falseTarget = f;
    in.operands[0] = index;
    in.operands[1] = objectTemp;
    in.operands[2] = priority;
    return in;
}

// Push Interaction queues interaction N of the object in a temp onto the stack object (a Sim),
// after its test tree agrees; failures leave a reason in my object data 0x34.
void pushInteractionQueuesForTheSim() {
    MemoryCodeSource code;
    auto fridgeType = typed(kFridgeGuid);
    fridgeType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    ts2::ttab::InteractionTable table;
    table.interactions.resize(2);
    table.interactions[0].action = 0x1000;
    table.interactions[1].action = 0x1001;
    table.interactions[1].test = 0x1002;
    code.addInteractionTable(kObjectGroup, 0x80, table);
    code.add(kObjectGroup, 0x1002, bhav({expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 0, F, F)}));
    code.add(kObjectGroup, 0x1100, bhav({pushInteraction(0, 1, 1, T, F)}));
    code.add(kObjectGroup, 0x1101, bhav({pushInteraction(1, 1, 3, T, F)}));
    code.add(kObjectGroup, 0x1102, bhav({pushInteraction(0, 1, 0, T, F)}));
    code.add(kObjectGroup, 0x1103, bhav({pushInteraction(5, 1, 1, T, F)}));

    ts2::sim::World world;
    auto& controller = world.create(kObjectGroup, typed(0x0BADF00D));
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& fridge = world.create(kObjectGroup, fridgeType);
    controller.thread.stackObjectId = sim.id;
    controller.thread.temps[1] = fridge.id;
    Vm vm(code, {}, &world);
    CHECK(vm.run(controller, 0x1100).outcome == Outcome::True);
    CHECK(sim.queue.size() == 1 && sim.queue[0].object == fridge.id && sim.queue[0].action == 0x1000 &&
          sim.queue[0].priority == 100);
    CHECK(vm.run(controller, 0x1101).outcome == Outcome::False && controller.data[0x34] == 2);
    const RunResult missing = vm.run(controller, 0x1103);
    CHECK(missing.outcome == Outcome::Error && missing.reason == "reference to tree table entry that does not exist");

    // A Sim pushing with its own priority needs one.
    sim.thread.stackObjectId = sim.id;
    sim.thread.temps[1] = fridge.id;
    CHECK(vm.run(sim, kObjectGroup, 0x1102).outcome == Outcome::False && sim.data[0x34] == 1);
    sim.personData[0x21] = 40;
    CHECK(vm.run(sim, kObjectGroup, 0x1102).outcome == Outcome::True && sim.queue.back().priority == 40);

    for (int i = 0; i < 5; ++i) {
        CHECK(vm.run(controller, 0x1100).outcome == Outcome::True);
    }
    CHECK(sim.queue.size() == 7);
    CHECK(vm.run(controller, 0x1100).outcome == Outcome::False && controller.data[0x34] == 3);
}

Instruction idleForInput(std::uint16_t counterParam, bool mayStart, bool immediate, std::uint16_t t,
                         std::uint16_t f) {
    Instruction in;
    in.opcode = 0x0011;
    in.trueTarget = t;
    in.falseTarget = f;
    in.operands[0] = static_cast<std::uint8_t>(counterParam);
    in.operands[2] = mayStart ? 1 : 0;
    in.operands[4] = immediate ? 1 : 0;
    return in;
}

// Idle for Input: waits, counting a parameter down each tick, and starts the first queued
// interaction (its action tree, with the object as stack object) when one is waiting.
void idleForInputStartsQueuedInteractions() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectAttribute, 0, op::kAdd, owner::kLiteral, 7, F, E),  // the interaction
    }));
    code.add(kObjectGroup, 0x1100, bhav({
        idleForInput(0, true, false, 1, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 1, T, E),
    }, 1));
    code.add(kObjectGroup, 0x1101, bhav({idleForInput(0, false, true, T, E)}));
    code.add(kObjectGroup, 0x1102, bhav({idleForInput(0, false, false, T, E)}, 1));

    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& fridge = world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);

    // Nothing queued: waits as many ticks as the parameter says, then goes on.
    const std::array<std::int16_t, 1> twoTicks{2};
    CHECK(vm.run(sim, kObjectGroup, 0x1100, twoTicks).outcome == Outcome::Sleeping);
    CHECK(vm.resume(sim).outcome == Outcome::Sleeping);
    CHECK(vm.resume(sim).outcome == Outcome::True && sim.thread.temps[0] == 1);

    // Something queued: it starts, and the caller goes on afterwards even though it returned false.
    ts2::sim::QueuedInteraction q;
    q.object = fridge.id;
    q.group = kObjectGroup;
    q.action = 0x1000;
    sim.queue.push_back(q);
    sim.thread.temps[0] = 0;
    CHECK(vm.run(sim, kObjectGroup, 0x1100, twoTicks).outcome == Outcome::True);
    CHECK(fridge.attributes[0] == 7 && sim.queue.empty() && sim.thread.temps[0] == 1);
    CHECK((sim.data[8] & 0x40) != 0);

    // Immediate mode with nothing queued is just true; without "may start" a waiting one stays.
    CHECK(vm.run(sim, kObjectGroup, 0x1101).outcome == Outcome::True);
    sim.queue.push_back(q);
    CHECK(vm.run(sim, kObjectGroup, 0x1102, twoTicks).outcome == Outcome::True && sim.queue.size() == 1);
}

// Animate Sim: plays the named animation from the object's list (else the global one) and waits
// until it has played by the world's clock, at its speed; Animate Object, Overlay and Stop go on at once.
void animateSimWaitsForItsAnimation() {
    auto animate = [](std::uint16_t entry, std::uint8_t list, std::uint8_t speed) {
        Instruction in;
        in.opcode = 0x006A;
        in.trueTarget = T;
        in.falseTarget = E;
        in.operands[0] = static_cast<std::uint8_t>(entry);
        in.operands[3] = speed;
        in.operands[6] = list;
        return in;
    };
    MemoryCodeSource code;
    code.addStrings(kObjectGroup, 0x81, {"", "a2o-shower-getOut-E"});
    code.addStrings(ts2::sim::kGlobalGroup, 0x81, {"a-noExpression", "a-blink", "a-idle-neutral-armsSide-wtC-breatheLoop"});
    Instruction getOut = animate(1, 0x81, 32);
    getOut.trueTarget = 1;
    code.add(kObjectGroup, 0x1000, bhav({
        getOut,
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 1, T, E),
    }));
    code.add(kObjectGroup, 0x1001, bhav({animate(2, 0x81, 64)}));
    Instruction overlay;
    overlay.opcode = 0x006B;
    overlay.trueTarget = T;
    overlay.falseTarget = E;
    code.add(kObjectGroup, 0x1002, bhav({overlay}));

    ts2::sim::World world;
    world.setAnimationLengths([](const std::string& name) -> std::optional<float> {
        if (name == "a2o-shower-getOut-E") return 1966.0f;
        if (name == "a-idle-neutral-armsSide-wtC-breatheLoop") return 3166.0f;
        return std::nullopt;
    });
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);

    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::Sleeping);
    CHECK(sim.animation && sim.animation->name == "a2o-shower-getOut-E");
    world.advanceClock(1000);
    CHECK(vm.resume(sim).outcome == Outcome::Sleeping && sim.thread.temps[0] == 0);
    world.advanceClock(1000);
    CHECK(vm.resume(sim).outcome == Outcome::True && sim.thread.temps[0] == 1 && !sim.animation);

    // From the global list, at double speed: half as long.
    CHECK(vm.run(sim, kObjectGroup, 0x1001).outcome == Outcome::Sleeping);
    CHECK(sim.animation && sim.animation->name == "a-idle-neutral-armsSide-wtC-breatheLoop");
    world.advanceClock(1600);
    CHECK(vm.resume(sim).outcome == Outcome::True);

    CHECK(vm.run(sim, kObjectGroup, 0x1002).outcome == Outcome::True);
}

// Find Best Object for Function: of the objects serving the function (their OBJf entry), the one
// whose rating, halved every 5 tiles away, is highest and whose check tree passes becomes the stack
// object; the room flags narrow the search.
void findBestObjectPicksTheBestRatedNearest() {
    constexpr std::uint32_t kPickyGroup = 0x7F000003;
    constexpr std::uint16_t kSit = 0x1A;  // the OBJf entry for "sit", rated by object data 0x1A
    auto find = [](std::uint8_t flags) {
        Instruction in;
        in.opcode = 0x000E;
        in.trueTarget = T;
        in.falseTarget = F;
        in.operands[0] = 0x08;  // sit
        in.operands[2] = flags;
        return in;
    };
    std::vector<ts2::objf::Function> sits(0x1B);
    sits[kSit].action = 0x1001;
    std::vector<ts2::objf::Function> pickySits = sits;
    pickySits[kSit].check = 0x1000;

    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({find(0)}));
    code.add(kObjectGroup, 0x1001, bhav({find(0x04)}));
    code.add(kPickyGroup, 0x1000, bhav({expr(owner::kLiteral, 0, op::kEquals, owner::kLiteral, 1, T, F)}));  // fails
    code.addObjectType(kObjectGroup, typed(kChairGuid));
    code.addObjectType(kPickyGroup, typed(0x0C0C0C0C));
    code.addFunctions(kObjectGroup, sits);
    code.addFunctions(kPickyGroup, pickySits);

    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto place = [](ts2::sim::SimObject& o, int x, int y, std::int16_t rating, std::int16_t room) {
        o.x = x * 65536;
        o.y = y * 65536;
        o.data[0x1A] = rating;
        o.data[0x1D] = room;
    };
    place(sim, 0, 0, 0, 1);
    auto& farGood = world.create(kObjectGroup, typed(kChairGuid));
    place(farGood, 10, 0, 100, 1);  // 100 / 3 = 33.3
    auto& nearPlain = world.create(kObjectGroup, typed(kChairGuid));
    place(nearPlain, 0, 0, 40, 2);  // 40, but in another room
    auto& unrated = world.create(kObjectGroup, typed(kChairGuid));
    place(unrated, 1, 0, 0, 1);
    auto& picky = world.create(kPickyGroup, typed(0x0C0C0C0C));
    place(picky, 1, 0, 100, 1);  // the best, but its check tree fails
    Vm vm(code, {}, &world);

    ThreadState& t = sim.thread;
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True && t.stackObjectId == nearPlain.id);
    CHECK(vm.run(sim, kObjectGroup, 0x1001).outcome == Outcome::True && t.stackObjectId == farGood.id);
    CHECK(sim.data[0x42] == 100);  // the function score of the last object rated

    // Nothing serves a function no object has.
    sits[kSit] = {};
    code.addFunctions(kObjectGroup, sits);
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::False);
}

// Run Functional Tree: calls the stack object's action tree for the function (its OBJf entry), with
// the object as stack object and its group's code; false when it has none.
void runFunctionalTreeCallsTheObjectsTree() {
    auto run = [](std::uint16_t function) {
        Instruction in;
        in.opcode = 0x0014;
        in.trueTarget = T;
        in.falseTarget = F;
        in.operands[0] = static_cast<std::uint8_t>(function);
        return in;
    };
    std::vector<ts2::objf::Function> functions(0x1B);
    functions[0x1A].action = 0x1001;  // sit
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({run(8)}));
    code.add(kObjectGroup, 0x1002, bhav({run(0)}));  // prepare food: the chair has none
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kStackObjectAttribute, 0, op::kAssign, owner::kLiteral, 9, T, E),
    }));
    code.addObjectType(kObjectGroup, typed(kChairGuid));
    code.addFunctions(kObjectGroup, functions);

    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& chair = world.create(kObjectGroup, typed(kChairGuid));
    chair.attributes.fill(0);
    Vm vm(code, {}, &world);
    sim.thread.stackObjectId = chair.id;
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True && chair.attributes[0] == 9);
    sim.thread.stackObjectId = chair.id;
    CHECK(vm.run(sim, kObjectGroup, 0x1002).outcome == Outcome::False);
}

// The Sim clock: two seconds a tick, carried into minutes, hours and the date; shown with the
// date's weekday, as the game shows it.
// The route grid splits leaves along an item's edges down to an eighth of a tile, and a leaf's passage
// follows its items: floor walks, walls block, joints cost, a doorway's solid square overrides a wall.
// Floor route items: one tile per floored tile with roads costing more, bare ground on the ground
// level, a foundation under raised floors, and strips along floor edges.
// A fence post routes as its post's footprint around the post's point (route axes swap the wall
// graph's), padded by an eighth of a tile; fences that don't stop Sims give nothing.
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

    // Types and costs, with the live flags seen in the game.
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
    // A 3 x 2 lot (route tiles) with two levels; the floor grid is indexed (level, y, x) in route terms.
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

    const auto ground = floorRouteItems(floors, patterns, 0, 3, 2);
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

    const auto upper = floorRouteItems(floors, patterns, 1, 3, 2);
    CHECK(has(upper, {2, 1, 3, 2}, 0, 1));
    CHECK(has(upper, {2, 1, 2.125f, 2}, 2, 1));      // edges all round the lone tile
    CHECK(upper.size() == 5);
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
    // and a primitive; the semi-global calls Init back and a tree that doesn't exist.
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
    resetObject(object, type, code);
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
    ts2::sim::advanceClock(g);
    CHECK(g[gl::kSecond] == 0 && g[gl::kMinute] == 23 && ts2::sim::clockText(g) == "Mon 3:23 pm");
    g[gl::kHour] = 23;
    g[gl::kMinute] = 59;
    g[gl::kSecond] = 58;
    g[gl::kDayOfMonth] = 30;
    ts2::sim::advanceClock(g);
    CHECK(g[gl::kHour] == 0 && g[gl::kDayOfMonth] == 1 && g[gl::kMonth] == 7 && ts2::sim::clockText(g) == "Tue 12:00 am");
    g[gl::kBudgetLow] = 968;
    g[gl::kBudgetHigh] = 2;
    CHECK(ts2::sim::funds(g) == 20968);
}

// Add/Change the Action String: the text from table 0x12E (own group, then semi-global, then
// global), or an icon; a missing text is an error.
void actionStringSetsTextAndIcon() {
    auto text = [](std::uint16_t index, std::uint8_t flags) {
        Instruction in;
        in.opcode = 0x0032;
        in.trueTarget = T;
        in.falseTarget = E;
        in.operands[2] = flags;
        in.operands[14] = static_cast<std::uint8_t>(index);
        return in;
    };
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x12E, {"Eat", "Sleep"});
    code.addStrings(kObjectGroup, 0x12E, {"", "Snack"});
    code.add(kObjectGroup, 0x1000, bhav({text(1, 0)}));
    code.add(kObjectGroup, 0x1001, bhav({text(0, 0)}));
    code.add(kObjectGroup, 0x1002, bhav({text(1, 0x04)}));
    code.add(kObjectGroup, 0x1003, bhav({text(7, 0)}));
    code.add(kObjectGroup, 0x1004, bhav({[] {
        Instruction in;
        in.opcode = 0x0032;
        in.trueTarget = T;
        in.falseTarget = E;
        in.operands[9] = 1;
        in.operands[11] = owner::kLiteral;
        in.operands[12] = 2;
        return in;
    }()}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True && sim.actionString == "Snack");
    CHECK(vm.run(sim, kObjectGroup, 0x1001).outcome == Outcome::True && sim.actionString == "Eat");
    CHECK(vm.run(sim, kObjectGroup, 0x1002).outcome == Outcome::True && sim.actionString == "Sleep");
    const RunResult missing = vm.run(sim, kObjectGroup, 0x1003);
    CHECK(missing.outcome == Outcome::Error && missing.reason == "missing action string in table");
    CHECK(vm.run(sim, kObjectGroup, 0x1004).outcome == Outcome::True && sim.actionIconObject == 2);
}

// Compiles Lua source into a chunk, as a stand-in for the game's compiled scripts.
ts2::luascript::Script compileScript(const std::string& name, const std::string& source) {
    lua_State* L = lua_open();
    ts2::luascript::Script script;
    script.name = name;
    if (luaL_loadbuffer(L, source.data(), source.size(), name.c_str()) == 0) {
        lua_dump(L, [](lua_State*, const void* p, size_t size, void* out) {
            auto* bytes = static_cast<std::vector<std::uint8_t>*>(out);
            bytes->insert(bytes->end(), static_cast<const std::uint8_t*>(p), static_cast<const std::uint8_t*>(p) + size);
            return 0;
        }, &script.chunk);
    }
    lua_close(L);
    return script;
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

// Primitive 0x7E runs a Lua script named in string table 0x130; the script reads the
// primitive's parameters and temps and sets the primitive's result.
void luaScriptPrimitiveRunsScripts() {
    auto runtime = std::make_shared<ts2::sim::LuaRuntime>();
    runtime->load({compileScript("Module", "Offset = 10")},
                  {compileScript("AddToTemp", "SetTemp(1, GetPrimitiveParameter(0) + Offset) "
                                              "SetScriptReturnValue(GetTemp(2) == 5)"),
                   compileScript("Bits", "SetTemp(0, BitwiseOr(SetBit(0, 3), 1)) SetTemp(1, RoundFloat(2.5))"),
                   compileScript("NeedsObjects", "GetCatalogName(1)"),
                   compileScript("BadTemp", "SetTemp(9, 1)"),
                   compileScript("Properties", "nGameState.SetProperty('PetsFreeWill', true) "
                                               "nGameState.SetProperty('Count', 2.7) "
                                               "SetTemp(3, nGameState.GetProperty('Count')) "
                                               "SetScriptReturnValue(nGameState.GetProperty('PetsFreeWill') and "
                                               "nGameState.GetProperty('Nope') == nil)")});
    CHECK(runtime->problems.empty());

    auto lua = [](std::uint16_t index, bool params, std::uint8_t paramOwner, std::uint16_t paramData) {
        Instruction in;
        in.opcode = 0x007E;
        in.trueTarget = T;
        in.falseTarget = F;
        in.operands[0] = 0x30;
        in.operands[1] = 0x01;
        in.operands[2] = static_cast<std::uint8_t>(index);
        in.operands[4] = params ? 0x08 : 0x00;
        in.operands[6] = paramOwner;
        in.operands[7] = static_cast<std::uint8_t>(paramData);
        return in;
    };
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x130, {"AddToTemp", "Bits", "NeedsObjects", "BadTemp", "Properties"});
    code.add(kObjectGroup, 0x1005, bhav({lua(5, false, 0, 0)}));
    code.add(kObjectGroup, 0x1000, bhav({lua(1, true, owner::kLiteral, 7)}));
    code.add(kObjectGroup, 0x1001, bhav({lua(2, false, 0, 0)}));
    code.add(kObjectGroup, 0x1002, bhav({lua(3, false, 0, 0)}));
    code.add(kObjectGroup, 0x1003, bhav({lua(4, false, 0, 0)}));
    code.add(kObjectGroup, 0x1004, bhav({lua(1, false, 0, 0)}));
    ts2::sim::World world;
    world.setScripts(runtime);
    auto& object = world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);

    CHECK(vm.run(object, 0x1000).outcome == Outcome::False && object.thread.temps[1] == 17);
    object.thread.temps[2] = 5;
    CHECK(vm.run(object, 0x1000).outcome == Outcome::True);
    CHECK(vm.run(object, 0x1001).outcome == Outcome::False && object.thread.temps[0] == 5 &&
          object.thread.temps[1] == 2);
    const RunResult missing = vm.run(object, 0x1002);
    CHECK(missing.outcome == Outcome::Unsupported && missing.reason == "script function not implemented: GetCatalogName");
    const RunResult bad = vm.run(object, 0x1003);
    CHECK(bad.outcome == Outcome::Error && bad.reason == "object error running script");
    const RunResult noParams = vm.run(object, 0x1004);
    CHECK(noParams.outcome == Outcome::Error);
    CHECK(vm.run(object, 0x1005).outcome == Outcome::True && object.thread.temps[3] == 2);
    CHECK(std::get<std::int32_t>(runtime->properties.at("Count")) == 2);
}

// The object-facing script functions read and write the same values the VM uses; the running
// object is the thread's owner and the running-on object the one whose code runs.
void objectScriptsReadAndWriteValues() {
    auto runtime = std::make_shared<ts2::sim::LuaRuntime>();
    runtime->load({compileScript("Guids", "FridgeGuid = 1817229926")}, {compileScript("Values", R"(
        local fridge = GetStackObjectId()
        SetTemp(0, GetRunningObjectId() * 10 + GetRunningOnObjectId())
        SetObjectAttribute(fridge, 3, GetObjectAttribute(fridge, 3) + 5)
        SetObjectSemiAttribute(fridge, 1, 9)
        SetObjectArray(fridge, 0, {4, 5, 6})
        local copy = GetObjectArrayCopy(fridge, 0)
        SetTemp(1, GetNumObjectArrays(fridge) * 100 + table.getn(copy) * 10 + copy[3])
        SetTemp(2, GetObjectIdInSlot(fridge, 0))
        SetPersonMotive(GetRunningObjectId(), 2, 42.9)
        SetTemp(3, GetPersonMotive(GetRunningObjectId(), 2))
        SetTemp(4, GetObjectDefinitionField(FridgeGuid, 0))
        SetNeighborPersonData(8, 3, GetNeighborPersonData(8, 3) + 1)
        SetSimulatorGlobal(5, 77)
        local x, y, level = nWorld.GetObjectTile(fridge)
        SetTemp(5, x * 100 + y * 10 + level)
        SetScriptReturnValue(GetObjectName(fridge) == "Fridge")
    )"), compileScript("BadAttribute", "GetObjectAttribute(GetStackObjectId(), 200)"),
           compileScript("BadMotive", "GetPersonMotive(GetRunningObjectId(), 16)"),
           compileScript("BadArray", "GetObjectArrayCopy(GetStackObjectId(), 1)"),
           compileScript("Running", "SetTemp(0, GetRunningObjectId() * 10 + GetRunningOnObjectId())")});
    CHECK(runtime->problems.empty());

    MemoryCodeSource code;
    auto fridgeType = typed(kFridgeGuid);
    fridgeType.name = "Fridge";
    code.addObjectType(kObjectGroup, fridgeType);
    code.addStrings(ts2::sim::kGlobalGroup, 0x130, {"Values", "BadAttribute", "BadMotive", "BadArray", "Running"});
    Instruction in;
    in.opcode = 0x007E;
    in.trueTarget = T;
    in.falseTarget = F;
    in.operands[0] = 0x30;
    in.operands[1] = 0x01;
    for (std::uint8_t i = 1; i <= 5; ++i) {
        in.operands[2] = i;
        code.add(kObjectGroup, static_cast<std::uint16_t>(0x0FFF + i), bhav({in}));
    }
    code.add(kObjectGroup, 0x1010, bhav({call(0x1000, T, F)}));
    code.add(kObjectGroup, 0x1011, bhav({call(0x1004, T, F)}));

    ts2::sim::World world;
    world.setScripts(runtime);
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {8};
    hood->neighbors[8].guid = 0x0D000008;
    hood->neighbors[8].personData.assign(ts2::sim::kNeighborPersonDataCount, 0);
    hood->neighbors[8].personData[3] = 6;
    world.setNeighborhood(hood);
    auto& sim = world.create(kSimGroup, typed(0x51515151, ts2::sim::kPersonType));  // 1
    auto& fridge = world.create(kObjectGroup, fridgeType);                          // 2
    fridge.attributes[3] = 10;
    fridge.arrays.resize(1);
    fridge.slots = {1};
    fridge.x = 3 << 16 | 0x8000;
    fridge.y = 7 << 16;
    fridge.level = 2;
    sim.thread.stackObjectId = fridge.id;
    Vm vm(code, {}, &world);

    CHECK(vm.run(sim, kObjectGroup, 0x1010).outcome == Outcome::True);
    const auto& t = sim.thread.temps;
    CHECK(t[0] == 12 && t[1] == 136 && t[2] == 1 && t[3] == 42 && t[4] == 0);
    CHECK(t[5] == 731);
    CHECK(fridge.attributes[3] == 15 && fridge.semiAttributes[1] == 9);
    CHECK((*world.neighborPersonData(8))[3] == 7);
    CHECK(vm.globals[5] == 77);
    for (std::uint16_t tree = 0x1001; tree <= 0x1003; ++tree) {
        CHECK(vm.run(sim, kObjectGroup, tree).outcome == Outcome::Error);
    }
    // The fridge running its own code runs on itself.
    fridge.thread.stackObjectId = fridge.id;
    CHECK(vm.run(fridge, 0x1011).outcome == Outcome::False && fridge.thread.temps[0] == 22);
}

// Instructions saved with an older node version get their operands rewritten as the game does on
// load: Manage Inventory repacks byte 0 and gives singular operations byte 9 = 0x0C; Add/Change
// the Action String moves byte 4 to bytes 14-15.
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
}

Instruction manageInventory(std::uint8_t inventory, std::uint8_t op, std::uint8_t byte9,
                            std::uint8_t indexOwner, std::uint16_t indexData,
                            std::uint8_t valueOwner, std::uint16_t valueData) {
    Instruction in;
    in.opcode = 0x0033;
    in.nodeVersion = 2;
    in.trueTarget = T;
    in.falseTarget = F;
    in.operands[0] = inventory;
    in.operands[1] = owner::kLiteral;
    in.operands[2] = 8;  // neighbor 8
    in.operands[4] = op;
    in.operands[9] = byte9;
    in.operands[10] = indexOwner;
    in.operands[11] = static_cast<std::uint8_t>(indexData);
    in.operands[13] = valueOwner;
    in.operands[14] = static_cast<std::uint8_t>(valueData);
    return in;
}

// Each instruction continues with the next on true; the last returns true.
std::vector<Instruction> chained(std::vector<Instruction> instructions) {
    for (std::size_t i = 0; i + 1 < instructions.size(); ++i) {
        instructions[i].trueTarget = static_cast<std::uint16_t>(i + 1);
    }
    return instructions;
}

// Manage Inventory works on the saved inventories: singular tokens by GUID (0: the stack object's)
// and flags, their properties, and counted tokens per GUID and kind.
void manageInventoryChangesTokens() {
    constexpr std::uint8_t kNeighborVisible = 0x13;  // neighbor inventory, flags value 1 (visible)
    constexpr std::uint8_t kNeighborCounted = 0x0B;
    MemoryCodeSource code;
    // add a visible fridge token, find it into temp 0, push 42 onto it, read it back into temp 1
    Instruction read = manageInventory(kNeighborVisible, 15, 0x0C, owner::kLiteral, 1, owner::kTemp, 1);
    read.operands[6] = owner::kTemp;
    read.operands[7] = 0;
    code.add(kObjectGroup, 0x1000, bhav(chained({
        manageInventory(kNeighborVisible, 0, 0x0C, 0, 0, 0, 0),
        manageInventory(kNeighborVisible, 3, 0x0C, owner::kLiteral, 0, owner::kTemp, 0),
        manageInventory(kNeighborVisible, 4, 0x0C, owner::kTemp, 0, owner::kLiteral, 42),
        read,
        manageInventory(kNeighborVisible, 10, 0x0C, 0, 0, owner::kTemp, 2),
    })));
    // counted: add 3 twice of kind 5, remove 2, find it (count into temp 3, position into temp 4)
    code.add(kObjectGroup, 0x1001, bhav(chained({
        manageInventory(kNeighborCounted, 0, 5, 0, 0, owner::kLiteral, 3),
        manageInventory(kNeighborCounted, 0, 5, 0, 0, owner::kLiteral, 3),
        manageInventory(kNeighborCounted, 2, 5, 0, 0, owner::kLiteral, 2),
        manageInventory(kNeighborCounted, 6, 5, owner::kTemp, 4, owner::kTemp, 3),
        manageInventory(kNeighborCounted, 9, 5, owner::kTemp, 5, owner::kTemp, 6),
    })));
    // a missing token is false; another neighbor's inventory doesn't exist
    Instruction missing = manageInventory(kNeighborVisible, 4, 0x0C, owner::kLiteral, 9, owner::kLiteral, 1);
    Instruction otherNeighbor = manageInventory(kNeighborVisible, 10, 0x0C, 0, 0, owner::kTemp, 2);
    otherNeighbor.operands[2] = 9;
    code.add(kObjectGroup, 0x1002, bhav({missing}));
    code.add(kObjectGroup, 0x1003, bhav({otherNeighbor}));

    auto record = std::make_shared<ts2::ngbh::Neighborhood>();
    ts2::xobj::Token saved;
    saved.guid = 4242;
    record->neighbors[8].singular = {saved};
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->record = record;
    ts2::sim::World world;
    world.setNeighborhood(hood);
    auto& sim = world.create(kSimGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& fridge = world.create(kObjectGroup, typed(kFridgeGuid));
    sim.thread.stackObjectId = fridge.id;
    Vm vm(code, {}, &world);

    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True);
    const auto& t = sim.thread.temps;
    CHECK(t[0] == 2 && t[1] == 42 && t[2] == 2);
    const auto* inventory = world.inventory(ts2::sim::InventoryKind::Neighbor, 8);
    CHECK(inventory->singular[1]->guid == kFridgeGuid && inventory->singular[1]->flags == 1);
    sim.thread.temps[5] = 0;
    CHECK(vm.run(sim, kObjectGroup, 0x1001).outcome == Outcome::True && t[3] == 4 && t[4] == 1);
    // the next counted token of kind 5 after position 0: position 1, the fridge's GUID in temps 6-7
    CHECK(t[5] == 1 && static_cast<std::uint16_t>(t[6]) == (kFridgeGuid & 0xFFFF) &&
          static_cast<std::uint16_t>(t[7]) == (kFridgeGuid >> 16));
    CHECK(inventory->counted.size() == 1 && inventory->counted[0]->properties == std::vector<std::int16_t>({4, 5}));
    CHECK(vm.run(sim, kObjectGroup, 0x1002).outcome == Outcome::False);
    CHECK(vm.run(sim, kObjectGroup, 0x1003).outcome == Outcome::Error);
}

// Saved data tables: a version, the tables (id, name, key-value pairs), each value a type byte
// and the value. A neighbor's saved tables are what NeighborDataTable reads.
void savedDataTablesAreRead() {
    const std::vector<std::uint8_t> record = {
        1, 0, 0, 0,  1, 0, 0, 0,                    // version 1, one table
        5, 0, 0, 0,  4, 0, 0, 0, 'B', 'F', 'F', 'L',  // id 5, "BFFL"
        2, 0, 0, 0,                                 // two values
        1, 25, 0, 0, 0,                             // int 25 ->
        5, 2, 0, 0, 0,                              // a table of two:
        1, 1, 0, 0, 0,  0, 0x00, 0x00, 0x80, 0x3F,   //   1 -> 1.0
        1, 2, 0, 0, 0,  0, 0x00, 0x00, 0x40, 0x42,   //   2 -> 48.0
        4, 2, 0, 0, 0, 'h', 'i',                    // "hi" ->
        3, 1, 0, 0, 0,                              // true
    };
    const auto saved = ts2::datatables::parse(record);
    CHECK(saved.version == 1 && saved.tables.size() == 1 && saved.tables[0].name == "BFFL" && saved.extraBytes == 0);
    bool threw = false;
    try {
        auto bad = record;
        bad[24] = 9;  // an unknown value type
        ts2::datatables::parse(bad);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    auto runtime = std::make_shared<ts2::sim::LuaRuntime>();
    runtime->load({}, {compileScript("Read", R"(
        local t = NeighborDataTable.new(8)
        local friends = t:getTableValue(5, 25)
        SetTemp(0, friends[2])
        SetScriptReturnValue(t:getTableName(5) == "BFFL" and t:getTableValue(5, "hi") == true)
    )")});
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x130, {"Read"});
    Instruction in;
    in.opcode = 0x007E;
    in.trueTarget = T;
    in.falseTarget = F;
    in.operands[0] = 0x30;
    in.operands[1] = 0x01;
    in.operands[2] = 1;
    code.add(kObjectGroup, 0x1000, bhav({in}));
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {8};
    hood->neighbors[8].guid = 0x0D000008;
    hood->dataTables[8] = ts2::sim::toDataTables(saved);
    ts2::sim::World world;
    world.setScripts(runtime);
    world.setNeighborhood(hood);
    auto& object = world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);
    CHECK(vm.run(object, 0x1000).outcome == Outcome::True && object.thread.temps[0] == 48);
}

// Scenegraph resources are filed under name hashes: instance 0xFF000000 | crc24, high instance
// CRC-32/MPEG-2 of the lowercase name.
// DXT1 blocks hold two 5:6:5 colors and a 2-bit index per pixel.
void lightResourcesAreRead() {
    // A spot light as the game saves one (built by hand): container, the light's classes, its name,
    // then intensity, specular, colour, falloff rate, cone angle, penumbra and range.
    std::vector<std::uint8_t> b;
    auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    auto f32 = [&](float v) { u32(std::bit_cast<std::uint32_t>(v)); };
    auto str = [&](const std::string& s) {
        b.push_back(static_cast<std::uint8_t>(s.size()));
        b.insert(b.end(), s.begin(), s.end());
    };
    auto block = [&](const std::string& name, std::uint32_t version) {
        str(name);
        u32(0);
        u32(version);
    };
    u32(0xFFFF0001);
    u32(0);           // no links
    u32(1);
    u32(0xC9C81BAD);  // one block
    block("cSpotLight", 5);
    block("cStandardLightBase", 11);
    block("cSGResource", 2);
    str("window_light_lght");
    block("cLightT", 1);
    block("cSGResource", 2);
    str("window_light_lght");
    block("cReferentNode", 1);
    block("cObjectGraphNode", 4);
    u32(0);
    b.push_back(0);
    str("window_light");
    for (float v : {1.0f, 1.0f, 1.0f, 0.99362f, 0.772f, 0.4f, 1.65807f, 0.17453f, 2.35293f}) f32(v);
    const auto light = ts2::scenegraph::parseLight(b);
    CHECK(light.kind == "cSpotLight" && light.name == "window_light");
    CHECK((light.color == std::array<float, 3>{1.0f, 0.99362f, 0.772f}));
    CHECK(light.intensity == 1 && light.falloffRate == 0.4f && light.coneAngle == 1.65807f && light.range == 2.35293f);
    // Cut short or of another kind: refused.
    auto cut = b;
    cut.resize(cut.size() - 6);
    auto refused = [](const std::vector<std::uint8_t>& data) {
        try {
            ts2::scenegraph::parseLight(data);
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    CHECK(refused(cut));
    auto other = b;
    other[18] = 'X';  // "cXpotLight"
    CHECK(refused(other));
}

void scenegraphHashesAndTextures() {
    const auto key = ts2::hash::scenegraphKey("fridgeCheap_cres");
    CHECK(key.instance == 0xFF4F7CA0 && key.instanceHigh == 0x128A6A6A);

    const std::vector<std::uint8_t> block = {0x00, 0xF8, 0x1F, 0x00, 0b11100100, 0, 0, 0};  // red, blue; 0 1 2 3
    const auto rgba = ts2::scenegraph::decodeRgba(block, 4, 4, 4);
    auto px = [&](int i) { return std::array<int, 4>{rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3]}; };
    CHECK((px(0) == std::array<int, 4>{255, 0, 0, 255}) && (px(1) == std::array<int, 4>{0, 0, 255, 255}));
    CHECK((px(2) == std::array<int, 4>{170, 0, 85, 255}) && (px(3) == std::array<int, 4>{85, 0, 170, 255}));
    CHECK((px(4) == std::array<int, 4>{255, 0, 0, 255}));  // second row: index 0 again

    // Format 3 is alpha only: black, with each byte as the alpha.
    const std::vector<std::uint8_t> alphas = {0, 155, 255, 7};
    const auto shadow = ts2::scenegraph::decodeRgba(alphas, 2, 2, 3);
    CHECK(shadow[0] == 0 && shadow[3] == 0 && shadow[4] == 0 && shadow[7] == 155 && shadow[11] == 255 && shadow[15] == 7);
    // A size whose pixel count would wrap around to nothing is refused, not decoded into no memory.
    bool refused = false;
    try {
        ts2::scenegraph::decodeRgba(alphas, 0x80000000u, 0x80000000u, 1);
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);

    // A lot's scene record names the object's model and its material overrides (group: material).
    std::vector<std::uint8_t> record(64, 0);
    auto str = [&](std::string_view t) {
        record.push_back(static_cast<std::uint8_t>(t.size()));
        record.insert(record.end(), t.begin(), t.end());
    };
    auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            record.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    u32(0xFA1C39F7);
    u32(0x11);
    str("cObject");
    str("couchModern");
    u32(1);
    str("couchModern");
    u32(12);
    u32(1);
    str("couchmodern_fabric_blue");
    str("fabric");
    const auto so = ts2::scenegraph::parseSceneObject(record);
    CHECK(so.model == "couchModern" && so.overrides.size() == 1 && so.overrides[0].material == "couchmodern_fabric_blue" &&
          so.overrides[0].group == "fabric");

    // Older records (cObject version 0x10, often inside a cAnimatable block) have no overrides, and
    // a record without entries has none either; the position that follows isn't read as a count.
    record.assign(64, 0);
    u32(0x7D4F2BB8);
    u32(0x0F);
    str("cAnimatable");
    u32(0xFA1C39F7);
    u32(0x10);
    str("cObject");
    str("windowtwotilewideloft");
    u32(1);
    str("windowtwotilewideloft");
    u32(0);
    u32(0x41780000);  // 15.5, the object's x
    const auto older = ts2::scenegraph::parseSceneObject(record);
    CHECK(older.model == "windowtwotilewideloft" && older.overrides.empty());
    record.assign(64, 0);
    u32(0xFA1C39F7);
    u32(0x11);
    str("cObject");
    str("noModel");
    u32(0);
    u32(0x41A14138);  // a position, not an override count
    const auto empty = ts2::scenegraph::parseSceneObject(record);
    CHECK(empty.model == "noModel" && empty.overrides.empty());
}

// Inventories come from the saved neighborhood; scripts list, count, add and change tokens. Token
// lists keep each token's position in the whole list as its key.
void inventoryScriptsUseSavedTokens() {
    auto runtime = std::make_shared<ts2::sim::LuaRuntime>();
    runtime->load({}, {compileScript("Tokens", R"(
        local inv = Inventory.new(Inventory.kTypeNeighbor, 8)
        inv:addToken(1000, 5, 6)
        inv:addTokenWithCategory(2000, 3, {7})
        local plain = inv:getTokens()
        local byCategory = inv:getSingularTokensByCategory(3)
        local counted = inv:getCountedTokens()
        local found = inv:findToken(4242) ~= nil and inv:findToken(9999) == nil and
                      inv:findToken(1000, 0) ~= nil and inv:findToken(1000, 1) == nil
        SetTemp(0, inv:countTokens() * 10 + table.getn(plain))
        SetTemp(1, byCategory[3]:getProperty(1))
        SetTemp(2, counted[1]:getProperty(1))
        local added = inv:getTokenAtIndex(2)
        local ok, last = added:popProperty()
        SetTemp(3, last * 10 + added:countProperties())
        added:setFlag(InventoryToken.kFlagMarkedForDeletion)
        inv:removeTokensMarkedForDeletion()
        SetTemp(4, inv:countTokens())
        SetTemp(5, inv:countTokensByGUID(4242))
        SetScriptReturnValue(found and ok and added:getGUID() == 1000 and added:testFlag(4) and
                             inv:getTokenAtIndex(9) == nil and plain[1]:getGUID() == 4242)
    )"), compileScript("BadKind", "Inventory.new(7, 0)"),
           compileScript("NoInventory", "Inventory.new(Inventory.kTypeNeighbor, 99)"),
           compileScript("NewToken", "InventoryToken.new()"),
           compileScript("BadProperty", "Inventory.new(3, 8):getTokenAtIndex(1):getProperty(9)")});
    CHECK(runtime->problems.empty());

    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x130, {"Tokens", "BadKind", "NoInventory", "NewToken", "BadProperty"});
    Instruction in;
    in.opcode = 0x007E;
    in.trueTarget = T;
    in.falseTarget = F;
    in.operands[0] = 0x30;
    in.operands[1] = 0x01;
    for (std::uint8_t i = 1; i <= 5; ++i) {
        in.operands[2] = i;
        code.add(kObjectGroup, static_cast<std::uint16_t>(0x0FFF + i), bhav({in}));
    }

    auto record = std::make_shared<ts2::ngbh::Neighborhood>();
    ts2::xobj::Token saved;
    saved.guid = 4242;
    saved.properties = {1};
    ts2::xobj::Token counted;
    counted.guid = 77;
    counted.properties = {12, 0};
    record->neighbors[8].singular = {saved};
    record->neighbors[8].counted = {counted};
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->record = record;
    ts2::sim::World world;
    world.setScripts(runtime);
    world.setNeighborhood(hood);
    auto& object = world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);

    CHECK(vm.run(object, 0x1000).outcome == Outcome::True);
    const auto& t = object.thread.temps;
    CHECK(t[0] == 32 && t[1] == 7 && t[2] == 12 && t[3] == 61 && t[4] == 2 && t[5] == 1);
    CHECK(world.inventory(ts2::sim::InventoryKind::Neighbor, 8)->singular.size() == 2);
    CHECK(record->neighbors[8].singular.size() == 1);  // the saved record is left as it was
    for (std::uint16_t tree = 0x1001; tree <= 0x1004; ++tree) {
        CHECK(vm.run(object, tree).outcome == Outcome::Error);
    }
}

// Data tables keep keyed values per object or neighbor: keys and setTableValueAsInt values are
// whole numbers, setTableValue values single-precision; a missing table or key reads as nil, and
// writing to a missing table is error 0x9B.
void dataTableScriptsKeepValues() {
    auto runtime = std::make_shared<ts2::sim::LuaRuntime>();
    runtime->load({}, {compileScript("Tables", R"(
        local t = ObjectDataTable.new(1)
        t:newTable(5, "Scores")
        t:newTable(2, "Other")
        t:setTableValue(5, 2.7, 1.5)
        t:setTableValueAsInt(5, "count", 3.9)
        t:setTableValue(5, "nested", {[1.8] = 2, flag = true})
        local nested = t:getTableValue(5, "nested")
        local ids = t:getTableIds()
        SetTemp(0, t:getTableValue(5, "count"))
        SetTemp(1, t:getTableValue(5, 2) * 2)
        SetTemp(2, table.getn(t:getTableKeys(5)) * 10 + table.getn(ids))
        SetTemp(3, ids[1] * 10 + ids[2])
        SetTemp(4, nested[1])
        t:removeTableValue(5, 2)
        local n = NeighborDataTable.new(8)
        n:newTable(1, "Visits")
        n:setTableValueAsInt(1, 1, 4)
        SetTemp(5, nDataTable.GetNeighborTableValue(8, 1, 1) + nDataTable.GetObjectTableValue(1, 5, "count"))
        SetScriptReturnValue(t:getTableName(5) == "Scores" and t:getTableValue(6, 1) == nil and
                             t:getTableValue(5, "none") == nil and not t:hasTable(6) and t:hasTable(2) and
                             not t:hasTableValue(5, 2) and nested.flag == true and t:removeTable(2) and
                             not t:removeTable(2))
    )"), compileScript("Twice", "local t = ObjectDataTable.new(1) t:newTable(5, 'Again')"),
           compileScript("NoTable", "ObjectDataTable.new(1):setTableValue(7, 1, 1)"),
           compileScript("BadValue", "ObjectDataTable.new(1):setTableValue(5, 1, nil)"),
           compileScript("NoNeighbor", "NeighborDataTable.new(9)")});
    CHECK(runtime->problems.empty());

    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x130, {"Tables", "Twice", "NoTable", "BadValue", "NoNeighbor"});
    Instruction in;
    in.opcode = 0x007E;
    in.trueTarget = T;
    in.falseTarget = F;
    in.operands[0] = 0x30;
    in.operands[1] = 0x01;
    for (std::uint8_t i = 1; i <= 5; ++i) {
        in.operands[2] = i;
        code.add(kObjectGroup, static_cast<std::uint16_t>(0x0FFF + i), bhav({in}));
    }
    ts2::sim::World world;
    world.setScripts(runtime);
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {8};
    hood->neighbors[8].guid = 0x0D000008;
    world.setNeighborhood(hood);
    auto& object = world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);

    CHECK(vm.run(object, 0x1000).outcome == Outcome::True);
    const auto& t = object.thread.temps;
    CHECK(t[0] == 3 && t[1] == 3 && t[2] == 32 && t[3] == 25 && t[4] == 2 && t[5] == 7);
    CHECK(object.dataTables.size() == 1 && object.dataTables.at(5).values.size() == 2);
    CHECK(world.neighborDataTables(8).at(1).name == "Visits");
    for (std::uint16_t tree = 0x1001; tree <= 0x1004; ++tree) {
        CHECK(vm.run(object, tree).outcome == Outcome::Error);
    }
}

// ObjectManager answers object and neighbor lookups; id lists come back as Lua arrays in id order,
// leaving out objects whose data 0x38 has 0x1 or 0x4 set unless the caller asks for them.
void objectManagerScriptsFindObjects() {
    constexpr std::uint32_t kSimGuid = 0x51515151;
    auto runtime = std::make_shared<ts2::sim::LuaRuntime>();
    runtime->load({compileScript("Guids", "FridgeGuid = 1817229926 SimGuid = 1364283729")},
                  {compileScript("Lookups", R"(
        local om = ObjectManager.new()
        local fridges = om:getObjectIds(ObjectManager.kQuery_ByGUID, FridgeGuid)
        local all = om:getObjectIds(ObjectManager.kQuery_All, true)
        local byField = om:getObjectIds(ObjectManager.kQuery_ByObjectDataField, 16, 4)
        local people = om:getPersonIds()
        local dogs = om:getPersonIds(false, ObjectManager.kSpeciesFlag_DogLarge)
        local hood = om:getNeighborIds()
        SetTemp(1, table.getn(fridges) * 100 + fridges[1])
        SetTemp(2, table.getn(all))
        SetTemp(3, byField[1])
        SetTemp(4, table.getn(people) * 100 + table.getn(dogs) * 10 + table.getn(hood))
        SetTemp(5, om:getObjectIdFromGUID(FridgeGuid) * 10 + om:getPersonIdFromGUID(SimGuid))
        SetScriptReturnValue(om:verifyObjectId(1) and not om:verifyObjectId(99) and om:verifyNeighborId(8) and
                             not om:verifyNeighborId(9) and om:isValidObjectGUID(FridgeGuid) and
                             not om:isValidObjectGUID(1) and om:getObjectGUIDFromObjectId(1) == FridgeGuid)
    )"), compileScript("BadGuid", "ObjectManager.new():getObjectIdFromGUID(1)"),
           compileScript("MissingObject", "ObjectManager.new():getObjectGUIDFromObjectId(99)")});
    CHECK(runtime->problems.empty());

    MemoryCodeSource code;
    code.addObjectType(kObjectGroup, typed(kFridgeGuid));
    code.addObjectType(kObjectGroup, typed(kSimGuid, ts2::sim::kPersonType));
    code.addStrings(ts2::sim::kGlobalGroup, 0x130, {"Lookups", "BadGuid", "MissingObject"});
    Instruction in;
    in.opcode = 0x007E;
    in.trueTarget = T;
    in.falseTarget = F;
    in.operands[0] = 0x30;
    in.operands[1] = 0x01;
    for (std::uint8_t i = 1; i <= 3; ++i) {
        in.operands[2] = i;
        code.add(kObjectGroup, static_cast<std::uint16_t>(0x0FFF + i), bhav({in}));
    }

    ts2::sim::World world;
    world.setScripts(runtime);
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {3, 8};
    hood->neighbors[3].guid = 0x0D000003;
    hood->neighbors[8].guid = kSimGuid;
    world.setNeighborhood(hood);
    auto& fridge = world.create(kObjectGroup, typed(kFridgeGuid));         // 1
    auto& hidden = world.create(kObjectGroup, typed(kFridgeGuid));         // 2
    auto& sim = world.create(kObjectGroup, typed(kSimGuid, ts2::sim::kPersonType));  // 3, a large dog
    hidden.data[0x38] = 0x04;
    sim.data[0x10] = 4;
    sim.personData[0xBA] = 1;
    Vm vm(code, {}, &world);

    CHECK(vm.run(fridge, 0x1000).outcome == Outcome::True);
    const auto& t = fridge.thread.temps;
    CHECK(t[1] == 101 && t[2] == 3 && t[3] == 3 && t[4] == 112 && t[5] == 13);
    CHECK(vm.run(fridge, 0x1001).outcome == Outcome::Error);
    CHECK(vm.run(fridge, 0x1002).outcome == Outcome::Error);
}

Instruction wantSatisfy(std::uint32_t guid, std::uint8_t argOwner, std::uint16_t arg, std::uint16_t value,
                        std::uint16_t t, std::uint16_t f) {
    Instruction in;
    in.opcode = 0x007C;
    in.trueTarget = t;
    in.falseTarget = f;
    in.operands[0] = owner::kStackObjectId;
    for (std::size_t i = 0; i < 4; ++i) {
        in.operands[3 + i] = static_cast<std::uint8_t>(guid >> (8 * i));
    }
    in.operands[7] = argOwner;
    in.operands[8] = static_cast<std::uint8_t>(arg);
    in.operands[9] = static_cast<std::uint8_t>(arg >> 8);
    in.operands[10] = owner::kLiteral;
    in.operands[11] = static_cast<std::uint8_t>(value);
    in.operands[12] = static_cast<std::uint8_t>(value >> 8);
    return in;
}

// Want Satisfy matches the stack object's Sim's current wants: by want GUID, then by argument
// (0 in the want accepts any) and the definition's operator; matched wants leave the list.
void wantSatisfyMatchesCurrentWants() {
    constexpr std::uint32_t kSimGuidA = 0x51515151;
    constexpr std::uint32_t kAnySimWant = 0xADA6814F, kSkillWant = 0x2DCA4468, kLevelWant = 0x0DDDDDDD,
                            kListWant = 0x0EEEEEEE;
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 1, 1, E),
        wantSatisfy(kAnySimWant, owner::kStackObjectId, 0, 0, T, F),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 1, 1, E),
        wantSatisfy(kSkillWant, owner::kLiteral, 15, 7, T, F),
    }));
    code.add(kObjectGroup, 0x1002, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 1, 1, E),
        wantSatisfy(kLevelWant, owner::kLiteral, 6, 0, T, F),
    }));
    code.add(kObjectGroup, 0x1003, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 1, 1, E),
        wantSatisfy(0x0BADF00D, owner::kLiteral, 0, 0, T, F),
    }));
    code.add(kObjectGroup, 0x1004, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 1, 1, E),
        wantSatisfy(kListWant, owner::kLiteral, 0, 0, T, F),
    }));

    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(kSimGuidA, ts2::sim::kPersonType));
    CHECK(sim.id == 1);
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::Unsupported);

    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {27};
    hood->neighbors[27].guid = kSimGuidA;
    auto want = [](std::uint32_t guid, std::uint8_t type, std::int32_t argument, std::int16_t value) {
        ts2::swaf::Want w;
        w.id = 27;
        w.guid = guid;
        w.type = type;
        w.argument = argument;
        w.value = value;
        return w;
    };
    hood->wants[27] = {want(kAnySimWant, 1, 0, 0), want(kSkillWant, 4, 15, 8), want(kLevelWant, 0, 0, 5)};
    world.setNeighborhood(hood);
    auto defs = std::make_shared<ts2::content::WantDefinitions>();
    using ts2::content::WantArgument;
    using ts2::content::WantOperator;
    defs->byGuid[kAnySimWant] = {kAnySimWant, "any Sim", WantArgument::Sim, WantOperator::None, 3000, ""};
    defs->byGuid[kSkillWant] = {kSkillWant, "skill", WantArgument::Skill, WantOperator::None, 2500, ""};
    defs->byGuid[kLevelWant] = {kLevelWant, "level", WantArgument::None, WantOperator::GreaterThanOrEquals, 500, ""};
    defs->byGuid[kListWant] = {kListWant, "family", WantArgument::None, WantOperator::None, 500, "CT - nID Array - Family"};
    world.setWantDefinitions(defs);

    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);   // a want for any Sim, satisfied by itself
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::False);  // ...and gone from the list
    CHECK(vm.run(sim, 0x1001).outcome == Outcome::False);  // skill 15 but level 7, the want says 8
    CHECK(vm.run(sim, 0x1002).outcome == Outcome::True);   // 6 >= 5
    CHECK(world.neighborWants(27).size() == 1 && hood->wants[27].size() == 3);
    const RunResult unknown = vm.run(sim, 0x1003);
    CHECK(unknown.outcome == Outcome::Error && unknown.reason == "data number out of range");
    CHECK(vm.run(sim, 0x1004).outcome == Outcome::Unsupported);
}

// Owner 0x20 "neighbor's person data": the stack object id names a neighbor; writes change this
// world's copy, not the saved neighborhood.
void neighborPersonDataReadsAndWrites() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 9, 1, E),
        expr(owner::kTemp, 0, op::kAssign, ts2::sim::owner::kNeighborPersonData, 0x3A, 2, E),
        expr(ts2::sim::owner::kNeighborPersonData, 0xE6, op::kAssign, owner::kLiteral, 77, 3, E),
        expr(owner::kTemp, 1, op::kAssign, ts2::sim::owner::kNeighborPersonData, 0xE6, T, E),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 9, 1, E),
        expr(owner::kTemp, 0, op::kAssign, ts2::sim::owner::kNeighborPersonData, 0xE7, T, E),
    }));
    code.add(kObjectGroup, 0x1002, bhav({
        expr(owner::kStackObjectId, 0, op::kAssign, owner::kLiteral, 4, 1, E),
        expr(owner::kTemp, 0, op::kAssign, ts2::sim::owner::kNeighborPersonData, 0, T, E),
    }));
    ts2::sim::World world;
    auto& a = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::Unsupported);

    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {9};
    hood->neighbors[9].personData.assign(0x40, 0);
    hood->neighbors[9].personData[0x3A] = 3;
    world.setNeighborhood(hood);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::True);
    CHECK(a.thread.temps[0] == 3 && a.thread.temps[1] == 77);
    CHECK(hood->neighbors[9].personData.size() == 0x40);
    const RunResult range = vm.run(a, 0x1001);
    CHECK(range.outcome == Outcome::Error && range.reason == "person data field is out of range");
    const RunResult missing = vm.run(a, 0x1002);
    CHECK(missing.outcome == Outcome::Error && missing.reason == "missing neighbor for data access");
}

void constantsByTempIndexWithTempZero() {
    MemoryCodeSource code;
    code.addConstants(ts2::sim::kGlobalGroup, 0x0103, {10, 20, 30, 40});
    code.addConstants(kObjectGroup, 0x1002, {7, 8, 9});
    const std::uint16_t table = (0x800 + 3) << 3;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 2, 1, E),
        expr(owner::kTemp, 1, op::kAssign, ts2::sim::owner::kConstantByTemp, table, 2, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 9, 3, E),
        expr(owner::kTemp, 2, op::kAssign, ts2::sim::owner::kConstantByTemp, table, T, E),
    }));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(t.temps[1] == 30);
    CHECK(r.outcome == Outcome::Error);
    CHECK(r.reason == "constant index out of range");

    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 1, 1, E),
        expr(owner::kTemp, 3, op::kAssign, ts2::sim::owner::kConstantByTemp, 2 << 3, T, E),
    }));
    ThreadState t2;
    CHECK(runPrivate(code, t2, 0x1001).outcome == Outcome::True);
    CHECK(t2.temps[3] == 8);
}

void motivesAreClampedAndNeedAPerson() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kMyMotives, 5, op::kAssign, owner::kLiteral, 60, 1, E),
        expr(owner::kMyMotives, 5, op::kAdd, owner::kLiteral, 70, 2, E),
        expr(owner::kStackObjectMotives, 3, op::kAssign, owner::kLiteral, static_cast<std::uint16_t>(-500), 3, E),
        expr(owner::kTemp, 2, op::kAssign, owner::kLiteral, 3, 4, E),
        expr(owner::kTemp, 0, op::kAssign, owner::kStackObjectMotiveByTemp, 2, T, E),
    }));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& other = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.thread.stackObjectId = other.id;
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True);
    CHECK(sim.motives[5] == 100.0f);
    CHECK(other.motives[3] == -100.0f);
    CHECK(sim.thread.temps[0] == -100);

    MemoryCodeSource bad;
    bad.add(kObjectGroup, 0x1000, bhav({expr(owner::kMyMotives, 16, op::kAssign, owner::kLiteral, 1, T, E)}));
    Vm badVm(bad, {}, &world);
    CHECK(badVm.run(sim, 0x1000).outcome == Outcome::Error);
}

Instruction removeObject(std::uint8_t who, std::uint8_t flags, std::uint16_t t, std::uint16_t f) {
    Instruction in = primitiveCall(0x0012, t, f);
    in.operands[0] = who;
    in.operands[2] = flags;
    return in;
}

Instruction changeLight(std::uint8_t flags, std::int16_t object, std::uint8_t percent, std::uint8_t light) {
    Instruction in = primitiveCall(0x006F, T, F);
    in.operands[1] = flags;
    in.operands[2] = owner::kLiteral;
    in.operands[3] = static_cast<std::uint8_t>(object);
    in.operands[4] = static_cast<std::uint8_t>(object >> 8);
    in.operands[7] = percent;
    in.operands[8] = light;
    return in;
}

void changeLightSetsLightStrength() {
    auto run = [](const Instruction& in, std::int16_t temp0, Outcome& outcome) {
        MemoryCodeSource code;
        code.add(kObjectGroup, 0x1000, bhav({in}));
        SmallWorld w;
        w.controller->thread.temps[0] = temp0;
        Vm vm(code, {}, &w.world);
        outcome = vm.run(*w.controller, 0x1000).outcome;
        return w.world.find(3) ? w.world.find(3)->lightStrength : -1.0f;
    };
    Outcome outcome{};
    CHECK(run(changeLight(0, 3, 40, 0xFF), 0, outcome) == static_cast<float>(40 * 0.01) && outcome == Outcome::True);
    CHECK(run(changeLight(0, 3, 150, 0xFF), 0, outcome) == 0.5f);        // out of range: half
    CHECK(run(changeLight(0x02, 3, 0, 0xFF), 75, outcome) == 0.75f);     // from Temp 0
    CHECK(run(changeLight(0x02, 3, 0, 0xFF), -3, outcome) == 0.5f);
    run(changeLight(0, 3, 40, 0), 0, outcome);                            // a named light with no name table
    CHECK(outcome == Outcome::Error);
    run(changeLight(0, 99, 40, 0xFF), 0, outcome);                        // no such object
    CHECK(outcome == Outcome::Error);
}

void removeObjectRemovesTheStackObject() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        removeObject(1, 0x00, 1, F),
        setToNext(0x04, owner::kStackObjectId, 0, kChairGuid, F, T),
    }));
    SmallWorld w;
    w.controller->thread.stackObjectId = 3;
    Vm vm(code, {}, &w.world);
    CHECK(vm.run(*w.controller, 0x1000).outcome == Outcome::True);
    CHECK(w.world.find(3) == nullptr);
    CHECK(w.world.size() == 6);
    w.world.purge();
    CHECK(w.world.size() == 6);
}

void removeObjectRefusesProtectedObjects() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({removeObject(1, 0x00, T, F)}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& special = world.create(kObjectGroup, typed(0xCC7B08F8));
    sim.thread.stackObjectId = special.id;
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::False);
    CHECK(world.find(special.id) != nullptr);
}

void removingMeEndsTheRunUnlessReturnImmediately() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({removeObject(0, 0x00, T, F)}));
    code.add(kObjectGroup, 0x1001, bhav({
        removeObject(0, 0x01, 1, F),
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 4, T, E),
    }));
    ts2::sim::World world;
    auto& a = world.create(kObjectGroup, typed(kFridgeGuid));
    auto& b = world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);
    CHECK(vm.run(a, 0x1000).outcome == Outcome::MeRemoved);
    CHECK(!a.thread.isRunning());
    CHECK(vm.run(b, 0x1001).outcome == Outcome::True);
    CHECK(b.thread.temps[0] == 4);
    CHECK(world.size() == 0);
}

// A 6x6 lot with walls around the square (1,1)-(3,3); walls go clockwise on screen (y down),
// so the room inside (5) is on their right and the yard (0) on their left.
ts2::wallgraph::WallGraph squareRoom(bool splitDiagonally) {
    ts2::wallgraph::WallGraph g;
    g.width = 6;
    g.height = 6;
    g.levels = 1;
    g.points = {{0, 1, 1, 0}, {1, 3, 1, 0}, {2, 3, 3, 0}, {3, 1, 3, 0}};
    const std::int32_t lowerLeft = splitDiagonally ? 6 : 5;
    g.walls = {{0, 0, 0, 1, 5}, {1, 1, 0, 2, 5}, {2, 2, 0, 3, lowerLeft}, {3, 3, 0, 0, lowerLeft}};
    if (splitDiagonally) {
        g.walls.push_back({4, 0, 5, 2, 6});
    }
    return g;
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

void roomFieldsAreReadThroughTempZero() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 5, 1, E),
        expr(owner::kTemp, 1, op::kAssign, owner::kRoomByTemp, 3, 2, E),
        expr(owner::kTemp, 2, op::kAssign, owner::kRoomByTemp, 2, T, E),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 3, op::kAssign, owner::kRoomByTemp, 4, T, E),
    }));
    ts2::sim::World world;
    world.setRooms(ts2::sim::RoomMap(squareRoom(false)));
    auto& me = world.create(kObjectGroup, definition("Me", 0));
    Vm vm(code, {}, &world);
    CHECK(vm.run(me, 0x1000).outcome == Outcome::True);
    CHECK(me.thread.temps[1] == 4);
    CHECK(me.thread.temps[2] == 1);
    const RunResult pool = vm.run(me, 0x1001);
    CHECK(pool.outcome == Outcome::Unsupported && pool.reason == "room field not implemented");
}

// "Outside" (field 1) is the room's region in layer 0; field 7 reads layer 1.
void outsideIsTheRegionInLayerZero() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({
        expr(owner::kTemp, 1, op::kAssign, owner::kRoomByTemp, 1, T, E),
    }));
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 1, op::kAssign, owner::kRoomByTemp, 7, T, E),
    }));
    ts2::sim::RoomMap rooms(squareRoom(true));
    rooms.assignLayer(0, ts2::sim::RoomMap(squareRoom(false)));
    ts2::sim::World world;
    world.setRooms(std::move(rooms));
    auto& me = world.create(kObjectGroup, definition("Me", 0));
    Vm vm(code, {}, &world);
    for (const auto [room, region] : {std::pair{5, 5}, std::pair{6, 5}, std::pair{0, 0}}) {
        me.thread.temps[0] = static_cast<std::int16_t>(room);
        CHECK(vm.run(me, 0x1000).outcome == Outcome::True);
        CHECK(me.thread.temps[1] == region);
    }
    me.thread.temps[0] = 5;
    const RunResult layer1 = vm.run(me, 0x1001);
    CHECK(layer1.outcome == Outcome::Unsupported && layer1.reason == "room has no region in that layer");
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

    // A layer whose walls are all on level -1 encloses nothing on level 0: region 0 there.
    auto basementOnly = squareRoom(false);
    for (auto& p : basementOnly.points) {
        p.level = -1;
    }
    ts2::sim::RoomMap withLayer(grid, -1);
    withLayer.assignLayer(0, ts2::sim::RoomMap(basementOnly));
    CHECK(withLayer.room(7)->layerRegion[0] == 0 && withLayer.room(8)->layerRegion[0] == 0);
}

} // namespace

// With an argument, runs only the tests whose names contain it.
// A UI script: windows with their attributes, quoted text with entities, lists and keys, and a
// window's children.
void uiScriptsParse() {
    const std::string_view script =
        "# Generated by UI editor\r\n"
        "<LEGACY clsid=GZWinGen iid=IGZWinGen id=0xcda360ce area=(137,85,839,647) caption=\"Main menu\" winflag_visible=yes >\r\n"
        "<CHILDREN>\r\n"
        "   <LEGACY clsid=GZWinBtn iid=IGZWinBtn id=0x000000a2 area=(-26,19,91,72) caption=\"Say &quot;hi&quot; &amp; go\" "
        "image={499db772,ccc30090} captionres={7f3a262a,00840025} winflag_visible=no >\r\n"
        "   <CHILDREN>\r\n"
        "      <LEGACY clsid=GZWinText iid=IGZWinText area=(0,0,10,10) >\r\n"
        "   </CHILDREN>\r\n"
        "</CHILDREN>\r\n"
        "<LEGACY clsid=GZWinBMP iid=IGZWinBMP area=(0,0,1,1) >\r\n";
    const auto windows = ts2::ui::parse(script);
    CHECK(windows.size() == 2 && windows[0].children.size() == 1 && windows[0].children[0].children.size() == 1);
    // Windows nested far deeper than any of the game's scripts are refused rather than overflowing
    // the stack.
    std::string deep;
    for (int i = 0; i < 10000; ++i) {
        deep += "<LEGACY clsid=GZWinGen >\r\n<CHILDREN>\r\n";
    }
    bool refused = false;
    try {
        ts2::ui::parse(deep);
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);
    const auto& root = windows[0];
    CHECK(root.text("iid") == "IGZWinGen" && root.number("id") == 0xcda360ceu && root.flag("winflag_visible"));
    CHECK((root.list("area") == std::vector<std::int64_t>{137, 85, 839, 647}));
    const auto& button = root.children[0];
    CHECK(button.text("caption") == "Say \"hi\" & go" && !button.flag("winflag_visible"));
    CHECK((button.list("area") == std::vector<std::int64_t>{-26, 19, 91, 72}));
    CHECK((button.list("image") == std::vector<std::int64_t>{0x499db772, 0xccc30090}));
    CHECK((button.list("captionres") == std::vector<std::int64_t>{0x7f3a262a, 0x00840025}));  // leakcheck: ok
    CHECK(windows[1].text("iid") == "IGZWinBMP" && !windows[1].get("caption"));
}

// Targa images: true color stored bottom row first, and run-length encoded with alpha.
void targaImagesDecode() {
    std::vector<std::uint8_t> plain(18, 0);
    plain[2] = 2;
    plain[12] = 2;
    plain[14] = 2;
    plain[16] = 24;
    // Bottom row then top row, blue-green-red per pixel.
    const std::uint8_t rows[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    plain.insert(plain.end(), std::begin(rows), std::end(rows));
    const auto a = ts2::tga::decode(plain);
    CHECK(a && a->width == 2 && a->height == 2);
    CHECK(!ts2::tga::decode(plain, 1) && !ts2::image::decode(plain, 1) && ts2::image::decode(plain, 2));  // a caller's size limit
    CHECK((std::vector<std::uint8_t>(a->rgba.begin(), a->rgba.begin() + 4) == std::vector<std::uint8_t>{9, 8, 7, 255}));
    CHECK((std::vector<std::uint8_t>(a->rgba.begin() + 8, a->rgba.begin() + 12) == std::vector<std::uint8_t>{3, 2, 1, 255}));

    std::vector<std::uint8_t> packed(18, 0);
    packed[2] = 10;
    packed[12] = 3;
    packed[14] = 1;
    packed[16] = 32;
    packed[17] = 0x28;  // top row first, 8 alpha bits
    const std::uint8_t runs[] = {0x81, 10, 20, 30, 40, 0x00, 50, 60, 70, 80};
    packed.insert(packed.end(), std::begin(runs), std::end(runs));
    const auto b = ts2::tga::decode(packed);
    CHECK(b && b->width == 3 && b->height == 1);
    CHECK((b->rgba == std::vector<std::uint8_t>{30, 20, 10, 40, 30, 20, 10, 40, 70, 60, 50, 80}));
    CHECK(!ts2::tga::decode(std::vector<std::uint8_t>(10, 0)));
    // A header claiming far more pixels than the data can hold is refused before any memory is set
    // aside for them, plain or run-length coded.
    auto huge = packed;
    huge[12] = huge[13] = huge[14] = huge[15] = 0xFF;  // 65535 x 65535
    CHECK(!ts2::tga::decode(huge));
    huge[13] = huge[15] = 0x30;  // 12543 x 12543: within the size limit, still far too few bytes
    CHECK(!ts2::tga::decode(huge));
    huge[2] = 2;  // plain
    CHECK(!ts2::tga::decode(huge));
}

// A neighborhood's moving picture: one 32 x 32 block, a pixel written out then runs of one color,
// added to a black start; the next frame leaves the block as it was, the one after adds to it.
void neighborhoodVideosDecode() {
    std::vector<std::uint8_t> file;
    auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            file.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    auto tag = [&](const char* t) { file.insert(file.end(), t, t + 4); };
    tag("RIFF");
    u32(0);
    tag("Reia");
    tag("head");
    u32(24);
    for (const std::uint32_t v : {1u, 32u, 32u, 10u, 1u, 2u}) {
        u32(v);
    }
    std::vector<std::uint8_t> frame{1, 0x00, 30, 20, 10};  // block changed; one pixel: blue 30, green 20, red 10
    for (int left = 1023; left > 0;) {
        const int n = std::min(left, 129);
        const std::uint8_t px[] = {static_cast<std::uint8_t>(257 - n), 3, 2, 1};
        frame.insert(frame.end(), std::begin(px), std::end(px));
        left -= n;
    }
    tag("frme");
    u32(static_cast<std::uint32_t>(frame.size()));
    file.insert(file.end(), frame.begin(), frame.end());
    if (frame.size() & 1) {
        file.push_back(0);
    }
    tag("frme");
    u32(1);
    file.push_back(0);  // unchanged
    file.push_back(0);
    tag("frme");
    u32(static_cast<std::uint32_t>(frame.size()));
    file.insert(file.end(), frame.begin(), frame.end());
    const auto video = ts2::reia::parse(file);
    CHECK(video.width == 32 && video.height == 32 && video.frames.size() == 3 && video.rateNumerator == 10);
    ts2::tga::Pixels picture;
    CHECK(ts2::reia::decodeFrame(video, 0, picture));
    CHECK((std::vector<std::uint8_t>(picture.rgba.begin(), picture.rgba.begin() + 8) == std::vector<std::uint8_t>{10, 20, 30, 255, 1, 2, 3, 255}));
    CHECK(ts2::reia::decodeFrame(video, 1, picture) && picture.rgba[0] == 10 && picture.rgba[4 * 1023] == 1);
    CHECK(ts2::reia::decodeFrame(video, 2, picture) && picture.rgba[0] == 20 && picture.rgba[2] == 60 && picture.rgba[4 * 1023] == 2);
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

void neighborhoodLightMapLooksUpNormals() {
    // A 4x4 lookup image whose pixel (x, y) is (40 * x + 10, 40 * y + 10, 200).
    ts2::scene::NeighborhoodLight light;
    light.sun = {0.6f, 0, 0.8f};
    light.lookup.width = light.lookup.height = 4;
    for (std::uint8_t y = 0; y < 4; ++y) {
        for (std::uint8_t x = 0; x < 4; ++x) {
            light.lookup.rgba.insert(light.lookup.rgba.end(), {static_cast<std::uint8_t>(40 * x + 10), static_cast<std::uint8_t>(40 * y + 10), 200, 255});
        }
    }
    // Flat ground, 5 x 3 vertices: every texel looks up the middle pixel (2, 2): (90, 90, 200), the
    // 200 rounded up twice as the game does (200 -> 201 -> 202).
    const std::uint32_t cols = 5, rows = 3;
    std::vector<float> heights(cols * rows, 100.0f), normals;
    for (std::size_t i = 0; i < heights.size(); ++i) {
        normals.insert(normals.end(), {0, 0, 1});
    }
    auto map = ts2::scene::neighborhoodLightMap(cols, rows, heights, normals, 10, light);
    CHECK(map.width == 4 && map.height == 2);
    CHECK(map.rgba[0] == 90 && map.rgba[1] == 90 && map.rgba[2] == 202 && map.rgba[3] == 255);
    // A tall ridge on the sunny side (x = 4) shades the ground beside it: its normals lean away
    // from the sun, so they look up another pixel.
    for (std::uint32_t y = 0; y < rows; ++y) {
        heights[y * cols + 4] = 200.0f;
    }
    map = ts2::scene::neighborhoodLightMap(cols, rows, heights, normals, 10, light);
    CHECK(map.rgba[(0 * 4 + 3) * 4] != 90);
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
    const auto objects = ts2::nhood::parseObjects(d);
    CHECK(objects.version == 0xCB && objects.roads.size() == 1 && objects.flora.size() == 1);
    CHECK(objects.flora[0].position[1] == 560 && objects.flora[0].bounds[2] == 705 && objects.flora[0].angle == 270 &&
          objects.flora[0].type == 65);
    const auto& road = objects.roads[0];
    CHECK(road.piece == 0x4b00 && road.bounds[0] == 10 && road.bounds[3] == 1030);
    CHECK(road.corners[2][0] == 20 && road.corners[2][1] == 1030 && road.corners[2][2] == 368.5f && road.corners[3][3] == 1);
}

// Binary property sets (magic 0xCBE750E0) read like the XML ones; reference lists (3IDR) list keys,
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

// Animation curves run as cubic Hermite segments between keys, holding the end values outside.
void animationCurvesSample() {
    ts2::anim::Curve flat{{{0, 1, 0, 0}, {100, 1, 0, 0}}};
    CHECK(flat.sample(-5) == 1 && flat.sample(50) == 1 && flat.sample(500) == 1);
    // Slopes equal to the rise make the segment a straight line.
    ts2::anim::Curve line{{{0, 0, 0, 2}, {100, 2, 2, 0}}};
    CHECK(std::abs(line.sample(25) - 0.5f) < 1e-5f && std::abs(line.sample(50) - 1.0f) < 1e-5f);
    // Without slopes it eases in and out.
    ts2::anim::Curve eased{{{0, 0, 0, 0}, {100, 1, 0, 0}}};
    CHECK(std::abs(eased.sample(50) - 0.5f) < 1e-5f && eased.sample(10) < 0.1f);
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
        return d;
    };
    const std::uint16_t versions[2][2] = {{13, 6}, {18, 11}};
    for (const auto& [version, own] : versions) {
        const auto lot = ts2::nhood::parseLotDescription(make(version, own));
        CHECK(lot.width == 2 && lot.depth == 3 && lot.roadSides == 8 && lot.ownFacing == 3 && lot.name == "Lot");
        CHECK(lot.x == 98 && lot.y == 80 && lot.elevation == 324.1875f && lot.id == 32 && lot.facing == 1);
        CHECK(lot.texture == "lottexture");
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

// The packs are found in both installation layouts: the Ultimate Collection's folders named after
// their packs, and the Legacy Collection's EP1-EP9 and SP1-SP8 (where Mansion & Garden is EP9).
// The skin drawn under clothes, as the running game names it: "topnaked" under a body outfit,
// "bottomnaked" under a top and bottom; nothing for skin tones that aren't the game's own.
void skinUnderClothesNames() {
    ts2::scene::SimLook look;
    look.age = 8;
    look.gender = 1;
    look.skinTone = "00000003-0000-0000-0000-000000000000";
    CHECK(ts2::scene::skinUnderClothes(look) == "aftopnaked_nude_s3");
    CHECK(ts2::scene::skinUnderClothes(look, true) == "afbottomnaked_nude_s3");
    look.age = 4;
    look.gender = 2;
    CHECK(ts2::scene::skinUnderClothes(look, true) == "tmbottomnaked_nude_s3");
    look.skinTone = "12345678-0000-0000-0000-000000000000";
    CHECK(ts2::scene::skinUnderClothes(look).empty());
}

// A joint the animation doesn't move is posed with the mesh's own rest place for it, under its posed
// parent; long hair turns its hair joints further than the skeleton does.
void unanimatedJointsKeepTheMeshRest() {
    using ts2::scene::Transform;
    ts2::scene::Rig rig;
    rig.nodes.push_back({"head", -1, {0, 0, 1}, {0, 0, 0, 1}});
    rig.nodes.push_back({"f_hair", 0, {0, 0, 0}, {0.70710678f, 0, 0, 0.70710678f}});  // a quarter turn about x
    std::vector<Transform> world;
    ts2::scene::poseRig(rig, nullptr, 0, world);
    ts2::scene::Mesh mesh;
    mesh.skin.push_back({"f_hair", Transform{}, Transform{}, Transform::of({0, 0, 0}, {1, 0, 0, 0})});  // a half turn
    mesh.skin.push_back({"f_hair", Transform{}, Transform{}, std::nullopt});
    std::vector<Transform> out;
    ts2::scene::skinMatrices(mesh, {1, 1}, rig, nullptr, world, out);
    CHECK(out.size() == 2);
    float own[3] = {0, 1, 0}, skeleton[3] = {0, 1, 0};
    out[0].apply(own, true);
    out[1].apply(skeleton, true);
    CHECK(std::abs(own[1] + 1) < 1e-5f && std::abs(own[2] - 1) < 1e-5f);
    CHECK(std::abs(skeleton[1]) < 1e-5f && std::abs(skeleton[2] - 2) < 1e-5f);
}

// Objects' daylight: the values the game's shaders received on a lot by day (captured), from the
// "day" lighting state: each light as strength * (colour - 0.5), the skylight as the ambient.
// A floor grid of one room, every cell linked to its neighbours, lit only by its ambient.
ts2::scene::floorlight::Grid testFloor(int width, int height) {
    ts2::scene::floorlight::Grid g;
    g.width = width;
    g.height = height;
    g.insideAmbient = {0.05f, 0.05f, 0.05f};
    g.cells.resize(static_cast<std::size_t>(4 * width * height));
    g.tiles.assign(static_cast<std::size_t>(width * height), {true, 0});
    for (auto& cell : g.cells) {
        cell.halves[0].valid = true;
        cell.halves[0].room = 1;
        cell.halves[0].links = 0xFF;
    }
    return g;
}

float floorLight(ts2::scene::floorlight::Grid& g, int x, int y) {
    return g.at(x, y)->halves[0].light[0];
}

void floorLightSpreadsAndFades() {
    using namespace ts2::scene::floorlight;
    ts2::scene::LightSource lamp;
    lamp.color = {1, 1, 1};
    lamp.room = 1;
    lamp.x1 = lamp.y1 = 0.5f;  // the first half tile only

    auto g = testFloor(1, 1);
    light(g, {lamp}, nullptr);
    CHECK(std::abs(floorLight(g, 0, 0) - 1) < 1e-6f);
    CHECK(std::abs(floorLight(g, 1, 0) - (1 / 1.4f + 0.01f)) < 1e-6f);   // one step, falloff 1 x 0.4
    CHECK(std::abs(floorLight(g, 0, 1) - (1 / 1.4f + 0.01f)) < 1e-6f);
    CHECK(std::abs(floorLight(g, 1, 1) - (1 / 1.56f + 0.01f)) < 1e-6f);  // diagonal: 1.4 times the falloff

    // A wall between the first and second tile: no links across it.
    g = testFloor(2, 1);
    for (int y = 0; y < 2; ++y) {
        g.at(1, y)->halves[0].links &= static_cast<std::uint8_t>(~0xE0);  // directions 5, 6, 7 go right
        g.at(2, y)->halves[0].links &= static_cast<std::uint8_t>(~0x0E);  // 1, 2, 3 go left
    }
    light(g, {lamp}, nullptr);
    CHECK(floorLight(g, 1, 0) > 0.5f);
    CHECK(floorLight(g, 2, 0) == 0.05f && floorLight(g, 3, 1) == 0.05f);

    // A lamp in another room lights nothing.
    g = testFloor(1, 1);
    ts2::scene::LightSource other = lamp;
    other.room = 2;
    light(g, {other}, nullptr);
    CHECK(floorLight(g, 0, 0) == 0.05f);

    // From the level above: a floor between stops it, otherwise it loses half.
    ts2::scene::LightSource above = lamp;
    above.level = 1;
    g = testFloor(1, 1);
    light(g, {above}, [](int, int, int) { return true; });
    CHECK(floorLight(g, 0, 0) == 0.05f);
    light(g, {above}, [](int, int, int) { return false; });
    CHECK(std::abs(floorLight(g, 0, 0) - 0.5f) < 1e-6f);

    // A grid whose cells do not match its size is left alone.
    g = testFloor(1, 1);
    g.cells.pop_back();
    light(g, {lamp}, nullptr);
    CHECK(floorLight(g, 0, 0) == 0);
}

// One wall face two tiles long along x, looking toward -y, in room 1.
ts2::scene::walllight::Walls testWall() {
    ts2::scene::walllight::Walls w;
    ts2::scene::walllight::Run run;
    run.room = 1;
    for (int i = 0; i < 2; ++i) {
        ts2::scene::walllight::Segment s;
        s.x0 = static_cast<float>(i);
        s.x1 = static_cast<float>(i + 1);
        run.segments.push_back(s);
    }
    w.runs.push_back(run);
    return w;
}

// A level of plain floor for the grid builder, every tile floored and in room 1.
ts2::scene::lightgrid::LevelInput gridInput(int width, int height) {
    ts2::scene::lightgrid::LevelInput in;
    in.width = width;
    in.height = height;
    in.floor.assign(static_cast<std::size_t>(width * height), 1);
    in.walls.assign(static_cast<std::size_t>(width * height), 0);
    in.roomAt = [](float, float) { return std::int16_t{1}; };
    return in;
}

void lightGridIsBuiltAsTheGame() {
    using namespace ts2::scene;
    // Open floor: each half tile links to every neighbour inside the grid, nothing at the edge.
    auto g = lightgrid::build(gridInput(2, 1));
    CHECK(g.fits());
    CHECK(g.at(0, 0)->halves[0].links == 0x70);  // down, down-right, right
    CHECK(g.at(1, 0)->halves[0].links == 0x7C);
    CHECK(g.at(1, 1)->halves[0].links == 0xC7);
    CHECK(g.at(0, 0)->halves[0].valid && !g.at(0, 0)->halves[1].valid);
    CHECK(g.at(0, 0)->halves[0].room == 1 && !g.at(0, 0)->halves[0].outside);

    // A wall between the two tiles: nothing links across it, and lamp light stays on its side.
    auto in = gridInput(2, 1);
    in.walls[0] = lightgrid::kPlusX;
    in.walls[1] = lightgrid::kMinusX;
    g = lightgrid::build(in);
    for (int y = 0; y < 2; ++y) {
        CHECK((g.at(1, y)->halves[0].links & 0xE0) == 0);
        CHECK((g.at(2, y)->halves[0].links & 0x0E) == 0);
    }
    LightSource lamp;
    lamp.color = {1, 1, 1};
    lamp.room = 1;
    lamp.x1 = lamp.y1 = 0.5f;
    CHECK(floorlight::light(g, {lamp}, nullptr));
    CHECK(g.at(1, 0)->halves[0].light[0] > 0.5f);
    CHECK(g.at(2, 0)->halves[0].light[0] == 0);

    // A diagonal wall along x = y splits the tile: both halves of each quarter take part, each
    // with the room on its side, and nothing links across the wall.
    in = gridInput(1, 1);
    in.walls[0] = lightgrid::kDiagonalXEqualsY;
    in.roomAt = [](float x, float y) { return static_cast<std::int16_t>(y < x ? 2 : 0); };
    g = lightgrid::build(in);
    CHECK(g.tiles[0].diagonal == 2);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            const auto& c = *g.at(x, y);
            CHECK(c.halves[0].valid && c.halves[1].valid);
            CHECK(c.halves[0].room == 2 && !c.halves[0].outside);
            CHECK(c.halves[1].room == 0 && c.halves[1].outside);
        }
    }
    lamp.x0 = 0.5f;
    lamp.x1 = 1;
    lamp.y1 = 0.5f;
    lamp.room = 2;
    g = lightgrid::build(in);
    CHECK(floorlight::light(g, {lamp}, nullptr));
    CHECK(g.at(1, 0)->halves[0].light[0] > 0.5f);
    CHECK(g.at(0, 1)->halves[1].light[0] == 0);

    // Sizes that don't match or are out of range give an empty grid.
    in = gridInput(2, 2);
    in.floor.pop_back();
    CHECK(!lightgrid::build(in).fits());
    CHECK(!lightgrid::build(gridInput(0, 3)).fits());
    CHECK(!lightgrid::build(gridInput(257, 1)).fits());

    // From a lot: an upper tile counts as floor when it or the tile above has floor; walls come
    // from the room wall graph, fences and bad points left out.
    ts2::content::Lot lot;
    ts2::array3d::Array3D floors;
    floors.width = 3;
    floors.height = 1;
    floors.levels = 3;
    floors.elementSize = 8;
    floors.elements.assign(3 * 3 * 8, 0);
    floors.elements[(1 * 3 + 0) * 8] = 5;  // level 1, tile 0
    floors.elements[(2 * 3 + 1) * 8 + 6] = 5;  // level 2, tile 1, one quarter
    lot.grids[ts2::content::kFloorGridInstance] = floors;
    ts2::wallgraph::WallGraph graph;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    graph.points = {{1, 1, 0, 1}, {2, 1, 1, 1}, {3, 2, 0, 1}, {4, 2, 1, 1}, {5, nan, 0, 1}, {6, 0.5f, 1, 1}};
    graph.walls = {{1, 1, 0, 2, 0}, {2, 3, 0, 4, 0}, {3, 5, 0, 2, 0}, {4, 6, 0, 1, 0}};
    lot.roomGraph = graph;
    lot.wallCoverings = {{1, 1, 0, 0}, {2, 0x12345678, 0, 0}, {3, 1, 0, 0}, {4, 1, 0, 0}};
    const auto blocks = [](std::uint32_t covering) { return covering < 0x10000; };
    const auto walls = wallruns::wallsOf(lot, blocks);
    const auto ground = lightgrid::fromLot(lot, 0, walls);
    CHECK(ground.floor == std::vector<std::uint8_t>({1, 1, 1}));
    const auto upper = lightgrid::fromLot(lot, 1, walls);
    CHECK(upper.floor == std::vector<std::uint8_t>({1, 1, 0}));
    CHECK(upper.walls == std::vector<std::uint8_t>({lightgrid::kPlusX, lightgrid::kMinusX, 0}));
    CHECK(lightgrid::fromLot(lot, -1, walls).floor.empty());
}

void wallLightFadesAlongAndUp() {
    using namespace ts2::scene::walllight;
    const Pixel p = pack({0.25f, 0.5f, 1.0f});
    CHECK(p.r == 64 && p.g == 128 && p.b == 255 && p.a == 255);

    ts2::scene::LightSource lamp;
    lamp.color = {1, 1, 1};
    lamp.room = 1;
    lamp.x0 = lamp.x1 = 0.25f;
    lamp.y0 = lamp.y1 = -1;  // one tile in front of the first half

    auto w = testWall();
    std::set<int> changed;
    resetToAmbient(w);
    lightFromSources(w, {lamp}, nullptr, changed);
    // 1 x (1 / 1.4)^2 at two half-tiles' distance; a step along fades by 1 / 1.4 in fixed point.
    const auto& s0 = w.runs[0].segments[0];
    CHECK(changed.count(0) == 1 && w.runs[0].lamplit);
    CHECK(s0.halves[0][2].r == 131 && s0.halves[0][0].r == 93);
    CHECK(s0.halves[1][2].r == 93 && s0.halves[1][5].r == 66);

    // Behind the face, or in another room: nothing.
    w = testWall();
    changed.clear();
    ts2::scene::LightSource behind = lamp;
    behind.y0 = behind.y1 = 1;
    ts2::scene::LightSource elsewhere = lamp;
    elsewhere.room = 2;
    resetToAmbient(w);
    lightFromSources(w, {behind, elsewhere}, nullptr, changed);
    CHECK(changed.empty() && w.runs[0].segments[0].halves[0][2].r == 0);

    // The floor in front lights a dark wall at half its share.
    w = testWall();
    changed.clear();
    resetToAmbient(w);
    lightFromFloor(w, [](float, float, int) { return std::array<float, 3>{0.5f, 0.5f, 0.5f}; }, changed);
    CHECK(changed.count(0) == 1 && w.runs[0].segments[1].halves[1][3].r == 32);  // 0.5 x 0.5 x 0.5

    // A wall linked to itself above (bad data) still finishes.
    w = testWall();
    w.runs[0].segments[0].up = {0, 0};
    changed.clear();
    resetToAmbient(w);
    lightFromSources(w, {lamp}, nullptr, changed);
    spread(w, changed);
    CHECK(w.runs[0].segments[1].halves[1][2].r > 0);
}

void roomLightMatchesTheGame() {
    using namespace ts2::scene::roomlight;
    // A room with no light gets the unlit colour on its sides and floor (a captured room).
    std::array<std::array<float, 3>, 6> samples{};
    addUnlitLight(samples, {0.45f, 0.425f, 0.4f}, 0.4f, false);
    CHECK(samples[0][0] == 0.45f && samples[4][2] == 0.4f && samples[5][0] == 0);
    // Without a threshold, only rooms no lamp or window lights get it, all of it.
    std::array<std::array<float, 3>, 6> lit{}, unlit{};
    addUnlitLight(lit, {0.45f, 0.425f, 0.4f}, 0, true);
    addUnlitLight(unlit, {0.45f, 0.425f, 0.4f}, 0, false);
    CHECK(lit[0][0] == 0 && unlit[0][0] == 0.45f);

    Settings object;
    object.overall = 0.5f;
    object.directionalBoost = 4;
    const auto l = lightsFor(samples, object);
    CHECK(l && l->directionals.size() == 2);
    CHECK(std::abs(l->ambient[0] - 0.444375f) < 1e-5f);
    CHECK(std::abs(l->directionals[0].color[0] - 0.3825f) < 1e-5f);  // (0.54 - 0.444375) x 4
    CHECK(l->directionals[0].direction == (std::array<float, 3>{1, 0, -0.75f}));
    CHECK(l->directionals[1].direction == (std::array<float, 3>{-1, 0, -0.75f}));
    CHECK(std::abs(average(samples)[0] - 0.375f) < 1e-6f);

    Settings five = object;
    five.type = 0;
    CHECK(!lightsFor(samples, five));

    // A window facing +x and a little +y feeds those two sides by how much it faces them.
    Samples s;
    s.addFacing(0.8f, 0.6f, {1, 1, 1});
    const auto a = s.averages();
    CHECK(a[kPlusX][0] == 1 && a[kPlusY][0] == 1 && a[kMinusX][0] == 0 && s.weight[kPlusX] == 0.8f);
}

void windowLightMatchesTheGame() {
    using namespace ts2::scene::windowlight;
    // The captured day state of a lot.
    Daylight day;
    day.sunColor = {0.97f, 0.94f, 0.83f};
    day.sunStrength = 2;
    day.sunDirection = {0.54091f, 0.43273f, 0.72122f};
    day.sky = {0.5f, 0.5f, 0.5f};
    day.insidePortals = {0.5f, 1.3f, 0.7f, 1};
    day.insidePortalSpills = {0.9f, 0.4f, 0.8f, 0.9f};

    // Two windows a tile apart on the same wall take 0.2 x (1 - 1/4) of each other's light.
    Portal w;
    w.otherRoom = 1;
    w.nx = -1;
    w.y1 = 1;
    w.size = 1.5f;
    w.tint = {1, 0.99362f, 0.772f};
    w.source.color = {1.25f, 1.25f, 1.25f};
    std::vector<Portal> pair{w, w};
    pair[1].y0 = 1;
    pair[1].y1 = 2;
    suppress(pair, {});
    CHECK(std::abs(pair[0].suppression - 0.85f) < 1e-6f && std::abs(pair[1].suppression - 0.85f) < 1e-6f);

    // Looking out from the outdoors into room 1, against the sun's x.
    const auto l = lightsFor(pair[0], true, true, day, {0, 0, 0});
    CHECK(l && l->spill && l->sources.size() == 2);
    const float sun = 0.54091f;  // how much the window faces the sun
    CHECK(std::abs(l->beam.color[0] - 1.94f * 0.5f * 0.85f * 1.5f * sun) < 1e-5f && l->beam.falloff == 1.3f);
    CHECK(std::abs(l->spill->color[2] - 0.5f * 0.772f * 0.9f * 0.85f * 1.5f) < 1e-5f && l->spill->falloff == 0.4f);
    CHECK(std::abs(l->sources[0].color[0] - 1.25f * 1.94f * 0.7f * sun) < 1e-5f);  // the sun beam on floors and walls
    CHECK(std::abs(l->sources[1].color[0] - 1.25f * 0.5f * 0.8f) < 1e-5f && std::abs(l->sources[1].falloff - 0.9f) < 1e-6f);

    // A door lets no sun beam through (doorOrArchDirectMultiplier 0), and without sun nothing is supported.
    Portal door = pair[0];
    door.doorOrArch = true;
    const auto d = lightsFor(door, true, true, day, {0, 0, 0});
    CHECK(d && d->beam.color[0] == 0 && d->sources.size() == 1);
    CHECK(!lightsFor(pair[0], true, false, day, {0, 0, 0}));
}

void wallRunsAreLaidOutAsTheGame() {
    using namespace ts2::scene;
    // A room two tiles by one, the same on two levels.
    std::vector<wallruns::Wall> walls;
    for (int level = 0; level < 2; ++level) {
        walls.push_back({level, 0, 0, 2, 0});
        walls.push_back({level, 2, 0, 2, 1});
        walls.push_back({level, 2, 1, 0, 1});
        walls.push_back({level, 0, 1, 0, 0});
    }
    const auto roomAt = [](int, float x, float y) { return static_cast<std::int16_t>(x > 0 && x < 2 && y > 0 && y < 1 ? 1 : 0); };
    auto layout = wallruns::build(walls, roomAt, [](int, const wallruns::Key&) { return false; });
    CHECK(layout.complete);
    // Inside and outside of each level, one run per straight side.
    CHECK(layout.runs.size() == 16);
    int inside = 0, segments = 0, linked = 0;
    for (std::size_t r = 0; r < layout.runs.size(); ++r) {
        const auto& run = layout.runs[r];
        inside += run.room == 1;
        for (std::size_t s = 0; s < run.segments.size(); ++s) {
            ++segments;
            const auto& seg = run.segments[s];
            const auto& link = run.level == 0 ? seg.up : seg.down;
            CHECK((run.level == 0 ? seg.down : seg.up).run < 0);
            if (link.run >= 0) {
                ++linked;
                const auto& other = layout.keys[static_cast<std::size_t>(link.run)][static_cast<std::size_t>(link.segment)];
                auto same = layout.keys[r][s];
                same.level = other.level;
                CHECK(other == same && other.level != run.level);
            }
        }
    }
    CHECK(inside == 8 && segments == 24 && linked == 24);
    // The face along y = 0 looking into the room names tile (0, 0) from its -y side, and is walked
    // so that it looks to its right.
    bool found = false;
    for (std::size_t r = 0; r < layout.runs.size(); ++r) {
        for (std::size_t s = 0; s < layout.keys[r].size(); ++s) {
            if (layout.keys[r][s] == wallruns::Key{0, 0, 0, 1, 0}) {
                found = true;
                const auto& seg = layout.runs[r].segments[s];
                CHECK(seg.y0 == 0 && seg.y1 == 0 && seg.x1 < seg.x0 && layout.runs[r].room == 1);
            }
        }
    }
    CHECK(found);
    // Solid floor between the levels: no links.
    layout = wallruns::build(walls, roomAt, [](int, const wallruns::Key&) { return true; });
    for (const auto& run : layout.runs) {
        for (const auto& seg : run.segments) {
            CHECK(seg.up.run < 0 && seg.down.run < 0);
        }
    }

    // A lone diagonal wall: its two faces, one run each, naming the halves of the tile.
    layout = wallruns::build({{0, 0, 0, 1, 1}}, roomAt, nullptr);
    CHECK(layout.runs.size() == 2 && layout.runs[0].diagonal && layout.runs[1].diagonal);
    std::set<int> parts;
    for (const auto& keys : layout.keys) {
        CHECK(keys.size() == 1 && keys[0].side == 0x20);
        parts.insert(keys[0].part);
        CHECK(wallruns::quarters(keys[0]) == (keys[0].part == 1 ? 0xC : 3));
    }
    CHECK(parts == std::set<int>({1, 3}));

    // Bad walls are left out; too many stop at the limit.
    layout = wallruns::build({{0, -1, 0, 1, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 2, 1}, {0, 0, 0, 100000, 0}, {1000, 0, 0, 1, 0}}, roomAt, nullptr);
    CHECK(layout.runs.empty());
    std::vector<wallruns::Wall> many;
    for (int y = 0; y < 200; ++y) {
        many.push_back({0, 0, y, 20, y});
    }
    layout = wallruns::build(many, roomAt, nullptr);
    std::size_t faces = 0;
    for (const auto& run : layout.runs) {
        faces += run.segments.size();
    }
    CHECK(!layout.complete && faces <= 4096);

    // The wall light a portal reads: its face's texel in the second half, fourth row, over 255;
    // outdoors with the outside ambient taken off; black where no face is.
    layout = wallruns::build({{0, 0, 0, 1, 0}}, [](int, float, float y) { return static_cast<std::int16_t>(y > 0 ? 4 : 0); }, nullptr);
    CHECK(layout.runs.size() == 2);
    for (auto& run : layout.runs) {
        run.segments[0].halves[1][3] = {10, 20, 64, 255};
    }
    const std::size_t roomFace = layout.runs[0].room == 4 ? 0 : 1;
    const auto faceIn = layout.keys[roomFace][0], faceOut = layout.keys[1 - roomFace][0];
    const std::array<float, 3> white{1, 1, 1};
    auto in = wallruns::portalSample(layout.runs, layout, faceIn, true, white, 0.25f);
    CHECK(in[0] == static_cast<float>(64 * static_cast<double>(std::bit_cast<float>(0x3B808081u))) && in[2] == static_cast<float>(10 * static_cast<double>(std::bit_cast<float>(0x3B808081u))));
    auto out = wallruns::portalSample(layout.runs, layout, faceOut, true, white, 0.25f);
    CHECK(std::abs(out[0] - (64.0f / 255 - 0.25f)) < 1e-6f);
    CHECK(wallruns::portalSample(layout.runs, layout, faceOut, false, white, 0.25f) == in);
    auto missing = faceIn;
    missing.x = 40;
    CHECK((wallruns::portalSample(layout.runs, layout, missing, true, white, 0.25f) == std::array<float, 3>{0, 0, 0}));
}

void wallTexelsAndCornersAreFinishedAsTheGame() {
    using namespace ts2::scene;
    using walllight::Pixel;
    // A room two tiles by one on two levels: each loop's runs follow each other round it.
    std::vector<wallruns::Wall> walls;
    for (int level = 0; level < 2; ++level) {
        walls.push_back({level, 0, 0, 2, 0});
        walls.push_back({level, 2, 0, 2, 1});
        walls.push_back({level, 2, 1, 0, 1});
        walls.push_back({level, 0, 1, 0, 0});
    }
    const auto roomAt = [](int, float x, float y) { return static_cast<std::int16_t>(x > 0 && x < 2 && y > 0 && y < 1 ? 1 : 0); };
    auto layout = wallruns::build(walls, roomAt, [](int, const wallruns::Key&) { return false; });
    bool cycles = true;
    for (std::size_t r = 0; r < layout.runs.size(); ++r) {
        const auto& run = layout.runs[r];
        const auto& s = run.segments.front();
        cycles = cycles && run.next >= 0 && layout.runs[static_cast<std::size_t>(run.next)].previous == static_cast<int>(r) &&
                 run.angle == static_cast<float>(std::atan2(static_cast<double>(s.y1) - s.y0, static_cast<double>(s.x1) - s.x0));
    }
    CHECK(cycles);
    // Inside each room the loop turns right four times; the game misses the turn from -pi/2 to pi
    // (its angle test does not wrap), so three corners per level, none outside.
    std::vector<std::vector<lightcorners::Tile>> tiles;
    for (const auto& keys : layout.keys) {
        tiles.emplace_back();
        for (const auto& k : keys) tiles.back().push_back({k.x, k.y, k.level});
    }
    walllight::Walls lit;
    lit.runs = layout.runs;
    CHECK(lightcorners::corners(lit, tiles).size() == 6);
    for (auto& run : lit.runs) run.lamplit = true;
    CHECK(lightcorners::corners(lit, tiles).empty());  // both runs lamp-lit: no corner

    // Texels: each segment's halves with its neighbours' edges, the level above's bottom row on top.
    for (std::size_t r = 0; r < lit.runs.size(); ++r) {
        for (std::size_t s = 0; s < lit.runs[r].segments.size(); ++s) {
            for (std::size_t h = 0; h < 2; ++h) {
                for (std::size_t row = 0; row < 6; ++row) {
                    lit.runs[r].segments[s].halves[h][row] = {static_cast<std::uint8_t>(row), static_cast<std::uint8_t>(h), static_cast<std::uint8_t>(s),
                                                              static_cast<std::uint8_t>(r)};
                }
            }
        }
    }
    walllight::pad(lit);
    bool padded = true;
    int seen = 0;
    for (const auto& run : lit.runs) {
        if (run.level != 0 || run.segments.size() != 2 || run.segments[0].up.run < 0) continue;
        ++seen;
        const auto& first = run.segments[0];
        const auto& second = run.segments[1];
        const auto& aboveRun = lit.runs[static_cast<std::size_t>(first.up.run)];
        const auto at = static_cast<std::size_t>(first.up.segment);
        const auto& above = aboveRun.segments[at];
        const auto& corner = at > 0 ? aboveRun.segments[at - 1].halves[1][5] : above.halves[0][5];
        padded = padded && first.texels[1][3] == first.halves[0][2] && first.texels[3][3] == second.halves[0][2] &&
                 first.texels[0][3] == first.halves[0][2] &&            // no previous segment: its own edge
                 first.texels[1][0] == above.halves[0][5] &&            // the level above's bottom row
                 first.texels[1][7] == first.halves[0][5] &&            // nothing below: its own bottom row
                 first.texels[0][0] == corner;
    }
    CHECK(seen > 0 && padded);

    // A lamp-lit run turning a right angle into an unlit one: its last columns are pulled toward
    // the ambient (texel values from the game's walls on the Caliente lot).
    walllight::Walls corner;
    corner.insideAmbient = {0.00085f, 0.00075f, 0.00075f};
    corner.runs.resize(2);
    corner.runs[0].room = corner.runs[1].room = 4;
    corner.runs[0].lamplit = true;
    corner.runs[0].angle = std::bit_cast<float>(0x40490FDBu);  // pi
    corner.runs[1].angle = std::bit_cast<float>(0x3FC90FDBu);  // pi / 2
    corner.runs[0].next = 1;
    corner.runs[1].previous = 0;
    corner.runs[0].segments.resize(2);
    corner.runs[1].segments.resize(1);
    for (auto& s : corner.runs[0].segments) {
        for (auto& column : s.texels) column.fill({4, 4, 5, 255});
    }
    corner.runs[0].segments[1].texels[3].fill({3, 3, 4, 255});
    const auto untouched = corner.runs[1].segments[0].texels;
    lightcorners::blendWalls(corner);
    const auto& end = corner.runs[0].segments[1].texels;
    const auto& start = corner.runs[0].segments[0].texels;
    CHECK((end[3][3] == Pixel{1, 1, 1, 255}));  // 80%
    CHECK((end[1][3] == Pixel{3, 3, 4, 255}));  // 40%
    CHECK((start[3][3] == Pixel{3, 3, 4, 255}) && (start[2][3] == Pixel{4, 4, 5, 255}) && (end[0][3] == Pixel{4, 4, 5, 255}));
    CHECK(corner.runs[1].segments[0].texels == untouched);

    // Floor cells inside a corner are pulled toward the ambient, the nearest most. The game skips
    // the nearest seven cells without floor but blends the farther ones regardless.
    floorlight::Grid grid;
    grid.width = grid.height = 2;
    grid.cells.resize(16);
    grid.tiles.resize(4);
    for (auto& c : grid.cells) {
        for (auto& h : c.halves) {
            h.valid = true;
            h.light = {1, 1, 1};
        }
    }
    grid.at(3, 3)->halves[0].valid = false;  // nearest cell, no floor: left alone
    grid.at(0, 2)->halves[0].valid = false;  // eighth cell, no floor: blended anyway
    lightcorners::blendFloor(grid, {{1, 1, 0, 1, false}});
    const float w1 = std::bit_cast<float>(0x3F3DBDBEu), w7 = std::bit_cast<float>(0x3DA0A0A1u);
    CHECK(grid.at(3, 3)->halves[0].light[0] == 1);
    CHECK(grid.at(2, 3)->halves[0].light[0] == static_cast<float>(1.0 - w1));
    CHECK(grid.at(0, 2)->halves[0].light[0] == static_cast<float>(1.0 - w7));
    CHECK(grid.at(0, 0)->halves[0].light[0] == 1 && grid.at(2, 3)->halves[1].light[0] == 1);
    // A short run: two by two cells, both halves; other levels and even directions are left alone.
    lightcorners::blendFloor(grid, {{1, 1, 0, 1, true}, {1, 1, 1, 1, false}, {1, 1, 0, 2, false}});
    CHECK(grid.at(2, 3)->halves[1].light[0] == static_cast<float>(1.0 - std::bit_cast<float>(0x3E909091u)));
    // Far-off or broken corners do nothing.
    const auto before = grid.cells;
    lightcorners::blendFloor(grid, {{std::numeric_limits<int>::max(), 0, 0, 1, false}, {-5, -5, 0, 3, false}});
    bool same = true;
    for (std::size_t i = 0; i < grid.cells.size(); ++i) same = same && grid.cells[i].halves[0].light == before[i].halves[0].light;
    CHECK(same);
}

void lightFilesAreReadAsTheGame() {
    // Macros, overrides by light name in any case, conditions and state modifiers.
    const auto l = ts2::content::readLightingText(
        "seti lightsOff 0\n"
        "define Four(lightName)\n"
        "  lightAttribOverride &lightName diffusionSourceIntensity 1   # four\n"
        "  lightAttribOverride &{lightName} diffusionSourceFloorIntensityScale .65\n"
        "enddef\n"
        "define Amber(lightName)\n"
        "  lightAttribOverride &lightName diffusionSourceColor (1,0.85,0.7)\n"
        "  lightAttribOverride &lightName color(.5,.6,.7)\n"
        "enddef\n"
        "create Four(MyLamp_light)\n"
        "create Amber (MyLamp_light)\n"
        "lightAttribOverride MyLamp_light diffusionSourceMin (0,.2)\n"
        "lightAttribOverride MyLamp_light diffusionSourceIsDirectional 1\n"
        "if ($lightsOff)\n"
        "  create Four(Hidden_light)\n"
        "else\n"
        "  create Four(Shown_light)\n"
        "endif\n"
        "lightingState day\n"
        "  modifiers insideObjects .8 .7 .35 .5\n"
        "  diffParam insideAmbientColor (.85, .75, .75)\n"
        "end\n"
        "lightingState dusk : day\n"
        "  modifiers insideObjects 2 3\n"
        "end\n"
        "include \"elsewhere.txt\"\n");
    const auto* lamp = l.light("mylamp_LIGHT");
    CHECK(lamp != nullptr);
    if (lamp) {
        CHECK(lamp->diffusionIntensity == 1.0f && lamp->diffusionFloorScale == 0.65f);
        CHECK((lamp->diffusionColor == std::array<float, 3>{1, 0.85f, 0.7f}));
        CHECK((lamp->color == std::array<float, 3>{0.5f, 0.6f, 0.7f}));
        CHECK((lamp->diffusionMin == std::array<float, 2>{0, 0.2f} && lamp->diffusionDirectional == true));
        CHECK(!lamp->diffusionFalloff);
    }
    CHECK(l.light("hidden_light") == nullptr && l.light("shown_light") != nullptr);
    const auto& day = l.states.at("day").modifiers.at("insideObjects");
    CHECK(day.practicalStrength == 0.8f && day.diffusionStrength == 0.7f && day.practicalFalloff == 0.35f && day.diffusionFalloff == 0.5f);
    const auto& dusk = l.states.at("dusk").modifiers.at("insideObjects");
    CHECK(dusk.practicalStrength == 2 && dusk.diffusionStrength == 3 && dusk.diffusionFalloff == 0.5f);
    CHECK((l.states.at("day").diffParams.at("insideAmbientColor") == std::vector<float>{0.85f, 0.75f, 0.75f}));

    // Macros that create themselves or multiply stop at the limits instead of running away.
    const auto bomb = ts2::content::readLightingText(
        "define A(x)\n create A(&x)\n create A(&x)\n lightAttribOverride &x intensity 1\nenddef\n"
        "create A(boom_light)\n");
    CHECK(bomb.light("boom_light") != nullptr);
}

void lampsGiveTheGamesRecords() {
    using namespace ts2::scene;
    // A ceiling lamp of macro Six in the day state: 1.5 x .7 and falloff 1 x .5, as captured.
    ts2::content::Lighting::LightAttributes a;
    a.diffusionIntensity = 1.5f;
    a.diffusionColor = std::array<float, 3>{1, 0.86f, 0.75f};
    a.diffusionFloorScale = 0.65f;
    ts2::content::Lighting::Modifier inside{0.8f, 0.7f, 0.35f, 0.5f};
    const auto settings = lamplights::scaled(lamplights::settingsFor(&a), inside, 1);
    CHECK(settings.intensity == 1.05f && settings.falloff == 0.5f && settings.interFloorFalloff == 0.5f);
    CHECK(lamplights::settingsFor(nullptr).intensity == 0);

    lamplights::LotQueries lot;
    lot.highestLevel = 1;
    lot.floorHeight = [](int level, float, float) { return 3.0f * static_cast<float>(level); };
    lot.tileOnLot = [](int x, int y, int level) { return x >= 0 && y >= 0 && x < 20 && y < 20 && level >= 0 && level <= 1; };
    float roomX = -1, roomY = -1;
    lot.roomAt = [&](float x, float y, int level) {
        roomX = x;
        roomY = y;
        return static_cast<std::int16_t>(level + 2);
    };
    // Hanging from the ceiling of level 1, facing +x: its -z points down, so its +y gives the facing.
    lamplights::Placement p;
    p.position = {8.5f, 8.5f, 5.7f};
    p.rotation = {0, 0, -0.70710678f, 0.70710678f};
    const auto s = lamplights::sourceFor(settings, p, lot);
    CHECK(s.has_value());
    if (s) {
        CHECK(s->level == 1 && s->room == 3);
        CHECK(std::abs(s->angle) < 1e-6f);
        CHECK(s->x0 == 8 && s->y0 == 8 && s->x1 == 9 && s->y1 == 9);
        CHECK(std::abs(s->color[0] - 1.05f) < 1e-6f && s->floorIntensity == 0.65f && !s->directional);
        CHECK(roomX == 8.5f && roomY == 8.5f);
    }
    // A window-style light (its area wholly ahead) takes its room in front of it.
    auto ahead = settings;
    ahead.min = {0, 0.2f};
    ahead.max = {0, 2};
    const auto w = lamplights::sourceFor(ahead, p, lot);
    CHECK(w.has_value() && std::abs(roomX - 8.7f) < 1e-5f && std::abs(roomY - 8.5f) < 1e-5f);
    if (w) {
        CHECK(std::abs(w->x0 - 8.7f) < 1e-5f && std::abs(w->x1 - 10.5f) < 1e-5f && w->y0 == 8.5f && w->y1 == 8.5f);
    }
    // No colour, off the lot or nonsense positions: no record.
    auto dark = settings;
    dark.intensity = 0;
    CHECK(!lamplights::sourceFor(dark, p, lot));
    auto off = p;
    off.position = {-3, 4, 1};
    CHECK(!lamplights::sourceFor(settings, off, lot));
    off.position = {std::numeric_limits<float>::quiet_NaN(), 4, 1};
    CHECK(!lamplights::sourceFor(settings, off, lot));
    off.position = {1e30f, 4, 1};
    CHECK(!lamplights::sourceFor(settings, off, lot));

    // The game's turn for each saved facing, to the bit (its sign form included).
    const std::uint32_t turns[8][2] = {{0x3F3504F3, 0x3F3504F3}, {0x3EC3EF16, 0x3F6C835E}, {0, 0x3F800000}, {0x3EC3EF13, 0xBF6C835F},
                                       {0x3F3504F2, 0xBF3504F4}, {0x3F6C835E, 0xBEC3EF18}, {0x3F800000, 0xB33BBD2E}, {0x3F6C835E, 0x3EC3EF15}};
    for (int d = 0; d < 8; ++d) {
        const auto q = lamplights::facingRotation(d);
        CHECK(std::bit_cast<std::uint32_t>(q[2]) == turns[d][0] && std::bit_cast<std::uint32_t>(q[3]) == turns[d][1]);
        CHECK(q[0] == 0 && q[1] == 0);
    }

    // A model's light nodes, placed through the nodes above them, then where the object stands.
    ts2::scenegraph::ResourceNode model;
    model.nodes.resize(3);
    model.nodes[0].kind = ts2::scenegraph::SceneNode::Kind::Root;
    model.nodes[0].children = {1};
    model.nodes[1].kind = ts2::scenegraph::SceneNode::Kind::Transform;
    model.nodes[1].translation = {0, 0, 2};
    model.nodes[1].children = {2, 1, 7};  // itself again and a missing node are ignored
    model.nodes[2].kind = ts2::scenegraph::SceneNode::Kind::LightRef;
    model.nodes[2].name = "lamp_light";
    model.nodes[2].translation = {0, 0.5f, 0};
    const auto lights = lamplights::lightNodes(model, {{3, 4, 0}, lamplights::facingRotation(0)});  // turned a quarter
    CHECK(lights.size() == 1 && lights[0].name == "lamp_light");
    if (!lights.empty()) {
        const auto& at = lights[0].place;
        CHECK(std::abs(at.position[0] - 2.5f) < 1e-5f && std::abs(at.position[1] - 4) < 1e-5f && std::abs(at.position[2] - 2) < 1e-6f);
    }
}

void portalsAreBuiltAsTheGame() {
    using namespace ts2::scene;
    // A wall along x = 2 on the ground floor; west of it room 1, east room 2.
    portalrecords::LotQueries lot;
    lot.lamps.highestLevel = 1;
    lot.lamps.floorHeight = [](int level, float, float) { return 3.0f * static_cast<float>(level); };
    lot.lamps.tileOnLot = [](int x, int y, int level) { return x >= 0 && y >= 0 && x < 8 && y < 8 && level >= 0 && level <= 1; };
    lot.lamps.roomAt = [](float x, float, int) { return static_cast<std::int16_t>(x < 2 ? 1 : 2); };
    lot.inLot = [](float x, float y) { return x >= 0 && y >= 0 && x < 8 && y < 8; };
    lot.wallAt = [](const portalrecords::Edge& e) { return e.x0 == 2 && e.x1 == 2 && e.level == 0; };
    lot.roomInFront = [](const portalrecords::Edge& e) { return static_cast<std::int16_t>(e.y1 > e.y0 ? 2 : 1); };  // normal (dy, -dx)
    lot.roomLit = [](std::int16_t) { return true; };

    // A window light just west of the wall shining east (its -z turned to +x), and one further
    // along the wall, three tiles on.
    portalrecords::PortalObject window;
    window.edge = {2, 0, 0, 2, 1};
    portalrecords::PortalLight light;
    light.place.position = {1.9f, 0.5f, 1.5f};
    light.place.rotation = {0, -0.70710678f, 0, 0.70710678f};
    light.hasLight = true;
    light.intensity = 1.5f;
    light.color = {1, 0.99362f, 0.772f};
    light.falloffRate = 1;
    light.settings.intensity = 1.25f;
    light.settings.min = {0, 0.2f};
    light.settings.max = {0, 2};
    window.lights.push_back(light);
    light.place.position[1] = 3.5f;
    window.lights.push_back(light);
    const auto built = portalrecords::build(window, lot, 0.1f);
    CHECK(built.size() == 2);
    if (built.size() == 2) {
        const auto& b = built[0];
        CHECK(b.room == 1 && b.portal.otherRoom == 2);
        CHECK(b.portal.nx == 1 && b.portal.ny == 0);
        CHECK(b.portal.x0 == 2 && b.portal.x1 == 2 && b.portal.y1 - b.portal.y0 == -1);  // walked with its normal into room 1
        CHECK(b.portal.height == 0 && b.portal.size == 1.5f && b.portal.tint[2] == 0.772f && b.portal.reach == 1);
        CHECK(std::abs(b.spillPoint[0] - 2.1f) < 1e-6f && b.spillPoint[1] == 0.5f && b.spillPoint[2] == 1.5f);
        CHECK(std::abs(b.portal.source.color[0] - 1.25f) < 1e-6f && std::abs(b.portal.source.x0 - 2.1f) < 1e-5f);
        // The second light is moved onto its own tile of the wall.
        CHECK(std::min(built[1].portal.y0, built[1].portal.y1) == 3 && built[1].spillPoint[1] == 3.5f);
    }

    // No light resource: no size, a white tint; a light facing along the wall: no portal.
    window.lights.resize(1);
    window.lights[0].hasLight = false;
    auto plain = portalrecords::build(window, lot, 0.1f);
    CHECK(plain.size() == 1 && plain[0].portal.size == 0 && plain[0].portal.tint[2] == 1);
    window.lights[0].place.rotation = {0.70710678f, 0, 0, 0.70710678f};  // -z turned to +y
    CHECK(portalrecords::build(window, lot, 0.1f).empty());

    // Rooms not lit to 100, no wall at the light's tile, or broken positions: nothing.
    window.lights[0].place.rotation = {0, -0.70710678f, 0, 0.70710678f};
    lot.roomLit = [](std::int16_t room) { return room != 2; };
    CHECK(portalrecords::build(window, lot, 0.1f).empty());
    lot.roomLit = nullptr;
    window.lights[0].place.position = {1.9f, 0.5f, -1};
    CHECK(portalrecords::build(window, lot, 0.1f).empty());
    window.lights[0].place.position = {std::numeric_limits<float>::quiet_NaN(), 0.5f, 1.5f};
    CHECK(portalrecords::build(window, lot, 0.1f).empty());
    window.lights[0].place.position = {1.9f, 1e30f, 1.5f};
    CHECK(portalrecords::build(window, lot, 0.1f).empty());
    lot.wallAt = [](const portalrecords::Edge&) { return false; };
    window.lights[0].place.position = {1.9f, 0.5f, 1.5f};
    CHECK(portalrecords::build(window, lot, 0.1f).empty());
}

void roomSamplesAreGatheredAsTheGame() {
    using namespace ts2::scene;
    // The game's name hashes and thumbnail average.
    CHECK(ts2::content::nameHash32("TEST") == 0xCC743053u && ts2::content::nameHash24("test") == 0xF86ED0u);
    const std::vector<std::uint8_t> pixels{255, 0, 0, 255, 0, 255, 0, 0, 0, 0, 255, 255};  // the green one is transparent
    const auto avg = ts2::content::averageColor(pixels);
    CHECK(std::abs(avg[0] - 0.5f) < 1e-6f && avg[1] == 0 && std::abs(avg[2] - 0.5f) < 1e-6f);
    CHECK((ts2::content::averageColor({}) == std::array<float, 3>{0, 0, 0}));
    ts2::content::CoveringColors none{std::filesystem::path{}};
    CHECK(!none.color(ts2::content::CoveringKind::Floor, "abc"));

    // A lot two tiles wide with one floor covering, a wall between the tiles, and a portal.
    ts2::content::Lot lot;
    ts2::array3d::Array3D floors;
    floors.width = 2;
    floors.height = 1;
    floors.levels = 1;
    floors.elementSize = 8;
    floors.elements.assign(2 * 8, 0);
    for (std::size_t q = 0; q < 4; ++q) floors.elements[q * 2] = 1;  // tile 0 floored, tile 1 bare
    lot.grids[ts2::content::kFloorGridInstance] = floors;
    lot.floorPatterns[1] = {"abc", 1};
    lot.wallPatterns[2] = {"def", 1};
    ts2::wallgraph::WallGraph graph;
    graph.points = {{1, 1, 0, 0}, {2, 1, 1, 0}};
    graph.walls = {{7, 1, 0, 2, 0}, {8, 2, 0, 1, 0}};  // the same wall twice: counted once
    lot.roomGraph = graph;
    lot.wallCoverings = {{7, 1, 2, 2}};
    roomsamples::LotInputs in;
    in.quarterRoom = [](int, int x, int, int) { return static_cast<std::int16_t>(x == 0 ? 1 : 2); };
    in.faceRoom = [](const wallruns::Key& k) { return static_cast<std::int16_t>(k.side == 8 ? 1 : 2); };
    in.covering = [](bool floor, const std::string&) -> std::optional<roomsamples::Color> {
        if (floor) return roomsamples::Color{1, 1, 1};
        return std::nullopt;  // the wall's covering has no thumbnail: grey
    };
    in.floorLight = [](int, float, float) { return roomsamples::Color{0.8f, 0.8f, 0.8f}; };
    in.wallLight = [](const wallruns::Key&) { return roomsamples::Color{0.4f, 0.4f, 0.4f}; };
    const auto rooms = roomsamples::gather(lot, in, {{2, 1, 0, {0.2f, 0.2f, 0.2f}}, {0, 1, 0, {9, 9, 9}}}, roomsamples::Settings{});
    CHECK(rooms.count(1) && rooms.count(2));
    if (rooms.count(1) && rooms.count(2)) {
        const auto& one = rooms.at(1);
        CHECK(one.weight[roomlight::kFromFloor] == 4 && one.weight[roomlight::kFromCeiling] == 4);
        CHECK(std::abs(one.sum[roomlight::kFromFloor][0] - 4 * 0.2f) < 1e-6f);  // white floor x 0.25 x 0.8
        // The wall face looking into room 1 (-x) shows its grey covering half lit: 0.2 x 0.75.
        CHECK(one.weight[roomlight::kMinusX] == 1 && std::abs(one.sum[roomlight::kMinusX][0] - 0.15f) < 1e-6f);
        const auto& two = rooms.at(2);
        CHECK(two.weight[roomlight::kFromFloor] == 0);  // its tile is bare
        CHECK(two.weight[roomlight::kPlusX] == 2);       // the wall face and the portal
        CHECK(std::abs(two.sum[roomlight::kPlusX][0] - (0.15f + 0.15f)) < 1e-6f);
    }
    // Broken wall points and missing inputs are left out.
    lot.roomGraph->points[0].x = std::numeric_limits<float>::quiet_NaN();
    CHECK(roomsamples::gather(lot, in, {}, roomsamples::Settings{}).at(1).weight[roomlight::kMinusX] == 0);
    CHECK(roomsamples::gather(lot, roomsamples::LotInputs{}, {}, roomsamples::Settings{}).empty());
}

void lotLightingIsWorkedOutAsTheGame() {
    using namespace ts2::scene;
    // Settings: inside ambients, colour and thresholds from the state, outside ones always from the
    // global settings; room settings by group, else the plain name; the last unlit room line wins.
    const auto lighting = ts2::content::readLightingText(
        "set outsideFloorAmbient 0.25\n"
        "set outsideDiffusionAmbientColor (1, 0.5, 1)\n"
        "set insideFloorAmbient 0.5\n"
        "set roomOverallStrength 0.4\n"
        "set object:roomOverallStrength 0.5\n"
        "set portalSuppressionRadius 3\n"
        "lightingState day\n"
        "  skylight (1,1,1) 0.5\n"
        "  light sun (1.5, 1.2, 2) (1, .94, .875) 2\n"
        "  diffParam insideFloorAmbient 0.001\n"
        "  diffParam outsideFloorAmbient 0.9\n"
        "  diffParam insideAmbientColor (.85, .75, .75)\n"
        "  diffParam floorThreshold .3\n"
        "  unlitRoomColor (.3, .3, .3) -threshold .3\n"
        "  unlitRoomColor (.45, .425, .4) -threshold .4\n"
        "  modifiers insidePortals 0.5 .7 1.3 1\n"
        "end\n");
    CHECK(!lotlighting::settingsFor(lighting, "night"));
    const auto s = lotlighting::settingsFor(lighting, "day");
    CHECK(s.has_value());
    if (!s) return;
    CHECK(s->insideFloorAmbient == 0.001f && s->outsideFloorAmbient == 0.25f && s->outsideColor[1] == 0.5f);
    CHECK(s->insideColor[0] == 0.85f && s->floorThreshold == 0.3f && s->wallThreshold == 0.4f);
    CHECK(s->sun && s->day.sky[0] == 0.5f && std::abs(s->day.sunDirection[2] - 2 / std::sqrt(1.5f * 1.5f + 1.2f * 1.2f + 4)) < 1e-6f);
    CHECK(s->day.insidePortals.practical == 0.5f && s->day.insidePortals.diffusion == 0.7f && s->day.insidePortals.practicalFalloff == 1.3f);
    CHECK(s->groups[lotlighting::kObjects].overall == 0.5f && s->groups[lotlighting::kSims].overall == 0.4f);
    CHECK(s->unlit.color[0] == 0.45f && s->unlit.threshold == 0.4f && s->suppression.radius == 3);

    // A lot two tiles wide, floored, with a lamp over the first tile in room 1; room 2 has no tiles.
    ts2::content::Lot lot;
    ts2::array3d::Array3D floors;
    floors.width = 2;
    floors.height = 1;
    floors.levels = 1;
    floors.elementSize = 8;
    floors.elements.assign(2 * 8, 0);
    for (std::size_t q = 0; q < 8; ++q) floors.elements[q * 2] = 1;
    lot.grids[ts2::content::kFloorGridInstance] = floors;
    lot.floorPatterns[1] = {"abc", 1};
    lot.roomGraph = ts2::wallgraph::WallGraph{};
    lotlighting::Inputs in;
    in.lot = &lot;
    LightSource lamp;
    lamp.color = {1, 1, 1};
    lamp.room = 1;
    lamp.x0 = 0;
    lamp.y0 = 0;
    lamp.x1 = 1;
    lamp.y1 = 1;
    lamp.floorIntensity = 0.65f;
    in.lamps = {lamp};
    in.roomAt = [](int, float, float) { return std::int16_t{1}; };
    in.quarterRoom = [](int, int, int, int) { return std::int16_t{1}; };
    in.covering = [](bool, const std::string&) { return std::optional<roomsamples::Color>(roomsamples::Color{1, 1, 1}); };
    in.roomCount = 3;
    const auto r = lotlighting::compute(in, *s);
    CHECK(r.complete && r.floors.size() == 1 && r.rooms.size() == 2);
    if (r.floors.size() == 1) {
        const auto& g = r.floors[0];
        CHECK(g.at(0, 0) && g.at(3, 0) && g.at(0, 0)->halves[0].light[0] > g.at(3, 0)->halves[0].light[0]);  // brighter under the lamp
        CHECK(g.at(3, 0) && g.at(3, 0)->halves[0].light[0] > s->insideFloorAmbient * s->insideColor[0]);    // and it spread
    }
    if (r.rooms.count(1) && r.rooms.count(2)) {
        CHECK(r.rooms.at(1).groups[lotlighting::kObjects].has_value());
        // Nothing reaches room 2: the whole unlit room light on its sides and floor, none from above.
        const auto& two = r.rooms.at(2).samples;
        CHECK(two[roomlight::kPlusX] == s->unlit.color && two[roomlight::kFromFloor] == s->unlit.color);
        CHECK((two[roomlight::kFromCeiling] == std::array<float, 3>{0, 0, 0}));
    }
    // Filling a scene's light maps: the floor image gets the texels (the lamp's tile brighter), a
    // map of the wrong size and a missing wall image are left alone.
    Scene scene;
    scene.images.resize(2);
    for (auto& image : scene.images) {
        image.width = 16;
        image.height = 8;
        image.rgba.assign(16 * 8 * 4, 128);
    }
    scene.images[1].width = 8;
    scene.lightMaps.emplace();
    scene.lightMaps->floors = {0, 1};
    lotlightmaps::fill(scene, r, *s);
    const auto& floorMap = scene.images[0].rgba;
    CHECK(floorMap[(3 * 16 + 3) * 4] > floorMap[(3 * 16 + 12) * 4] && floorMap[3] == 255 && scene.images[1].rgba[0] == 128);
    lotlightmaps::fill(scene, lotlighting::Result{}, *s);  // nothing worked out: nothing changes
    CHECK(scene.images[0].rgba == floorMap);

    // No lot, or no floors: nothing.
    CHECK(lotlighting::compute(lotlighting::Inputs{}, *s).floors.empty());
    ts2::content::Lot bare;
    in.lot = &bare;
    CHECK(lotlighting::compute(in, *s).rooms.empty());
}

void lotSunShadowIsCastAsTheGame() {
    using namespace ts2::scene;
    // A flat lot four tiles square with one wall along x = 2 (a storey high, to the floor above).
    lotshadow::Inputs in;
    in.width = in.height = 4;
    in.levels = 2;
    in.corner = [](int, int, int level) { return 3.0f * static_cast<float>(level); };
    in.floorAt = [](int, int, int, int) { return false; };
    in.walls = {{0, 2, 0, 2, 4}};
    const float l = std::sqrt(1.5f * 1.5f + 1.2f * 1.2f + 4.0f);
    const std::array<float, 3> sun{1.5f / l, 1.2f / l, 2 / l};
    const auto g = lotshadow::build(in, sun);
    CHECK(g.width == 32 && g.height == 32);
    // The wall's top goes into the column on its side away from the sun (x < 2) ...
    CHECK(g.heights[static_cast<std::size_t>(12 * 32 + 15)] == 3.0f && g.heights[static_cast<std::size_t>(12 * 32 + 16)] == 0.0f);
    // ... and its shadow falls that way, sinking a sixth of a unit a sample (0.125 x 2 / 1.5).
    CHECK(std::abs(g.shadowAt(1.5f, 1.6f) - (3.0f - 0.16667f * 3)) < 0.01f);
    CHECK(g.shadowAt(2.6f, 1.6f) <= 0.0f && g.shadowAt(-1, 0) == -1000.0f);  // the sunny side: no higher than the ground
    // Under the shadow the sun fades over a unit; above it, all of it.
    CHECK(lotshadow::sunVisible(-1, 1) == 1 && lotshadow::sunVisible(0.25f, 1) == 0.75f && lotshadow::sunVisible(2, 1) == 0);
    // The topmost floor over a quarter is its height; the ground stays below.
    in.walls.clear();
    in.floorAt = [](int x, int y, int level, int) { return level == 1 && x == 1 && y == 1; };
    const auto upper = lotshadow::build(in, sun);
    CHECK(upper.heights[static_cast<std::size_t>(12 * 32 + 12)] == 3.0f && upper.groundAt(12, 12) == 0.0f && upper.heights[0] == 0.0f);
    // No sun from above, or nothing to shade: no shadow; a broken lot: nothing.
    CHECK(lotshadow::build(in, {0, 0, -1}).shadow[0] == -1000.0f);
    CHECK(lotshadow::build(lotshadow::Inputs{}, sun).width == 0);
    in.width = 100000;
    CHECK(lotshadow::build(in, sun).heights.empty());
    // Elevation between corners, and a storey per level beyond the lot's.
    in.width = 4;
    CHECK(lotshadow::elevation(in, 0.5f, 0.5f, 1) == 3.0f && lotshadow::elevation(in, 0.5f, 0.5f, 3) == 9.0f &&
          lotshadow::elevation(in, 0.5f, 0.5f, -1) == -3.0f);
}

void lightMapTexelsAreWrittenAsTheGame() {
    using namespace ts2::scene;
    using walllight::Pixel;
    // One tile: its left quarters lit 0.25, its right ones 0.75, no links to anything around it.
    auto tile = [](int level) {
        floorlight::Grid g;
        g.level = level;
        g.width = g.height = 1;
        g.cells.resize(4);
        g.tiles.resize(1);
        g.tiles[0].floor = true;
        for (int i = 0; i < 4; ++i) {
            auto& h = g.cells[static_cast<std::size_t>(i)].halves[0];
            h.valid = true;
            const float v = (i & 1) ? 0.75f : 0.25f;
            h.light = {v, v, v};
        }
        return g;
    };
    const Pixel fill{9, 9, 9, 255};
    // Ground level: texel centres at (i + 0.5) / 8, so the edges show each side's own light and the
    // middle blends; a texel's byte is 256 x the light (the shader doubles it again).
    const auto ground = lightmaptexels::floorTexels(tile(0), fill);
    CHECK(ground.width == 8 && ground.height == 8);
    CHECK(ground.at(0, 0) && ground.at(0, 0)->r == 64 && ground.at(7, 7)->r == 192 && ground.at(3, 4)->r == 112 && ground.at(4, 2)->r == 144);
    CHECK(ground.at(7, 0)->a == 255 && ground.at(5, 5)->g == ground.at(5, 5)->b);
    // Upper levels: texels from edge to edge (i / 7).
    const auto upper = lightmaptexels::floorTexels(tile(1), fill);
    CHECK(upper.at(0, 3)->r == 64 && upper.at(7, 3)->r == 192);
    // A tile without floor keeps the fill.
    auto bare = tile(0);
    bare.tiles[0].floor = false;
    CHECK((*lightmaptexels::floorTexels(bare, fill).at(4, 4) == fill));
    CHECK(lightmaptexels::floorTexels(floorlight::Grid{}, fill).texels.empty());

    // Rect sizes by face and height.
    CHECK(lightmaptexels::panelSize(true, 1).width == 4 && lightmaptexels::panelSize(false, 1).height == 8);
    CHECK(lightmaptexels::panelSize(true, 3).height == 24 && lightmaptexels::panelSize(false, 9).height == 32);
    // Walls: a flat 128 grid comes out at half (the game's 0.5 scale) in every rect shape.
    std::array<walllight::Column, 4> flat{};
    for (auto& c : flat) c.fill({128, 128, 128, 255});
    const auto normal = lightmaptexels::panelTexels(flat, {8, 24}, {0, 0, 0}, 0.5f, false, 0.137f);
    const auto small = lightmaptexels::panelTexels(flat, {4, 8}, {0, 0, 0}, 0.5f, false, 0.137f);
    CHECK(normal.size() == 8 * 24 && (normal.front() == Pixel{64, 64, 64, 255}) && (normal.back() == Pixel{64, 64, 64, 255}));
    CHECK(small.size() == 4 * 8 && (small[13] == Pixel{64, 64, 64, 255}));
    // A tall rect fades above the sampled rows (64 x 3883 / 4096); an attic's inside face clears them.
    const auto tall = lightmaptexels::panelTexels(flat, {8, 32}, {0, 0, 0}, 0.5f, false, 0.137f);
    const auto attic = lightmaptexels::panelTexels(flat, {8, 32}, {0, 0, 0}, 0.5f, true, 0.137f);
    CHECK(tall.size() == 8 * 32 && tall[24 * 8].r == 60 && tall[25 * 8].r == 56 && (attic[24 * 8] == Pixel{0, 0, 0, 0}) && attic[23 * 8].r == 64);
    // Faces looking outdoors have the outside ambient taken off first.
    const auto out = lightmaptexels::panelTexels(flat, {8, 24}, {0.25f, 0.25f, 0.25f}, 0.5f, false, 0.137f);
    CHECK(out[50].r == 32);
    // Shapes the game has no rule for give nothing written; broken sizes nothing at all.
    CHECK(lightmaptexels::panelTexels(flat, {8, 20}, {0, 0, 0}, 0.5f, false, 0.137f)[0].a == 0);
    CHECK(lightmaptexels::panelTexels(flat, {0, 24}, {0, 0, 0}, 0.5f, false, 0.137f).empty());

    // The unlit room light added to a room's texels: half the colour times its share, as bytes
    // (the game's own add on the Caliente lot: room 7, share 0.63356, gave 36, 34, 32).
    const std::array<float, 3> unlit{0.45f, 0.425f, 0.4f};
    CHECK((lightmaptexels::roomAdd(unlit, 0.633562f) == Pixel{32, 34, 36, 0}));
    CHECK((lightmaptexels::roomAdd(unlit, 0) == Pixel{0, 0, 0, 0}) && lightmaptexels::roomAdd({4, 4, 4}, 1).r == 255);
    auto panel = normal;
    lightmaptexels::addRoomLight(panel, {200, 1, 2, 0});
    CHECK((panel[7] == Pixel{255, 65, 66, 255}));  // saturating, alpha kept
    // On floors each tile takes its room's add; room 0 (outdoors) none.
    auto lit = tile(0);
    for (auto& c : lit.cells) c.halves[0].room = 3;
    auto image = lightmaptexels::floorTexels(lit, fill);
    lightmaptexels::addRoomLight(image, lit, [](std::int16_t room) { return room == 3 ? Pixel{10, 10, 10, 0} : Pixel{}; });
    CHECK(image.at(0, 0)->r == 74 && image.at(7, 7)->r == 202);
    for (auto& c : lit.cells) c.halves[0].room = 0;
    auto outdoors = lightmaptexels::floorTexels(lit, fill);
    lightmaptexels::addRoomLight(outdoors, lit, [](std::int16_t) { return Pixel{10, 10, 10, 0}; });
    CHECK(outdoors.at(0, 0)->r == 64);

    // Sunlight outdoors, with the day state's lights: the game's sunlit lot ground is 165, 159, 136.
    auto normalized = [](float x, float y, float z) {
        const float l = std::sqrt(x * x + y * y + z * z);
        return std::array<float, 3>{x / l, y / l, z / l};
    };
    lightmaptexels::Outdoor day;
    day.lights = {{{0.97f, 0.94f, 0.83f}, 2, normalized(1.5f, 1.2f, 2)}, {{0.8f, 0.8f, 0.73f}, 0.5f, normalized(-1.5f, -1.2f, 2)},
                  {{1, 0.8f, 0.9f}, 0.5f, normalized(1, 1, -1)}};
    day.skylight = {0.5f, 0.5f, 0.5f};
    CHECK((lightmaptexels::outdoorAdd(day, {0, 0, 1}) == Pixel{136, 159, 165, 0}));
    lightmaptexels::Outdoor none;
    CHECK((lightmaptexels::outdoorAdd(none, {0, 0, 1}) == Pixel{0, 0, 0, 0}));
    // Only tiles outdoors get it.
    auto yard = tile(0);
    for (auto& c : yard.cells) c.halves[0].outside = true;
    yard.cells[0].halves[0].light = {0.25f, 0.25f, 0.25f};
    auto ground2 = lightmaptexels::floorTexels(yard, fill);
    const auto before = *ground2.at(0, 0);
    lightmaptexels::addOutdoorLight(ground2, yard, day);
    CHECK(ground2.at(0, 0)->r == std::min(255, before.r + 165));
    auto indoors = lightmaptexels::floorTexels(tile(0), fill);
    lightmaptexels::addOutdoorLight(indoors, tile(0), day);
    CHECK(indoors.at(0, 0)->r == 64);
}

// A tiny baseline JPEG writer for the decoder tests: one quantizer for every coefficient, a DC
// table whose code for size s is s in 4 bits, and an AC table whose code for symbol v is v in 8 bits.
struct JpegWriter {
    std::vector<std::uint8_t> scan;
    int bits = 0, count = 0;
    std::vector<int> predictors;

    void put(int value, int n) {
        for (int i = n - 1; i >= 0; --i) {
            bits = (bits << 1) | ((value >> i) & 1);
            if (++count == 8) {
                scan.push_back(static_cast<std::uint8_t>(bits));
                if (bits == 0xFF) scan.push_back(0);
                bits = count = 0;
            }
        }
    }
    static int sizeOf(int v) {
        int s = 0;
        for (int a = std::abs(v); a; a >>= 1) ++s;
        return s;
    }
    void putValue(int v, int s) {
        if (s) put(v < 0 ? v + (1 << s) - 1 : v, s);
    }
    // One block of a component, its coefficients in natural order.
    void block(std::size_t component, const std::map<int, int>& coefficients) {
        static constexpr int kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                            41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                            30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
        std::array<int, 64> c{};
        for (const auto& [k, v] : coefficients) c[static_cast<std::size_t>(k)] = v;
        predictors.resize(std::max(predictors.size(), component + 1));
        const int diff = c[0] - predictors[component];
        predictors[component] = c[0];
        put(sizeOf(diff), 4);
        putValue(diff, sizeOf(diff));
        int run = 0;
        for (int k = 1; k < 64; ++k) {
            const int v = c[static_cast<std::size_t>(kZigzag[k])];
            if (!v) {
                ++run;
                continue;
            }
            for (; run > 15; run -= 16) put(0xF0, 8);
            put(run << 4 | sizeOf(v), 8);
            putValue(v, sizeOf(v));
            run = 0;
        }
        if (run) put(0, 8);
    }
    // The whole file; sampling holds each component's h << 4 | v. Its frame header starts at kFrame.
    static constexpr std::size_t kFrame = 2 + 69 + 33 + 276;
    std::vector<std::uint8_t> file(int width, int height, int quant, const std::vector<int>& sampling) {
        while (count) put(1, 1);
        std::vector<std::uint8_t> f{0xFF, 0xD8, 0xFF, 0xDB, 0, 67, 0};
        f.insert(f.end(), 64, static_cast<std::uint8_t>(quant));
        f.insert(f.end(), {0xFF, 0xC4, 0, 31, 0x00, 0, 0, 0, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
        for (int v = 0; v < 12; ++v) f.push_back(static_cast<std::uint8_t>(v));
        f.insert(f.end(), {0xFF, 0xC4, 0x01, 0x12, 0x10, 0, 0, 0, 0, 0, 0, 0, 255, 0, 0, 0, 0, 0, 0, 0, 0});
        for (int v = 0; v < 255; ++v) f.push_back(static_cast<std::uint8_t>(v));
        const auto n = static_cast<std::uint8_t>(sampling.size());
        f.insert(f.end(), {0xFF, 0xC0, 0, static_cast<std::uint8_t>(8 + 3 * n), 8, 0, static_cast<std::uint8_t>(height), 0,
                           static_cast<std::uint8_t>(width), n});
        for (std::uint8_t i = 0; i < n; ++i) f.insert(f.end(), {static_cast<std::uint8_t>(i + 1), static_cast<std::uint8_t>(sampling[i]), 0});
        f.insert(f.end(), {0xFF, 0xDA, 0, static_cast<std::uint8_t>(6 + 2 * n), n});
        for (std::uint8_t i = 0; i < n; ++i) f.insert(f.end(), {static_cast<std::uint8_t>(i + 1), 0x00});
        f.insert(f.end(), {0, 63, 0});
        f.insert(f.end(), scan.begin(), scan.end());
        f.insert(f.end(), {0xFF, 0xD9});
        return f;
    }
};

void jpegDecodesAsTheGame() {
    // Expected blocks come from running the game's JPEG library's own inverse DCT instructions.
    auto grey = [](int quant, const std::map<int, int>& coefficients) {
        JpegWriter w;
        w.block(0, coefficients);
        return ts2::jpeg::decode(w.file(8, 8, quant, {0x11}));
    };
    auto plane = [](const std::optional<ts2::tga::Pixels>& p) {
        std::vector<int> v;
        if (p) {
            for (std::size_t i = 0; i < p->rgba.size(); i += 4) v.push_back(p->rgba[i]);
        }
        return v;
    };
    const std::vector<int> dc{129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129,
                              128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
                              128, 128, 128, 128, 128, 128, 128, 128, 129, 129, 129, 129, 129, 129, 129, 129,
                              128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128};
    CHECK(plane(grey(1, {{0, 4}})) == dc);
    const std::vector<int> busy{144, 91,  64,  68,  56,  11, 0,   19, 114, 128, 113, 48,  35,  41,  43,  8,
                                112, 139, 122, 68,  37,  55, 54,  32, 156, 113, 108, 102, 97,  42,  54,  83,
                                160, 116, 93,  112, 101, 71, 64,  92, 135, 142, 131, 78,  92,  110, 115, 74,
                                121, 141, 127, 86,  81,  112, 114, 86, 142, 109, 108, 118, 112, 73,  88,  126};
    CHECK(plane(grey(1, {{0, -300}, {1, 200}, {8, -150}, {9, 90}, {2, 60}, {16, -45}, {27, 33}, {63, -20}, {7, 15}, {56, -10}, {36, 120}})) ==
          busy);
    // Coefficients times quantizers past 16 bits wrap as the game's do.
    const std::vector<int> wrap{128, 128, 0,   0,   0,   0,   0,   0, 255, 255, 255, 0,   0,   0,   0,   0,
                                255, 255, 255, 0,   0,   0,   0,   0, 255, 255, 255, 0,   0,   0,   0,   0,
                                255, 255, 255, 12,  0,   0,   0,   0, 255, 255, 255, 166, 0,   0,   0,   0,
                                255, 255, 255, 0,   0,   0,   0,   0, 255, 255, 255, 128, 128, 128, 255, 255};
    CHECK(plane(grey(255, {{0, 200}, {1, -130}, {8, 129}, {18, 7}})) == wrap);

    // Colour: four flat luma blocks and one chroma pair spread over them, in a picture smaller than
    // its MCU, turned to RGB with the game's integer steps.
    JpegWriter w;
    for (int dcValue : {80, 160, -80, 0}) w.block(0, {{0, dcValue}});
    w.block(1, {{0, -160}});
    w.block(2, {{0, 240}});
    const auto colourFile = w.file(13, 11, 1, {0x22, 0x11, 0x11});
    const auto colour = ts2::jpeg::decode(colourFile);
    CHECK(colour && colour->width == 13 && colour->height == 11);
    if (colour) {
        const std::array<std::array<int, 3>, 4> expected{{{180, 124, 102}, {190, 134, 112}, {160, 104, 82}, {170, 114, 92}}};
        bool same = true;
        for (std::size_t y = 0; y < 11; ++y) {
            for (std::size_t x = 0; x < 13; ++x) {
                const auto& e = expected[(y >= 8 ? 2u : 0u) + (x >= 8 ? 1u : 0u)];
                const auto* p = &colour->rgba[(y * 13 + x) * 4];
                same = same && p[0] == e[0] && p[1] == e[1] && p[2] == e[2] && p[3] == 255;
            }
        }
        CHECK(same);
        const auto viaImage = ts2::image::decode(colourFile);
        CHECK(viaImage && viaImage->rgba == colour->rgba);
        // Over a caller's size limit: refused by both the exact decoder and stb.
        CHECK(!ts2::jpeg::decode(colourFile, 12) && !ts2::image::decode(colourFile, 12) && ts2::image::decode(colourFile, 13));
    }

    // Broken or unsupported files give nothing; cut-off data never reads past its end.
    CHECK(!ts2::jpeg::decode({}));
    CHECK(!ts2::jpeg::decode(std::vector<std::uint8_t>{0xFF, 0xD8, 0xFF}));
    constexpr std::size_t frame = JpegWriter::kFrame;
    CHECK(colourFile[frame] == 0xFF && colourFile[frame + 1] == 0xC0);
    auto progressive = colourFile;
    progressive[frame + 1] = 0xC2;
    CHECK(!ts2::jpeg::decode(progressive));
    auto huge = colourFile;
    for (std::size_t i = frame + 5; i < frame + 9; ++i) huge[i] = 0xFF;
    CHECK(!ts2::jpeg::decode(huge));
    bool safe = true;
    for (std::size_t n = 0; n < colourFile.size(); ++n) {
        const std::vector<std::uint8_t> cut(colourFile.begin(), colourFile.begin() + static_cast<std::ptrdiff_t>(n));
        const auto p = ts2::jpeg::decode(cut);
        safe = safe && (!p || p->rgba.size() == std::size_t{13} * 11 * 4);
    }
    CHECK(safe);
}

void indoorLightStaysSafeOnBadData() {
    using namespace ts2::scene;
    // A grid with an absurd size gives no cells instead of overflowing.
    floorlight::Grid huge;
    huge.width = 0x7FFFFFFF;
    huge.height = 1;
    CHECK(!huge.at(0, 0) && !huge.at(-1, 5));

    // Colours round the same whatever the processor's rounding mode (halves to even).
    const int mode = std::fegetround();
    std::fesetround(FE_UPWARD);
    const auto up = walllight::pack({2.5f / 256, 0, 0.5f / 256});
    std::fesetround(mode);
    CHECK(up.r == 2 && up.b == 0);

    // A negative falloff makes light grow as it spreads; bright colours must not wrap to dark.
    auto w = testWall();
    w.falloffScale = -0.3f;
    LightSource lamp;
    lamp.color = {4, 4, 4};
    lamp.room = 1;
    lamp.x0 = lamp.x1 = 0.25f;
    lamp.y0 = lamp.y1 = -1;
    std::set<int> changed;
    walllight::resetToAmbient(w);
    walllight::lightFromSources(w, {lamp}, nullptr, changed);
    walllight::spread(w, changed);
    for (const auto& s : w.runs[0].segments) {
        for (const auto& half : s.halves) {
            for (const auto& p : half) {
                CHECK(p.r == 255);
            }
        }
    }

    // Walls stacked over 20 levels, each level two walls and every wall linking up to both
    // walls above: visiting every path would take about 2^20 visits a step. It must finish
    // quickly and still light the walls.
    walllight::Walls stack;
    for (int level = 0; level < 20; ++level) {
        for (int k = 0; k < 2; ++k) {
            walllight::Run run;
            run.level = level;
            run.room = 1;
            for (int i = 0; i < 2; ++i) {
                walllight::Segment s;
                s.x0 = static_cast<float>(2 * k + i);
                s.x1 = s.x0 + 1;
                if (level < 19) {
                    s.up = {2 * (level + 1) + i, 0};
                }
                if (level > 0) {
                    s.down = {2 * (level - 1) + i, 0};
                }
                run.segments.push_back(s);
            }
            stack.runs.push_back(run);
        }
    }
    stack.steps = 1000;
    CHECK(!walllight::light(stack, {lamp}, nullptr, nullptr));  // stopped at its limit, and says so
    CHECK(stack.runs[0].segments[0].halves[0][2].r > 0);
    auto plain = testWall();
    CHECK(walllight::light(plain, {lamp}, nullptr, nullptr));
    auto floor = testFloor(4, 4);
    CHECK(floorlight::light(floor, {lamp}, nullptr));
}

void portableMathMatchesTheLibrary() {
    using namespace ts2::scene::portable;
    // Within a few units in the last place of a double of the C library, over many values.
    auto close = [](double a, double b) { return std::abs(a - b) <= 4e-16 * std::max(1.0, std::abs(b)); };
    bool sinesOk = true, powersOk = true;
    for (int i = -20000; i <= 20000; ++i) {
        const double x = i * 0.000731;  // about -14.6 to 14.6
        sinesOk = sinesOk && close(sine(x), std::sin(x)) && close(cosine(x), std::cos(x));
    }
    for (int i = 1; i <= 400; ++i) {
        for (int j = 0; j <= 100; ++j) {
            const double x = i / 400.0, y = 1 + j * 0.37;
            const double want = std::pow(x, y);
            // The error grows with |y log x| (here up to 600); what the lighting uses is the
            // value rounded to float, which must be the same.
            powersOk = powersOk && std::abs(power(x, y) - want) <= 1e-13 * want &&
                       static_cast<float>(power(x, y)) == static_cast<float>(want);
        }
    }
    CHECK(sinesOk && powersOk);
    CHECK(sine(0) == 0 && cosine(0) == 1 && power(0.5, 2) == 0.25 && power(2, 0) == 1);
    CHECK(std::isnan(sine(std::numeric_limits<double>::infinity())) && std::isnan(power(-1, 2)) && std::isnan(ts2::scene::portable::log(-1)));
    CHECK(ts2::scene::portable::exp(1000) == std::numeric_limits<double>::infinity() && ts2::scene::portable::exp(-1000) == 0 && power(0, 2) == 0);
}

void objectDaylightMatchesTheGame() {
    ts2::content::Lighting lighting;
    auto& day = lighting.states["day"];
    day.skylight = {0.5f, 0.5f, 0.5f};
    day.lights["sun"] = {{1.5f, 1.2f, 2}, {0.97f, 0.94f, 0.83f}, 2};
    day.lights["sky"] = {{-1.5f, -1.2f, 2}, {0.8f, 0.8f, 0.73f}, 0.5f};
    day.lights["fill1"] = {{1, 1, -1}, {1, 0.8f, 0.9f}, 0.5f};
    const auto l = ts2::scene::objectDaylight(lighting);
    CHECK(l && l->lights.size() == 3);
    auto near = [](const std::array<float, 3>& a, std::array<float, 3> b) {
        return std::abs(a[0] - b[0]) < 1e-4f && std::abs(a[1] - b[1]) < 1e-4f && std::abs(a[2] - b[2]) < 1e-4f;
    };
    CHECK(near(l->ambient, {0.5f, 0.5f, 0.5f}));
    CHECK(near(l->lights[0].color, {0.94f, 0.88f, 0.66f}));  // sun
    CHECK(near(l->lights[1].color, {0.25f, 0.15f, 0.2f}));   // fill1
    CHECK(near(l->lights[2].color, {0.15f, 0.15f, 0.115f})); // sky
    CHECK(near(l->lights[0].direction, {0.5409f, 0.4327f, 0.7212f}));
    CHECK(!ts2::scene::objectDaylight(ts2::content::Lighting{}));
}

void installedPacksAreFound() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "neots2-install-test";
    fs::remove_all(root);
    auto pack = [&](const fs::path& folder) {
        fs::create_directories(root / folder / "TSData" / "Res" / "Objects");
        std::ofstream(root / folder / "TSData" / "Res" / "Objects" / "objects.package").put('\0');
    };
    auto codes = [&] {
        std::string out;
        for (const auto& product : ts2::content::findProducts(root)) {
            out += product.code + " ";
        }
        return out;
    };
    for (const char* folder : {"Legacy/Base", "Legacy/EP4", "Legacy/SP8", "Legacy/EP9"}) {
        pack(folder);
    }
    CHECK(codes() == "base ep4 sp8 sp9 ");
    CHECK((ts2::content::installedPacksMask(ts2::content::findProducts(root)) & (1u << 16)) != 0);  // Mansion & Garden
    fs::remove_all(root);
    for (const char* folder : {"Double Deluxe/Base", "Fun with Pets/SP9", "Seasons"}) {
        pack(folder);
    }
    CHECK(codes() == "base ep5 sp9 ");
    fs::remove_all(root);
}

void lotNumberComesFromThePackageName() {
    CHECK(ts2::content::lotNumber("Neighborhoods/N001/Lots/N001_Lot18.package") == 18);
    CHECK(ts2::content::lotNumber("N002_Lot7.package") == 7);
    CHECK(!ts2::content::lotNumber("N001_Lot.package"));
    CHECK(!ts2::content::lotNumber("N001_Lot1x.package"));
    CHECK(!ts2::content::lotNumber("N001_Neighborhood.package"));
    CHECK(!ts2::content::lotNumber("N001_Lot12345678901.package"));  // too long to be a lot number
}

void portraitImageIsTheLifeStageFlag() {
    CHECK(ts2::content::portraitImage(1) == 0x20u);   // baby
    CHECK(ts2::content::portraitImage(2) == 0x01u);   // toddler
    CHECK(ts2::content::portraitImage(3) == 0x02u);   // child
    CHECK(ts2::content::portraitImage(16) == 0x04u);  // teen
    CHECK(ts2::content::portraitImage(19) == 0x08u);  // adult
    CHECK(ts2::content::portraitImage(51) == 0x10u);  // elder
    CHECK(!ts2::content::portraitImage(0));
    CHECK(!ts2::content::portraitImage(-1));
}

// Rows the game showed for Dina's needs (whole motives) and the two Calientes' moods.
void needsBarRowsMatchTheGame() {
    const std::pair<int, int> seen[] = {{59, 49}, {58, 48}, {88, 58}, {49, 46}, {39, 43}, {78, 55}, {56, 48}};
    for (const auto& [value, row] : seen) {
        CHECK(ts2::content::meterRow(value, -100, 100, 62) == row);
    }
    CHECK(ts2::content::meterRow(100, -100, 100, 62) == 61);
    CHECK(ts2::content::meterRow(-100, -100, 100, 62) == 0);
    CHECK(ts2::content::meterRow(500, -100, 100, 62) == 61);  // kept within the range
    CHECK(ts2::content::meterRow(0, 0, 0, 62) == 0);          // no range
    CHECK(ts2::content::meterRow(0, -100, 100, 0) == 0);      // no picture
}

void moodRowsMatchTheGame() {
    CHECK(ts2::content::moodRow(45, 200) == 55);
    CHECK(ts2::content::moodRow(59, 200) == 41);
    CHECK(ts2::content::moodRow(100, 200) == 0);
    CHECK(ts2::content::moodRow(-100, 200) == 199);
    CHECK(ts2::content::moodRow(-300, 200) == 199);
    CHECK(ts2::content::moodRow(0, 0) == 0);
}

void barPaintingKeepsTheFillShape() {
    ts2::tga::Pixels fill{3, 2, {9, 9, 9, 0, 9, 9, 9, 128, 9, 9, 9, 255, 9, 9, 9, 255, 9, 9, 9, 64, 9, 9, 9, 0}};
    ts2::tga::Pixels gradient{3, 2, {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255, 13, 14, 15, 255, 16, 17, 18, 255}};
    const auto painted = ts2::content::paintBar(fill, gradient, 1);
    CHECK(painted.width == 3 && painted.height == 2);
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(painted.rgba[i * 4] == gradient.rgba[(3 + i % 3) * 4]);
        CHECK(painted.rgba[i * 4 + 3] == fill.rgba[i * 4 + 3]);
    }
    ts2::tga::Pixels broken{4, 4, {1, 2, 3}};
    CHECK(ts2::content::paintBar(broken, gradient, 0).rgba.size() == 3);  // left as it is
    CHECK(ts2::content::pixel(gradient, 9, 9) == (std::array<std::uint8_t, 3>{0, 0, 0}));
}

void moodColourFillsTheBackgroundShape() {
    ts2::tga::Pixels shape{2, 1, {255, 255, 0, 0, 200, 200, 10, 255}};
    const auto filled = ts2::content::fillShape(shape, {1, 203, 57});
    CHECK(filled.rgba == (std::vector<std::uint8_t>{1, 203, 57, 0, 1, 203, 57, 255}));
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

// The mood meter the game showed for Nina (held value 50: gradient column 98 of 131).
void moodMeterPicksAColumn() {
    CHECK(ts2::content::meterRow(50, -100, 100, 131) == 98);
    CHECK(ts2::content::moodMeterValue(50, 19) == 50);
    CHECK(ts2::content::moodMeterValue(-20, 75) == 100);  // platinum: full
    CHECK(ts2::content::moodMeterValue(-20, 74) == -20);
    CHECK(ts2::content::isPlatinum(75) && !ts2::content::isPlatinum(74));
    ts2::tga::Pixels fill{2, 2, {9, 9, 9, 255, 9, 9, 9, 0, 9, 9, 9, 128, 9, 9, 9, 255}};
    ts2::tga::Pixels gradient{2, 2, {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255}};
    const auto painted = ts2::content::paintColumns(fill, gradient, 1);
    CHECK(painted.rgba == (std::vector<std::uint8_t>{4, 5, 6, 255, 4, 5, 6, 0, 10, 11, 12, 128, 10, 11, 12, 255}));
}

// A want's icon is placed 2 pixels in on a transparent 44 x 44 picture (as the game showed it).
void wantIconIsPlacedOnItsPicture() {
    ts2::tga::Pixels icon{2, 1, {1, 2, 3, 255, 4, 5, 6, 128}};
    const auto out = ts2::content::placed(icon, 4, 2, 2, 0);
    CHECK(out.width == 4 && out.height == 2 && out.rgba.size() == 32);
    CHECK(out.rgba[8] == 1 && out.rgba[11] == 255 && out.rgba[12] == 4 && out.rgba[15] == 128);
    CHECK(out.rgba[3] == 0 && out.rgba[19] == 0);
    const auto cut = ts2::content::placed(icon, 4, 2, 3, -1);  // partly outside: cut
    CHECK(cut.rgba.size() == 32 && cut.rgba[31] == 0);
    CHECK(ts2::content::placed(ts2::tga::Pixels{9, 9, {1}}, 2, 2, 0, 0).rgba == std::vector<std::uint8_t>(16, 0));
}

// A portrait's UI pose by age and gender, and its camera in front of the head (as the game drew
// five Sims: 1.4506 ahead and 0.05 above the head joint, 20 degrees, looking straight back).
void portraitPoseAndCamera() {
    using namespace ts2::scene;
    CHECK(portraitPose(8, 2, 3) == "a-UIpose-M03");
    CHECK(portraitPose(4, 1, 8) == "a-UIpose-F08");
    CHECK(portraitPose(2, 1, 1) == "c-UIpose-F01");
    CHECK(portraitPose(1, 2, 5) == "p-UIpose-m05");
    CHECK(portraitPose(0x20, 1, 9) == "b-UIpose-04");  // babies have four; out of range is clamped
    CHECK(portraitPose(8, 2, 0) == "a-UIpose-M01");
    CHECK(portraitPoseCount(0x20) == 4 && portraitPoseCount(8) == 8);
    const auto c = portraitCamera(Transform{}, {-0.0088f, 0.0132f, 1.6752f});
    CHECK(std::abs(c.eye[0] + 0.0088f) < 1e-5f && std::abs(c.eye[1] - 1.4638f) < 1e-4f && std::abs(c.eye[2] - 1.7252f) < 1e-4f);
    CHECK(std::abs(c.target[1] - 0.4638f) < 1e-4f && std::abs(c.target[2] - c.eye[2]) < 1e-6f);
    CHECK(std::abs(c.fovY - 0.3490659f) < 1e-6f && c.nearPlane > 0.68f && c.farPlane < 1.75f);
    // A Sim turned a quarter at (3, 4, 0): the camera stays 1.4506 from it, at head height + 0.05.
    const auto t = portraitCamera(Transform::at(3, 4, 0, 1.5707963f), {0, 0, 1.6f});
    CHECK(std::abs(t.eye[2] - 1.65f) < 1e-4f && std::abs(std::hypot(t.eye[0] - 3, t.eye[1] - 4) - 1.4506f) < 1e-4f);
    CHECK(std::abs(std::hypot(t.target[0] - 3, t.target[1] - 4) - 0.4506f) < 1e-4f);
    // Five lights fixed to the camera: the key comes from the Sim's front left, above.
    const auto lit = portraitLighting(c);
    CHECK(lit.lights.size() == 5 && lit.ambient == (std::array<float, 3>{0, 0, 0}));
    CHECK(std::abs(lit.thumbnailScale - 0.4f) < 1e-6f);  // lit as the game lights family thumbnails
    CHECK(std::abs(lit.lights[0].direction[0] - 0.43493f) < 1e-4f && std::abs(lit.lights[0].direction[1] - 0.79454f) < 1e-4f &&
          std::abs(lit.lights[0].direction[2] - 0.42373f) < 1e-4f && std::abs(lit.lights[0].color[0] - 1.6452f) < 1e-6f);
    CHECK(std::abs(lit.lights[1].direction[2] + 1) < 1e-4f);
}

// Thumbnails show the middle of a portrait: 180 square of a 256 picture, 38 off each side (90 of 128).
// A portrait's shadow: black, its outline blurred, at most 210 of 255 opaque (matched against the
// silhouette a character file keeps beside its portrait).
void portraitShadowIsItsBlurredOutline() {
    ts2::tga::Pixels portrait{256, 256, std::vector<std::uint8_t>(256 * 256 * 4, 255)};
    const auto full = ts2::content::portraitSilhouette(portrait);
    CHECK(full.width == 256 && full.rgba[0] == 0 && full.rgba[3] == 210 && full.rgba[(128 * 256 + 128) * 4 + 3] == 210);
    // Half opaque (the left half): sharp in the portrait, soft in its shadow.
    for (std::uint32_t y = 0; y < 256; ++y) {
        for (std::uint32_t x = 128; x < 256; ++x) {
            portrait.rgba[(y * 256 + x) * 4 + 3] = 0;
        }
    }
    const auto half = ts2::content::portraitSilhouette(portrait);
    const auto alpha = [&](std::uint32_t x) { return half.rgba[(100 * 256 + x) * 4 + 3]; };
    CHECK(alpha(0) == 210 && alpha(255) == 0 && alpha(120) > alpha(128) && alpha(128) > alpha(136) && alpha(136) > 0);
    CHECK(ts2::content::portraitSilhouette(ts2::tga::Pixels{4, 4, {1}}).rgba.empty());  // broken: nothing
    // Larger than any portrait: refused rather than blurred.
    const ts2::tga::Pixels huge{2048, 2048, std::vector<std::uint8_t>(2048u * 2048u * 4u, 255)};
    CHECK(ts2::content::portraitSilhouette(huge).rgba.empty() && ts2::content::portraitCrop(huge).rgba.empty());
}

// Thumbnail pictures are scaled by area (colour weighted by alpha) and cut to the thumbnail's
// rounded shape (alpha only); drawn portraits are made straight alpha.
void thumbnailPicturesAreScaledAndMasked() {
    ts2::tga::Pixels two{2, 1, {0, 0, 0, 0, 200, 100, 50, 255}};
    const auto one = ts2::content::scaled(two, 1, 1);
    CHECK(one.width == 1 && one.rgba == (std::vector<std::uint8_t>{200, 100, 50, 128}));  // the clear pixel adds no colour
    const auto up = ts2::content::scaled(ts2::tga::Pixels{1, 1, {9, 9, 9, 9}}, 3, 2);
    CHECK(up.rgba.size() == 24 && up.rgba[20] == 9);
    CHECK(ts2::content::scaled(two, 0, 5).rgba.empty() && ts2::content::scaled(two, 100000, 1).rgba.empty());
    ts2::tga::Pixels shape{2, 1, {0, 0, 0, 255, 0, 0, 0, 0}};
    const auto cut = ts2::content::masked(ts2::tga::Pixels{2, 1, {10, 20, 30, 255, 10, 20, 30, 255}}, shape);
    CHECK(cut.rgba == (std::vector<std::uint8_t>{10, 20, 30, 255, 10, 20, 30, 0}));
    const auto straight = ts2::content::unpremultiplied(ts2::tga::Pixels{2, 1, {50, 25, 0, 128, 7, 7, 7, 0}});
    CHECK(straight.rgba == (std::vector<std::uint8_t>{100, 50, 0, 128, 7, 7, 7, 0}));
    CHECK(ts2::content::masked(two, ts2::tga::Pixels{1, 1, {0, 0, 0, 0}}).rgba == two.rgba);  // sizes differ: as it is
}

void portraitIsCroppedForThumbnails() {
    ts2::tga::Pixels portrait{256, 256, std::vector<std::uint8_t>(256 * 256 * 4, 0)};
    portrait.rgba[(38 * 256 + 38) * 4] = 7;
    const auto crop = ts2::content::portraitCrop(portrait);
    CHECK(crop.width == 180 && crop.height == 180 && crop.rgba[0] == 7);
    const auto half = ts2::content::portraitCrop(ts2::tga::Pixels{128, 128, std::vector<std::uint8_t>(128 * 128 * 4, 0)});
    CHECK(half.width == 90);
    CHECK(ts2::content::portraitCrop(ts2::tga::Pixels{4, 2, {1}}).width == 4);  // broken: left as it is
    CHECK(ts2::content::placed(ts2::tga::Pixels{1, 1, {1, 2, 3, 4}}, 100000, 100000, 0, 0).rgba.empty());  // too big: nothing
}

// Want target pictures load in the background: unknown until they arrive, at most four at once,
// a failed read leaves none, and forgetting them while loads run doesn't wait for those.
void targetPicturesLoadInTheBackground() {
    std::promise<void> open;
    const std::shared_future<void> gate = open.get_future().share();
    ts2::content::TargetPictures pictures([gate](const std::filesystem::path& file, std::uint32_t) -> std::optional<ts2::tga::Pixels> {
        gate.wait();
        if (file == "broken") {
            throw std::runtime_error("broken file");
        }
        if (file == "none") {
            return std::nullopt;
        }
        return ts2::tga::Pixels{256, 256, std::vector<std::uint8_t>(256 * 256 * 4, 255)};
    });
    CHECK(pictures.load(1, "good", 0x10) && pictures.load(2, "broken", 0x10) && pictures.load(3, "none", 0x10));
    CHECK(pictures.known(1) && pictures.find(1) == nullptr && !pictures.load(1, "good", 0x10));  // loading
    CHECK(pictures.load(4, "good", 0x10) && !pictures.load(5, "good", 0x10));  // four at most
    CHECK(!pictures.collect());  // nothing finished yet
    open.set_value();
    for (int tries = 0; tries < 500 && pictures.loading() > 0; ++tries) {
        pictures.collect();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(pictures.loading() == 0);
    const auto* good = pictures.find(1);
    CHECK(good && good->width == 44 && good->height == 44 && good->rgba[3] == 255);
    CHECK(pictures.known(2) && pictures.find(2) == nullptr && pictures.find(3) == nullptr);
    pictures.make(6, ts2::tga::Pixels{2048, 2048, std::vector<std::uint8_t>(2048u * 2048u * 4u, 255)});
    CHECK(pictures.known(6) && pictures.find(6) == nullptr);  // too large to be a portrait

    // Forgetting while a load runs returns at once; the load finishes on its own.
    std::promise<void> later;
    const std::shared_future<void> slow = later.get_future().share();
    ts2::content::TargetPictures waiting([slow](const std::filesystem::path&, std::uint32_t) -> std::optional<ts2::tga::Pixels> {
        slow.wait();
        return std::nullopt;
    });
    CHECK(waiting.load(1, "slow", 0x10));
    waiting.clear();
    CHECK(!waiting.known(1) && waiting.loading() == 0);
    later.set_value();  // let it end before the pictures go

    // Going away while a load is stuck returns at once; the load ends later on its own.
    std::promise<void> stuck;
    const std::shared_future<void> held = stuck.get_future().share();
    std::promise<void> ended;
    auto endedFuture = ended.get_future();
    {
        auto endedShared = std::make_shared<std::promise<void>>(std::move(ended));
        ts2::content::TargetPictures leaving([held, endedShared](const std::filesystem::path&, std::uint32_t) -> std::optional<ts2::tga::Pixels> {
            held.wait();
            endedShared->set_value();
            return std::nullopt;
        });
        CHECK(leaving.load(1, "stuck", 0x10));
    }
    stuck.set_value();
    CHECK(endedFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
}

// The run's log: every line in its file, the last 64 kept in memory (cut to fit), and only the last
// ten logs kept in its folder.
void logKeepsItsLinesAndFiles() {
    namespace fs = std::filesystem;
    ts2::diag::reset();
    const fs::path folder = fs::temp_directory_path() / ("neots2-log-test-" + std::to_string(std::random_device{}()));
    fs::create_directories(folder);
    for (int i = 0; i < 12; ++i) {  // older runs' logs
        std::ofstream(folder / ("neots2-20000101-0000" + std::to_string(10 + i) + ".log")) << "old";
    }
    CHECK(ts2::diag::open(folder, "NeoTS2 test"));
    ts2::diag::step("lot opened");
    ts2::diag::error(std::string(400, 'x'));
    std::size_t logs = 0;
    for (const auto& e : fs::directory_iterator(folder)) {
        logs += e.path().filename().string().rfind("neots2-", 0) == 0 ? 1 : 0;
    }
    CHECK(logs == ts2::diag::kKeptLogs && fs::exists(ts2::diag::file()));
    std::string text;
    {
        std::ifstream in(ts2::diag::file());
        text.assign(std::istreambuf_iterator<char>(in), {});
    }
    CHECK(text.rfind("NeoTS2 test\n", 0) == 0 && text.find("step: lot opened") != std::string::npos);
    CHECK(text.find(std::string(400, 'x')) != std::string::npos);  // whole in the file
    auto recent = ts2::diag::recentLines();
    CHECK(recent.size() == 2 && recent[0].find("lot opened") != std::string::npos && recent[1].size() < ts2::diag::kLineLength);
    for (int i = 0; i < 100; ++i) {
        ts2::diag::info("line " + std::to_string(i));
    }
    recent = ts2::diag::recentLines();
    CHECK(recent.size() == ts2::diag::kRecentLines && recent.front().find("line 36") != std::string::npos &&
          recent.back().find("line 99") != std::string::npos);
    char small[10];
    CHECK(ts2::diag::recentLines(small, sizeof small) == 9 && small[9] == '\0');
    ts2::diag::reset();
    std::error_code ec;
    fs::remove_all(folder, ec);
}

// A crash report names what happened and where; its issue link keeps to module names and offsets
// (no folders, no log lines) and stays short enough for a browser.
void crashReportsSayWhatHappened() {
    using namespace ts2::diag;
    CHECK(exceptionName(0xC0000005) == "access violation" && exceptionName(0xC00000FD) == "stack overflow");
    CHECK(exceptionName(0x123) == "exception 0x123");
    CHECK(fileName("D:\\Profiles\\Me\\neots2.exe") == "neots2.exe" && fileName("a/b/c.cpp:3") == "c.cpp:3" && fileName("x") == "x");
    CHECK(linkEncoded("a b&c/\xC3\xA9") == "a%20b%26c%2F%C3%A9");
    Crash crash;
    crash.version = "abc1234";
    crash.kind = exceptionName(0xC0000005);
    crash.code = 0xC0000005;
    crash.where = {"neots2.exe", 0x4a955, "app::crash::test", "crash_handler.cpp:318"};
    for (int i = 0; i < 40; ++i) {
        crash.stack.push_back({"D:\\Profiles\\Me\\Games\\NeoTS2\\neots2.exe", 0x1000u + i, "app::f" + std::to_string(i),
                               "D:\\Profiles\\Me\\src\\file.cpp:" + std::to_string(i)});
    }
    const std::string text = crashText(crash, {"12:00:00.000 step: lot opened"});
    CHECK(text.find("Version: abc1234") != std::string::npos && text.find("access violation (0xc0000005)") != std::string::npos);
    CHECK(text.find("neots2.exe+0x4a955  app::crash::test  (crash_handler.cpp:318)") != std::string::npos);
    CHECK(text.find("step: lot opened") != std::string::npos && text.find("Nothing is sent anywhere") != std::string::npos);
    const std::string link = issueLink(crash);
    CHECK(link.rfind(std::string(kIssuesPage) + "?title=", 0) == 0 && link.size() <= 2000);
    CHECK(link.find("abc1234") != std::string::npos && link.find("Users") == std::string::npos && link.find("lot%20opened") == std::string::npos);
}

// A run that ended without a word is told from what Windows noted in its event log: the fields of
// its "Application Error" event, and the crash they amount to.
void unexpectedExitsSayWhatWindowsNoted() {
    using namespace ts2::diag;
    const auto fields = eventFields("<EventData><Data Name='AppName'>neots2.exe</Data><Data Name='ExceptionCode'>c0000409</Data>"
                                    "<Data Name=\"FaultingOffset\">000000000004c172</Data><Data Name='PackageFullName'></Data>"
                                    "<Data Name='Empty'/><Data Name='ModuleName'>neots2.exe</Data></EventData>");
    CHECK(fields.size() == 5 && fields.at("AppName") == "neots2.exe" && fields.at("FaultingOffset") == "000000000004c172");
    CHECK(fields.at("PackageFullName").empty() && !fields.count("Empty"));
    CHECK(eventFields("<Data Name='broken").empty() && eventFields("").empty());

    UnexpectedExit exit;
    exit.version = "abc1234";
    exit.started = "2026-10-12 21:12:10";
    const Crash none = crashOf(exit);
    CHECK(none.kind == "closed unexpectedly" && none.code == 0 && none.version == "abc1234");
    CHECK(unexpectedExitText(exit, {"21:12:10.142 step: lot opened"}).find("Windows noted no crash") != std::string::npos);
    exit.windows = fields;
    const Crash noted = crashOf(exit);
    CHECK(noted.code == 0xC0000409 && noted.where.module == "neots2.exe" && noted.where.offset == 0x4c172);
    CHECK(noted.kind == exceptionName(0xC0000409));
    const std::string text = unexpectedExitText(exit, {"21:12:10.142 step: lot opened"});
    CHECK(text.find("Where: neots2.exe+0x4c172") != std::string::npos && text.find("step: lot opened") != std::string::npos);
    CHECK(text.find("Nothing is sent anywhere") != std::string::npos);
    exit.windows["ExceptionCode"] = "not hex";
    CHECK(crashOf(exit).code == 0);  // nonsense is left out, not thrown
    CHECK(issueLink(noted).find("Users") == std::string::npos);
}

// The pieces of sending a report: a stored zip (checked against the standard CRC and its own
// directory), the upload link read from BugSplat's JSON, and the form the upload is committed with.
void reportUploadPieces() {
    using namespace ts2::diag;
    const std::string check = "123456789";
    CHECK(crc32(reinterpret_cast<const std::uint8_t*>(check.data()), check.size()) == 0xCBF43926u);
    const auto z = zip({{"neots2.dmp", {1, 2, 3}}, {"crash.txt", {'h', 'i'}}});
    auto u32 = [&](std::size_t at) { return z[at] | z[at + 1] << 8 | z[at + 2] << 16 | static_cast<std::uint32_t>(z[at + 3]) << 24; };
    CHECK(z.size() > 22 && u32(0) == 0x04034B50u && u32(z.size() - 22) == 0x06054B50u);
    CHECK(z[z.size() - 22 + 10] == 2);  // two files
    CHECK(std::string(z.begin() + 30, z.begin() + 40) == "neots2.dmp" && z[40] == 1 && z[42] == 3);
    CHECK(zip({}).empty());
    std::ofstream(std::filesystem::temp_directory_path() / "neots2-zip-test.zip", std::ios::binary)
        .write(reinterpret_cast<const char*>(z.data()), static_cast<std::streamsize>(z.size()));

    // As JSON writes a link: slashes escaped, & as &.
    const std::string reply = "{\"status\":\"success\",\"url\":\"https:\\/\\/s3.example.com\\/a?x=1\\u0026y=2\"}";
    CHECK(jsonString(reply, "url") == "https://s3.example.com/a?x=1&y=2");
    CHECK(jsonString(R"({"note":"url", "url" : "b"})", "url") == "b");  // the key's text in a value isn't the key
    CHECK(jsonString(R"({"url":12})", "url").empty() && jsonString(R"({"url":"cut)", "url").empty() && jsonString("", "url").empty());

    const std::string form = formBody({{"appName", "NeoTS2"}, {"md5", "abc"}}, "XYZ");
    CHECK(form == "--XYZ\r\nContent-Disposition: form-data; name=\"appName\"\r\n\r\nNeoTS2\r\n"
                  "--XYZ\r\nContent-Disposition: form-data; name=\"md5\"\r\n\r\nabc\r\n--XYZ--\r\n");
}

// Reports write the user's own folder as %USERPROFILE%: any case, either slash, whole folder names
// only, everywhere it appears.
void reportsDontNameTheUser() {
    using ts2::diag::withoutUserFolder;
    const std::string user = "D:\\Profiles\\Sam";
    CHECK(withoutUserFolder("logging to D:\\Profiles\\Sam\\Documents\\NeoTS2\\logs\\a.log", user) ==
          "logging to %USERPROFILE%\\Documents\\NeoTS2\\logs\\a.log");
    CHECK(withoutUserFolder("d:/profiles/sam/Desktop and \"D:\\Profiles\\Sam\"", user) == "%USERPROFILE%/Desktop and \"%USERPROFILE%\"");
    CHECK(withoutUserFolder("D:\\Profiles\\Sam2\\x and D:\\Profiles\\Sam.old", user) == "D:\\Profiles\\Sam2\\x and D:\\Profiles\\Sam.old");
    CHECK(withoutUserFolder("D:\\Profiles\\Sam\\", "D:\\Profiles\\Sam\\") == "%USERPROFILE%\\");  // given with a slash at its end
    CHECK(withoutUserFolder("C:\\Games", "C:") == "C:\\Games" && withoutUserFolder("text", "") == "text");
}

// A run's log file stops growing at its limit, with a line saying so; the recent lines go on.
void logFileStopsAtItsLimit() {
    namespace fs = std::filesystem;
    ts2::diag::reset();
    const fs::path folder = fs::temp_directory_path() / ("neots2-logcap-test-" + std::to_string(std::random_device{}()));
    CHECK(ts2::diag::open(folder, "header", 300));
    for (int i = 0; i < 40; ++i) {
        ts2::diag::info("line " + std::to_string(i));
    }
    const fs::path file = ts2::diag::file();
    std::string text;
    {
        std::ifstream in(file);
        text.assign(std::istreambuf_iterator<char>(in), {});
    }
    CHECK(text.find("(the log is full") != std::string::npos && text.find("line 39") == std::string::npos);
    CHECK(text.size() < 300 + 80);
    const auto recent = ts2::diag::recentLines();
    CHECK(!recent.empty() && recent.back().find("line 39") != std::string::npos);
    ts2::diag::reset();
    std::error_code ec;
    fs::remove_all(folder, ec);
}

// Only names the program makes are cleaned up or read; paths become UTF-8 text whatever the
// Windows language; loads left running by a household change still count toward the limit.
void diagnosticsKeepToTheirOwnFiles() {
    using namespace ts2::diag;
    CHECK(isLogName("neots2-20261012-153000.log") && !isLogName("neots2-notes.log") && !isLogName("neots2-20261012-153000.log.bak"));
    CHECK(isReportFolderName("crash-20261012-153000") && isReportFolderName("crash-20261012-153000-unexpected-1234"));
    CHECK(!isReportFolderName("crash-course") && !isReportFolderName("crash-20261012-153000-unexpected-") &&
          !isReportFolderName("crash-20261012-153000-old") && !isReportFolderName("Crash-20261012-153000"));
    CHECK(isMarkerName("running-1234.txt") && !isMarkerName("running-.txt") && !isMarkerName("running-12ab.txt") &&
          !isMarkerName("running-1234.txt.old"));
    CHECK(utf8(std::filesystem::path(L"D:\\Profiles\\Jos\u00e9\\\u4e2d\u6587")) == "D:\\Profiles\\Jos\xC3\xA9\\\xE4\xB8\xAD\xE6\x96\x87");

    std::promise<void> open;
    const std::shared_future<void> gate = open.get_future().share();
    ts2::content::TargetPictures pictures([gate](const std::filesystem::path&, std::uint32_t) -> std::optional<ts2::tga::Pixels> {
        gate.wait();
        return std::nullopt;
    });
    for (std::int16_t n = 1; n <= 4; ++n) {
        CHECK(pictures.load(n, "slow", 0x10));
    }
    pictures.clear();  // a new household while all four still load
    CHECK(!pictures.load(5, "slow", 0x10));
    open.set_value();
    bool loaded = false;
    for (int tries = 0; tries < 500 && !loaded; ++tries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        loaded = pictures.load(5, "slow", 0x10);  // room again once the abandoned loads end
    }
    CHECK(loaded);
    for (int tries = 0; tries < 500 && pictures.loading() > 0; ++tries) {
        pictures.collect();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int main(int argc, char** argv) {
    const std::string_view filter = argc > 1 ? argv[1] : "";
    const std::pair<const char*, void (*)()> tests[] = {
        {"assign and return true", assignAndReturnTrue},
        {"comparison takes false branch", comparisonTakesFalseBranch},
        {"loop sums 1..10", loopSumsOneToTen},
        {"call passes literal arguments and returns", callPassesLiteralArgumentsAndReturns},
        {"call passes temps when arguments are all 0xFF", callPassesTempsWhenArgumentsAreAllFF},
        {"locals are per frame", localsArePerFrame},
        {"calls resolve global/private/semi-global groups", callsResolveGlobalPrivateAndSemiGlobalGroups},
        {"constants resolve by id range", constantsResolveByIdRange},
        {"constant bit 15 picks the upper tables", constantBit15PicksTheUpperTables},
        {"flags are one-based", flagsAreOneBased},
        {"arithmetic wraps at 16 bits", arithmeticWrapsAt16Bits},
        {"division by zero is an error", divisionByZeroIsAnError},
        {"writing a literal is an error", writingALiteralIsAnError},
        {"out-of-range temp is an error", outOfRangeTempIsAnError},
        {"infinite loop hits step limit", infiniteLoopHitsStepLimit},
        {"infinite recursion hits depth limit", infiniteRecursionHitsDepthLimit},
        {"missing call target is reported", missingCallTargetIsReported},
        {"unimplemented primitive stops with its opcode", unimplementedPrimitiveStopsWithItsOpcode},
        {"jump past the end is an error", jumpPastTheEndIsAnError},
        {"attributes belong to each object", attributesBelongToEachObject},
        {"Init - Object sets defaults; attributes by param", initObjectSetsDefaultsAndAttributesByParam},
        {"object id field matches the object", objectIdFieldMatchesTheObject},
        {"stack object is reached through its id", stackObjectIsReachedThroughItsId},
        {"each frame keeps its own stack object", eachFrameKeepsItsOwnStackObject},
        {"rooms are filled between walls", roomsAreFilledBetweenWalls},
        {"diagonal walls split tiles", diagonalWallsSplitTiles},
        {"room fields are read through temp 0", roomFieldsAreReadThroughTempZero},
        {"outside is the region in layer 0", outsideIsTheRegionInLayerZero},
        {"saved room grid is read by quarter", savedRoomGridIsReadByQuarter},
        {"missing stack object is an error", missingStackObjectIsAnError},
        {"definition writes change only that object's copy", definitionWritesChangeOnlyThatObjectsCopy},
        {"scoped arguments beyond callee arg count are ignored", scopedArgumentsBeyondCalleeArgCountAreIgnored},
        {"Set to Next visits every object of a type", setToNextVisitsEveryObjectOfAType},
        {"Set to Next finds objects by category", setToNextFindsObjectsByCategory},
        {"Set to Next finds objects on the same tile", setToNextFindsObjectsOnTheSameTile},
        {"Set to Next walks multi-tile parts", setToNextWalksMultiTileParts},
        {"Set to Next finds an object type from temps", setToNextObjectOfTypeInTemps},
        {"Set to Next walks neighbors by species", setToNextWalksNeighborsBySpecies},
        {"Set to Next finds adjacent objects", setToNextFindsAdjacentObjects},
        {"Set Motive Change keeps changes", setMotiveChangeKeepsChanges},
        {"neighbor person data reads and writes", neighborPersonDataReadsAndWrites},
        {"Want Satisfy matches current wants", wantSatisfyMatchesCurrentWants},
        {"neighbor object definition reads", neighborObjectDefinitionReads},
        {"Get Direction To measures towards the stack object", getDirectionToMeasuresTowardsStackObject},
        {"Get Distance To measures in tiles", getDistanceToMeasuresInTiles},
        {"Push Interaction queues for the Sim", pushInteractionQueuesForTheSim},
        {"Idle for Input starts queued interactions", idleForInputStartsQueuedInteractions},
        {"Animate Sim waits for its animation", animateSimWaitsForItsAnimation},
        {"the Sim clock advances and reads", clockAdvancesAndReads},
        {"the route grid splits along items and passes by their types", routeGridSplitsAndPasses},
        {"floor route items follow the floors", floorRouteItemsFollowTheFloors},
        {"post route items use the post footprint", postRouteItemsUseThePostFootprint},
        {"object footprints follow the model", objectFootprintsFollowTheModel},
        {"type signatures sum the call graph", typeSignaturesSumTheCallGraph},
        {"reset objects start over", resetObjectsStartOver},
        {"object route items follow the game rules", objectRouteItemsFollowTheGameRules},
        {"Test Object Type compares the original GUID", testObjectTypeComparesTheOriginalGuid},
        {"Find Best Object picks the best rated nearest", findBestObjectPicksTheBestRatedNearest},
        {"Run Functional Tree calls the object's tree", runFunctionalTreeCallsTheObjectsTree},
        {"Add/Change the Action String sets text and icon", actionStringSetsTextAndIcon},
        {"Lua script primitive runs scripts", luaScriptPrimitiveRunsScripts},
        {"Lua endless loop is stopped", luaEndlessLoopIsStopped},
        {"Lua memory is capped", luaMemoryIsCapped},
        {"Lua error without text is reported", luaErrorWithoutTextIsReported},
        {"Installed packs are found", installedPacksAreFound},
        {"Skin under clothes is named as the game names it", skinUnderClothesNames},
        {"Objects' daylight matches the game", objectDaylightMatchesTheGame},
        {"Floor light spreads and fades as the game's", floorLightSpreadsAndFades},
        {"Floor light grid is built as the game builds it", lightGridIsBuiltAsTheGame},
        {"Wall runs are laid out as the game lays them out", wallRunsAreLaidOutAsTheGame},
        {"Wall texels and corners are finished as the game finishes them", wallTexelsAndCornersAreFinishedAsTheGame},
        {"Light files are read as the game reads them", lightFilesAreReadAsTheGame},
        {"Lamps give the game's floor and wall light records", lampsGiveTheGamesRecords},
        {"Portals are built as the game builds them", portalsAreBuiltAsTheGame},
        {"Room samples are gathered as the game gathers them", roomSamplesAreGatheredAsTheGame},
        {"Lot lighting is worked out as the game works it out", lotLightingIsWorkedOutAsTheGame},
        {"Light-map texels are written as the game writes them", lightMapTexelsAreWrittenAsTheGame},
        {"The lot's sun shadow is cast as the game casts it", lotSunShadowIsCastAsTheGame},
        {"JPEG pictures decode as the game decodes them", jpegDecodesAsTheGame},
        {"Wall light fades along and up the walls as the game's", wallLightFadesAlongAndUp},
        {"Room light matches the game", roomLightMatchesTheGame},
        {"Window light matches the game", windowLightMatchesTheGame},
        {"Indoor light stays safe on bad data", indoorLightStaysSafeOnBadData},
        {"Portable maths matches the C library", portableMathMatchesTheLibrary},
        {"Joints the animation leaves keep the mesh's own rest", unanimatedJointsKeepTheMeshRest},
        {"ObjectManager scripts find objects", objectManagerScriptsFindObjects},
        {"data table scripts keep values", dataTableScriptsKeepValues},
        {"object scripts read and write values", objectScriptsReadAndWriteValues},
        {"inventory scripts use saved tokens", inventoryScriptsUseSavedTokens},
        {"old operands are upgraded", oldOperandsAreUpgraded},
        {"Manage Inventory changes tokens", manageInventoryChangesTokens},
        {"saved data tables are read", savedDataTablesAreRead},
        {"scenegraph hashes and textures", scenegraphHashesAndTextures},
        {"Light resources are read", lightResourcesAreRead},
        {"SLOT resource counts container slots", slotResourceCountsContainerSlots},
        {"pattern map reads names by id", patternMapReadsNamesById},
        {"name map reads names", nameMapReadsNames},
        {"scene transforms turn and move", sceneTransformsTurnAndMove},
        {"scene quads face their normal", sceneQuadsFaceTheirNormal},
        {"scene hip roof rises to its ridge", sceneHipRoofRisesToItsRidge},
        {"scene grid roofs match the game", sceneGridRoofsMatchTheGame},
        {"terrain resources parse", terrainResourcesParse},
        {"scene cutaway cuts walls in front of rooms", sceneCutawayCutsWallsInFrontOfRooms},
        {"scene views show storeys, walls and roofs", sceneViewsShowStoreysWallsAndRoofs},
        {"roofs read every version", roofsReadEveryVersion},
        {"ui scripts parse", uiScriptsParse},
        {"targa images decode", targaImagesDecode},
        {"neighborhood videos decode", neighborhoodVideosDecode},
        {"neighborhood terrain parses", neighborhoodTerrainParses},
        {"neighborhood camera reads", neighborhoodCameraReads},
        {"neighborhood paint rules read", neighborhoodPaintRulesRead},
        {"neighborhood light map looks up normals", neighborhoodLightMapLooksUpNormals},
        {"neighborhood roads parse", neighborhoodRoadsParse},
        {"lot description parse", lotDescriptionParse},
        {"binary property sets and reference lists", binaryPropertySetsAndReferenceLists},
        {"animation curves sample", animationCurvesSample},
        {"vertex aliases follow kind", vertexAliasesFollowKind},
        {"fence post layer reads posts", fencePostLayerReadsPosts},
        {"build scripts read patterns and wall styles", buildScriptsReadPatternsAndWallStyles},
        {"wall layer reads coverings per side", wallLayerReadsCoveringsPerSide},
        {"wants and fears parse", wantsAndFearsParse},
        {"interaction table parse", interactionTableParse},
        {"slot owners read the held object", slotOwnersReadTheHeldObject},
        {"Set to Next separates people from objects", setToNextSeparatesPeopleFromObjects},
        {"Set to Next can target a local", setToNextCanTargetALocal},
        {"Set to Next without target flag uses stack object", setToNextWithoutTargetFlagUsesStackObject},
        {"Set to Next skips flagged objects unless asked", setToNextSkipsFlaggedObjectsUnlessAsked},
        {"Set to Next can require a data field value", setToNextCanRequireADataFieldValue},
        {"person searches filter by species", personSearchesFilterBySpecies},
        {"verify searches check the target without moving it", verifySearchesCheckTheTargetWithoutMovingIt},
        {"verify neighbor checks the neighborhood", verifyNeighborChecksTheNeighborhood},
        {"Sim description reads person data by version", simDescriptionReadsPersonDataByVersion},
        {"family record is read by version", familyRecordIsReadByVersion},
        {"family ties are read per Sim", familyTiesAreReadPerSim},
        {"array operations work on object arrays", arrayOperationsWorkOnObjectArrays},
        {"neighbor fields come from the neighbor", neighborFieldsComeFromTheNeighbor},
        {"Create Object makes it the stack object", createObjectMakesItTheStackObject},
        {"Create Object can take its GUID from temps", createObjectCanTakeItsGuidFromTemps},
        {"Create Object with unknown GUID returns false", createObjectWithUnknownGuidReturnsFalse},
        {"Create Object refuses templates", createObjectRefusesTemplates},
        {"Create Object rejects two passing values", createObjectRejectsTwoPassingValues},
        {"Run Tree by Name runs the stack object's tree", runTreeByNameRunsTheStackObjectsTree},
        {"Run Tree by Name takes false when the tree is missing", runTreeByNameTakesFalseWhenTheTreeIsMissing},
        {"Run Tree by Name can pass the caller's params", runTreeByNameCanPassTheCallersParams},
        {"Run Tree by Name other modes report themselves", runTreeByNameOtherModesReportThemselves},
        {"Run Tree by Name pushes onto my stack", runTreeByNamePushesOntoMyStack},
        {"Relationship writes and reads back", relationshipWritesAndReadsBack},
        {"Relationship read creates the variable unless asked", relationshipReadCreatesTheVariableUnlessAsked},
        {"Relationship with a missing object is an error", relationshipWithMissingObjectIsAnError},
        {"Relationship with neighbor ids uses the neighborhood", relationshipWithNeighborIdsUsesTheNeighborhood},
        {"Constants [temp] index with temp 0", constantsByTempIndexWithTempZero},
        {"motives are clamped and need a person", motivesAreClampedAndNeedAPerson},
        {"Change Light sets how strongly an object's lights shine", changeLightSetsLightStrength},
        {"Remove Object removes the stack object", removeObjectRemovesTheStackObject},
        {"Remove Object refuses protected objects", removeObjectRefusesProtectedObjects},
        {"removing me ends the run unless return immediately", removingMeEndsTheRunUnlessReturnImmediately},
        {"Set to Next wraps around like 'Hide all'", setToNextWrapsAroundLikeHideAll},
        {"Test Object Type compares GUIDs", testObjectTypeComparesGuids},
        {"Test Object Type on missing object is false", testObjectTypeOnMissingObjectIsFalse},
        {"Test Object Type can ask whether a GUID exists", testObjectTypeCanAskWhetherAGuidExists},
        {"Random Number stays in range", randomNumberStaysInRange},
        {"Random Number range can come from a local", randomNumberRangeCanComeFromALocal},
        {"Random Number with zero range writes nothing", randomNumberWithZeroRangeWritesNothing},
        {"Break Point ignored in release continues", breakPointIgnoredInReleaseContinues},
        {"Break Point with false condition continues", breakPointWithFalseConditionContinues},
        {"Break Point with true condition is an error", breakPointWithTrueConditionIsAnError},
        {"Expression ignores stale operand bytes", expressionIgnoresStaleOperandBytes},
        {"Sleep yields inside a call and resumes", sleepYieldsInsideACallAndResumes},
        {"Sleep as last instruction completes on resume", sleepAsLastInstructionCompletesOnResume},
        {"run on a busy thread is an error", runOnABusyThreadIsAnError},
        {"resume with nothing to run is an error", resumeWithNothingToRunIsAnError},
        {"errors leave the thread idle", errorsLeaveTheThreadIdle},
        {"person data belongs to each Sim", personDataBelongsToEachSim},
        {"person data [temp] uses a temp as the index", personDataByTempUsesATempAsTheIndex},
        {"person data on a non-person is an error", personDataOnANonPersonIsAnError},
        {"interaction runs object code in the Sim's thread", interactionRunsObjectCodeInTheSimsThread},
        {"presentation-only Generic Sims Calls continue", presentationOnlyGenericCallsContinue},
        {"other Generic Sims Calls report their id", otherGenericCallsReportTheirId},
        {"attributes need a 'me' object", attributesNeedAMeObject},
        {"lot number comes from the package name", lotNumberComesFromThePackageName},
        {"portrait image is the life stage flag", portraitImageIsTheLifeStageFlag},
        {"needs bar rows match the game", needsBarRowsMatchTheGame},
        {"mood rows match the game", moodRowsMatchTheGame},
        {"bar painting keeps the fill shape", barPaintingKeepsTheFillShape},
        {"mood colour fills the background shape", moodColourFillsTheBackgroundShape},
        {"motives are shown as whole numbers", motivesAreShownAsWholeNumbers},
        {"mood meter picks a column", moodMeterPicksAColumn},
        {"want icon is placed on its picture", wantIconIsPlacedOnItsPicture},
        {"portrait pose and camera", portraitPoseAndCamera},
        {"portrait is cropped for thumbnails", portraitIsCroppedForThumbnails},
        {"thumbnail pictures are scaled and masked", thumbnailPicturesAreScaledAndMasked},
        {"target pictures load in the background", targetPicturesLoadInTheBackground},
        {"log keeps its lines and files", logKeepsItsLinesAndFiles},
        {"crash reports say what happened", crashReportsSayWhatHappened},
        {"unexpected exits say what Windows noted", unexpectedExitsSayWhatWindowsNoted},
        {"report upload pieces", reportUploadPieces},
        {"reports don't name the user", reportsDontNameTheUser},
        {"log file stops at its limit", logFileStopsAtItsLimit},
        {"diagnostics keep to their own files", diagnosticsKeepToTheirOwnFiles},
        {"portrait shadow is its blurred outline", portraitShadowIsItsBlurredOutline},
    };

    for (const auto& [name, test] : tests) {
        if (!filter.empty() && std::string_view(name).find(filter) == std::string_view::npos) {
            continue;
        }
        const int before = failures;
#if defined(_MSC_VER) && defined(_DEBUG)
        // In debug builds, memory a test leaves allocated is a failure.
        _CrtMemState start, end, change;
        _CrtMemCheckpoint(&start);
        test();
        _CrtMemCheckpoint(&end);
        if (_CrtMemDifference(&change, &start, &end) && change.lCounts[_NORMAL_BLOCK] > 0) {
            std::printf("  leaked %zu blocks, %zu bytes\n", change.lCounts[_NORMAL_BLOCK], change.lSizes[_NORMAL_BLOCK]);
            _CrtMemDumpAllObjectsSince(&start);
            ++failures;
        }
#else
        test();
#endif
        std::printf("%s %s\n", failures == before ? "[pass]" : "[FAIL]", name);
    }
    std::printf("\n%zu tests, %d failed checks\n", std::size(tests), failures);
    return failures == 0 ? 0 : 1;
}
