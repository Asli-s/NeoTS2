// Lua scripts and what they reach: inventory, data tables, the object manager.
#include "test_support.hpp"

namespace {

// Whether `run` ends false after its script raised an object error, as the game goes on past one.
template <typename Run>
bool scriptRaisesError(const ts2::sim::World& world, Run run) {
    const std::size_t before = world.scripts()->scriptErrors;
    return run().outcome == Outcome::False && world.scripts()->scriptErrors == before + 1;
}

enum class ScriptEnd { True, False, Stopped };

// How one script ends when an object runs it through primitive 0x7E: stopped when it raised an
// object error or a Lua error.
ScriptEnd runScript(const char* source) {
    auto runtime = std::make_shared<ts2::sim::LuaRuntime>();
    runtime->load({}, {compileScript("Script", source)});
    CHECK(runtime->problems.empty());
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x130, {"Script"});
    Instruction in;
    in.opcode = 0x007E;
    in.trueTarget = T;
    in.falseTarget = F;
    in.operands[0] = 0x30;
    in.operands[1] = 0x01;
    in.operands[2] = 1;
    in.operands[4] = 0x01;
    code.add(kObjectGroup, 0x1000, bhav({in}));
    ts2::sim::World world;
    world.setScripts(runtime);
    auto& object = world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);
    const RunResult r = vm.run(object, 0x1000);
    if (runtime->scriptErrors != 0 || r.outcome == Outcome::Unsupported) {
        return ScriptEnd::Stopped;
    }
    return r.outcome == Outcome::True ? ScriptEnd::True : ScriptEnd::False;
}

// Scripts can't make a userdata of their own, which could pass for an engine object.
void scriptsCantMakeUserdata() {
    CHECK(runScript("SetScriptReturnValue(newproxy == nil)") == ScriptEnd::True);
    CHECK(runScript("newproxy(true)") == ScriptEnd::Stopped);
}

// Scripts can't start coroutines, which would run without the instruction count.
void scriptsCantStartCoroutines() {
    CHECK(runScript("SetScriptReturnValue(coroutine == nil)") == ScriptEnd::True);
    CHECK(runScript("coroutine.wrap(function() while true do end end)()") == ScriptEnd::Stopped);
}

// getmetatable on an engine object gives its class name, so scripts can't give it a finalizer.
void scriptsCantChangeClassMetatables() {
    CHECK(runScript("SetScriptReturnValue(getmetatable(ObjectManager.new()) == 'ObjectManager')") == ScriptEnd::True);
    CHECK(runScript("getmetatable(ObjectManager.new()).__gc = function() while true do end end") == ScriptEnd::Stopped);
}

// A table holding the same table twice at every level stops the script instead of copying to
// billions of values.
void sharedTablesCopyWithinALimit() {
    CHECK(runScript(R"(
        local t = ObjectDataTable.new(1)
        t:newTable(5, "Shared")
        local s = {}
        for i = 1, 30 do s = {s, s} end
        t:setTableFromCopy(5, s)
    )") == ScriptEnd::Stopped);
}

// Patterns that would recurse past the C stack or search without end stop the script. The
// pattern the game's own scripts use still works.
void patternsStopWithinLimits() {
    CHECK(runScript("SetScriptReturnValue((string.gsub('Hi $Me', '%$Me', 'Bob')) == 'Hi Bob')") == ScriptEnd::True);
    CHECK(runScript("string.find(string.rep('a', 100000), string.rep('a?', 100000))") == ScriptEnd::Stopped);
    CHECK(runScript("string.find(string.rep('a', 3000), string.rep('a*', 40) .. 'b')") == ScriptEnd::Stopped);
}

// Without byte 4 bit 0, primitive 0x7E runs the Lua source in the string table entry's description,
// kept compiled as "<name>_0x<group>". Text that looks like a compiled chunk is refused.
void luaScriptPrimitiveRunsDescriptions() {
    auto runtime = std::make_shared<ts2::sim::LuaRuntime>();
    runtime->load({}, {});
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x130, {"CountThings", "Binary"});
    code.addStringDescriptions(ts2::sim::kGlobalGroup, 0x130,
                               {"SetTemp(0, GetTemp(0) + 1) SetScriptReturnValue(true)", "\x1bLua"});
    auto call = [](std::uint8_t index) {
        Instruction in;
        in.opcode = 0x007E;
        in.trueTarget = T;
        in.falseTarget = F;
        in.operands[0] = 0x30;
        in.operands[1] = 0x01;
        in.operands[2] = index;
        return in;
    };
    code.add(kObjectGroup, 0x1000, bhav({call(1)}));
    code.add(kObjectGroup, 0x1001, bhav({call(2)}));
    ts2::sim::World world;
    world.setScripts(runtime);
    auto& object = world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);
    CHECK(vm.run(object, 0x1000).outcome == Outcome::True && object.thread.temps[0] == 1);
    CHECK(vm.run(object, 0x1000).outcome == Outcome::True && object.thread.temps[0] == 2);
    CHECK(runtime->hasScript("CountThings_0x7fd46cd0") && !runtime->hasScript("CountThings"));
    CHECK(vm.run(object, 0x1001).outcome == Outcome::Unsupported);
}

// Primitive 0x7E runs a Lua script named in string table 0x130. The script reads the
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
                                               "nGameState.GetProperty('Nope') == nil and RoundFloat(-12582913) == 1950351360)")});
    CHECK(runtime->problems.empty());

    auto lua = [](std::uint16_t index, bool params, std::uint8_t paramOwner, std::uint16_t paramData) {
        Instruction in;
        in.opcode = 0x007E;
        in.trueTarget = T;
        in.falseTarget = F;
        in.operands[0] = 0x30;
        in.operands[1] = 0x01;
        in.operands[2] = static_cast<std::uint8_t>(index);
        in.operands[4] = params ? 0x09 : 0x01;
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
    // Byte 4 bit 1 names it from the code owner's own table, bit 2 from its semi-global group's.
    code.addStrings(kObjectGroup, 0x130, {"Bits"});
    code.addStrings(kSemiGroup, 0x130, {"Properties"});
    Instruction own = lua(1, false, 0, 0), semi = lua(1, false, 0, 0);
    own.operands[4] = 0x03;
    semi.operands[4] = 0xFD;  // as shipped trees have it
    code.add(kObjectGroup, 0x1006, bhav({own}));
    code.add(kObjectGroup, 0x1007, bhav({semi}));
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
    // A script that raises an object error makes the primitive false and the tree goes on.
    CHECK(scriptRaisesError(world, [&] { return vm.run(object, 0x1003); }));
    CHECK(scriptRaisesError(world, [&] { return vm.run(object, 0x1004); }));
    CHECK(vm.run(object, 0x1005).outcome == Outcome::True && object.thread.temps[3] == 2);
    CHECK(std::get<std::int32_t>(runtime->properties.at("Count")) == 2);
    object.thread.temps[0] = 0;
    CHECK(vm.run(object, 0x1006).outcome == Outcome::False && object.thread.temps[0] == 5);
    object.thread.temps[3] = 0;
    CHECK(vm.run(object, 0x1007).outcome == Outcome::True && object.thread.temps[3] == 2);
}

// The object-facing script functions read and write the same values the VM uses. The running
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
        SetTemp(6, GetNeighborData(8, 0) * 10 + GetNeighborData(8, 2))
        SetTemp(7, GetNeighborData(8, -1) + 5)  -- a negative field has no value, not an error
        nMessaging.PostMessage(3, true, 2, "changed")
        SetSimulatorGlobal(5, 77)
        local x, y, level = nWorld.GetObjectTile(fridge)
        SetTemp(5, x * 100 + y * 10 + level)
        SetScriptReturnValue(GetObjectName(fridge) == "Fridge")
    )"), compileScript("BadAttribute", "GetObjectAttribute(GetStackObjectId(), 200)"),
           compileScript("BadMotive", "GetPersonMotive(GetRunningObjectId(), 16)"),
           compileScript("BadArray", "GetObjectArrayCopy(GetStackObjectId(), 1)"),
           compileScript("Running", "SetTemp(0, GetRunningObjectId() * 10 + GetRunningOnObjectId())"),
           compileScript("BadNeighborData", "GetNeighborData(8, 11)"),
           compileScript("BadMessage", "nMessaging.PostMessage(3, \"a\", \"b\")")});
    CHECK(runtime->problems.empty());

    MemoryCodeSource code;
    auto fridgeType = typed(kFridgeGuid);
    fridgeType.name = "Fridge";
    code.addObjectType(kObjectGroup, fridgeType);
    code.addStrings(ts2::sim::kGlobalGroup, 0x130, {"Values", "BadAttribute", "BadMotive", "BadArray", "Running", "BadNeighborData", "BadMessage"});
    Instruction in;
    in.opcode = 0x007E;
    in.operands[4] = 0x01;
    in.trueTarget = T;
    in.falseTarget = F;
    in.operands[0] = 0x30;
    in.operands[1] = 0x01;
    for (std::uint8_t i = 1; i <= 7; ++i) {
        in.operands[2] = i;
        code.add(kObjectGroup, static_cast<std::uint16_t>(0x0FFF + i), bhav({in}));
    }
    code.add(kObjectGroup, 0x1010, bhav({call(0x1000, T, F)}));
    code.add(kObjectGroup, 0x1011, bhav({call(0x1004, T, F)}));

    ts2::sim::World world;
    world.setScripts(runtime);
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {8};
    hood->neighbors[8].guid = 0x51515151;
    hood->neighbors[8].personData.assign(ts2::sim::kNeighborPersonDataCount, 0);
    hood->neighbors[8].personData[3] = 6;
    hood->neighbors[8].personData[ts2::sim::kLifeStagePersonData] = 3;
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
    CHECK(t[5] == 731 && t[6] == 13 && t[7] == 5);
    CHECK(fridge.attributes[3] == 15 && fridge.semiAttributes[1] == 9);
    CHECK((*world.neighborPersonData(8))[3] == 7);
    CHECK(vm.globals[5] == 77);
    for (const std::uint16_t tree : std::array<std::uint16_t, 5>{0x1001, 0x1002, 0x1003, 0x1005, 0x1006}) {
        CHECK(scriptRaisesError(world, [&] { return vm.run(sim, kObjectGroup, tree); }));
    }
    // The fridge running its own code runs on itself.
    fridge.thread.stackObjectId = fridge.id;
    CHECK(vm.run(fridge, 0x1011).outcome == Outcome::False && fridge.thread.temps[0] == 22);
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

// Each instruction continues with the next on true. The last returns true.
std::vector<Instruction> chained(std::vector<Instruction> instructions) {
    for (std::size_t i = 0; i + 1 < instructions.size(); ++i) {
        instructions[i].trueTarget = static_cast<std::uint16_t>(i + 1);
    }
    return instructions;
}

// Manage Inventory 0x10 puts the stack object away: a visible token of its type with a new archive
// id each time. No stack object is false.
void manageInventoryPutsTheStackObjectAway() {
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({manageInventory(0x13, 0x10, 0x0C, 0, 0, 0, 0)}));
    auto record = std::make_shared<ts2::ngbh::Neighborhood>();
    record->neighbors[8];
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->record = record;
    ts2::sim::World world;
    world.setNeighborhood(hood);
    auto& sim = world.create(kSimGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& fridge = world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::False);
    sim.thread.stackObjectId = fridge.id;
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True);
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True);
    const auto& tokens = world.inventory(ts2::sim::InventoryKind::Neighbor, 8)->singular;
    CHECK(tokens.size() == 2 && tokens[0]->guid == kFridgeGuid && tokens[0]->flags == 1);
    CHECK(tokens[0]->value2 != 0 && tokens[1]->value2 != tokens[0]->value2);
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
        manageInventory(kNeighborVisible, 3, 0x0C, owner::kTemp, 0, 0, 0),
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
    // a missing token is false. A neighbor without an inventory gets an empty one, a lot without one has none
    Instruction missing = manageInventory(kNeighborVisible, 4, 0x0C, owner::kLiteral, 9, owner::kLiteral, 1);
    Instruction otherNeighbor = manageInventory(kNeighborVisible, 10, 0x0C, 0, 0, owner::kTemp, 2);
    otherNeighbor.operands[2] = 9;
    code.add(kObjectGroup, 0x1002, bhav({missing}));
    code.add(kObjectGroup, 0x1003, bhav({otherNeighbor}));
    Instruction otherLot = otherNeighbor;
    otherLot.operands[0] = 0x11;  // lot inventory
    code.add(kObjectGroup, 0x1005, bhav({otherLot}));
    // finding again from the last position goes on past it
    code.add(kObjectGroup, 0x1004, bhav({manageInventory(kNeighborVisible, 3, 0x0C, owner::kTemp, 0, 0, 0)}));

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
    sim.thread.temps[2] = 7;
    CHECK(vm.run(sim, kObjectGroup, 0x1003).outcome == Outcome::True && t[2] == 0);
    CHECK(world.inventory(ts2::sim::InventoryKind::Neighbor, 9) != nullptr);
    CHECK(vm.run(sim, kObjectGroup, 0x1005).outcome == Outcome::Error);
    sim.thread.temps[0] = 2;
    CHECK(vm.run(sim, kObjectGroup, 0x1004).outcome == Outcome::False);
}

// Data owner 0x27 (Temporary Token) is the object's token reference that a copy into its token
// sets: kind, id, counted, index from 0, GUID halves, flags and property count. Changed flags are
// what a copy back into the inventory gets. Fields 8 and 9 are not values, and past 9 is an error.
void temporaryTokenReadsTheTokenReference() {
    constexpr std::uint8_t kNeighborVisible = 0x13;
    constexpr std::uint8_t kToken = 0x27;
    using ts2::sim::op::kAssign;
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav(chained({
        manageInventory(kNeighborVisible, 6, 0x0C, owner::kLiteral, 1, 0, 0),
        expr(owner::kTemp, 0, kAssign, kToken, 0, 0, 0),
        expr(owner::kTemp, 1, kAssign, kToken, 1, 0, 0),
        expr(owner::kTemp, 2, kAssign, kToken, 3, 0, 0),
        expr(owner::kTemp, 3, kAssign, kToken, 4, 0, 0),
        expr(owner::kTemp, 4, kAssign, kToken, 7, 0, 0),
        expr(kToken, 6, ts2::sim::op::kClearFlag, owner::kLiteral, 1, 0, 0),
        expr(owner::kTemp, 5, kAssign, kToken, 6, 0, 0),
        manageInventory(kNeighborVisible, 11, 0x0C, 0, 0, 0, 0),
    })));
    code.add(kObjectGroup, 0x1001, bhav({expr(owner::kTemp, 0, kAssign, kToken, 8, T, F)}));
    code.add(kObjectGroup, 0x1002, bhav({expr(owner::kTemp, 0, kAssign, kToken, 10, T, F)}));

    auto record = std::make_shared<ts2::ngbh::Neighborhood>();
    ts2::xobj::Token saved;
    saved.guid = 4242;
    saved.flags = 1;
    saved.properties = {7, 8, 9};
    record->neighbors[8].singular = {saved};
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->record = record;
    ts2::sim::World world;
    world.setNeighborhood(hood);
    auto& fridge = world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);

    CHECK(vm.run(fridge, kObjectGroup, 0x1000).outcome == Outcome::True);
    const auto& t = fridge.thread.temps;
    CHECK(t[0] == 3 && t[1] == 8 && t[2] == 0 && t[3] == 4242 && t[4] == 3 && t[5] == 0);
    const auto& tokens = world.inventory(ts2::sim::InventoryKind::Neighbor, 8)->singular;
    CHECK(tokens.size() == 2 && tokens[1]->guid == 4242 && tokens[1]->flags == 0);
    CHECK(vm.run(fridge, kObjectGroup, 0x1001).outcome == Outcome::Unsupported);
    CHECK(vm.run(fridge, kObjectGroup, 0x1002).outcome == Outcome::Error);
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
    // Tables nested 32 deep, each claiming the largest count, in a small resource: refused as too
    // short without allocating for the claimed pairs.
    std::vector<std::uint8_t> nested = {1, 0, 0, 0, 1, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0};
    for (int level = 0; level < 32; ++level) {
        nested.insert(nested.end(), {0xA0, 0x86, 0x01, 0x00});  // 100000 pairs
        nested.push_back(5);  // whose first key is another table
    }
    nested.resize(nested.size() + 1000000, 0);
    threw = false;
    try {
        ts2::datatables::parse(nested);
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
    in.operands[4] = 0x01;
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

// Inventories come from the saved neighborhood. Scripts list, count, add and change tokens. Token
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
           compileScript("NoInventory", "Inventory.new(Inventory.kTypeLot, 99)"),
           compileScript("NewToken", "InventoryToken.new()"),
           compileScript("BadProperty", "Inventory.new(3, 8):getTokenAtIndex(1):getProperty(9)"),
           compileScript("WrongSelf", "Inventory.countTokens(Inventory.new(Inventory.kTypeNeighbor, 8):getTokenAtIndex(1))"),
           compileScript("EarlyGc", R"(
               local token = Inventory.new(Inventory.kTypeNeighbor, 8):getTokenAtIndex(1)
               local gc = getmetatable(token).__gc
               gc(token)
               gc(token)
               gc()
               token:getGUID()
           )"),
           compileScript("GcOnOther", R"(
               local inv = Inventory.new(Inventory.kTypeNeighbor, 8)
               local meta = getmetatable(inv:getTokenAtIndex(1))
               meta.__gc(inv)
               meta.__index.getGUID(inv)
           )")});
    CHECK(runtime->problems.empty());

    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x130, {"Tokens", "BadKind", "NoInventory", "NewToken", "BadProperty", "WrongSelf",
                                                     "EarlyGc", "GcOnOther"});
    Instruction in;
    in.opcode = 0x007E;
    in.operands[4] = 0x01;
    in.trueTarget = T;
    in.falseTarget = F;
    in.operands[0] = 0x30;
    in.operands[1] = 0x01;
    for (std::uint8_t i = 1; i <= 8; ++i) {
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
    // A token passed as an inventory's self is refused, not read as one.
    for (std::uint16_t tree = 0x1001; tree <= 0x1005; ++tree) {
        CHECK(scriptRaisesError(world, [&] { return vm.run(object, tree); }));
    }
    // Scripts can't reach the token finalizer to call it themselves: getmetatable gives the class name.
    CHECK(vm.run(object, 0x1006).outcome == Outcome::Unsupported);
    CHECK(vm.run(object, 0x1007).outcome == Outcome::Unsupported);
}

// NeighborhoodManager answers from the Neighborhoods folder's neighborhoods: validity, type names,
// the current neighborhood's type, the season of a quadrant (-1 for an unknown neighborhood) and
// how many of the current neighborhood's sub-neighborhoods have a type.
void neighborhoodManagerReadsNeighborhoods() {
    auto runtime = std::make_shared<ts2::sim::LuaRuntime>();
    runtime->load({}, {compileScript("Hoods", R"(
        local nm = NeighborhoodManager.new()
        SetTemp(0, nm:getSeasonOfQuadrant(27, 0))
        SetTemp(1, nm:getSeasonOfQuadrant(27, 3))
        SetTemp(2, nm:getSeasonOfQuadrant(99, 0))
        SetTemp(3, nm:getNumNeighborhoodsOfType("university"))
        SetTemp(4, nm:getNumNeighborhoodsOfType("suburb"))
        SetTemp(5, nm:getNumNeighborhoodsOfType("downtown"))
        SetTemp(6, nm:getNumNeighborhoodsOfType("anything"))
        SetTemp(7, nm:getNumNeighborhoodsOfType("suburb", 6))
        SetScriptReturnValue(nm:isValidNeighborhood(2) and not nm:isValidNeighborhood(99) and
            nm:getNeighborhoodType(27) == "university" and nm:getNeighborhoodType(99) == "unknown" and
            nm:getCurrentNeighborhoodType() == "primary")
    )"), compileScript("BadQuadrant", "NeighborhoodManager.new():getSeasonOfQuadrant(2, 4)")});
    CHECK(runtime->problems.empty());
    ts2::content::Neighborhood loaded;
    loaded.identity = ts2::nhoodid::NeighborhoodId{};
    loaded.identity->id = 2;
    loaded.identity->type = 1;
    loaded.identity->prefix = "N002";
    ts2::nhoodid::NeighborhoodId university;
    university.id = 27;
    university.type = 2;
    university.prefix = "N002";
    university.subPrefix = "U001";
    university.seasons = {3, 0, 2, 3};
    ts2::nhoodid::NeighborhoodId suburb = university, otherSuburb = university, elsewhere = university;
    suburb.id = 12;
    suburb.type = 4;
    suburb.value2 = 6;
    otherSuburb.id = 13;
    otherSuburb.type = 4;
    otherSuburb.value2 = 7;
    elsewhere.id = 9;
    elsewhere.prefix = "N001";
    ts2::sim::World world;
    world.setNeighborhood(ts2::sim::savedNeighborhood(loaded, {{27, university}, {12, suburb}, {13, otherSuburb}, {9, elsewhere}}));
    std::array<std::int16_t, 8> temps{};
    ts2::sim::ScriptCall call;
    call.world = &world;
    call.temps = &temps;
    CHECK(runtime->run("Hoods", call) && call.returnValue);
    CHECK(temps[0] == 3 && temps[1] == 3 && temps[2] == -1);
    // Only the current neighborhood's sub-neighborhoods count, not itself or another's.
    CHECK(temps[3] == 1 && temps[4] == 2 && temps[5] == 0 && temps[6] == 3 && temps[7] == 1);
    ts2::sim::ScriptCall bad;
    bad.world = &world;
    CHECK(!runtime->run("BadQuadrant", bad) && !bad.unsupported.empty());
}

// A module replacing the globals' metatable can't make the runtime's own lookups raise errors
// outside a protected call (which would end the process) or loop.
void scriptGlobalsAreLookedUpRaw() {
    auto runtime = std::make_shared<ts2::sim::LuaRuntime>();
    runtime->load({compileScript("Hostile", R"(
        setmetatable(_G, {__index = function() error("read") end, __newindex = function() error("write") end})
    )")}, {compileScript("Plain", "local x = 1")});
    CHECK(runtime->problems.empty());
    CHECK(runtime->hasScript("Plain") && !runtime->hasScript("Missing"));
    ts2::sim::ScriptCall call;
    CHECK(runtime->run("Plain", call) && !runtime->run("Missing", call));
}

// Running out of script memory inside an engine function (here while it builds a table) is Lua's
// own memory error, which unwinds the function like an exception. The runtime goes on working.
void scriptMemoryRunsOutInsideFunctions() {
    auto runtime = std::make_shared<ts2::sim::LuaRuntime>();
    runtime->load({}, {compileScript("Fill", R"(
        local t = ObjectDataTable.new(1)
        t:newTable(5, "Big")
        local value = {}
        for k = 1, 64 do value[k] = k end
        t:setTableValue(5, 1, value)
        local keep = {}
        for i = 1, 1000000 do keep[i] = t:getTableValue(5, 1) end
    )"), compileScript("After", "local s = {} for i = 1, 1000 do s[i] = {i} end SetScriptReturnValue(table.getn(s) == 1000)")});
    CHECK(runtime->problems.empty());
    ts2::sim::World world;
    world.setScripts(runtime);
    world.create(kObjectGroup, typed(kFridgeGuid));
    ts2::sim::ScriptCall fill;
    fill.world = &world;
    CHECK(!runtime->run("Fill", fill) && fill.luaError.find("not enough memory") != std::string::npos);
    ts2::sim::ScriptCall after;
    after.world = &world;
    CHECK(runtime->run("After", after) && after.returnValue);
}

// Data tables keep keyed values per object or neighbor: keys and setTableValueAsInt values are
// whole numbers, setTableValue values single-precision. A missing table or key reads as nil, and
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
           compileScript("NoNeighbor", "NeighborDataTable.new(9)"),
           // Tables nested deeper than saved tables may be, and a table holding itself, stop the script.
           compileScript("Deep", "local t = {} for i = 1, 100 do t = {t} end ObjectDataTable.new(1):setTableValue(5, 1, t)"),
           compileScript("Loop", "local t = {} t[1] = t ObjectDataTable.new(1):setTableFromCopy(5, t)")});
    CHECK(runtime->problems.empty());

    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x130, {"Tables", "Twice", "NoTable", "BadValue", "NoNeighbor", "Deep", "Loop"});
    Instruction in;
    in.opcode = 0x007E;
    in.operands[4] = 0x01;
    in.trueTarget = T;
    in.falseTarget = F;
    in.operands[0] = 0x30;
    in.operands[1] = 0x01;
    for (std::uint8_t i = 1; i <= 7; ++i) {
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
    for (std::uint16_t tree = 0x1001; tree <= 0x1006; ++tree) {
        CHECK(scriptRaisesError(world, [&] { return vm.run(object, tree); }));
    }
}

// CachedNeighbor reads and writes the neighbor's own person data (0 outside 0-230) and gives its
// GUID as a signed number.
// nWorld.HasSolidFloor: whether the point's quarter of its tile (split along both diagonals) has a
// floor pattern. Off the grid or without floors, no.
void hasSolidFloorReadsTheFloorQuarter() {
    auto runtime = std::make_shared<ts2::sim::LuaRuntime>();
    runtime->load({}, {compileScript("Floor", R"(
        SetTemp(0, (nWorld.HasSolidFloor(3, 4, 0) and 1 or 0) + (nWorld.HasSolidFloor(3.9, 4.5, 0) and 10 or 0) +
                   (nWorld.HasSolidFloor(3.5, 4.9, 0) and 100 or 0) + (nWorld.HasSolidFloor(99, 4, 0) and 1000 or 0))
        local x, y, level = nWorld.GetObjectTile(GetStackObjectId())
        local here = nWorld.GetObjectsOnTile(x, y, level)
        SetTemp(1, table.getn(here) * 100 + here[1])
        SetScriptReturnValue(true)
    )")});
    CHECK(runtime->problems.empty());
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x130, {"Floor"});
    Instruction in;
    in.opcode = 0x007E;
    in.operands[4] = 0x01;
    in.trueTarget = T;
    in.falseTarget = F;
    in.operands[0] = 0x30;
    in.operands[1] = 0x01;
    in.operands[2] = 1;
    code.add(kObjectGroup, 0x1000, bhav({in}));
    ts2::sim::World world;
    world.setScripts(runtime);
    world.setFloorPattern([](int level, int x, int y, int quarter) -> std::optional<std::uint16_t> {
        if (level != 0 || x != 3 || y != 4) return std::nullopt;
        return quarter == 0 ? std::uint16_t{0} : quarter == 2 ? std::uint16_t{7} : std::uint16_t{0};
    });
    auto& object = world.create(kObjectGroup, typed(kFridgeGuid));
    auto& other = world.create(kObjectGroup, typed(kChairGuid));
    auto& hidden = world.create(kObjectGroup, typed(kChairGuid));
    object.x = other.x = hidden.x = 5 << 16;
    object.y = other.y = hidden.y = 6 << 16;
    object.level = other.level = hidden.level = 1;
    object.tile = other.tile = hidden.tile = ts2::sim::Tile{5, 6, 1};
    hidden.data[0x38] = 0x04;
    object.thread.stackObjectId = other.id;
    Vm vm(code, {}, &world);
    CHECK(vm.run(object, 0x1000).outcome == Outcome::True && object.thread.temps[0] == 10);
    CHECK(object.thread.temps[1] == 200 + object.id);
}

// CachedPerson:isQueuedObjAdvertisingForX: true when an interaction in the Sim's queue advertises the
// motive for the Sim's age group. Motives 2 and 10-12 never count.
void queuedInteractionAdvertisingIsSeen() {
    auto runtime = std::make_shared<ts2::sim::LuaRuntime>();
    runtime->load({}, {compileScript("Queued", R"(
        local p = CachedPerson.new(GetStackObjectId())
        SetTemp(0, (p:isQueuedObjAdvertisingForX(5) and 1 or 0) + (p:isQueuedObjAdvertisingForX(6) and 10 or 0) +
                   (p:isQueuedObjAdvertisingForX(2) and 100 or 0))
        SetScriptReturnValue(true)
    )")});
    CHECK(runtime->problems.empty());
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x130, {"Queued"});
    auto fridgeType = typed(kFridgeGuid);
    fridgeType.fields[ts2::objd::field::kInteractionTableId] = 0x80;
    ts2::ttab::InteractionTable table;
    table.interactions.resize(1);
    table.interactions[0].stringIndex = 3;
    table.interactions[0].motives.resize(1);
    table.interactions[0].motives[0].resize(16);
    table.interactions[0].motives[0][5].delta = 40;
    table.interactions[0].motives[0][2].min = 10;
    code.addInteractionTable(kObjectGroup, 0x80, table);
    Instruction in;
    in.opcode = 0x007E;
    in.operands[4] = 0x01;
    in.trueTarget = T;
    in.falseTarget = F;
    in.operands[0] = 0x30;
    in.operands[1] = 0x01;
    in.operands[2] = 1;
    code.add(kObjectGroup, 0x1000, bhav({in}));
    ts2::sim::World world;
    world.setScripts(runtime);
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.personData[0x3A] = 30;
    auto& fridge = world.create(kObjectGroup, fridgeType);
    ts2::sim::QueuedInteraction q;
    q.object = fridge.id;
    q.index = 3;
    sim.queue.push_back(q);
    sim.thread.stackObjectId = sim.id;
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, 0x1000).outcome == Outcome::True && sim.thread.temps[0] == 1);
}

void cachedNeighborScriptsReadTheNeighbor() {
    auto runtime = std::make_shared<ts2::sim::LuaRuntime>();
    runtime->load({}, {compileScript("Neighbor", R"(
        local n = CachedNeighbor.new(8)
        n:setPersonData(5, 77)
        n:setPersonData(300, 1)
        SetTemp(0, n:getPersonData(5))
        SetTemp(1, n:getPersonData(-1) + n:getPersonData(231))
        SetTemp(2, n:getOutfitProperty(1, 0) * 100 + n:getOutfitProperty(1, 1) * 10 + n:getOutfitProperty(1, 2))
        SetTemp(3, n:getOutfitProperty(1, 8) + n:getOutfitProperty(1, 12) + n:getOutfitProperty(1, 5) * 10)
        SetScriptReturnValue(n:getGUID() == -2147483640)
    )")});
    CHECK(runtime->problems.empty());
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x130, {"Neighbor"});
    code.addObjectType(kSimGroup, typed(0x80000008, ts2::sim::kPersonType));
    code.addAgeData(kSimGroup, {{"haircolor", "00000003-0000-0000-0000-000000000000"},
                                {"skincolor", "B6EE1DBC-5BB3-4146-8315-02BD64EDA707"},
                                {"eyecolor", "00000000-0000-0000-0000-000000000000"},
                                {"species", "0"}});
    Instruction in;
    in.opcode = 0x007E;
    in.operands[4] = 0x01;
    in.trueTarget = T;
    in.falseTarget = F;
    in.operands[0] = 0x30;
    in.operands[1] = 0x01;
    in.operands[2] = 1;
    code.add(kObjectGroup, 0x1000, bhav({in}));
    ts2::sim::World world;
    world.setScripts(runtime);
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {8};
    hood->neighbors[8].guid = 0x80000008;
    hood->neighbors[8].personData.assign(231, 0);
    world.setNeighborhood(hood);
    world.setOutfitProperty([](std::int16_t neighbor, std::uint16_t category, std::uint16_t property) -> std::optional<std::int16_t> {
        return neighbor == 8 && category == 1 && property == 5 ? std::optional<std::int16_t>(std::int16_t{1}) : std::nullopt;
    });
    auto& object = world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);
    CHECK(vm.run(object, 0x1000).outcome == Outcome::True);
    CHECK(object.thread.temps[0] == 77 && object.thread.temps[1] == 0);
    CHECK(object.thread.temps[2] == 471 && object.thread.temps[3] == 10);
    CHECK((*world.neighborPersonData(8))[5] == 77);
}

// Neighborhood:isApartmentDoor is false off apartment lots. On one (type 8 or 9) it isn't ported yet.
void apartmentDoorIsFalseOffApartmentLots() {
    auto runtime = std::make_shared<ts2::sim::LuaRuntime>();
    runtime->load({}, {compileScript("Door", "SetScriptReturnValue(not Neighborhood.new():isApartmentDoor(1))")});
    CHECK(runtime->problems.empty());
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x130, {"Door"});
    Instruction in;
    in.opcode = 0x007E;
    in.operands[4] = 0x01;
    in.trueTarget = T;
    in.falseTarget = F;
    in.operands[0] = 0x30;
    in.operands[1] = 0x01;
    in.operands[2] = 1;
    code.add(kObjectGroup, 0x1000, bhav({in}));
    ts2::sim::World world;
    world.setScripts(runtime);
    auto& door = world.create(kObjectGroup, typed(kChairGuid));
    Vm vm(code, {}, &world);
    CHECK(vm.run(door, 0x1000).outcome == Outcome::True);
    world.setLotType(8);
    CHECK(vm.run(door, 0x1000).outcome == Outcome::Unsupported);
}

// GetNeighborRelationship gives a neighbor's variable towards another as the unsigned 32-bit value
// saved, false past the end. With `create` a missing variable is added as 0. A missing neighbor
// is error 0x37.
void neighborRelationshipsReadAsSaved() {
    auto runtime = std::make_shared<ts2::sim::LuaRuntime>();
    runtime->load({}, {compileScript("Rel", R"(
        SetTemp(0, GetNeighborRelationship(1, 9, 1) / 16384)
        SetTemp(1, GetNeighborRelationship(1, 9, 2) == 4294967291 and 1 or 0)
        SetTemp(2, GetNeighborRelationship(1, 9, 5) == false and 1 or 0)
        SetTemp(3, GetNeighborRelationship(9, 1, 3, true) == 0 and 1 or 0)
        SetScriptReturnValue(true)
    )"), compileScript("Missing", "GetNeighborRelationship(1, 77, 0)")});
    CHECK(runtime->problems.empty());
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->neighborIds = {1, 9};
    hood->relationships[{std::int16_t{1}, std::int16_t{9}}] = {10, 49152, -5};
    ts2::sim::World world;
    world.setNeighborhood(hood);
    std::array<std::int16_t, 8> temps{};
    ts2::sim::ScriptCall call;
    call.world = &world;
    call.temps = &temps;
    CHECK(runtime->run("Rel", call));
    CHECK(temps[0] == 3 && temps[1] == 1 && temps[2] == 1 && temps[3] == 1);
    CHECK(world.neighborRelationshipSize(9, 1) == 4);
    ts2::sim::ScriptCall missing;
    missing.world = &world;
    CHECK(!runtime->run("Missing", missing) && missing.errorCode == 0x37);
}

// Neighborhood:getLotOwner gives the owner its description names. getWitchLotType is 0 off lots of
// type 10. A lot the neighborhood doesn't place is error 0x9B for both.
void neighborhoodReadsLotOwners() {
    auto runtime = std::make_shared<ts2::sim::LuaRuntime>();
    runtime->load({}, {compileScript("Owner", R"(
        local n = Neighborhood.new()
        SetTemp(0, n:getLotOwner(29))
        SetTemp(1, n:getWitchLotType())
        SetScriptReturnValue(true)
    )"), compileScript("NoLot", "Neighborhood.new():getLotOwner(99)")});
    CHECK(runtime->problems.empty());
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    hood->lotTypes[10] = 0;
    hood->lotTypes[29] = 1;
    hood->lotTypes[40] = 10;
    hood->lotOwners = {{10, 0}, {29, 160}};
    ts2::sim::World world;
    world.setNeighborhood(hood);
    world.setLotNumber(10);
    std::array<std::int16_t, 8> temps{};
    ts2::sim::ScriptCall call;
    call.world = &world;
    call.temps = &temps;
    CHECK(runtime->run("Owner", call) && temps[0] == 160 && temps[1] == 0);
    ts2::sim::ScriptCall noLot;
    noLot.world = &world;
    CHECK(!runtime->run("NoLot", noLot) && noLot.errorCode == 0x9B);
    world.setLotNumber(40);
    ts2::sim::ScriptCall witch;
    witch.world = &world;
    witch.temps = &temps;
    CHECK(!runtime->run("Owner", witch) && !witch.unsupported.empty());
}

// kQuery_ForPartsofMultiTileObject lists every part of the given object's multi-tile object (they
// share the lead's id in object data 0x13), highest id first. An object that isn't multi-tile is an error.
void objectManagerListsMultiTileParts() {
    auto runtime = std::make_shared<ts2::sim::LuaRuntime>();
    runtime->load({}, {compileScript("Parts", R"(
        local om = ObjectManager.new()
        local parts = om:getObjectIds(ObjectManager.kQuery_ForPartsofMultiTileObject, 2)
        SetTemp(0, table.getn(parts))
        SetTemp(1, parts[1] * 100 + parts[2] * 10 + parts[3])
        SetScriptReturnValue(true)
    )"), compileScript("NotMultiTile", "ObjectManager.new():getObjectIds(ObjectManager.kQuery_ForPartsofMultiTileObject, 4)")});
    CHECK(runtime->problems.empty());
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x130, {"Parts", "NotMultiTile"});
    Instruction in;
    in.opcode = 0x007E;
    in.operands[4] = 0x01;
    in.trueTarget = T;
    in.falseTarget = F;
    in.operands[0] = 0x30;
    in.operands[1] = 0x01;
    for (std::uint8_t i = 1; i <= 2; ++i) {
        in.operands[2] = i;
        code.add(kObjectGroup, static_cast<std::uint16_t>(0x0FFF + i), bhav({in}));
    }
    ts2::sim::World world;
    world.setScripts(runtime);
    auto part = typed(kFridgeGuid);
    part.fields[ts2::objd::field::kMultiTileMasterId] = 7;
    for (int i = 0; i < 3; ++i) {
        world.create(kObjectGroup, part).data[0x13] = 1;  // ids 1-3, led by 1
    }
    world.create(kObjectGroup, typed(kChairGuid));          // id 4, on its own
    Vm vm(code, {}, &world);
    auto& me = *world.find(1);
    CHECK(vm.run(me, 0x1000).outcome == Outcome::True);
    CHECK(me.thread.temps[0] == 3 && me.thread.temps[1] == 321);
    CHECK(scriptRaisesError(world, [&] { return vm.run(me, 0x1001); }));
}

// ObjectManager answers object and neighbor lookups. Id lists come back as Lua arrays in id order,
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
        local humans, hoodDogs, named = om:getNeighborIds(1), om:getNeighborIds(2), om:getNeighborIds("x")
        SetTemp(6, table.getn(humans) * 100 + table.getn(hoodDogs) * 10 + table.getn(named))
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
    code.addObjectType(kObjectGroup, typed(0x0D000005, 0x11));
    code.addStrings(ts2::sim::kGlobalGroup, 0x130, {"Lookups", "BadGuid", "MissingObject"});
    Instruction in;
    in.opcode = 0x007E;
    in.operands[4] = 0x01;
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
    hood->neighborIds = {3, 5, 8};
    hood->neighbors[3].guid = 0x0D000003;
    hood->neighbors[5].guid = 0x0D000005;  // object type 0x11: never listed
    hood->neighbors[8].guid = kSimGuid;
    hood->neighbors[8].personData.assign(0xBB, 0);
    hood->neighbors[8].personData[0xBA] = 1;  // a large dog
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
    CHECK(t[1] == 101 && t[2] == 3 && t[3] == 3 && t[4] == 112 && t[5] == 13 && t[6] == 112);
    CHECK(scriptRaisesError(world, [&] { return vm.run(fridge, 0x1001); }));
    CHECK(scriptRaisesError(world, [&] { return vm.run(fridge, 0x1002); }));
}

} // namespace

std::vector<Test> vmScriptTests() {
    return {
        {"Manage Inventory puts the stack object away", manageInventoryPutsTheStackObjectAway},
        {"CachedNeighbor scripts read the neighbor", cachedNeighborScriptsReadTheNeighbor},
        {"CachedPerson sees queued interactions that advertise a motive", queuedInteractionAdvertisingIsSeen},
        {"nWorld.HasSolidFloor reads the floor quarter", hasSolidFloorReadsTheFloorQuarter},
        {"ObjectManager lists a multi-tile object's parts", objectManagerListsMultiTileParts},
        {"Neighborhood:isApartmentDoor is false off apartment lots", apartmentDoorIsFalseOffApartmentLots},
        {"Neighborhood reads lot owners", neighborhoodReadsLotOwners},
        {"neighbor relationships read as saved", neighborRelationshipsReadAsSaved},
        {"Lua script primitive runs scripts", luaScriptPrimitiveRunsScripts},
        {"ObjectManager scripts find objects", objectManagerScriptsFindObjects},
        {"data table scripts keep values", dataTableScriptsKeepValues},
        {"object scripts read and write values", objectScriptsReadAndWriteValues},
        {"inventory scripts use saved tokens", inventoryScriptsUseSavedTokens},
        {"Manage Inventory changes tokens", manageInventoryChangesTokens},
        {"Lua primitive runs descriptions", luaScriptPrimitiveRunsDescriptions},
        {"NeighborhoodManager reads neighborhoods", neighborhoodManagerReadsNeighborhoods},
        {"Temporary Token reads the token reference", temporaryTokenReadsTheTokenReference},
        {"saved data tables are read", savedDataTablesAreRead},
        {"script globals are looked up raw", scriptGlobalsAreLookedUpRaw},
        {"script memory runs out inside functions", scriptMemoryRunsOutInsideFunctions},
        {"scripts can't make userdata", scriptsCantMakeUserdata},
        {"scripts can't start coroutines", scriptsCantStartCoroutines},
        {"scripts can't change class metatables", scriptsCantChangeClassMetatables},
        {"shared tables copy within a limit", sharedTablesCopyWithinALimit},
        {"patterns stop within limits", patternsStopWithinLimits},
    };
}
