#include "content/install.hpp"
#include "content/lot.hpp"
#include "content/neighborhood.hpp"
#include "content/scripts.hpp"
#include "content/wants.hpp"
#include "sim/lua_runtime.hpp"
#include "content/resource_index.hpp"
#include "formats/objd.hpp"
#include "formats/resource_types.hpp"
#include "formats/str.hpp"
#include "sim/index_code_source.hpp"
#include "sim/lot_simulation.hpp"
#include "sim/lot_world.hpp"
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

// The install's want definitions; null for a single package or an install without them.
WantDefinitionsPtr wantDefinitionsFor(const std::filesystem::path& source) {
    if (!std::filesystem::is_directory(source)) {
        return nullptr;
    }
    const auto package = ts2::content::wantsPackage(source);
    return package ? std::make_shared<ts2::content::WantDefinitions>(ts2::content::loadWantDefinitions(*package))
                   : nullptr;
}

using ScriptsPtr = std::shared_ptr<ts2::sim::LuaRuntime>;

// The game's Lua state with the install's object scripts loaded; null for a single package.
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
            const auto bhav = code.behavior(s.group, s.instance);
            std::printf("%*s[%s] %3u  opcode 0x%04X\n", static_cast<int>(s.depth * 2), "",
                        bhav ? bhav->name.c_str() : "?", s.pc, s.instruction.opcode);
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

// Brings a saved lot to life and runs it for a number of ticks, then says why scripts stopped.
int runLot(ResourceIndex& index, const std::filesystem::path& install, const std::filesystem::path& lotPath,
           std::size_t ticks) {
    const auto primitives = editorTable(index, kPrimitiveNames);
    const auto owners = editorTable(index, kDataOwners);
    const auto genericCalls = editorTable(index, kGenericSimsCalls);
    const auto searchTypes = editorTable(index, kSearchTypes);

    ts2::sim::LotSimulation sim(install, lotPath);
    auto& world = sim.world();
    const auto& report = sim.report();
    std::printf("%zu objects built (%zu with a saved thread, %zu reset: type changed since the save), %zu neighbors, "
                "%zu stale data table records\n",
                report.objects, report.threads, sim.resets(), world.neighbors() ? world.neighbors()->size() : 0,
                report.staleDataTables);
    std::map<std::string, std::size_t> problems;
    for (const auto& p : report.problems) {
        ++problems[p.message];
    }
    for (const auto& [message, n] : problems) {
        std::printf("  build problem: %s: %zu\n", message.c_str(), n);
    }

    for (std::size_t tick = 0; tick < ticks; ++tick) {
        sim.tick();
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
            std::printf("  Sim %d (%s): ", o.id, o.definition.name.c_str());
            if (o.thread.frames.empty()) {
                std::printf("no script running");
            } else {
                const auto& f = o.thread.frames.back();
                std::printf("in %s (0x%08X 0x%04X) at %u, %zu frames deep, %s", f.code ? f.code->name.c_str() : "?", f.group,
                            f.instance, f.pc, o.thread.frames.size(), o.thread.sleepTicks > 0 ? "sleeping" : "running");
            }
            std::printf("%s%s, %zu queued\n", o.animation ? ", animating " : "", o.animation ? o.animation->name.c_str() : "",
                        o.queue.size());
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
                     "  simrun <install-folder> --lot <lot.package> [ticks]\n"
                     "  simrun <install-folder | file.package> --strings <group> <instance>\n");
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
                    hood = ts2::sim::savedNeighborhood(ts2::content::loadNeighborhood(
                        ts2::content::neighborhoodPackage(std::filesystem::path(argv[++i]))));
                } else {
                    maxSteps = std::stoul(argv[i]);
                }
            }
            return scan(index, maxSteps, interactions, hood, wantDefinitionsFor(argv[1]), scriptsFor(argv[1], index));
        }
        if (mode == "--lot" && argc >= 4) {
            const std::size_t ticks = argc >= 5 ? std::stoul(argv[4]) : 100;
            return runLot(index, std::filesystem::path(argv[1]), std::filesystem::path(argv[3]), ticks);
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
        if (mode == "--guid") {
            return lookupGuids(index, argc - 3, argv + 3);
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
