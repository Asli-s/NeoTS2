#include "sim/vm.hpp"
#include "formats/datatables.hpp"
#include "formats/fami.hpp"
#include "formats/famt.hpp"
#include "formats/hash.hpp"
#include "formats/scenegraph.hpp"
#include "formats/sdsc.hpp"
#include "formats/slot.hpp"
#include "content/build_scripts.hpp"
#include "formats/fencepostlayer.hpp"
#include "scene/neighborhood_lighting.hpp"
#include "scene/neighborhood_paint.hpp"
#include "scene/lot_parts.hpp"
#include "scene/view.hpp"
#include "formats/namemap.hpp"
#include "formats/roofs.hpp"
#include "formats/terrain.hpp"
#include "formats/stringmap.hpp"
#include "formats/walllayer.hpp"
#include "formats/swaf.hpp"
#include "formats/ttab.hpp"
#include "formats/tga.hpp"
#include "formats/reia.hpp"
#include "formats/camera_config.hpp"
#include "formats/nhood_paint_rules.hpp"
#include "formats/nhood_terrain.hpp"
#include "formats/ui_script.hpp"
#include "sim/lua_runtime.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <algorithm>
#include <bit>
#include <cstring>
#include <functional>
#include <array>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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
    Instruction animate;
    animate.opcode = 0x006A;
    animate.trueTarget = T;
    animate.falseTarget = F;
    code.add(kObjectGroup, 0x1000, bhav({animate}));
    ThreadState t;
    const RunResult r = runPrivate(code, t, 0x1000);
    CHECK(r.outcome == Outcome::Unsupported);
    CHECK(r.detail == "0x006A");
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

// A lot's roofs: version 1 entries, and version 3 adding each roof's slope.
void roofsReadBothVersions() {
    for (const std::uint32_t version : {1u, 3u}) {
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
        if (version == 3) {
            u32(0x3F000000);  // 0.5: 45 degrees
            u32(4);
        }
        const auto roofs = ts2::roofs::parse(d);
        CHECK(roofs.size() == 1 && roofs[0].id == 0x42 && roofs[0].x0 == 25.0f && roofs[0].y1 == 15.0f &&
              roofs[0].level1 == 2 && roofs[0].type == 2 && roofs[0].pattern == 0x8CDCBEF5);
        CHECK(roofs[0].slope == (version == 3 ? 0.5f : 0.0f));
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
void scenegraphHashesAndTextures() {
    const auto key = ts2::hash::scenegraphKey("fridgeCheap_cres");
    CHECK(key.instance == 0xFF4F7CA0 && key.instanceHigh == 0x128A6A6A);

    const std::vector<std::uint8_t> block = {0x00, 0xF8, 0x1F, 0x00, 0b11100100, 0, 0, 0};  // red, blue; 0 1 2 3
    const auto rgba = ts2::scenegraph::decodeRgba(block, 4, 4, 4);
    auto px = [&](int i) { return std::array<int, 4>{rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3]}; };
    CHECK((px(0) == std::array<int, 4>{255, 0, 0, 255}) && (px(1) == std::array<int, 4>{0, 0, 255, 255}));
    CHECK((px(2) == std::array<int, 4>{170, 0, 85, 255}) && (px(3) == std::array<int, 4>{85, 0, 170, 255}));
    CHECK((px(4) == std::array<int, 4>{255, 0, 0, 255}));  // second row: index 0 again

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
        {"Add/Change the Action String sets text and icon", actionStringSetsTextAndIcon},
        {"Lua script primitive runs scripts", luaScriptPrimitiveRunsScripts},
        {"ObjectManager scripts find objects", objectManagerScriptsFindObjects},
        {"data table scripts keep values", dataTableScriptsKeepValues},
        {"object scripts read and write values", objectScriptsReadAndWriteValues},
        {"inventory scripts use saved tokens", inventoryScriptsUseSavedTokens},
        {"old operands are upgraded", oldOperandsAreUpgraded},
        {"Manage Inventory changes tokens", manageInventoryChangesTokens},
        {"saved data tables are read", savedDataTablesAreRead},
        {"scenegraph hashes and textures", scenegraphHashesAndTextures},
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
        {"roofs read both versions", roofsReadBothVersions},
        {"ui scripts parse", uiScriptsParse},
        {"targa images decode", targaImagesDecode},
        {"neighborhood videos decode", neighborhoodVideosDecode},
        {"neighborhood terrain parses", neighborhoodTerrainParses},
        {"neighborhood camera reads", neighborhoodCameraReads},
        {"neighborhood paint rules read", neighborhoodPaintRulesRead},
        {"neighborhood light map looks up normals", neighborhoodLightMapLooksUpNormals},
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
    };

    for (const auto& [name, test] : tests) {
        if (!filter.empty() && std::string_view(name).find(filter) == std::string_view::npos) {
            continue;
        }
        const int before = failures;
        test();
        std::printf("%s %s\n", failures == before ? "[pass]" : "[FAIL]", name);
    }
    std::printf("\n%zu tests, %d failed checks\n", std::size(tests), failures);
    return failures == 0 ? 0 : 1;
}
