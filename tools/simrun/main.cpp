#include "../../app/route_sources.hpp"
#include "content/install.hpp"
#include "content/lot.hpp"
#include "content/neighborhood.hpp"
#include "content/scripts.hpp"
#include "content/wants.hpp"
#include "scene/animation_library.hpp"
#include "scene/outfit_properties.hpp"
#include "scene/assets.hpp"
#include "sim/lua_runtime.hpp"
#include "sim/ground_click.hpp"
#include "sim/route_portals.hpp"
#include "content/resource_index.hpp"
#include "formats/objd.hpp"
#include "formats/resource_types.hpp"
#include "formats/str.hpp"
#include "sim/index_code_source.hpp"
#include "sim/lot_simulation.hpp"
#include "sim/lot_world.hpp"
#include "sim/pie_menu.hpp"
#include "sim/type_signature.hpp"
#include "sim/vm.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ts2::content::ResourceIndex;
using ts2::sim::Outcome;
using ts2::sim::RunResult;

constexpr std::uint32_t kEditorStringsGroup = 0x7FE59FD0;
constexpr std::uint32_t kPrimitiveNames = 0x8B;
constexpr std::uint32_t kDataOwners = 0x84;
constexpr std::uint32_t kGenericSimsCalls = 0xDC;
constexpr std::uint32_t kSearchTypes = 0xA4;

std::vector<std::string> editorTable(ResourceIndex& index, std::uint32_t instance,
                                     std::uint32_t group = kEditorStringsGroup) {
    std::vector<std::string> labels;
    if (const auto* r = index.find({ts2::types::kStringTable, group, instance, 0})) {
        const auto table = ts2::str::parse(index.read(*r));
        for (const auto* entry : table.forLanguage(1)) {
            labels.push_back(entry->value);
        }
    }
    return labels;
}

std::string labelFor(const std::vector<std::string>& labels, const std::string& hexText) {
    const auto value = std::stoul(hexText, nullptr, 16);
    return value < labels.size() ? labels[value] : "";
}

void loadSource(ResourceIndex& index, const std::filesystem::path& source) {
    if (std::filesystem::is_directory(source)) {
        for (const auto& path : ts2::content::objectLoadOrder(ts2::content::findProducts(source))) {
            index.addPackage(path);
        }
    } else {
        index.addPackage(source);
    }
}

std::string describe(const RunResult& r, const std::vector<std::string>& primitives,
                     const std::vector<std::string>& owners,
                     const std::vector<std::string>& genericCalls = {},
                     const std::vector<std::string>& searchTypes = {}) {
    std::string text = std::string(ts2::sim::outcomeName(r.outcome));
    if (!r.reason.empty()) {
        text += ": " + r.reason;
    }
    if (r.reason == "primitive not implemented") {
        text += " " + r.detail + " " + labelFor(primitives, r.detail);
    } else if (r.reason == "Generic Sims Call not implemented") {
        text += " " + r.detail + " " + labelFor(genericCalls, r.detail);
    } else if (r.reason == "data owner not implemented") {
        text += " " + r.detail + " " + labelFor(owners, r.detail);
    } else if (r.reason == "Set to Next search type not implemented") {
        text += " " + r.detail + " " + labelFor(searchTypes, r.detail);
    } else if (r.reason == "expression operator not implemented" ||
               r.reason == "expression byte 4 is non-zero" ||
               r.reason == "Test Object Type byte 7 is non-zero" ||
               r.reason == "object error running script") {
        text += " " + r.detail;
    }
    return text;
}

struct ObjectCatalog {
    std::map<std::uint32_t, ts2::objd::ObjectDefinition> definitions;
    std::map<std::uint32_t, std::uint32_t> userOfSemiGlobal;
    std::uint32_t standInGroup = 0;
    std::size_t personDefinitions = 0;

    // The object a BHAV should run as: its own object, an object using its semi-global group,
    // or (for globals) a stand-in person. Empty when no object can reach the BHAV.
    // Container slots of the object type in `group` (from its SLOT resource).
    std::size_t containerSlots(std::uint32_t group, ts2::sim::CodeSource& code) const {
        const auto* type = code.objectType(definitions.at(group).guid());
        return type ? type->containerSlots : 0;
    }

    std::optional<std::uint32_t> objectFor(std::uint32_t group, std::uint16_t instance,
                                           ts2::sim::CodeSource& code) const {
        std::uint32_t objectGroup = standInGroup;
        if (definitions.contains(group)) {
            objectGroup = group;
        } else if (const auto it = userOfSemiGlobal.find(group); it != userOfSemiGlobal.end()) {
            objectGroup = it->second;
        }
        const std::uint32_t resolved = instance < 0x1000 ? ts2::sim::kGlobalGroup
                                     : instance < 0x2000 ? objectGroup
                                                         : code.semiGlobalGroup(objectGroup);
        if (resolved != group) {
            return std::nullopt;
        }
        return objectGroup;
    }
};

ObjectCatalog buildObjects(ResourceIndex& index, const ts2::sim::IndexCodeSource& code) {
    ObjectCatalog catalog;
    for (const auto& r : index.resources()) {
        if (r.entry.key.type != ts2::types::kObjectDefinition ||
            catalog.definitions.contains(r.entry.key.group)) {
            continue;
        }
        try {
            catalog.definitions.emplace(r.entry.key.group, ts2::objd::parse(index.read(r)));
        } catch (const std::exception&) {
        }
    }
    for (const auto& [objectGroup, semiGroup] : code.semiGlobalGroups()) {
        if (catalog.definitions.contains(objectGroup)) {
            catalog.userOfSemiGlobal.try_emplace(semiGroup, objectGroup);
        }
    }
    catalog.standInGroup = catalog.definitions.begin()->first;
    for (const auto& [group, definition] : catalog.definitions) {
        if (definition.field(ts2::objd::field::kType) == ts2::sim::kPersonType &&
            catalog.personDefinitions++ == 0) {
            catalog.standInGroup = group;
        }
    }
    return catalog;
}

using WantDefinitionsPtr = std::shared_ptr<const ts2::content::WantDefinitions>;

// The install's want definitions, or null for a single package or an install without them.
WantDefinitionsPtr wantDefinitionsFor(const std::filesystem::path& source) {
    if (!std::filesystem::is_directory(source)) {
        return nullptr;
    }
    const auto package = ts2::content::wantsPackage(source);
    return package ? std::make_shared<ts2::content::WantDefinitions>(ts2::content::loadWantDefinitions(*package))
                   : nullptr;
}

using ScriptsPtr = std::shared_ptr<ts2::sim::LuaRuntime>;

// The game's Lua state with the install's object scripts loaded, or null for a single package.
ScriptsPtr scriptsFor(const std::filesystem::path& source, ResourceIndex& index) {
    if (!std::filesystem::is_directory(source)) {
        return nullptr;
    }
    const auto package = ts2::content::objectScriptsPackage(source);
    if (!package) {
        return nullptr;
    }
    const auto loaded = ts2::content::loadObjectScripts(*package);
    auto runtime = std::make_shared<ts2::sim::LuaRuntime>();
    runtime->installedPacks = ts2::content::installedPacksMask(ts2::content::findProducts(source));
    ts2::sim::IndexCodeSource code(index);
    runtime->load(loaded.modules, loaded.scripts, &code);
    std::printf("Lua: %zu modules, %zu scripts, %zu problems\n", loaded.modules.size(), loaded.scripts.size(),
                loaded.problems.size() + runtime->problems.size());
    for (const auto& p : runtime->problems) {
        std::printf("  %s\n", p.c_str());
    }
    return runtime;
}

int scan(ResourceIndex& index, std::size_t maxSteps, bool interactions,
         const std::shared_ptr<const ts2::sim::SavedNeighborhood>& hood, const WantDefinitionsPtr& wants,
         const ScriptsPtr& scripts) {
    ts2::sim::IndexCodeSource code(index);
    const auto primitives = editorTable(index, kPrimitiveNames);
    const auto owners = editorTable(index, kDataOwners);
    const auto genericCalls = editorTable(index, kGenericSimsCalls);
    const auto searchTypes = editorTable(index, kSearchTypes);
    const ObjectCatalog objects = buildObjects(index, code);
    const std::uint32_t standInGroup = objects.standInGroup;
    const std::size_t personDefinitions = objects.personDefinitions;
    std::size_t skipped = 0;
    std::size_t slept = 0;
    constexpr std::size_t kMaxTicks = 50;

    std::map<Outcome, std::size_t> outcomes;
    std::map<std::string, std::size_t> reasons;
    std::map<std::string, RunResult> examples;
    std::map<std::string, std::pair<std::uint32_t, std::uint16_t>> exampleSources;
    std::vector<std::pair<std::size_t, std::string>> completed;
    std::size_t total = 0;

    const auto start = std::chrono::steady_clock::now();
    for (const auto& r : index.resources()) {
        if (r.entry.key.type != ts2::types::kBehavior) {
            continue;
        }
        const auto group = r.entry.key.group;
        const auto instance = static_cast<std::uint16_t>(r.entry.key.instance);

        const auto chosen = objects.objectFor(group, instance, code);
        if (!chosen) {
            ++skipped;
            continue;
        }
        const std::uint32_t objectGroup = *chosen;
        ++total;

        ts2::sim::World world;
        if (hood) {
            world.setNeighborhood(hood);
        }
        if (wants) {
            world.setWantDefinitions(wants);
        }
        world.setScripts(scripts);
        ts2::sim::Vm vm(code, {maxSteps, 64}, &world);
        ts2::sim::SimObject* runner = nullptr;
        RunResult result;
        if (interactions && objectGroup != standInGroup) {
            ts2::sim::SimObject& sim = world.create(standInGroup, objects.definitions.at(standInGroup),
                                                               objects.containerSlots(standInGroup, code));
            ts2::sim::SimObject& object = world.create(objectGroup, objects.definitions.at(objectGroup),
                                                                  objects.containerSlots(objectGroup, code));
            sim.thread.stackObjectId = object.id;
            runner = &sim;
            result = vm.run(sim, objectGroup, instance);
        } else {
            ts2::sim::SimObject& me = world.create(objectGroup, objects.definitions.at(objectGroup),
                                                                  objects.containerSlots(objectGroup, code));
            me.thread.stackObjectId = me.id;
            runner = &me;
            result = vm.run(me, instance);
        }
        ts2::sim::SimObject& me = *runner;
        std::size_t ticks = 0;
        while (result.outcome == Outcome::Sleeping && ticks < kMaxTicks) {
            ++ticks;
            result = vm.resume(me);
        }
        if (ticks > 0) {
            ++slept;
        }

        ++outcomes[result.outcome];
        if (result.outcome == Outcome::True || result.outcome == Outcome::False) {
            if (result.steps > 3) {
                const auto bhav = code.behavior(group, instance);
                char line[160];
                std::snprintf(line, sizeof(line), "0x%08X 0x%04X  %-44s %5zu steps, %s", group,
                              instance, bhav ? bhav->name.c_str() : "?", result.steps,
                              ts2::sim::outcomeName(result.outcome));
                completed.emplace_back(result.steps, line);
            }
        } else {
            const std::string text = describe(result, primitives, owners, genericCalls, searchTypes);
            ++reasons[text];
            examples.try_emplace(text, result);
            exampleSources.try_emplace(text, group, instance);
        }
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);

    if (interactions) {
        std::printf("Ran %zu BHAVs in %.1f s as interactions: a Sim runs each object's code, stack object = the object\n",
                    total, elapsed.count());
    } else {
        std::printf("Ran %zu BHAVs in %.1f s, each as a fresh object in an empty world, stack object = itself\n",
                    total, elapsed.count());
    }
    std::printf("Global BHAVs ran as stand-in object \"%s\" (%s; %zu person definitions found)\n",
                objects.definitions.at(standInGroup).name.c_str(),
                personDefinitions > 0 ? "a person" : "not a person", personDefinitions);
    std::printf("%zu BHAVs skipped (no object can reach them)\n", skipped);
    std::printf("%zu BHAVs slept at least once and were resumed (up to %zu ticks)\n\n", slept, kMaxTicks);
    for (const auto& [outcome, n] : outcomes) {
        std::printf("  %-16s %7zu  (%.1f%%)\n", ts2::sim::outcomeName(outcome), n,
                    100.0 * static_cast<double>(n) / static_cast<double>(total));
    }

    std::vector<std::pair<std::size_t, std::string>> sorted;
    for (const auto& [text, n] : reasons) {
        sorted.emplace_back(n, text);
    }
    std::sort(sorted.rbegin(), sorted.rend());
    std::printf("\nWhere BHAVs stop (top 25):\n");
    for (std::size_t i = 0; i < std::min<std::size_t>(sorted.size(), 25); ++i) {
        std::printf("  %7zu  %s\n", sorted[i].first, sorted[i].second.c_str());
    }

    std::printf("\nScript functions not implemented (all):\n");
    for (const auto& [n, text] : sorted) {
        if (text.find("script function not implemented") != std::string::npos ||
            text.find("script error") != std::string::npos) {
            std::printf("  %7zu  %s\n", n, text.c_str());
        }
    }

    std::printf("\nErrors by reason:\n");
    for (const auto& [n, text] : sorted) {
        if (text.rfind("error", 0) == 0 || text.rfind("step limit", 0) == 0 || text.rfind("missing code", 0) == 0) {
            const RunResult& ex = examples.at(text);
            const auto& [scannedGroup, scannedInstance] = exampleSources.at(text);
            std::printf("  %7zu  %-40s e.g. 0x%08X 0x%04X instruction %u  %s  (running 0x%08X 0x%04X)\n",
                        n, text.c_str(), ex.group, ex.instance, ex.pc, ex.detail.c_str(),
                        scannedGroup, scannedInstance);
        }
    }

    std::sort(completed.rbegin(), completed.rend());
    std::printf("\nLongest complete runs (%zu BHAVs ran to completion in more than 3 steps):\n",
                completed.size());
    for (std::size_t i = 0; i < std::min<std::size_t>(completed.size(), 12); ++i) {
        std::printf("  %s\n", completed[i].second.c_str());
    }
    return 0;
}

// An interaction table's lines with their pie-menu texts.
int printInteractionTable(ResourceIndex& index, std::uint32_t group, std::uint16_t instance) {
    ts2::sim::IndexCodeSource code(index);
    const auto table = code.interactionTable(group, instance);
    if (!table) {
        std::fprintf(stderr, "no interaction table 0x%08X 0x%04X\n", group, instance);
        return 1;
    }
    std::printf("version 0x%X, %zu lines\n", table->version, table->interactions.size());
    for (std::size_t i = 0; i < table->interactions.size(); ++i) {
        const auto& line = table->interactions[i];
        const auto text = code.interactionString(group, instance, line.stringIndex);
        std::printf("%3zu  action 0x%04X test 0x%04X flags 0x%08X string %u \"%s\"\n", i, line.action, line.test, line.flags,
                    line.stringIndex, text ? text->c_str() : "-");
    }
    for (std::size_t s = 0;; ++s) {
        const auto text = code.interactionString(group, instance, s);
        if (!text) {
            break;
        }
        std::printf("  TTAs %zu \"%s\"\n", s, text->c_str());
    }
    return 0;
}

int runOne(ResourceIndex& index, std::uint32_t group, std::uint16_t instance,
           const std::vector<std::int16_t>& args, bool trace) {
    ts2::sim::IndexCodeSource code(index);
    const auto primitives = editorTable(index, kPrimitiveNames);
    const auto owners = editorTable(index, kDataOwners);

    const ObjectCatalog objects = buildObjects(index, code);
    const auto chosen = objects.objectFor(group, instance, code);
    if (!chosen) {
        std::fprintf(stderr, "no object can reach BHAV 0x%08X 0x%04X\n", group, instance);
        return 1;
    }
    ts2::sim::World world;
    ts2::sim::SimObject& me = world.create(*chosen, objects.definitions.at(*chosen), objects.containerSlots(*chosen, code));
    me.thread.stackObjectId = me.id;
    std::printf("Running as object %d \"%s\" (%s), stack object = itself\n\n", me.id,
                me.definition.name.c_str(), me.isPerson() ? "person" : "not a person");

    ts2::sim::Vm vm(code, {}, &world);
    if (trace) {
        vm.onStep = [&](const ts2::sim::StepInfo& s) {
            std::printf("%*s[%s] %3u  opcode 0x%04X\n", static_cast<int>(s.depth * 2), "", s.treeName.c_str(), s.pc,
                        s.instruction.opcode);
        };
    }

    RunResult r = vm.run(me, instance, args);
    for (int tick = 0; r.outcome == Outcome::Sleeping && tick < 50; ++tick) {
        r = vm.resume(me);
    }
    const ts2::sim::ThreadState& thread = me.thread;

    std::printf("\nResult : %s\n", describe(r, primitives, owners).c_str());
    if (!r.detail.empty()) {
        std::printf("Detail : %s\n", r.detail.c_str());
    }
    std::printf("Steps  : %zu\n", r.steps);
    if (r.outcome != Outcome::True && r.outcome != Outcome::False) {
        const auto bhav = code.behavior(r.group, r.instance);
        std::printf("Stopped: \"%s\" (0x%08X 0x%04X) instruction %u\n",
                    bhav ? bhav->name.c_str() : "?", r.group, r.instance, r.pc);
    }
    std::printf("Temps  :");
    for (std::size_t i = 0; i < thread.temps.size(); ++i) {
        if (thread.temps[i] != 0) {
            std::printf(" t%zu=%d", i, thread.temps[i]);
        }
    }
    std::printf("\n");
    return 0;
}

// Prints the lot's outside-for-weather grid: "width height levels", then a line of tile bytes (hex)
// per level and x.
int printOutside(const std::filesystem::path& lotPath) {
    const ts2::content::Lot lot = ts2::content::loadLot(lotPath);
    ts2::sim::RoomMap regions;
    if (const auto graph = lot.layerGraphs.find(ts2::content::kLayerGraphInstances[1]); graph != lot.layerGraphs.end()) {
        regions = ts2::sim::RoomMap(graph->second);
    }
    const ts2::sim::OutsideGrid grid(lot, regions);
    std::printf("%d %d %d\n", grid.width(), grid.height(), grid.levels());
    for (std::int32_t level = 0; level < grid.levels(); ++level) {
        for (std::int32_t x = 0; x < grid.width(); ++x) {
            for (std::int32_t y = 0; y < grid.height(); ++y) {
                std::printf("%02x ", grid.tile(level, x, y));
            }
            std::printf("\n");
        }
    }
    return 0;
}

// Prints every quarter with a platform region as "level x y quarter region", levels as scripts
// count them (0 = the ground storey) from one below the ground to the top.
int printPlatforms(const std::filesystem::path& lotPath) {
    const ts2::content::Lot lot = ts2::content::loadLot(lotPath);
    const auto regions = ts2::sim::platformRegions(lot);
    const auto grid = lot.grids.find(ts2::content::kPlatformRegionGridInstance);
    if (!regions || grid == lot.grids.end()) {
        std::printf("no platform region grid\n");
        return 1;
    }
    const int ground = ts2::content::groundGridLevel(lot);
    for (int level = -ground; level < static_cast<int>(grid->second.levels) - ground; ++level) {
        for (int x = 0; x < static_cast<int>(grid->second.width); ++x) {
            for (int y = 0; y < static_cast<int>(grid->second.height); ++y) {
                for (int quarter = 0; quarter < 4; ++quarter) {
                    const auto region = regions(level, x, y, quarter);
                    if (region && *region != -1) {
                        std::printf("%d %d %d %d %d\n", level, x, y, quarter, *region);
                    }
                }
            }
        }
    }
    return 0;
}

// Prints how many of the lot's saved wall segments have each wall layer type, "type count".
int printWallTypes(const std::filesystem::path& lotPath) {
    const ts2::content::Lot lot = ts2::content::loadLot(lotPath);
    std::map<std::uint32_t, std::size_t> counts;
    for (const auto& entry : lot.wallCoverings) {
        ++counts[entry.type];
    }
    for (const auto& [type, n] : counts) {
        std::printf("0x%X %zu\n", type, n);
    }
    std::printf("total %zu\n", lot.wallCoverings.size());
    return 0;
}

// Prints every saved wall segment in file order: "segment type", then for a segment of the room graph
// its two points (x y level) and the rooms on its left and right.
int printWallList(const std::filesystem::path& lotPath) {
    const ts2::content::Lot lot = ts2::content::loadLot(lotPath);
    std::map<std::int32_t, const ts2::wallgraph::Wall*> walls;
    std::optional<ts2::wallgraph::PointIndex> points;
    if (lot.roomGraph) {
        for (const auto& w : lot.roomGraph->walls) {
            walls.emplace(w.id, &w);
        }
        points.emplace(*lot.roomGraph);
    }
    for (const auto& entry : lot.wallCoverings) {
        std::printf("%u 0x%X", entry.wall, entry.type);
        if (const auto it = walls.find(static_cast<std::int32_t>(entry.wall)); it != walls.end()) {
            const auto* a = (*points)(it->second->from);
            const auto* b = (*points)(it->second->to);
            if (a != nullptr && b != nullptr) {
                std::printf(" %g %g %d %g %g %d rooms %d %d", a->x, a->y, a->level, b->x, b->y, b->level, it->second->leftRoom,
                            it->second->rightRoom);
            }
        }
        std::printf("\n");
    }
    return 0;
}

// Prints the room on each side of every full room wall as scripts number them, "index side0 side1".
int printWallSides(const std::filesystem::path& install, const std::filesystem::path& lotPath) {
    ts2::sim::LotSimulation sim(install, lotPath);
    const auto& sections = sim.world().wallSections();
    if (!sections) {
        std::printf("no walls\n");
        return 1;
    }
    for (std::size_t i = 0; i < sections->size(); ++i) {
        const auto& wall = (*sections)[i];
        if (!wall.placed) {
            std::printf("%zu not in the room graph\n", i);
            continue;
        }
        std::printf("%zu %d %d\n", i, ts2::sim::roomOnSideOfWall(sim.world().rooms(), wall.from, wall.to),
                    ts2::sim::roomOnSideOfWall(sim.world().rooms(), wall.to, wall.from));
    }
    return 0;
}

// Compares the object type table saved with a lot (its Init and Main signatures and two OBJD counts)
// with the installed object types, one line per type: saved and installed values, and "changed".
int typeChanges(ResourceIndex& index, const std::filesystem::path& lotPath) {
    ts2::sim::IndexCodeSource code(index);
    const auto lot = ts2::content::loadLot(lotPath);
    std::size_t changed = 0;
    for (const auto& e : lot.types.entries) {
        std::uint32_t saved[5];
        for (std::size_t i = 0; i < 5; ++i) {
            saved[i] = e.unknown[2 * i] | static_cast<std::uint32_t>(e.unknown[2 * i + 1]) << 16;
        }
        const auto* type = code.objectType(e.guid);
        if (type == nullptr) {
            std::printf("%08X  not installed  %s\n", e.guid, e.name.c_str());
            continue;
        }
        const auto* functions = code.objectFunctions(*type);
        auto signature = [&](std::size_t entry) -> std::uint32_t {
            if (functions == nullptr || entry >= functions->size()) {
                return 0;
            }
            return ts2::sim::treeSignature(code, type->group, (*functions)[entry].action);
        };
        const std::uint32_t init = signature(0), main = signature(1);
        const std::uint32_t attributes = type->definition.field(0x3A), arrays = type->definition.field(0x3B);
        const bool differs = ts2::sim::typeChangedSinceSave(code, *type, e);
        changed += differs;
        std::printf("%08X  saved %08X %08X %08X %08X %08X  installed %08X %08X  objd 3A %u 3B %u%s  %s\n", e.guid, saved[0],
                    saved[1], saved[2], saved[3], saved[4], init, main, attributes, arrays, differs ? "  changed" : "",
                    e.name.c_str());
    }
    std::printf("%zu types, %zu with changed code\n", lot.types.entries.size(), changed);
    return 0;
}

void printStep(const ts2::sim::StepInfo& s, const std::string& when) {
    std::printf("%s  %*s0x%08X 0x%04X %s at %u: opcode 0x%04X\n", when.c_str(), static_cast<int>(2 * s.depth), "", s.group,
                s.instance, s.treeName.c_str(), s.pc, s.instruction.opcode);
}

// Brings a saved lot to life and runs it for a number of ticks, then says why scripts stopped.
// Every object's pie menu for `actor`. The steps of `traced`'s check trees are printed too.
void printPieMenus(ts2::sim::LotSimulation& sim, ts2::sim::SimObject& actor, std::optional<std::int16_t> traced) {
    std::vector<std::int16_t> objects;
    sim.world().nextAfter(0, [&](const ts2::sim::SimObject& o) {
        objects.push_back(o.id);
        return false;
    });
    for (const auto id : objects) {
        auto* object = sim.world().find(id);
        if (object == nullptr) {
            continue;
        }
        if (traced == id) {
            sim.vm().onStep = [](const ts2::sim::StepInfo& s) { printStep(s, "check"); };
        }
        const auto menu = ts2::sim::pieMenu(sim.vm(), sim.code(), actor, *object);
        sim.vm().onStep = nullptr;
        if (menu.choices.empty() && menu.problems.empty()) {
            continue;
        }
        std::printf("pie menu of %d (%s, group 0x%08X, table 0x%04X) for Sim %d:\n", id, object->definition.name.c_str(), object->group,
                    object->definition.field(ts2::objd::field::kInteractionTableId), actor.id);
        for (const auto& c : menu.choices) {
            std::printf("  %s  [id %u, order %d, params %d %d %d %d%s]\n", c.name.c_str(), c.interaction, c.order, c.params[0], c.params[1],
                        c.params[2], c.params[3], c.disabled != ts2::sim::OfferedAction::Disabled::No ? ", disabled" : "");
        }
        for (const auto& p : menu.problems) {
            std::printf("  problem: %s\n", p.c_str());
        }
    }
}

int runLot(ResourceIndex& index, const std::filesystem::path& install, const std::filesystem::path& lotPath,
           std::size_t ticks, std::optional<std::size_t> traceTick, std::optional<std::int16_t> traceObject, bool onScreen,
           bool personData, bool overlays, bool motion, std::optional<std::int16_t> pieSim, std::optional<std::int16_t> pieTrace,
           std::optional<std::array<int, 3>> queueChoice, std::size_t queueTick, std::optional<std::size_t> queueAgainTick, std::optional<std::size_t> cancelTick,
           std::optional<std::array<float, 3>> groundAt, bool portals, std::optional<std::array<float, 4>> groundLater,
           const std::vector<std::array<float, 5>>& goHere) {
    const auto primitives = editorTable(index, kPrimitiveNames);
    const auto owners = editorTable(index, kDataOwners);
    const auto genericCalls = editorTable(index, kGenericSimsCalls);
    const auto searchTypes = editorTable(index, kSearchTypes);

    std::function<void(const ts2::sim::StepInfo&)> traceLoad;
    if (traceObject) {
        traceLoad = [&](const ts2::sim::StepInfo& s) {
            if (s.object == *traceObject) {
                printStep(s, "load");
            }
        };
    }
    ts2::sim::LotSimulation sim(install, lotPath, traceLoad);
    auto& world = sim.world();
    ts2::scene::Assets assets(install);
    ts2::scene::AnimationLibrary animations(assets);
    // Routing as the app has it: the lot's route grid from the objects' models.
    sim.setRouteSources(app::routeSources(assets, app::readRouteModels(assets, lotPath)));
    world.setAnimationLengths([&](const std::string& name) { return animations.length(name); });
    world.setAnimationCarries([&](const std::string& name) { return animations.hasTargetType(name, 2); });
    world.setLocoClips([&](const std::string& name) -> std::optional<ts2::sim::LocoClip> {
        const auto a = animations.find(name);
        return a ? std::optional<ts2::sim::LocoClip>({name, a->duration, a->travel, a->loopPhase, a->match, a->speed, a->rootTurn}) : std::nullopt;
    });
    world.setAnimationEventTime([&](const std::string& name, int id) { return animations.eventTime(name, id); });
    world.setAnimationEvents([&](const std::string& name) {
        std::vector<ts2::sim::AnimationEvent> events;
        for (const auto& [ms, id] : animations.simAnticsEvents(name)) {
            events.push_back({ms, id});
        }
        return events;
    });
    world.setAnimationTurns([&](const std::string& name, float ms) { return animations.rootTurn(name, ms); });
    ts2::scene::OutfitProperties outfits(assets);
    world.setOutfitProperty([&](std::int16_t neighbor, std::uint16_t category, std::uint16_t property) -> std::optional<std::int16_t> {
        if (const auto file = sim.characterFile(neighbor)) {
            return outfits.get(*file, category, property);
        }
        const auto group = sim.characterGroup(neighbor);
        return group ? std::optional<std::int16_t>(outfits.get(*group, category, property)) : std::nullopt;
    });
    world.setHasOutfit([&](const ts2::sim::SimObject& object, std::uint32_t category) -> std::optional<bool> {
        const std::int16_t neighbor = world.neighborIdOf(object);
        if (const auto file = neighbor != 0 ? sim.characterFile(neighbor) : std::nullopt) {
            return outfits.hasOutfit(*file, category);
        }
        return outfits.hasOutfit(object.group, category);
    });
    const auto& report = sim.report();
    std::printf("%zu objects built (%zu with a saved thread, %zu reset: type changed since the save), %zu neighbors, "
                "%zu stale data table records\n",
                report.objects, report.threads, sim.resets(), world.neighbors() ? world.neighbors()->size() : 0,
                report.staleDataTables);
    if (const auto* ids = world.neighbors()) {
        // Neighbors whose Sim has no object type (no GUID, or no OBJD for it): scripts can't read their definition.
        std::size_t untyped = 0;
        for (const std::int16_t id : *ids) {
            const auto guid = world.neighborGuid(id);
            if (!guid || sim.code().objectType(*guid) == nullptr) {
                if (++untyped <= 20) {
                    std::printf("  neighbor %d has no Sim type (GUID 0x%08X)\n", id, guid.value_or(0));
                }
            }
        }
        if (untyped > 20) {
            std::printf("  ... %zu neighbors without a Sim type in all\n", untyped);
        }
    }
    std::map<std::string, std::size_t> problems;
    for (const auto& p : report.problems) {
        ++problems[p.message];
    }
    for (const auto& [message, n] : problems) {
        std::printf("  build problem: %s: %zu\n", message.c_str(), n);
    }

    if (portals) {
        sim.routeWorld();  // the models' joints are known once routing is set up
        for (const auto& p : ts2::sim::stairPortals(world, sim.code())) {
            std::printf("stair %d: %.3f %.3f level %d (part %d slot %d) -> %.3f %.3f level %d (part %d slot %d)%s\n", p.owner, p.from.x, p.from.y,
                        p.from.level, p.from.part, p.from.slot, p.to.x, p.to.y, p.to.level, p.to.part, p.to.slot, p.twoWay ? ", both ways" : "");
        }
    }

    // A ground click's destination object (object -1 for --pie-trace and --queue).
    std::int16_t ground = 0;
    if (groundAt) {
        const auto* actor = world.find(queueChoice ? static_cast<std::int16_t>((*queueChoice)[0]) : pieSim.value_or(0));
        if (auto* made = ts2::sim::groundDestination(sim.vm(), world, actor, {(*groundAt)[0], (*groundAt)[1], static_cast<int>((*groundAt)[2])})) {
            ground = made->id;
            std::printf("ground destination %d (%s) at %.3f %.3f level %d\n", made->id, made->definition.name.c_str(), made->x / 65536.0,
                        made->y / 65536.0, made->level);
        } else {
            std::printf("no ground destination there\n");
        }
        if (queueChoice && (*queueChoice)[1] == -1) {
            (*queueChoice)[1] = ground;
        }
    }

    // A pie menu choice queued before the ticks, or at a later tick: Sim, object, the choice's place in the menu.
    const auto queuePieChoice = [&] {
        auto* actor = world.find(static_cast<std::int16_t>((*queueChoice)[0]));
        auto* object = world.find(static_cast<std::int16_t>((*queueChoice)[1]));
        const auto menu = actor && object ? ts2::sim::pieMenu(sim.vm(), sim.code(), *actor, *object) : ts2::sim::PieMenu{};
        const auto at = static_cast<std::size_t>((*queueChoice)[2]);
        if (at < menu.choices.size() && ts2::sim::runsAtOnce(sim.code(), *object, menu.choices[at])) {
            const auto r = ts2::sim::runAtOnce(sim.vm(), sim.code(), world, *actor, *object, menu.choices[at]);
            std::printf("light strength of %d now %.2f\n", object->id, object->lightStrength);
            std::printf("ran %s at once for Sim %d: %s %s %s\n", menu.choices[at].name.c_str(), actor->id, ts2::sim::outcomeName(r.outcome), r.reason.c_str(),
                        r.detail.c_str());
            return;
        }
        const auto queued = at < menu.choices.size() ? ts2::sim::queuedChoice(sim.code(), world, *object, menu.choices[at]) : std::nullopt;
        if (queued) {
            ts2::sim::queueInteraction(*actor, *queued);
            std::printf("queued %s for Sim %d\n", menu.choices[at].name.c_str(), actor->id);
        } else {
            std::printf("nothing to queue\n");
        }
    };
    if (queueChoice && queueTick == 0) {
        queuePieChoice();
    }

    // The queued Sim's running interaction as it changes, and the player's cancel of it at a tick.
    std::uint32_t lastRunning = 0;
    for (std::size_t tick = 0; tick < ticks; ++tick) {
        if (auto* actor = queueChoice ? world.find(static_cast<std::int16_t>((*queueChoice)[0])) : nullptr) {
            const std::uint32_t running = actor->running ? actor->running->id : 0;
            if (running != lastRunning) {
                std::printf("tick %zu: Sim %d runs interaction %u\n", tick, actor->id, running);
                lastRunning = running;
            }
            // A second Go Here on the ground, chosen at a tick.
            if (groundLater && tick == static_cast<std::size_t>((*groundLater)[0])) {
                auto* made = ts2::sim::groundDestination(sim.vm(), world, actor, {(*groundLater)[1], (*groundLater)[2], static_cast<int>((*groundLater)[3])});
                const auto menu = made ? ts2::sim::pieMenu(sim.vm(), sim.code(), *actor, *made) : ts2::sim::PieMenu{};
                const auto queued = !menu.choices.empty() ? ts2::sim::queuedChoice(sim.code(), world, *made, menu.choices.front()) : std::nullopt;
                if (queued) {
                    ts2::sim::queueInteraction(*actor, *queued);
                }
                std::printf("tick %zu: %s\n", tick, queued ? "queued Go Here again" : "no second Go Here");
            }
            if ((queueTick != 0 && tick == queueTick) || (queueAgainTick && tick == *queueAgainTick)) {
                queuePieChoice();
            }
            if (cancelTick && tick == *cancelTick) {
                const bool done = actor->running && ts2::sim::cancelInteraction(*actor, actor->running->id);
                std::printf("tick %zu: %s\n", tick, done ? "cancelled it" : "nothing to cancel");
            }
        }
        // Go Here orders for any Sim (Sim, tick, x, y, level), as a click on the ground would queue them.
        for (const auto& order : goHere) {
            if (tick != static_cast<std::size_t>(order[1])) {
                continue;
            }
            auto* actor = world.find(static_cast<std::int16_t>(order[0]));
            auto* made = actor ? ts2::sim::groundDestination(sim.vm(), world, actor, {order[2], order[3], static_cast<int>(order[4])}) : nullptr;
            const auto menu = made ? ts2::sim::pieMenu(sim.vm(), sim.code(), *actor, *made) : ts2::sim::PieMenu{};
            const auto queued = !menu.choices.empty() ? ts2::sim::queuedChoice(sim.code(), world, *made, menu.choices.front()) : std::nullopt;
            if (queued) {
                ts2::sim::queueInteraction(*actor, *queued);
            }
            std::printf("tick %zu: %s for Sim %d\n", tick, queued ? "queued Go Here" : "no Go Here there", static_cast<int>(order[0]));
        }
        if ((traceTick && tick == *traceTick) || traceObject) {
            sim.vm().onStep = [&, tick](const ts2::sim::StepInfo& s) {
                if (!traceObject || s.object == *traceObject) {
                    printStep(s, traceObject ? std::to_string(tick) : "");
                }
            };
        } else {
            sim.vm().onStep = nullptr;
        }
        if (onScreen) {
            std::vector<std::int16_t> sims;
            world.nextAfter(0, [&](const ts2::sim::SimObject& o) {
                if (o.isPerson()) {
                    sims.push_back(o.id);
                }
                return false;
            });
            sim.markOnScreen(sims);
        }
        const std::size_t resetsBefore = sim.resets();
        sim.tick();
        if (sim.resets() != resetsBefore && motion) {
            std::printf("motion tick %zu: %zu Sim resets\n", tick, sim.resets() - resetsBefore);
        }
        if (motion) {
            // A line for each Sim whose place, facing, own animation or walk changed this tick.
            world.nextAfter(0, [&](const ts2::sim::SimObject& o) {
                if (!o.isPerson()) {
                    return false;
                }
                const char* phase = !o.walking ? "-"
                                    : o.walking->phase == ts2::sim::Walking::Phase::Start ? "start"
                                    : o.walking->phase == ts2::sim::Walking::Phase::Stop  ? "stop"
                                                                                          : "walk";
                const float heading = o.heading ? *o.heading : ts2::sim::compassHeading(o.data[1]);
                char line[256];
                // Then object data 1 (direction) and data 2-3 (the object holding me and its slot).
                std::snprintf(line, sizeof line, "%.3f %.3f %.0f %s %s %s dir %d in %d/%d", o.x / 65536.0, o.y / 65536.0, heading * 57.29578f,
                              o.animation ? o.animation->name.c_str() : "-", phase, o.turning ? "turning" : "", o.data[1], o.data[2], o.data[3]);
                static std::map<std::int16_t, std::string> last;
                if (last[o.id] != line) {
                    last[o.id] = line;
                    std::printf("motion tick %zu Sim %d %s\n", tick, o.id, line);
                }
                return false;
            });
        }
        if (overlays) {
            world.nextAfter(0, [&](const ts2::sim::SimObject& o) {
                if (!o.overlays.empty() && o.overlays.back().startMs == world.clockMs()) {
                    const auto& last = o.overlays.back();
                    std::printf("overlay tick %zu %s %d %s%s type %d stay %d, event tree 0x%04X of 0x%08X, stack object %d, "
                                "priority %u, plays %d\n",
                                tick, o.isPerson() ? "Sim" : "object", o.id, last.name.c_str(), last.flipped ? " flipped" : "", o.semiAttributes[0], o.semiAttributes[3], last.eventTree,
                                last.codeOwner, last.stackObject, last.priority, last.plays);
                }
                if (o.animation && o.animation->startMs == world.clockMs()) {
                    std::printf("animate tick %zu %s %d %s speed %.2f length %.0f ms%s ik %d\n", tick, o.isPerson() ? "Sim" : "object", o.id,
                                o.animation->name.c_str(), o.animation->speed, o.animation->lengthMs, o.animation->flipped ? " flipped" : "",
                                o.animation->ikTarget);
                }
                return false;
            });
        }
    }

    if (auto* actor = pieSim ? world.find(*pieSim) : nullptr) {
        printPieMenus(sim, *actor, pieTrace && *pieTrace == -1 ? std::optional<std::int16_t>(ground) : pieTrace);
    }

    std::size_t running = 0;
    world.nextAfter(0, [&](const ts2::sim::SimObject& o) {
        running += o.thread.isRunning();
        return false;
    });
    std::printf("after %zu ticks: %zu threads still running\n", ticks, running);
    // Where each Sim's script is.
    world.nextAfter(0, [&](const ts2::sim::SimObject& o) {
        if (o.isPerson()) {
            std::printf("  Sim %d (%s, group 0x%08X) at %.2f %.2f level %d: ", o.id, o.definition.name.c_str(), o.group,
                        o.x / 65536.0, o.y / 65536.0, o.level);
            if (o.thread.frames.empty()) {
                std::printf("no script running");
            } else {
                const auto& f = o.thread.frames.back();
                std::printf("in %s (0x%08X 0x%04X) at %u, %zu frames deep, %s", f.code ? f.code->name.c_str() : "?", f.group,
                            f.instance, f.pc, o.thread.frames.size(), o.thread.sleepTicks > 0 ? "sleeping" : "running");
                std::printf(" (args");
                for (const auto arg : f.params) {
                    std::printf(" %d", arg);
                }
                std::printf(")");
            }
            std::printf("%s%s, %zu queued", o.animation ? ", animating " : "", o.animation ? o.animation->name.c_str() : "",
                        o.queue.size());
            for (const auto& q : o.queue) {
                const auto* target = world.find(q.object);
                std::printf(", queued: object %d (%s) interaction %u action 0x%04X", q.object,
                            target ? target->definition.name.c_str() : "gone", q.index, q.action);
            }
            for (const auto& overlay : o.overlays) {
                std::printf(", overlay %s", overlay.name.c_str());
            }
            if (personData) {
                std::printf(", person data:");
                for (const auto value : o.personData) {
                    std::printf(" %d", value);
                }
            }
            std::printf("\n");
        }
        return false;
    });
    std::map<std::string, std::pair<std::size_t, std::string>> stops;  // described reason -> count, example
    for (const auto& [key, stop] : sim.stops()) {
        const RunResult& r = stop.first;
        const std::string why = describe(r, primitives, owners, genericCalls, searchTypes);
        const auto* object = world.find(stop.object);
        const auto bhav = sim.code().behavior(r.group, r.instance);
        char where[200];
        std::snprintf(where, sizeof(where), "tick %zu, object %d (%s), in %s (0x%08X 0x%04X) at %u", stop.tick, stop.object,
                      object ? object->definition.name.c_str() : "removed", bhav ? bhav->name.c_str() : "?", r.group,
                      r.instance, r.pc);
        if (object && object->isPerson()) {
            std::printf("  Sim stop: %s%s%s\n         at %s\n", why.c_str(), r.detail.empty() ? "" : " - ", r.detail.c_str(), where);
        }
        auto& entry = stops[why];
        if (entry.first == 0) {
            entry.second = where;
        }
        entry.first += stop.count;
    }
    std::vector<std::pair<std::size_t, std::string>> sorted;
    for (const auto& [why, entry] : stops) {
        sorted.emplace_back(entry.first, why);
    }
    std::sort(sorted.rbegin(), sorted.rend());
    for (const auto& [n, why] : sorted) {
        std::printf("  %5zu  %s\n         e.g. %s\n", n, why.c_str(), stops[why].second.c_str());
    }
    return 0;
}

int lookupGuids(ResourceIndex& index, int count, char** guids) {
    ts2::sim::IndexCodeSource code(index);
    for (int i = 0; i < count; ++i) {
        const auto guid = static_cast<std::uint32_t>(std::stoul(guids[i], nullptr, 16));
        if (const auto* type = code.objectType(guid)) {
            std::printf("0x%08X  group 0x%08X  %s\n", guid, type->group, type->definition.name.c_str());
        } else {
            std::printf("0x%08X  not found\n", guid);
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "Usage:\n"
                     "  simrun <install-folder | file.package> --scan [max-steps] [--interactions] [--hood <folder>]\n"
                     "  simrun <install-folder | file.package> --run <group> <instance> [args...] [--trace]\n"
                     "  simrun <install-folder | file.package> --guid <hex-guid> [...]\n"
                     "  simrun <install-folder> --lot <lot.package> [ticks [--trace-tick <tick>] [--trace-object <id>] [--on-screen] [--pie <sim> [--pie-trace <object>]] [--queue <sim> <object> <choice> [--queue-at <tick>] [--queue-again <tick>] [--cancel <tick>]] [--ground <x> <y> <level> [--ground-later <tick> <x> <y> <level>]] [--go-here <sim> <tick> <x> <y> <level>]... [--portals]]\n"
                     "  simrun <install-folder> --loco <clip>...\n"
                     "  simrun <install-folder> --wall-types <lot.package>\n"
                     "  simrun <install-folder> --wall-list <lot.package>\n"
                     "  simrun <install-folder> --wall-sides <lot.package>\n"
                     "  simrun <install-folder | file.package> --strings <group> <instance>\n"
                     "  simrun <install-folder | file.package> --ttab <group> <instance>\n");
        return 1;
    }

    try {
        ResourceIndex index;
        loadSource(index, argv[1]);

        const std::string_view mode = argv[2];
        if (mode == "--scan") {
            bool interactions = false;
            std::size_t maxSteps = 10000;
            std::shared_ptr<const ts2::sim::SavedNeighborhood> hood;
            for (int i = 3; i < argc; ++i) {
                if (std::string_view(argv[i]) == "--interactions") {
                    interactions = true;
                } else if (std::string_view(argv[i]) == "--hood" && i + 1 < argc) {
                    const std::filesystem::path folder(argv[++i]);
                    ts2::content::addCharacters(index, folder);  // the neighbors' Sims
                    ts2::sim::IndexCodeSource code(index);
                    hood = ts2::sim::savedNeighborhood(ts2::content::loadNeighborhood(ts2::content::neighborhoodPackage(folder)), code);
                } else {
                    maxSteps = std::stoul(argv[i]);
                }
            }
            return scan(index, maxSteps, interactions, hood, wantDefinitionsFor(argv[1]), scriptsFor(argv[1], index));
        }
        if (mode == "--lot" && argc >= 4) {
            const std::size_t ticks = argc >= 5 ? std::stoul(argv[4]) : 100;
            std::optional<std::size_t> traceTick;
            std::optional<std::int16_t> traceObject;
            bool onScreen = false;
            bool personData = false;
            bool overlays = false;
            bool motion = false;
            std::optional<std::int16_t> pieSim;
            std::optional<std::int16_t> pieTrace;
            std::optional<std::array<int, 3>> queueChoice;
            std::size_t queueTick = 0;
            std::optional<std::size_t> cancelTick;
            std::optional<std::size_t> queueAgainTick;
            std::optional<std::array<float, 3>> groundAt;
            bool portals = false;
            std::optional<std::array<float, 4>> groundLater;
            std::vector<std::array<float, 5>> goHere;
            for (int i = 5; i < argc; ++i) {
                if (std::string_view(argv[i]) == "--trace-tick" && i + 1 < argc) {
                    traceTick = std::stoul(argv[++i]);
                } else if (std::string_view(argv[i]) == "--trace-object" && i + 1 < argc) {
                    traceObject = static_cast<std::int16_t>(std::stoi(argv[++i]));
                } else if (std::string_view(argv[i]) == "--on-screen") {
                    onScreen = true;
                } else if (std::string_view(argv[i]) == "--person-data") {
                    personData = true;
                } else if (std::string_view(argv[i]) == "--overlays") {
                    overlays = true;
                } else if (std::string_view(argv[i]) == "--motion") {
                    motion = true;
                } else if (std::string_view(argv[i]) == "--pie" && i + 1 < argc) {
                    pieSim = static_cast<std::int16_t>(std::stoi(argv[++i]));
                } else if (std::string_view(argv[i]) == "--pie-trace" && i + 1 < argc) {
                    pieTrace = static_cast<std::int16_t>(std::stoi(argv[++i]));
                } else if (std::string_view(argv[i]) == "--queue" && i + 3 < argc) {
                    queueChoice = std::array<int, 3>{std::stoi(argv[i + 1]), std::stoi(argv[i + 2]), std::stoi(argv[i + 3])};
                    i += 3;
                } else if (std::string_view(argv[i]) == "--queue-at" && i + 1 < argc) {
                    queueTick = std::stoul(argv[++i]);
                } else if (std::string_view(argv[i]) == "--queue-again" && i + 1 < argc) {
                    queueAgainTick = std::stoul(argv[++i]);
                } else if (std::string_view(argv[i]) == "--cancel" && i + 1 < argc) {
                    cancelTick = std::stoul(argv[++i]);
                } else if (std::string_view(argv[i]) == "--ground-later" && i + 4 < argc) {
                    groundLater = std::array<float, 4>{std::stof(argv[i + 1]), std::stof(argv[i + 2]), std::stof(argv[i + 3]), std::stof(argv[i + 4])};
                    i += 4;
                } else if (std::string_view(argv[i]) == "--go-here" && i + 5 < argc && goHere.size() < 64) {
                    goHere.push_back({std::stof(argv[i + 1]), std::stof(argv[i + 2]), std::stof(argv[i + 3]), std::stof(argv[i + 4]), std::stof(argv[i + 5])});
                    i += 5;
                } else if (std::string_view(argv[i]) == "--portals") {
                    portals = true;
                } else if (std::string_view(argv[i]) == "--ground" && i + 3 < argc) {
                    groundAt = std::array<float, 3>{std::stof(argv[i + 1]), std::stof(argv[i + 2]), std::stof(argv[i + 3])};
                    i += 3;
                }
            }
            return runLot(index, std::filesystem::path(argv[1]), std::filesystem::path(argv[3]), ticks, traceTick, traceObject, onScreen,
                          personData, overlays, motion, pieSim, pieTrace, queueChoice, queueTick, queueAgainTick, cancelTick, groundAt, portals, groundLater, goHere);
        }
        if (mode == "--outside" && argc >= 4) {
            return printOutside(std::filesystem::path(argv[3]));
        }
        if (mode == "--platforms" && argc >= 4) {
            return printPlatforms(std::filesystem::path(argv[3]));
        }
        if (mode == "--wall-types" && argc >= 4) {
            return printWallTypes(std::filesystem::path(argv[3]));
        }
        if (mode == "--wall-list" && argc >= 4) {
            return printWallList(std::filesystem::path(argv[3]));
        }
        if (mode == "--wall-sides" && argc >= 4) {
            return printWallSides(argv[1], std::filesystem::path(argv[3]));
        }
        if (mode == "--type-changes" && argc >= 4) {
            return typeChanges(index, std::filesystem::path(argv[3]));
        }
        if (mode == "--strings" && argc >= 5) {
            const auto labels = editorTable(index, static_cast<std::uint32_t>(std::stoul(argv[4], nullptr, 0)),
                                            static_cast<std::uint32_t>(std::stoul(argv[3], nullptr, 0)));
            for (std::size_t i = 0; i < labels.size(); ++i) {
                std::printf("0x%02zX  %s\n", i, labels[i].c_str());
            }
            return 0;
        }
        if (mode == "--ttab" && argc >= 5) {
            return printInteractionTable(index, static_cast<std::uint32_t>(std::stoul(argv[3], nullptr, 0)),
                                         static_cast<std::uint16_t>(std::stoul(argv[4], nullptr, 0)));
        }
        if (mode == "--guid") {
            return lookupGuids(index, argc - 3, argv + 3);
        }
        // Locomotion clip headers, as the walk planner reads them.
        if (mode == "--loco") {
            ts2::scene::Assets assets(argv[1]);
            ts2::scene::AnimationLibrary animations(assets);
            for (int i = 3; i < argc; ++i) {
                if (const auto a = animations.find(argv[i])) {
                    std::printf("%s: %.1f ms, travel %.6f, foot %.3f, match %.6f, speed %.9f, turn %.4f\n", argv[i], a->duration, a->travel,
                                a->loopPhase, a->match, a->speed, a->rootTurn);
                } else {
                    std::printf("%s: not found\n", argv[i]);
                }
            }
            return 0;
        }
        if (mode == "--run" && argc >= 5) {
            std::vector<std::int16_t> args;
            bool trace = false;
            for (int i = 5; i < argc; ++i) {
                if (std::string_view(argv[i]) == "--trace") {
                    trace = true;
                } else {
                    args.push_back(static_cast<std::int16_t>(std::stoi(argv[i], nullptr, 0)));
                }
            }
            return runOne(index, static_cast<std::uint32_t>(std::stoul(argv[3], nullptr, 0)),
                          static_cast<std::uint16_t>(std::stoul(argv[4], nullptr, 0)), args, trace);
        }
        std::fprintf(stderr, "unknown or incomplete command\n");
        return 1;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
