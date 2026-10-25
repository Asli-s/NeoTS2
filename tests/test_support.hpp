#pragma once

// Shared by the test files: the includes, CHECK, the in-memory code source, the
// instruction builders and the small worlds several areas use.

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
#include <limits>
#include <cmath>
#include "sim/clock.hpp"
#include "sim/advertisement.hpp"
#include "sim/advertisers.hpp"
#include "sim/happiness.hpp"
#include "sim/motive_update.hpp"
#include "sim/idle_spot.hpp"
#include "sim/object_footprint.hpp"
#include "sim/outside_grid.hpp"
#include "sim/object_reset.hpp"
#include "sim/overlay_events.hpp"
#include "sim/pie_menu.hpp"
#include "ui/font_metrics.hpp"
#include "ui/pie_layout.hpp"
#include "sim/route_grid.hpp"
#include "sim/route_goals.hpp"
#include "sim/route_items.hpp"
#include "sim/route_search.hpp"
#include "sim/route_world.hpp"
#include "sim/snap.hpp"
#include "sim/type_signature.hpp"
#include "sim/vm.hpp"
#include "formats/bcon.hpp"
#include "formats/datatables.hpp"
#include "formats/dbpf.hpp"
#include "formats/resource_types.hpp"
#include "formats/fami.hpp"
#include "formats/famt.hpp"
#include "formats/hash.hpp"
#include "formats/scenegraph.hpp"
#include "formats/sdsc.hpp"
#include "formats/slot.hpp"
#include "content/animation_lists.hpp"
#include "content/build_scripts.hpp"
#include "content/install.hpp"
#include "content/neighborhood.hpp"
#include "content/object_thumbnails.hpp"
#include "content/ui_meters.hpp"
#include "content/ui_text.hpp"
#include "content/wants.hpp"
#include "content/target_pictures.hpp"
#include "diag/log.hpp"
#include "diag/report.hpp"
#include "diag/timing.hpp"
#include "diag/upload.hpp"
#include "sim/motives.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/rig_layers.hpp"
#include "scene/object_lights.hpp"
#include "scene/object_sun.hpp"
#include "scene/pick.hpp"
#include "scene/rig.hpp"
#include "scene/sims.hpp"
#include "scene/texture_blend.hpp"
#include "scene/portrait.hpp"
#include "formats/cobject.hpp"
#include "formats/fencepostlayer.hpp"
#include "formats/neighbor_remap.hpp"
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
#include "scene/pool_water.hpp"
#include "scene/lot_scene.hpp"
#include "scene/view.hpp"
#include "formats/namemap.hpp"
#include "formats/roofs.hpp"
#include "formats/terrain.hpp"
#include "formats/stringmap.hpp"
#include "formats/walllayer.hpp"
#include "formats/weather.hpp"
#include "formats/neighborhood_id.hpp"
#include "sim/lot_world.hpp"
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

#include "lauxlib.h"
#include "lua.h"

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

using ts2::bhav::Behavior;
using ts2::bhav::Instruction;
using ts2::sim::Outcome;
using ts2::sim::RunResult;
using ts2::sim::ThreadState;
using ts2::sim::Vm;
namespace owner = ts2::sim::owner;
namespace op = ts2::sim::op;

inline int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("    FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

inline constexpr std::uint32_t kObjectGroup = 0x7F000001;
inline constexpr std::uint32_t kSemiGroup = 0x7F000002;
inline constexpr std::uint16_t T = ts2::bhav::kTargetTrue;
inline constexpr std::uint16_t F = ts2::bhav::kTargetFalse;
inline constexpr std::uint16_t E = ts2::bhav::kTargetError;

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
    void addObjectType(std::uint32_t group, ts2::objd::ObjectDefinition definition,
                       std::vector<ts2::slot::Entry> routingSlots = {}, std::vector<ts2::slot::Entry> containerSlots = {}) {
        const std::uint32_t guid = definition.guid();
        objectTypes_[guid] = ts2::sim::ObjectType{group, std::move(definition), 0, containerSlots.size(), {}, std::move(routingSlots),
                                                  std::move(containerSlots)};
    }
    void addInteractionTable(std::uint32_t group, std::uint16_t instance, ts2::ttab::InteractionTable table) {
        tables_[{group, instance}] = std::make_shared<const ts2::ttab::InteractionTable>(std::move(table));
    }
    std::shared_ptr<const ts2::ttab::InteractionTable> interactionTable(std::uint32_t group,
                                                                       std::uint16_t instance) override {
        const auto it = tables_.find({group, instance});
        return it != tables_.end() ? it->second : nullptr;
    }
    std::vector<ts2::slot::Entry> globals;
    const std::vector<ts2::slot::Entry>& globalSlots() override { return globals; }
    const ts2::sim::ObjectType* objectType(std::uint32_t guid) override {
        const auto it = objectTypes_.find(guid);
        return it != objectTypes_.end() ? &it->second : nullptr;
    }
    const ts2::sim::ObjectType* addMadeObjectType(ts2::sim::ObjectType type) override {
        const auto [it, added] = objectTypes_.try_emplace(type.definition.guid(), std::move(type));
        return added ? &it->second : nullptr;
    }
    void setMultiTileMaster(std::uint32_t partGuid, std::uint32_t masterGuid) { masters_[partGuid] = masterGuid; }
    const ts2::sim::ObjectType* multiTileMaster(const ts2::sim::ObjectType& part) override {
        const auto it = masters_.find(part.definition.guid());
        return it != masters_.end() ? objectType(it->second) : nullptr;
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
    void addStringDescriptions(std::uint32_t group, std::uint16_t instance, std::vector<std::string> values) {
        descriptions_[{group, instance}] = std::move(values);
    }
    std::optional<std::string> stringDescription(std::uint32_t group, std::uint16_t instance, std::size_t index) override {
        const auto it = descriptions_.find({group, instance});
        if (it == descriptions_.end() || index >= it->second.size()) {
            return std::nullopt;
        }
        return it->second[index];
    }
    std::optional<std::string> string(std::uint32_t group, std::uint16_t instance,
                                      std::size_t index) override {
        const auto it = strings_.find({group, instance});
        if (it == strings_.end() || index >= it->second.size()) {
            return std::nullopt;
        }
        return it->second[index];
    }
    void setTunings(ts2::fcns::Tunings tunings) { tunings_ = std::move(tunings); }
    const ts2::fcns::Tunings* tunings() override { return tunings_ ? &*tunings_ : nullptr; }
    void addAgeData(std::uint32_t group, std::map<std::string, std::string> properties) {
        ageData_[group] = std::move(properties);
    }
    const std::map<std::string, std::string>* ageData(std::uint32_t group) override {
        const auto it = ageData_.find(group);
        return it != ageData_.end() ? &it->second : nullptr;
    }
    void addInteractionStrings(std::uint32_t group, std::uint16_t instance, std::vector<std::string> values) {
        interactionStrings_[{group, instance}] = std::move(values);
    }
    std::optional<std::string> interactionString(std::uint32_t group, std::uint16_t instance, std::size_t index) override {
        const auto it = interactionStrings_.find({group, instance});
        if (it == interactionStrings_.end() || index >= it->second.size()) {
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
    std::optional<ts2::fcns::Tunings> tunings_;
    std::map<std::uint32_t, std::vector<ts2::objf::Function>> functions_;
    std::map<std::uint32_t, std::uint32_t> masters_;
    std::map<std::pair<std::uint32_t, std::uint16_t>, std::vector<std::string>> strings_;
    std::map<std::pair<std::uint32_t, std::uint16_t>, std::vector<std::string>> descriptions_;
    std::map<std::uint32_t, std::map<std::string, std::string>> ageData_;
    std::map<std::pair<std::uint32_t, std::uint16_t>, std::vector<std::string>> interactionStrings_;
    std::map<std::uint32_t, ts2::sim::ObjectType> objectTypes_;
    std::map<std::pair<std::uint32_t, std::uint16_t>, std::shared_ptr<const ts2::ttab::InteractionTable>> tables_;
    std::map<std::pair<std::uint32_t, std::uint16_t>, std::shared_ptr<const Behavior>> behaviors_;
    std::map<std::pair<std::uint32_t, std::uint16_t>, std::vector<std::int16_t>> constants_;
};

inline Instruction expr(std::uint8_t lhsOwner, std::uint16_t lhsData, std::uint8_t operation,
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

inline Instruction call(std::uint16_t opcode, std::uint16_t t, std::uint16_t f,
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

inline Behavior bhav(std::vector<Instruction> code, std::uint8_t args = 0, std::uint8_t locals = 0) {
    Behavior b;
    b.format = 0x8009;
    b.argCount = args;
    b.localCount = locals;
    b.instructions = std::move(code);
    return b;
}

inline RunResult runPrivate(MemoryCodeSource& code, ThreadState& thread, std::uint16_t entry,
                     ts2::sim::Limits limits = {}) {
    Vm vm(code, limits);
    return vm.run(thread, kObjectGroup, entry);
}

inline ts2::objd::ObjectDefinition definition(const char* name, std::uint16_t price) {
    ts2::objd::ObjectDefinition d;
    d.name = name;
    d.fields.assign(ts2::objd::kMaxFields, 0);
    d.fields[ts2::objd::field::kPrice] = price;
    return d;
}

inline constexpr std::uint32_t kFridgeGuid = 0x6C50BA66;
inline constexpr std::uint32_t kChairGuid = 0x12345678;

inline ts2::objd::ObjectDefinition typed(std::uint32_t guid, std::uint16_t type = 4) {
    ts2::objd::ObjectDefinition d;
    d.fields.assign(ts2::objd::kMaxFields, 0);
    d.fields[ts2::objd::field::kGuid1] = static_cast<std::uint16_t>(guid);
    d.fields[ts2::objd::field::kGuid2] = static_cast<std::uint16_t>(guid >> 16);
    d.fields[ts2::objd::field::kType] = type;
    return d;
}

// The object types SmallWorld's searches look for, known to the code source like the game's catalog.
inline void addSmallWorldTypes(MemoryCodeSource& code) {
    code.addObjectType(kObjectGroup, typed(kFridgeGuid));
    code.addObjectType(kObjectGroup, typed(kChairGuid));
}

inline Instruction setToNext(std::uint8_t searchType, std::uint8_t targetOwner, std::uint8_t targetData,
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

inline Instruction primitiveCall(std::uint16_t opcode, std::uint16_t t, std::uint16_t f);

inline Instruction primitiveCall(std::uint16_t opcode, std::uint16_t t, std::uint16_t f) {
    Instruction in;
    in.opcode = opcode;
    in.trueTarget = t;
    in.falseTarget = f;
    return in;
}

inline Instruction sleepFor(std::uint16_t paramIndex, std::uint16_t t) {
    Instruction in = primitiveCall(0x0000, t, E);
    in.operands[0] = static_cast<std::uint8_t>(paramIndex);
    return in;
}

inline Instruction createObject(std::uint32_t guid, std::uint8_t where, std::uint8_t flags,
                         std::uint16_t t, std::uint16_t f) {
    Instruction in = primitiveCall(0x002A, t, f);
    for (std::size_t i = 0; i < 4; ++i) {
        in.operands[i] = static_cast<std::uint8_t>(guid >> (8 * i));
    }
    in.operands[4] = where;
    in.operands[5] = flags;
    return in;
}

inline constexpr std::uint32_t kLampGroup = 0x7F000077;
inline constexpr std::uint32_t kLampGuid = 0x1A2B3C4D;
inline constexpr std::uint32_t kSimGroup = 0x7F000099;

// Compiles Lua source into a chunk, as a stand-in for the game's compiled scripts.
inline ts2::luascript::Script compileScript(const std::string& name, const std::string& source) {
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

inline Instruction removeObject(std::uint8_t who, std::uint8_t flags, std::uint16_t t, std::uint16_t f) {
    Instruction in = primitiveCall(0x0012, t, f);
    in.operands[0] = who;
    in.operands[2] = flags;
    return in;
}

// A 6x6 lot with walls around the square (1,1)-(3,3). Walls go clockwise on screen (y down),
// so the room inside (5) is on their right and the yard (0) on their left.
inline ts2::wallgraph::WallGraph squareRoom(bool splitDiagonally) {
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

inline float floorLight(ts2::scene::floorlight::Grid& g, int x, int y) {
    return g.at(x, y)->halves[0].light[0];
}

using Test = std::pair<const char*, void (*)()>;

std::vector<Test> vmCoreTests();
std::vector<Test> vmObjectTests();
std::vector<Test> vmInteractionTests();
std::vector<Test> vmScriptTests();
std::vector<Test> simulationTests();
std::vector<Test> routePlanningTests();
std::vector<Test> vmWalkingTests();
std::vector<Test> vmSetToNextTests();
std::vector<Test> vmAnimationTests();
std::vector<Test> formatTests();
std::vector<Test> modelAndImageTests();
std::vector<Test> sceneTests();
std::vector<Test> assetTests();
std::vector<Test> lightingTests();
std::vector<Test> contentTests();
std::vector<Test> uiAndDiagTests();
#ifdef NEOTS2_TEXT_LAYOUT_TESTS
std::vector<Test> textLayoutTests();
#endif
std::vector<Test> pathsTests();
std::vector<Test> fuzzRegressionTests();
