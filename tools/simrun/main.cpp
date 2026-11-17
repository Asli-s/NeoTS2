#include "../../app/route_sources.hpp"
#include "save_fields.hpp"
#include "content/install.hpp"
#include "content/lot.hpp"
#include "content/neighborhood.hpp"
#include "content/scripts.hpp"
#include "content/ui_data.hpp"
#include "content/wants.hpp"
#include "scene/animation_library.hpp"
#include "scene/outfit_properties.hpp"
#include "scene/assets.hpp"
#include "sim/lua_runtime.hpp"
#include "sim/ground_click.hpp"
#include "sim/route_portals.hpp"
#include "content/resource_index.hpp"
#include "formats/cobject_record.hpp"
#include "formats/objd.hpp"
#include "formats/resource_types.hpp"
#include "formats/objm.hpp"
#include "formats/objt.hpp"
#include "formats/lot_description.hpp"
#include "formats/sdsc.hpp"
#include "formats/fami.hpp"
#include "formats/famt.hpp"
#include "formats/lot_records.hpp"
#include "formats/propset.hpp"
#include "formats/qfs.hpp"
#include "sim/genetics.hpp"
#include "formats/simi.hpp"
#include "formats/str.hpp"
#include "sim/index_code_source.hpp"
#include "formats/dbpf_writer.hpp"
#include "formats/xobj.hpp"
#include "sim/character_file.hpp"
#include "sim/character_look.hpp"
#include "sim/character_models.hpp"
#include "sim/newborn_file.hpp"
#include "sim/newborn_save.hpp"
#include "scene/sims.hpp"
#include "scene/newborn_face.hpp"
#include "scene/newborn_face_shape.hpp"
#include "formats/face_shape.hpp"
#include "formats/rcol.hpp"
#include "formats/scenegraph.hpp"
#include "formats/hash.hpp"
#include "formats/reference_list.hpp"
#include "sim/hood_save.hpp"
#include "sim/lot_save.hpp"
#include "sim/lot_simulation.hpp"
#include "sim/lot_world.hpp"
#include "sim/pie_menu.hpp"
#include "sim/type_signature.hpp"
#include "sim/vm.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <tuple>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

// The checks below read game and save files that may be broken, so these never read past a list.

// A material record's properties, none when its first block isn't a material.
const std::vector<std::pair<std::string, std::string>>& materialProperties(const ts2::rcol::Resource& record) {
    static const std::vector<std::pair<std::string, std::string>> none;
    const auto* material = record.blocks.empty() ? nullptr : std::get_if<ts2::rcol::Material>(&record.blocks[0]);
    return material != nullptr ? material->properties : none;
}

// The link of a mesh's first group, when the mesh has one that points at a link it holds.
const ts2::scenegraph::ElementLink* firstGroupLink(const ts2::scenegraph::Mesh& mesh) {
    return !mesh.groups.empty() && mesh.groups[0].link < mesh.links.size() ? &mesh.links[mesh.groups[0].link] : nullptr;
}

// Writes a file whole. False, with a message, when it can't be written.
bool writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
        std::fprintf(stderr, "can't write %s\n", path.string().c_str());
        return false;
    }
    return true;
}

// A "bone_adjust" data list: the bone's name, its move (3 floats) and its turn (4 floats).
bool isGripList(const ts2::rcol::DataList* list) {
    return list != nullptr && list->value.name == "bone_adjust" && list->value.items.size() == 3 && list->value.items[1].floats.size() >= 3 &&
           list->value.items[2].floats.size() >= 4;
}

// --list-objects: list every object but the Sims when a lot run ends.
bool listObjects = false;
bool savedQueues = false;
bool savedAnimations = false;
bool upgradeCheck = false;
// A lot package the game saved, to compare with what NeoTS2 would save after the run.
std::optional<std::filesystem::path> saveCompare;
// --last-hood <id>: the neighborhood shown before the lot's own, as the game gives it to the lot.
std::optional<std::int16_t> lastHood;
std::optional<std::filesystem::path> saveTo;
std::optional<std::filesystem::path> saveFolder;
// The first names a birth gives, as the naming dialog would.
std::optional<std::pair<std::string, std::string>> babyNames;

using ts2::content::ResourceIndex;
using ts2::sim::Outcome;
using ts2::sim::RunResult;

constexpr std::uint32_t kEditorStringsGroup = 0x7FE59FD0;
constexpr std::uint32_t kPrimitiveNames = 0x8B;
constexpr std::uint32_t kDataOwners = 0x84;
constexpr std::uint32_t kGenericSimsCalls = 0xDC;
constexpr std::uint32_t kSearchTypes = 0xA4;
constexpr std::uint32_t kGlobalNames = 0x81;

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

    // Container slots of the object type in `group` (from its SLOT resource).
    std::size_t containerSlots(std::uint32_t group, ts2::sim::CodeSource& code) const {
        const auto* type = code.objectType(definitions.at(group).guid());
        return type ? type->containerSlots : 0;
    }

    // The object a BHAV should run as: its own object, an object using its semi-global group,
    // or (for globals) a stand-in person. Empty when no object can reach the BHAV.
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

// Runs every BHAV once and prints how they end. onResult sees each final result and onStep each
// instruction run. quiet leaves the printing to the caller.
int scan(ResourceIndex& index, std::size_t maxSteps, bool interactions,
         const std::shared_ptr<const ts2::sim::SavedNeighborhood>& hood, const WantDefinitionsPtr& wants,
         const ScriptsPtr& scripts, const std::function<void(const RunResult&)>& onResult = {},
         const std::function<void(const ts2::sim::StepInfo&)>& onStep = {}, bool quiet = false) {
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
        vm.onStep = onStep;
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
        if (onResult) {
            onResult(result);
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
    if (quiet) {
        return 0;
    }

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

// A short report of how far the engine gets with what the game uses: every BHAV runs once as an
// interaction (with the --hood neighborhood loaded when given), and the report counts how the runs
// end and which primitives the game's BHAVs use, run, or refuse a mode of. A stop on something only
// a running lot provides (rooms, a route grid, the neighborhood) isn't counted as missing. Writes
// the same numbers as JSON to `json` when given, for the progress chart.
int progress(ResourceIndex& index, const std::shared_ptr<const ts2::sim::SavedNeighborhood>& hood,
             const WantDefinitionsPtr& wants, const ScriptsPtr& scripts, const std::string& json) {
    ts2::sim::IndexCodeSource code(index);
    const auto names = editorTable(index, kPrimitiveNames);
    const auto owners = editorTable(index, kDataOwners);
    const auto genericCalls = editorTable(index, kGenericSimsCalls);
    const auto searchTypes = editorTable(index, kSearchTypes);

    std::map<std::uint16_t, std::size_t> uses;
    for (const auto& r : index.resources()) {
        if (r.entry.key.type != ts2::types::kBehavior) {
            continue;
        }
        if (const auto b = code.behavior(r.entry.key.group, static_cast<std::uint16_t>(r.entry.key.instance))) {
            for (const auto& in : b->instructions) {
                if (in.opcode < 0x100) {
                    ++uses[in.opcode];
                }
            }
        }
    }

    std::set<std::uint16_t> ran;
    std::map<std::uint16_t, std::size_t> refused;
    std::set<std::uint16_t> notImplemented;
    std::map<std::string, std::size_t> missing, needsLot;
    std::size_t ended = 0, waiting = 0, gameErrors = 0, other = 0, total = 0;
    const auto onResult = [&](const RunResult& r) {
        ++total;
        switch (r.outcome) {
        case Outcome::True:
        case Outcome::False:    ++ended; return;
        case Outcome::Sleeping: ++waiting; return;
        case Outcome::Error:    ++gameErrors; return;
        case Outcome::Unsupported: break;
        default:                ++other; return;
        }
        const std::string text = describe(r, names, owners, genericCalls, searchTypes).substr(13);
        // The scan's objects stand off any lot, without rooms or a route grid.
        if (r.reason.find("needs") != std::string::npos || r.reason == "room not known" ||
            r.reason.find("off the lot") != std::string::npos) {
            ++needsLot[text];
            return;
        }
        ++missing[text];
        const auto b = code.behavior(r.group, r.instance);
        if (b && r.pc < b->instructions.size() && b->instructions[r.pc].opcode < 0x100) {
            const auto op = b->instructions[r.pc].opcode;
            ++refused[op];
            if (r.reason == "primitive not implemented") {
                notImplemented.insert(op);
            }
        }
    };
    const auto onStep = [&](const ts2::sim::StepInfo& s) {
        if (s.instruction.opcode < 0x100) {
            ran.insert(s.instruction.opcode);
        }
    };
    scan(index, 10000, true, hood, wants, scripts, onResult, onStep, true);

    struct Primitive { std::uint16_t op; std::string name; std::size_t uses; const char* status; std::size_t refused; };
    std::vector<Primitive> prims;
    std::map<std::string, std::size_t> byStatus, callsByStatus;
    std::size_t calls = 0;
    for (const auto& [op, n] : uses) {
        const char* status = notImplemented.contains(op) ? "missing"
                           : refused.contains(op)        ? "some modes missing"
                           : ran.contains(op)            ? "runs"
                                                         : "not reached";
        const auto it = refused.find(op);
        prims.push_back({op, op < names.size() ? names[op] : "", n, status, it == refused.end() ? 0 : it->second});
        ++byStatus[status];
        callsByStatus[status] += n;
        calls += n;
    }
    const auto pct = [](std::size_t a, std::size_t b) { return b ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0; };
    const auto sorted = [](const std::map<std::string, std::size_t>& m) {
        std::vector<std::pair<std::size_t, std::string>> v;
        for (const auto& [k, n] : m) {
            v.emplace_back(n, k);
        }
        std::sort(v.rbegin(), v.rend());
        return v;
    };

    const auto sum = [](const std::map<std::string, std::size_t>& m) {
        std::size_t n = 0;
        for (const auto& [k, v] : m) {
            n += v;
        }
        return n;
    };
    std::printf("\nScripts: %zu run once each\n", total);
    std::printf("  ran to an end %.1f%%, waiting %.1f%%, stopped by the game's own error checks %.1f%%,\n"
                "  missing in our engine %.1f%%, need a running lot %.1f%%, other %.1f%%\n",
                pct(ended, total), pct(waiting, total), pct(gameErrors, total), pct(sum(missing), total),
                pct(sum(needsLot), total), pct(other, total));
    std::printf("\nPrimitives: %zu used by the game's BHAVs (%zu calls)\n", prims.size(), calls);
    for (const char* status : {"runs", "some modes missing", "missing", "not reached"}) {
        std::printf("  %-19s %3zu  (%.1f%% of calls)\n", status, byStatus[status], pct(callsByStatus[status], calls));
    }
    for (const auto& p : prims) {
        if (std::string_view(p.status) != "runs") {
            std::printf("    0x%02X %-34s %-19s %6zu uses\n", p.op, p.name.c_str(), p.status, p.uses);
        }
    }
    const auto luaFunctions = ts2::sim::scriptFunctions();
    const auto luaDone = static_cast<std::size_t>(
        std::count_if(luaFunctions.begin(), luaFunctions.end(), [](const auto& f) { return f.implemented; }));
    std::printf("\nLua functions the game registers: %zu, %zu implemented (%.1f%%), the rest named in --json\n",
                luaFunctions.size(), luaDone, pct(luaDone, luaFunctions.size()));
    std::printf("\nMissing in our engine (scripts stopped, top 20):\n");
    const auto missingSorted = sorted(missing);
    for (std::size_t i = 0; i < std::min<std::size_t>(missingSorted.size(), 20); ++i) {
        std::printf("  %6zu  %s\n", missingSorted[i].first, missingSorted[i].second.c_str());
    }

    if (!json.empty()) {
        std::ofstream f(json);
        if (!f) {
            std::fprintf(stderr, "can't write %s\n", json.c_str());
            return 1;
        }
        const auto quoted = [](const std::string& s) {
            std::string out = "\"";
            for (const char c : s) {
                if (c == '"' || c == '\\') {
                    out += '\\';
                }
                out += (static_cast<unsigned char>(c) < 0x20) ? ' ' : c;
            }
            return out + "\"";
        };
        const auto list = [&](const std::vector<std::pair<std::size_t, std::string>>& v) {
            std::string out;
            for (const auto& [n, text] : v) {
                out += std::string(out.empty() ? "[" : ",[") + quoted(text) + "," + std::to_string(n) + "]";
            }
            return out;
        };
        f << "{\"scripts\":{\"total\":" << total << ",\"ended\":" << ended << ",\"waiting\":" << waiting
          << ",\"gameErrors\":" << gameErrors << ",\"other\":" << other << "},\n\"primitives\":[";
        for (std::size_t i = 0; i < prims.size(); ++i) {
            f << (i ? "," : "") << "[" << prims[i].op << "," << quoted(prims[i].name) << "," << prims[i].uses << ",\""
              << prims[i].status << "\"," << prims[i].refused << "]";
        }
        f << "],\n\"lua\":[";
        for (std::size_t i = 0; i < luaFunctions.size(); ++i) {
            f << (i ? "," : "") << "[" << quoted(luaFunctions[i].name) << "," << (luaFunctions[i].implemented ? 1 : 0) << "]";
        }
        f << "],\n\"missing\":[" << list(missingSorted) << "],\n\"needsLot\":[" << list(sorted(needsLot)) << "]}\n";
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

// With --messages, each message box a script asks is printed and answered with its first button.
bool showMessages = false;

std::optional<ts2::simi::Record> simulatorRecordOf(const std::filesystem::path& lot) {
    ts2::dbpf::Package package{lot};
    for (const auto& e : package.entries()) {
        if (e.key.type == ts2::simi::kType) {
            return ts2::simi::readRecord(package.read(e));
        }
    }
    return std::nullopt;
}

// Orders resource keys so the two lots' objects can be matched up.
struct KeyOrder {
    bool operator()(const ts2::dbpf::ResourceKey& a, const ts2::dbpf::ResourceKey& b) const {
        return std::tie(a.type, a.group, a.instance, a.instanceHigh) < std::tie(b.type, b.group, b.instance, b.instanceHigh);
    }
};

// Every saved object of a lot by its resource key, with the kind its type gives.
std::map<ts2::dbpf::ResourceKey, std::pair<ts2::xobj::SavedObject, ts2::xobj::RecordKind>, KeyOrder> savedObjectsOf(
    const std::filesystem::path& lot) {
    std::map<ts2::dbpf::ResourceKey, std::pair<ts2::xobj::SavedObject, ts2::xobj::RecordKind>, KeyOrder> out;
    // Objects are read at the version the lot holds, so a lot the game has never saved compares too.
    // Only each object's current record is taken, not those left from objects deleted before.
    const ts2::content::Lot read = ts2::content::loadLot(lot);
    const std::uint32_t version = read.objectMap.version;
    std::set<std::pair<std::uint32_t, std::uint32_t>> current;
    for (const auto& object : read.objects) {
        current.insert({object.resourceType, object.id});
    }
    ts2::dbpf::Package package{lot};
    for (const auto& e : package.entries()) {
        if (!current.contains({e.key.type, e.key.instance})) {
            continue;
        }
        std::optional<ts2::xobj::RecordKind> kind;
        if (e.key.type == ts2::types::kSavedObject) {
            kind = ts2::xobj::RecordKind::Object;
        } else if (e.key.type == ts2::types::kSavedObjectWithMaterial) {
            kind = ts2::xobj::RecordKind::ObjectWithMaterial;
        } else if (e.key.type == ts2::types::kSavedPerson) {
            kind = ts2::xobj::RecordKind::Person;
        }
        if (!kind) {
            continue;
        }
        try {
            out.emplace(e.key, std::pair{ts2::xobj::parse(package.read(e), version, *kind), *kind});
        } catch (const std::exception&) {
        }
    }
    return out;
}

// Prints which fields of the saved objects differ between two lots, counted over every object the
// two share. Used to see what a save has to fill.
void compareSavedObjects(const std::filesystem::path& leftLot, const std::filesystem::path& rightLot, const char* what) {
    const auto left = savedObjectsOf(leftLot);
    const auto right = savedObjectsOf(rightLot);
    std::map<std::string, int> counts;
    std::map<std::string, std::string> examples;
    std::size_t shared = 0, differing = 0;
    for (const auto& [key, entry] : left) {
        const auto it = right.find(key);
        if (it == right.end()) {
            continue;
        }
        ++shared;
        const ts2::xobj::SavedObject& a = entry.first;
        const ts2::xobj::SavedObject& b = it->second.first;
        const std::size_t before = counts.size();
        std::size_t marks = 0;
        const auto mark = [&](const char* field, bool differs) {
            if (differs) {
                ++counts[field];
                ++marks;
            }
        };
        if (a.header.location != b.header.location && examples.find("header") == examples.end()) {
            char line[192];
            std::snprintf(line, sizeof line, "object %u: left %.4f %.4f level %d, right %.4f %.4f level %d", key.instance,
                          a.header.location[0], a.header.location[1], a.header.value1, b.header.location[0],
                          b.header.location[1], b.header.value1);
            examples["header"] = line;
        }
        mark("header", !(a.header.location == b.header.location && a.header.value1 == b.header.value1 &&
                         a.header.value2 == b.header.value2 && a.header.value3 == b.header.value3 &&
                         a.header.value4 == b.header.value4 && a.header.value5 == b.header.value5 &&
                         a.header.value6 == b.header.value6 && a.header.reserved == b.header.reserved));
        mark("attributes", a.attributes != b.attributes);
        mark("semi attributes", a.semiAttributes != b.semiAttributes);
        mark("temps", a.temps != b.temps);
        mark("token source", a.tokenSource != b.tokenSource);
        mark("rest", a.rest != b.rest);
        for (std::size_t i = 0; i < a.data.size() && i < b.data.size(); ++i) {
            if (a.data[i] != b.data[i]) {
                char name[32];
                std::snprintf(name, sizeof name, "data 0x%02zX", i);
                ++counts[name];
                ++marks;
                if (examples.find(name) == examples.end()) {
                    char line[96];
                    std::snprintf(line, sizeof line, "object %u: left %d, right %d", key.instance, a.data[i], b.data[i]);
                    examples[name] = line;
                }
                char tally[48];
                std::snprintf(tally, sizeof tally, "data 0x%02zX %d -> %d", i, a.data[i], b.data[i]);
                ++counts[tally];
            }
        }
        if (a.tail && b.tail) {
            const auto& x = *a.tail;
            const auto& y = *b.tail;
            mark("tail stack depth", x.stack.size() != y.stack.size());
            if (x.stack.size() == y.stack.size()) {
                bool node = false, locals = false, params = false;
                bool objectId = false, treeId = false, value1 = false, value2 = false, value3 = false, owner = false;
                for (std::size_t i = 0; i < x.stack.size(); ++i) {
                    node = node || x.stack[i].node != y.stack[i].node;
                    locals = locals || x.stack[i].locals != y.stack[i].locals;
                    params = params || x.stack[i].params != y.stack[i].params;
                    objectId = objectId || x.stack[i].objectId != y.stack[i].objectId;
                    treeId = treeId || x.stack[i].treeId != y.stack[i].treeId;
                    value1 = value1 || x.stack[i].value1 != y.stack[i].value1;
                    value2 = value2 || x.stack[i].value2 != y.stack[i].value2;
                    value3 = value3 || x.stack[i].value3 != y.stack[i].value3;
                    owner = owner || x.stack[i].codeOwner != y.stack[i].codeOwner;
                }
                mark("stack node", node);
                mark("stack locals", locals);
                mark("stack params", params);
                if (params && examples.find("stack params") == examples.end()) {
                    for (std::size_t i = 0; i < x.stack.size(); ++i) {
                        if (x.stack[i].params != y.stack[i].params) {
                            char line[192];
                            std::snprintf(line, sizeof line, "object %u frame %zu tree %d: left %zu params, right %zu",
                                          key.instance, i, x.stack[i].treeId, x.stack[i].params.size(), y.stack[i].params.size());
                            examples["stack params"] = line;
                            break;
                        }
                    }
                }
                mark("stack frame object", objectId);
                mark("stack frame tree", treeId);
                mark("stack frame value1", value1);
                mark("stack frame value2", value2);
                if (value2 && examples.find("stack frame value2") == examples.end()) {
                    for (std::size_t i = 0; i < x.stack.size(); ++i) {
                        if (x.stack[i].value2 != y.stack[i].value2) {
                            char line[192];
                            std::snprintf(line, sizeof line, "object %u frame %zu: left %d, right %d", key.instance, i,
                                          x.stack[i].value2, y.stack[i].value2);
                            examples["stack frame value2"] = line;
                            break;
                        }
                    }
                }
                mark("stack frame value3", value3);
                mark("stack frame code owner", owner);
                if (owner && examples.find("stack frame code owner") == examples.end()) {
                    for (std::size_t i = 0; i < x.stack.size(); ++i) {
                        if (x.stack[i].codeOwner != y.stack[i].codeOwner) {
                            char line[192];
                            std::snprintf(line, sizeof line, "object %u frame %zu tree %d: left entry %d, right %d",
                                          key.instance, i, x.stack[i].treeId, x.stack[i].codeOwner, y.stack[i].codeOwner);
                            examples["stack frame code owner"] = line;
                            break;
                        }
                    }
                }
            }
            mark("tail value1", x.value1 != y.value1);
            mark("tail value2", x.value2 != y.value2);
            mark("tail stack value", x.stackValue != y.stackValue);
            mark("tail slot marker", x.slots.marker != y.slots.marker || x.slots.extra != y.slots.extra);
            mark("tail material block", x.materialBlock != y.materialBlock);
            mark("tail token flags", x.token.flags != y.token.flags || x.token.value1 != y.token.value1 ||
                                         x.token.value2 != y.token.value2 || x.token.value3 != y.token.value3);
            mark("tail token", !(x.token.guid == y.token.guid && x.token.properties == y.token.properties));
            mark("tail arrays", x.arrays != y.arrays);
            mark("tail shorts", x.shorts != y.shorts);
            mark("tail strings", x.strings != y.strings);
            mark("tail slots", x.slots.values != y.slots.values);
            mark("tail relationships", x.relationshipIds != y.relationshipIds);
            mark("tail timers", x.timers.size() != y.timers.size());
            mark("tail effects", x.effects.size() != y.effects.size());
            char shape[96];
            std::snprintf(shape, sizeof shape, "tail shape: relationships v%d -> v%d, effects flag %d v%d -> flag %d v%d",
                          x.relationshipVersion, y.relationshipVersion, x.effectFlag, x.effectVersion, y.effectFlag,
                          y.effectVersion);
            if (x.relationshipVersion != y.relationshipVersion || x.effectFlag != y.effectFlag ||
                x.effectVersion != y.effectVersion) {
                ++counts[shape];
                if (examples.find(shape) == examples.end()) {
                    std::string line = "object " + std::to_string(key.instance) + ":";
                    for (const auto* list : {&x.effects, &y.effects}) {
                        line += list == &x.effects ? " left" : " | right";
                        for (const auto& fx : *list) {
                            line += " [" + fx.name + " @" + fx.slot + " " + std::to_string(fx.value1) + " " +
                                    std::to_string(fx.value2) + " " + std::to_string(fx.value3) + "]";
                        }
                    }
                    examples[shape] = line;
                }
            }
        } else {
            mark("tail", a.tail.has_value() != b.tail.has_value());
        }
        if (a.person && b.person) {
            mark("motives", a.person->motives != b.person->motives);
            mark("person data", a.person->personData != b.person->personData);
            const auto& x = a.person->state;
            const auto& y = b.person->state;
            const auto same = [](const ts2::xobj::SavedInteraction& p, const ts2::xobj::SavedInteraction& q) {
                return p.tree == q.tree && p.index == q.index && p.target == q.target && p.actor == q.actor &&
                       p.priority == q.priority && p.number == q.number && p.id == q.id && p.linkId == q.linkId &&
                       p.iconObject == q.iconObject && p.iconGuid == q.iconGuid && p.iconIndex == q.iconIndex &&
                       p.name == q.name && p.flags == q.flags && p.values == q.values && p.lineFlag1 == q.lineFlag1 &&
                       p.lineFlag2 == q.lineFlag2 && p.lineFlag3 == q.lineFlag3 && p.value1 == q.value1 &&
                       p.value2 == q.value2 && p.value5 == q.value5 && p.value6 == q.value6;
            };
            mark("running interaction", !same(x.running, y.running));
            mark("interrupted interaction", !same(x.interrupted, y.interrupted));
            mark("queue length", x.queue.size() != y.queue.size());
            if (x.queue.size() == y.queue.size()) {
                bool queued = false;
                for (std::size_t i = 0; i < x.queue.size(); ++i) {
                    queued = queued || !same(x.queue[i], y.queue[i]);
                }
                mark("queued interactions", queued);
            }
            mark("person runs", x.runs.size() != y.runs.size());
        }
        if (marks != 0 || counts.size() != before) {
            ++differing;
        }
    }
    std::printf("%s: %zu objects shared, %zu differ\n", what, shared, differing);
    std::vector<std::pair<int, std::string>> sorted;
    for (const auto& [field, n] : counts) {
        sorted.emplace_back(n, field);
    }
    std::sort(sorted.rbegin(), sorted.rend());
    for (const auto& [n, field] : sorted) {
        std::printf("  %5d  %s\n", n, field.c_str());
        if (const auto it = examples.find(field); it != examples.end()) {
            std::printf("         e.g. %s\n", it->second.c_str());
        }
    }
}

// The lot's object map (instance 1) and object type table, read from its package.
std::optional<ts2::objm::MapRecord> objectMapOf(const std::filesystem::path& lot) {
    ts2::dbpf::Package package{lot};
    for (const auto& e : package.entries()) {
        if (e.key.type == ts2::types::kObjectMap && e.key.instance == 1) {
            return ts2::objm::readMapRecord(package.read(e));
        }
    }
    return std::nullopt;
}

std::optional<ts2::objt::ObjectTypeTable> objectTypesOf(const std::filesystem::path& lot) {
    ts2::dbpf::Package package{lot};
    for (const auto& e : package.entries()) {
        if (e.key.type == ts2::types::kObjectTypeTable) {
            return ts2::objt::parse(package.read(e));
        }
    }
    return std::nullopt;
}

// Prints how the object map NeoTS2 would save now differs from the one the game saved.
void compareObjectMap(ts2::sim::LotSimulation& sim, const std::filesystem::path& lotPath, const std::filesystem::path& gameLot) {
    const auto loaded = objectMapOf(lotPath);
    const auto game = objectMapOf(gameLot);
    const auto types = objectTypesOf(lotPath);
    if (!loaded || !game || !types) {
        std::printf("save compare: ObjM not readable in %s\n", loaded && types ? "the game's lot" : "the loaded lot");
        return;
    }
    const ts2::objm::MapRecord ours = ts2::sim::objectMapRecord(*loaded, *types, sim);
    std::size_t differences = 0;
    if (ours.objects != game->objects) {
        ++differences;
        std::printf("save compare: ObjM objects: ours %zu, game %zu, loaded %zu\n", ours.objects.size(), game->objects.size(),
                    loaded->objects.size());
        for (std::size_t i = 0, shown = 0; i < std::max(ours.objects.size(), game->objects.size()) && shown < 5; ++i) {
            const auto ourEntry = i < ours.objects.size() ? ours.objects[i] : std::pair<std::int32_t, std::int32_t>{-1, -1};
            const auto gameEntry = i < game->objects.size() ? game->objects[i] : std::pair<std::int32_t, std::int32_t>{-1, -1};
            if (ourEntry != gameEntry) {
                ++shown;
                std::printf("  at %zu: ours object %d type %d, game object %d type %d\n", i, ourEntry.first, ourEntry.second,
                            gameEntry.first, gameEntry.second);
            }
        }
    }
    if (ours.waits != game->waits) {
        ++differences;
        std::printf("save compare: ObjM waits: ours %zu entries, game %zu\n", ours.waits.size(), game->waits.size());
        for (std::size_t i = 0, shown = 0; i < std::min(ours.waits.size(), game->waits.size()) && shown < 5; ++i) {
            if (ours.waits[i] != game->waits[i]) {
                ++shown;
                std::printf("  object %zu: ours 0x%08X, game 0x%08X, loaded 0x%08X\n", i + 1, ours.waits[i], game->waits[i],
                            i < loaded->waits.size() ? loaded->waits[i] : 0);
            }
        }
    }
    if (ours.waitsWithObject != game->waitsWithObject) {
        ++differences;
        std::printf("save compare: ObjM objects with a wait: ours %d, game %d, loaded %d\n", ours.waitsWithObject,
                    game->waitsWithObject, loaded->waitsWithObject);
    }
    if (ts2::objm::writeMapRecord(ours) != ts2::objm::writeMapRecord(*game) && differences == 0) {
        ++differences;
        std::printf("save compare: ObjM differs in values NeoTS2 keeps as loaded\n");
    }
    std::printf("save compare: ObjM %s\n", differences == 0 ? "the same" : "differs");
}

// Prints where the records NeoTS2 would save now differ from those the game saved in `gameLot`,
// with the values the lot was loaded with.
void compareSave(ResourceIndex& index, ts2::sim::LotSimulation& sim, const std::filesystem::path& lotPath,
                 const std::filesystem::path& gameLot) {
    const auto loaded = simulatorRecordOf(lotPath);
    const auto game = simulatorRecordOf(gameLot);
    if (!loaded || !game) {
        std::printf("save compare: SIMI not readable in %s\n", loaded ? "the game's lot" : "the loaded lot");
        return;
    }
    const auto names = editorTable(index, kGlobalNames);
    const ts2::simi::Record ours = ts2::sim::simulatorRecord(*loaded, sim);
    std::size_t differences = 0;
    for (std::size_t i = 0; i < ours.globals.size(); ++i) {
        if (ours.globals[i] != game->globals[i]) {
            ++differences;
            std::printf("save compare: SIMI global 0x%02zX %s: ours %d, game %d, loaded %d\n", i, i < names.size() ? names[i].c_str() : "",
                        ours.globals[i], game->globals[i], loaded->globals[i]);
        }
    }
    if (ours.tickRandomState != game->tickRandomState) {
        ++differences;
        std::printf("save compare: SIMI tick random state: ours 0x%08X, game 0x%08X, loaded 0x%08X\n", ours.tickRandomState,
                    game->tickRandomState, loaded->tickRandomState);
    }
    if (ts2::simi::writeRecord(ours) != ts2::simi::writeRecord(*game) && differences == 0) {
        ++differences;
        std::printf("save compare: SIMI differs in values NeoTS2 keeps as loaded\n");
    }
    std::printf("save compare: SIMI %s\n", differences == 0 ? "the same" : "differs");
    compareObjectMap(sim, lotPath, gameLot);
    compareSavedObjects(lotPath, gameLot, "save compare: objects as loaded against the game's save");
}




// Counts how many of a lot's records can be read at the version the lot holds and written at the
// version the game writes now, which is what saving a lot the game has never saved would need.
// With --save-compare naming the game's own save of the same lot, the upgraded records are also
// compared with the ones the game wrote. The game ran the lot a little before it saved, so some
// values differ by play.
void printUpgradeCheck(const std::filesystem::path& lot) {
    const ts2::content::Lot read = ts2::content::loadLot(lot);
    std::printf("lot version 0x%X\n", read.objectMap.version);
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::uint8_t>> game;
    if (saveCompare) {
        ts2::dbpf::Package theirs{*saveCompare};
        for (const auto& e : theirs.entries()) {
            game[{e.key.type, e.key.instance}] = theirs.read(e);
        }
    }
    int same = 0, differ = 0, shown = 0;
    const auto compare = [&](const std::vector<std::uint8_t>& ours, std::uint32_t type, std::uint32_t instance) {
        const auto theirs = game.find({type, instance});
        if (theirs == game.end()) {
            return;
        }
        if (ours == theirs->second) {
            ++same;
            return;
        }
        ++differ;
        if (shown++ >= 16) {
            return;
        }
        std::string where;
        int listed = 0;
        const auto& g = theirs->second;
        for (std::size_t k = 0; k + 1 < std::max(ours.size(), g.size()) && listed < 8; k += 2) {
            const int a = k + 1 < ours.size() ? ours[k] | ours[k + 1] << 8 : -1;
            const int b = k + 1 < g.size() ? g[k] | g[k + 1] << 8 : -1;
            if (a != b) {
                where += " " + std::to_string(k) + ":" + std::to_string(a) + "/" + std::to_string(b);
                ++listed;
            }
        }
        std::printf("  %08X %u: %zu bytes, game %zu, words at ours/game%s\n", type, instance, ours.size(), g.size(),
                    where.c_str());
    };
    int objects = 0, upgraded = 0, failed = 0, leftOver = 0, scenesRead = 0, scenesUnread = 0;
    std::string unreadScenes;
    std::map<std::string, int> problems;
    std::map<std::string, std::string> failedRecords;
    ts2::dbpf::Package package{lot};
    for (const auto& e : package.entries()) {
        if (e.key.type == ts2::simi::kType) {
            const auto record = ts2::simi::readRecord(package.read(e));
            std::printf("simulator record: %s\n", record ? "read and written at the game's version" : "could not be read");
            if (record) {
                compare(ts2::simi::writeRecord(*record), e.key.type, e.key.instance);
            }
        }
        // Scene records of objects on the lot, which hold their poses: those that read can be written.
        if (e.key.type == ts2::cobject::kType && read.objectMap.typeIndexByObject.contains(e.key.instance)) {
            if (ts2::cobject::readRecord(package.read(e))) {
                ++scenesRead;
            } else {
                ++scenesUnread;
                if (unreadScenes.size() < 120) {
                    unreadScenes += " " + std::to_string(e.key.instance);
                }
            }
        }
        std::optional<ts2::xobj::RecordKind> kind;
        if (e.key.type == ts2::types::kSavedObject) {
            kind = ts2::xobj::RecordKind::Object;
        } else if (e.key.type == ts2::types::kSavedObjectWithMaterial) {
            kind = ts2::xobj::RecordKind::ObjectWithMaterial;
        } else if (e.key.type == ts2::types::kSavedPerson) {
            kind = ts2::xobj::RecordKind::Person;
        }
        if (!kind) {
            continue;
        }
        // Only an object's current record counts: one listed in the object map, of the kind its type
        // asks for. A lot keeps records of objects deleted long ago, which the game never reads.
        const auto listed = read.objectMap.typeIndexByObject.find(e.key.instance);
        const auto* type = listed != read.objectMap.typeIndexByObject.end()
                               ? read.types.find(static_cast<std::uint16_t>(listed->second))
                               : nullptr;
        const std::uint32_t wanted = type == nullptr                               ? 0
                                     : type->kind == ts2::objt::kPersonKind        ? ts2::types::kSavedPerson
                                     : (type->flag & 1)                            ? ts2::types::kSavedObjectWithMaterial
                                                                                   : ts2::types::kSavedObject;
        if (e.key.type != wanted) {
            ++leftOver;
            continue;
        }
        ++objects;
        try {
            ts2::xobj::SavedObject saved = ts2::xobj::parse(package.read(e), read.objectMap.version, *kind);
            if (saved.version != ts2::xobj::kWrittenVersion) {
                auto moved = ts2::xobj::upgraded(saved);
                if (!moved) {
                    throw std::runtime_error("no upgrade from this version");
                }
                saved = std::move(*moved);
            }
            const auto written = ts2::xobj::write(saved, *kind);
            ++upgraded;
            compare(written, e.key.type, e.key.instance);
        } catch (const std::exception& ex) {
            ++failed;
            ++problems[ex.what()];
            auto& list = failedRecords[ex.what()];
            if (list.size() < 60) {
                char one[24];
                std::snprintf(one, sizeof one, " %08X:%u", e.key.type, e.key.instance);
                list += one;
            }
        }
    }
    std::printf("objects: %d read at the lot's version and written at the game's, %d could not be, of %d (and %d "
                "records left from objects deleted before)\n",
                upgraded, failed, objects, leftOver);
    for (const auto& [why, n] : problems) {
        std::printf("  %d: %s, e.g.%s\n", n, why.c_str(), failedRecords[why].c_str());
    }
    std::printf("scene records of objects on the lot: %d read, %d do not:%s\n", scenesRead, scenesUnread,
                unreadScenes.c_str());
    if (saveCompare) {
        std::printf("against the game's save: %d records the same, %d differ\n", same, differ);
    }
}

// Prints the animations a lot's objects were saved playing, to see what a save has to write.
void printSavedAnimations(const std::filesystem::path& lot) {
    const ts2::content::Lot read = ts2::content::loadLot(lot);
    std::map<std::pair<std::string, std::int32_t>, int> byTypeAndLayer;
    for (const auto& [id, list] : read.savedAnimations) {
        for (const auto& a : list) {
            byTypeAndLayer[{a.type, a.layer}] += 1;
            if (a.state != 1) {
                continue;
            }
        }
    }
    for (const auto& [id, list] : read.savedAnimations) {
        std::printf("  object %u: %zu animations\n", id, list.size());
        for (const auto& a : list) {
            std::printf("    kind %d type %-4s layer %d state %d plays %d rate %.2f ago %d \"%s\" events to object %d tree 0x%04X guid 0x%08X\n",
                        static_cast<int>(a.kind), a.type.c_str(), a.layer, a.state, a.plays, a.rate, a.startedMsAgo,
                        a.name.c_str(), a.eventObject, a.eventTree, a.eventObjectGuid);
        }
    }
    std::printf("saved animations by type and layer:\n");
    for (const auto& [key, n] : byTypeAndLayer) {
        std::printf("  %-6s layer %d: %d\n", key.first.c_str(), key.second, n);
    }
}

// Prints the interactions a lot's Sims were saved with, to see what a load has to give them back.
void printSavedQueues(const std::filesystem::path& lot) {
    const auto one = [](const char* what, const ts2::xobj::SavedInteraction& i) {
        std::printf("    %-11s tree %u line %d target %d actor %d priority %d number %d id %d link %d icon %d/%u/%u"
                    " flags 0x%X line flags %u/%u/%u \"%s\"\n",
                    what, i.tree, i.index, i.target, i.actor, i.priority, i.number, i.id, i.linkId, i.iconObject,
                    static_cast<unsigned>(i.iconGuid), static_cast<unsigned>(i.iconIndex), static_cast<unsigned>(i.flags),
                    i.lineFlag1, i.lineFlag2, i.lineFlag3, i.name.c_str());
    };
    ts2::dbpf::Package package{lot};
    for (const auto& e : package.entries()) {
        if (e.key.type != ts2::types::kSavedPerson) {
            continue;
        }
        ts2::xobj::SavedObject saved;
        try {
            saved = ts2::xobj::parse(package.read(e), ts2::xobj::kWrittenVersion, ts2::xobj::RecordKind::Person);
        } catch (const std::exception&) {
            continue;
        }
        if (!saved.person) {
            continue;
        }
        const ts2::xobj::PersonState& state = saved.person->state;
        std::printf("  Sim %u: %zu queued, %zu in the sub-queue, %zu in the list, %zu runs%s\n", e.key.instance,
                    state.queue.size(), state.subQueue.size(), state.list.size(), state.runs.size(),
                    saved.person->stateUnread ? ", state unread" : "");
        one("running", state.running);
        one("interrupted", state.interrupted);
        if (state.runningSub) {
            one("running sub", *state.runningSub);
        }
        for (const auto& i : state.queue) {
            one("queued", i);
        }
        for (const auto& i : state.subQueue) {
            one("sub-queued", i);
        }
        for (const auto& i : state.list) {
            one("listed", i);
        }
    }
}

// Writes the lot package NeoTS2 would save now: the loaded package with the records it can build
// from the running lot put back in their places. The lot being played is never touched.
void writeLot(ts2::sim::LotSimulation& sim, const std::filesystem::path& lotPath, const std::filesystem::path& out) {
    std::error_code error;
    if (std::filesystem::equivalent(lotPath, out, error)) {
        std::printf("save: the output is the lot being run, which is never overwritten\n");
        return;
    }
    std::ifstream in(lotPath, std::ios::binary);
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (file.empty()) {
        std::printf("save: the lot can't be read\n");
        return;
    }
    ts2::dbpf::PackageImage package = ts2::dbpf::readImage(file);
    if (!ts2::sim::canSaveLot(package)) {
        std::printf("save: the lot was last saved by an older version of the game, which NeoTS2 cannot write yet\n");
        return;
    }
    const ts2::sim::LotSaveCounts counts = ts2::sim::fillLotPackage(package, sim, nullptr);
    const std::vector<std::uint8_t> bytes = ts2::dbpf::writeImage(package);
    ts2::dbpf::verifyImage(bytes, package);
    if (!writeBytes(out, bytes)) {
        return;
    }
    std::printf("save: %zu bytes, %d simulator, %d object map, %d camera, %d objects (%d added, %d of them started fresh, %d unnamed, %d gone, %d stacks kept), %d types added, %d kept as loaded\n", bytes.size(), counts.simulator,
                counts.objectMap, counts.camera, counts.objects, counts.objectsAdded, counts.objectsFresh, counts.objectsUnnamed, counts.objectsKept, counts.stacksKept,
                counts.typesAdded, counts.keptAsLoaded);
}

// Writes the app's save of the running lot (the lot, the neighborhood and the character files of
// Sims made this run) into `out`, a folder laid out as a neighborhood folder. Looks changed at a
// mirror are not written. The lot's own neighborhood folder is only read, and refused as `out`.
void writeSaveFolder(ts2::sim::LotSimulation& sim, const std::filesystem::path& install, const std::filesystem::path& lotPath,
                     const std::filesystem::path& out) {
    const auto readFile = [](const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const auto writeFile = [](const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        return writeBytes(path, bytes);
    };
    const std::filesystem::path hoodFolder = lotPath.parent_path().parent_path();
    std::error_code error;
    if (std::filesystem::equivalent(hoodFolder, out, error)) {
        std::printf("save: the output is the lot's own neighborhood folder, which is never overwritten\n");
        return;
    }
    const std::string hoodName = hoodFolder.filename().string();
    const std::string lotName = lotPath.stem().string();
    const auto lotAt = lotName.rfind("_Lot");
    if (lotAt == std::string::npos) {
        std::printf("save: the lot's file name has no lot number\n");
        return;
    }
    const auto lotId = static_cast<std::uint32_t>(std::stoul(lotName.substr(lotAt + 4)));
    ts2::scene::Assets assets(install);
    const ts2::scene::BabyCatalog catalog = ts2::scene::readBabyCatalog(assets.skinPackages());
    const ts2::sim::NewCharacterFiles made = ts2::sim::newCharacterFiles(sim, assets, catalog, hoodFolder);
    if (!made.problem.empty()) {
        std::printf("save: the Sims made this run are left out: %s\n", made.problem.c_str());
    }
    ts2::dbpf::PackageImage lot = ts2::dbpf::readImage(readFile(lotPath));
    if (!ts2::sim::canSaveLot(lot)) {
        std::printf("save: the lot was last saved by an older version of the game, which NeoTS2 cannot write yet\n");
        return;
    }
    const ts2::sim::LotSaveCounts counts = ts2::sim::fillLotPackage(lot, sim, nullptr, made.neighbors);
    ts2::dbpf::PackageImage hood = ts2::dbpf::readImage(readFile(hoodFolder / (hoodName + "_Neighborhood.package")));
    const ts2::sim::HoodSaveCounts hoodCounts =
        ts2::sim::fillNeighborhoodPackage(hood, lot, lotId, sim.world(), sim.globals(),
                                          made.problem.empty() ? ts2::sim::NewNeighbors::Written : ts2::sim::NewNeighbors::LeftOut);
    if (!writeFile(out / "Lots" / lotPath.filename(), ts2::dbpf::writeImage(lot)) ||
        !writeFile(out / (hoodName + "_Neighborhood.package"), ts2::dbpf::writeImage(hood))) {
        return;
    }
    for (const auto& file : made.files) {
        if (!writeFile(out / "Characters" / file.path.filename(), file.bytes)) {
            return;
        }
        std::printf("save: character file %s for neighbor %d, %zu bytes\n", file.path.filename().string().c_str(), file.neighbor, file.bytes.size());
    }
    std::printf("save: %d objects (%d added, %d unnamed), %d types added, %d descriptions, %d new neighbors, %d new DNA\n", counts.objects,
                counts.objectsAdded, counts.objectsUnnamed, counts.typesAdded, hoodCounts.descriptions, hoodCounts.newNeighbors,
                hoodCounts.newDna);
}

// Brings a saved lot to life and runs it for a number of ticks, then says why scripts stopped.
int runLot(ResourceIndex& index, const std::filesystem::path& install, const std::filesystem::path& lotPath,
           std::size_t ticks, std::optional<std::size_t> traceTick, std::optional<std::int16_t> traceObject, bool onScreen,
           bool personData, bool overlays, bool motion, std::optional<std::int16_t> pieSim, std::optional<std::int16_t> pieTrace,
           std::optional<std::array<int, 3>> queueChoice, std::size_t queueTick, std::optional<std::size_t> queueAgainTick, std::optional<std::size_t> cancelTick,
           std::optional<std::array<float, 3>> groundAt, bool portals, std::optional<std::array<float, 4>> groundLater,
           const std::vector<std::array<float, 5>>& goHere, std::optional<std::uint32_t> seed,
           const std::vector<std::array<float, 3>>& motives, std::optional<std::array<float, 2>> camera) {
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
    ts2::sim::LotSimulation sim(install, lotPath, traceLoad, seed, lastHood);
    if (babyNames) {
        sim.world().setBabyFirstNames(babyNames->first, babyNames->second);
    }
    auto& world = sim.world();
    std::optional<ts2::content::UiData> ui;
    if (showMessages) {
        ui.emplace(install);
        sim.setTextTables([&](std::uint32_t group, std::uint16_t instance) { return ui->texts(group, instance); });
    }
    ts2::scene::Assets assets(install);
    ts2::scene::AnimationLibrary animations(assets);
    // Routing as the app has it: the lot's route grid from the objects' models.
    sim.setRouteSources(app::routeSources(assets, app::readRouteModels(assets, lotPath)));
    world.setAnimationLengths([&](const std::string& name) { return animations.length(name); });
    world.setAnimationCarries([&](const std::string& name) { return animations.hasTargetType(name, 2); });
    world.setAnimationMovesSkeleton([&](const std::string& name) { return animations.hasTargetType(name, 1); });
    world.setLocoClips([&](const std::string& name) -> std::optional<ts2::sim::LocoClip> {
        const auto a = animations.find(name);
        return a ? std::optional<ts2::sim::LocoClip>({name, a->duration, a->travel, a->loopPhase, a->match, a->speed, a->rootTurn, a->category}) : std::nullopt;
    });
    world.setAnimationEventTime([&](const std::string& name, int id) { return animations.eventTime(name, id); });
    // The world reads each clip's events in place, so they are kept for the whole run.
    std::map<std::string, std::vector<ts2::sim::AnimationEvent>, std::less<>> animationEvents;
    world.setAnimationEvents([&](const std::string& name) -> const std::vector<ts2::sim::AnimationEvent>& {
        if (const auto it = animationEvents.find(name); it != animationEvents.end()) {
            return it->second;
        }
        auto& events = animationEvents[name];
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
    // Where the camera is (x, y in tiles), for scripts that turn Sims towards it.
    if (camera) {
        world.setCameraPoint([camera]() -> std::optional<std::array<float, 2>> { return *camera; });
    }
    // Motives set before the first tick (Sim, motive, value), to bring on what low motives do.
    for (const auto& [id, motive, value] : motives) {
        auto* who = world.find(static_cast<std::int16_t>(id));
        if (who != nullptr && motive >= 0 && motive < static_cast<float>(ts2::sim::kMotiveCount)) {
            who->motives[static_cast<std::size_t>(motive)] = value;
        }
        std::printf("motive %d of Sim %d %s\n", static_cast<int>(motive), static_cast<int>(id), who != nullptr ? "set" : "not set: no such object");
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
                const bool done = actor->running && ts2::sim::cancelInteraction(world, *actor, actor->running->id);
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
        world.setAsksMessages(showMessages);
        sim.tick();
        if (const auto* asked = world.messageAsked()) {
            std::printf("tick %zu: message box for %d (picture: neighbor %d, type 0x%08X): [%s] %s", tick, asked->asker, asked->pictureNeighbor,
                        asked->pictureType, asked->title.c_str(), asked->message.c_str());
            for (const std::string& b : asked->buttons) {
                std::printf(" <%s>", b.c_str());
            }
            std::printf("\n");
            world.answerMessage(0);
        }
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
                // Then object data 1 (direction) and data 2-3 (the object holding the Sim and its slot).
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
    // The neighbors Make New Character made this run, with what they were made as.
    for (const std::int16_t id : world.addedNeighbors()) {
        const auto* n = world.neighbor(id);
        if (n == nullptr) {
            continue;
        }
        std::printf("made neighbor %d GUID 0x%08X '%s'", id, n->guid, world.neighborFirstName(id).value_or("").c_str());
        if (n->madeAs) {
            std::printf(" from a template: age %d gender %d skin %d outfit 0x%08X", n->madeAs->age, n->madeAs->gender, n->madeAs->skin,
                        n->madeAs->outfit);
        }
        std::printf("\n");
    }

    std::size_t running = 0;
    world.nextAfter(0, [&](const ts2::sim::SimObject& o) {
        running += o.thread.isRunning();
        return false;
    });
    std::printf("after %zu ticks (%02d:%02d): %zu threads still running, Sim %d selected\n", ticks,
                sim.globals()[ts2::sim::global::kHour], sim.globals()[ts2::sim::global::kMinute], running,
                sim.globals()[ts2::sim::global::kSelectedPerson]);
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
                // The --trace object's whole stack, outermost first.
                if (traceObject && *traceObject == o.id) {
                    for (const auto& frame : o.thread.frames) {
                        std::printf("\n    %s (0x%08X 0x%04X) at %u, stack object %d", frame.code ? frame.code->name.c_str() : "?",
                                    frame.group, frame.instance, frame.pc, frame.stackObjectId);
                    }
                    std::printf("\n   ");
                }
            }
            if (o.resumedClip) {
                std::printf(", resuming %s at %.0f ms", o.resumedClip->name.c_str(), o.resumedClip->playedMs);
            }
            if (o.running) {
                std::printf(", doing \"%s\" (object %d line %u tree 0x%04X link %u/%u)", o.running->name.c_str(),
                            o.running->object, o.running->index, o.running->action, o.running->linkId, o.running->linkedId);
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
    if (saveCompare) {
        compareSave(index, sim, lotPath, *saveCompare);
    }
    if (saveTo) {
        writeLot(sim, lotPath, *saveTo);
    }
    if (saveFolder) {
        writeSaveFolder(sim, install, lotPath, *saveFolder);
    }
    if (upgradeCheck) {
        printUpgradeCheck(lotPath);
    }
    if (savedAnimations) {
        printSavedAnimations(lotPath);
    }
    if (savedQueues) {
        std::printf("saved queues of %s:\n", lotPath.filename().string().c_str());
        printSavedQueues(lotPath);
    }
    if (listObjects) {
        // Every object but the Sims: where it is, its direction (data 1) and its lead (data 0x13).
        world.nextAfter(0, [&](const ts2::sim::SimObject& o) {
            if (!o.isPerson()) {
                std::printf("object %d %s at %.2f %.2f level %d dir %d lead %d\n", o.id, o.definition.name.c_str(), o.x / 65536.0,
                            o.y / 65536.0, o.level, o.data[1], o.data[0x13]);
            }
            return false;
        });
    }
    return 0;
}

// Checks, for each lot given, how the entries of its object type table relate to the installed
// types, to learn how the game fills an entry: its kind, name, flag, second GUID and fifth value.
int checkTypeTables(ResourceIndex& index, int count, char** lots) {
    ts2::sim::IndexCodeSource code(index);
    std::map<std::string, int> tally;
    std::map<std::string, std::string> example;
    std::map<std::size_t, std::set<std::uint16_t>> flagThreeValues, flagZeroValues;
    const auto mark = [&](const std::string& what, bool ok, const std::string& detail) {
        ++tally[what + (ok ? ": yes" : ": no")];
        if (!ok && !example.contains(what)) {
            example[what] = detail;
        }
    };
    for (int i = 0; i < count; ++i) {
        ts2::dbpf::Package package{lots[i]};
        for (const auto& e : package.entries()) {
            if (e.key.type != ts2::types::kObjectTypeTable) {
                continue;
            }
            const auto table = ts2::objt::parse(package.read(e));
            if (table.version != ts2::objt::kWrittenVersion) {
                continue;
            }
            for (const auto& entry : table.entries) {
                const ts2::sim::ObjectType* type = code.objectType(entry.guid);
                if (type == nullptr) {
                    ++tally["not installed"];
                    continue;
                }
                const auto& d = type->definition;
                const auto word = [&](std::size_t w) {
                    return static_cast<std::uint32_t>(entry.unknown[2 * w]) | static_cast<std::uint32_t>(entry.unknown[2 * w + 1]) << 16;
                };
                char detail[256];
                std::snprintf(detail, sizeof detail, "%s 0x%08X: kind %u field9 %u, flag %u, second 0x%08X pair208 0x%08X, word4 %u, name \"%s\" objd \"%s\"",
                              lots[i], entry.guid, entry.kind, d.field(9), entry.flag, entry.secondGuid,
                              static_cast<std::uint32_t>(d.scriptField(208).value_or(0)), word(4), entry.name.c_str(), d.name.c_str());
                mark("kind is OBJD 9", entry.kind == d.field(9), detail);
                mark("second GUID is the pair at 0x1D", entry.secondGuid == static_cast<std::uint32_t>(d.scriptField(208).value_or(0)), detail);
                mark("name is the OBJD name", entry.name == d.name, detail);
                mark("name empty", entry.name.empty(), detail);
                ++tally["flag " + std::to_string(entry.flag)];
                const auto values = ts2::sim::savedTypeValues(code, *type);
                mark("values match", values[0] == word(0) && values[1] == word(1) && values[2] == word(2) && values[3] == word(3), detail);
                // The fifth value: the entries of the type's string table 0x100, if it has one.
                std::size_t strings = 0;
                if (const auto* r = index.find({ts2::types::kStringTable, type->group, 0x100, 0})) {
                    strings = ts2::str::parse(index.read(*r)).forLanguage(1).size();
                }
                mark("word4 is the count of string table 0x100", word(4) == strings, std::string(detail) + " strings " + std::to_string(strings));
                std::size_t semiStrings = 0;
                const std::uint32_t semi = code.semiGlobalGroup(type->group);
                if (const auto* r = semi != 0 ? index.find({ts2::types::kStringTable, semi, 0x100, 0}) : nullptr) {
                    semiStrings = ts2::str::parse(index.read(*r)).forLanguage(1).size();
                }
                mark("word4 is the count of the semi-global's string table 0x100", word(4) == semiStrings,
                     std::string(detail) + " semi strings " + std::to_string(semiStrings));
                std::uint32_t firstNumber = 0;
                if (const auto* r = semi != 0 ? index.find({ts2::types::kStringTable, semi, 0x100, 0}) : nullptr) {
                    const auto texts = ts2::str::parse(index.read(*r)).forLanguage(1);
                    if (!texts.empty()) {
                        firstNumber = static_cast<std::uint32_t>(std::strtoul(texts[0]->value.c_str(), nullptr, 10));
                    }
                }
                mark("word4 is the number in the semi-global's string 0x100 #0", word(4) == firstNumber,
                     std::string(detail) + " first number " + std::to_string(firstNumber));
                // Which OBJD fields set the flag 3 types apart: per field, the values of flag 3 and flag 0 types.
                if (entry.flag == 3 || entry.flag == 0) {
                    for (std::size_t field = 0; field < ts2::objd::kMaxFields; ++field) {
                        auto& seen = entry.flag == 3 ? flagThreeValues[field] : flagZeroValues[field];
                        seen.insert(d.field(field));
                    }
                }
                const bool part = d.field(ts2::objd::field::kMultiTileMasterId) != 0 &&
                                  d.field(ts2::objd::field::kMultiTileSubIndex) != 0xFFFF;
                mark("flag 1 is a multi-tile part", (entry.flag == 1) == part, detail);
                // The whole rule for the flag and the name, as a save would fill a new entry.
                const bool master = d.isMultiTileMaster();
                const std::uint32_t flagNow = part ? 1u : master ? 3u : 0u;
                mark("flag is 1 for a part, 3 for a master, else 0", entry.flag == flagNow, detail);
                const auto catalogLine = code.catalogString(type->group, d.field(ts2::objd::field::kCatalogStringsId), 0);
                const std::string nameNow = part ? std::string() : (catalogLine && !catalogLine->empty()) ? *catalogLine : "*" + d.name;
                mark("name is the catalog name, else * + the OBJD name, none for a part", entry.name == nameNow,
                     std::string(detail) + " expected \"" + nameNow + "\"");
                if (!entry.name.empty() && entry.name[0] == '*') {
                    mark("a starred name is * + the OBJD name", entry.name == "*" + d.name, detail);
                }
                if (word(4) != semiStrings) {
                    std::size_t all = 0;
                    if (const auto* r = semi != 0 ? index.find({ts2::types::kStringTable, semi, 0x100, 0}) : nullptr) {
                        all = ts2::str::parse(index.read(*r)).entries.size();
                    }
                    char key[160];
                    std::snprintf(key, sizeof key, "miss: word4 %u, semi 0x%08X english %zu all %zu, flag %u", word(4), semi,
                                  semiStrings, all, entry.flag);
                    ++tally[key];
                }
                // The catalog strings (CTSS) the OBJD names: their first text, and whether there are any.
                const auto* catalog = index.find({ts2::types::kCatalogString, type->group, d.field(ts2::objd::field::kCatalogStringsId), 0});
                std::string catalogName;
                if (catalog != nullptr) {
                    const auto texts = ts2::str::parse(index.read(*catalog)).forLanguage(1);
                    if (!texts.empty()) {
                        catalogName = texts[0]->value;
                    }
                }
                mark("flag 0 has catalog strings", (entry.flag == 0) == (catalog != nullptr), detail);
                mark("name is the catalog name (flag 0)", entry.flag != 0 || entry.name == catalogName, std::string(detail) + " catalog \"" + catalogName + "\"");
                if (entry.flag == 3 && tally["flag 3 shown"]++ < 4) {
                    std::printf("  flag 3: %s\n", detail);
                }
                if (word(4) != 0 && tally["word4 shown"]++ < 8) {
                    std::printf("  word4 %u for %s (group 0x%08X, catalog strings id %u)\n", word(4), d.name.c_str(), type->group,
                                d.field(ts2::objd::field::kCatalogStringsId));
                }
            }
        }
    }
    for (const auto& [what, n] : tally) {
        std::printf("%6d  %s\n", n, what.c_str());
    }
    for (const auto& [what, line] : example) {
        std::printf("  e.g. not %s: %s\n", what.c_str(), line.c_str());
    }
    for (const auto& [field, values] : flagThreeValues) {
        const auto& others = flagZeroValues[field];
        const bool apart = std::none_of(values.begin(), values.end(), [&](std::uint16_t v) { return others.contains(v); });
        if (apart) {
            std::string list;
            for (const std::uint16_t v : values) {
                list += " " + std::to_string(v);
            }
            std::printf("  OBJD field 0x%02zX sets flag 3 apart: flag 3 values%s, flag 0 has %zu values\n", field, list.c_str(),
                        others.size());
        }
    }
    return 0;
}

// Counts, over the current object records of the lots given, the values of the record fields the
// running object does not fill, to see what a record started without another of its type needs.
int surveyRecordFields(int count, char** lots) {
    std::map<std::string, std::map<std::string, int>> seen;
    for (int i = 0; i < count; ++i) {
        for (const auto& [key, entry] : savedObjectsOf(lots[i])) {
            const auto& [o, kind] = entry;
            const std::string k = kind == ts2::xobj::RecordKind::Person ? "PERS" : kind == ts2::xobj::RecordKind::ObjectWithMaterial ? "XMTO" : "XOBJ";
            const auto& h = o.header;
            char buf[160];
            std::snprintf(buf, sizeof buf, "%d %g %d %d %g", h.value2, h.value3, h.value4, h.value5, h.value6);
            ++seen[k + " header values 2-6"][buf];
            std::snprintf(buf, sizeof buf, "%g %g %g %g", h.reserved[0], h.reserved[1], h.reserved[2], h.reserved[3]);
            ++seen[k + " header reserved"][buf];
            if (o.tail) {
                const auto& t = *o.tail;
                std::snprintf(buf, sizeof buf, "value1 %d value2 %u stack value %d", t.value1, t.value2, t.stackValue);
                ++seen[k + " tail values"][buf];
                std::snprintf(buf, sizeof buf, "relationships v%d, slot marker %d extra %d, effects flag %d v%d", t.relationshipVersion,
                              t.slots.marker, t.slots.extra, t.effectFlag, t.effectVersion);
                ++seen[k + " tail shape"][buf];
                if (kind == ts2::xobj::RecordKind::ObjectWithMaterial) {
                    std::string block;
                    for (const auto b : t.materialBlock) {
                        char x[4];
                        std::snprintf(x, sizeof x, "%02x", b);
                        block += x;
                    }
                    ++seen[k + " material block"][block];
                }
            }
        }
    }
    for (const auto& [what, values] : seen) {
        std::vector<std::pair<int, std::string>> sorted;
        int total = 0;
        for (const auto& [v, n] : values) {
            sorted.emplace_back(n, v);
            total += n;
        }
        std::sort(sorted.rbegin(), sorted.rend());
        std::printf("%s: %zu values over %d records\n", what.c_str(), values.size(), total);
        for (std::size_t j = 0; j < sorted.size() && j < 5; ++j) {
            std::printf("  %6d  %s\n", sorted[j].first, sorted[j].second.c_str());
        }
    }
    return 0;
}

// For each "line:tree" given, the interaction table lines of the lot's object types (and the Sims')
// with that string index and tree: the type, and the line's flags and other values. Used to see
// which saved interaction flags come from the line.
int printLineFlags(ResourceIndex& index, const char* lot, int count, char** wanted) {
    ts2::sim::IndexCodeSource code(index);
    const ts2::content::Lot read = ts2::content::loadLot(lot);
    for (int w = 0; w < count; ++w) {
        const std::string_view text = wanted[w];
        const auto colon = text.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        const auto line = static_cast<std::uint32_t>(std::stoul(std::string(text.substr(0, colon))));
        const auto tree = static_cast<std::uint32_t>(std::stoul(std::string(text.substr(colon + 1)), nullptr, 16));
        int found = 0;
        for (const auto& entry : read.types.entries) {
            const ts2::sim::ObjectType* type = code.objectType(entry.guid);
            if (type == nullptr) {
                continue;
            }
            std::uint16_t tableId = type->definition.field(ts2::objd::field::kInteractionTableId);
            if (tableId == 0 || tableId >= 0x8000) {
                continue;
            }
            auto table = code.interactionTable(type->group, tableId);
            if (!table) {
                table = code.interactionTable(code.semiGlobalGroup(type->group), tableId);
            }
            if (!table) {
                continue;
            }
            const ts2::ttab::Interaction* i = ts2::ttab::find(*table, line);
            if (i == nullptr || i->action != tree) {
                continue;
            }
            ++found;
            std::printf("line %u tree 0x%04X: %-40s flags 0x%08X attenuation %u %g autonomy %u joining %d\n", line, tree,
                        type->definition.name.c_str(), i->flags, i->attenuationCode, i->attenuation, i->autonomy, i->joiningIndex);
        }
        if (found == 0) {
            std::printf("line %u tree 0x%04X: no type of the lot has it\n", line, tree);
        }
    }
    return 0;
}

// Checks over each lot's current object records how many attributes and semi-global attributes a
// record holds against the object's definition and data.
int checkAttributeCounts(ResourceIndex& index, int count, char** lots) {
    ts2::sim::IndexCodeSource code(index);
    std::map<std::string, int> tally;
    for (int i = 0; i < count; ++i) {
        const ts2::content::Lot read = ts2::content::loadLot(lots[i]);
        for (const auto& object : read.objects) {
            if (!object.type) {
                continue;
            }
            const ts2::sim::ObjectType* type = code.objectType(object.type->guid);
            if (type == nullptr) {
                continue;
            }
            const std::size_t defined = type->definition.field(ts2::objd::field::kNumAttributes);
            const std::size_t attributes = object.state.attributes.size();
            ++tally[attributes == defined ? "attributes = OBJD 0x3A" : "attributes != OBJD 0x3A"];
            ++tally[attributes == std::max<std::size_t>(defined, 8) ? "attributes = max(8, OBJD 0x3A)" : "attributes != max(8, OBJD 0x3A)"];
            if (attributes != std::max<std::size_t>(defined, 8)) {
                ++tally["  attributes " + std::to_string(attributes) + " with OBJD 0x3A " + std::to_string(defined)];
            }
            // Each saved stack frame: its first value against the object's id, and how many parameters
            // it saves against its tree's own count.
            if (object.state.tail) {
                for (const auto& frame : object.state.tail->stack) {
                    ++tally[frame.value1 == static_cast<std::int16_t>(object.id) ? "frame value1 = the object's id: yes"
                                                                                 : "frame value1 = the object's id: no"];
                    const ts2::objt::Entry* ownerEntry = read.types.find(static_cast<std::uint16_t>(frame.codeOwner));
                    const ts2::sim::ObjectType* owner = ownerEntry != nullptr ? code.objectType(ownerEntry->guid) : nullptr;
                    if (owner == nullptr) {
                        continue;
                    }
                    const auto tree = static_cast<std::uint16_t>(frame.treeId);
                    const std::uint32_t group = tree < 0x1000 ? ts2::sim::kGlobalGroup : tree < 0x2000 ? owner->group : code.semiGlobalGroup(owner->group);
                    const auto behavior = code.behavior(group, tree);
                    if (behavior) {
                        ++tally[frame.params.size() == behavior->argCount ? "frame params = the tree's argument count: yes"
                                                                          : "frame params = the tree's argument count: no"];
                    }
                }
            }
            const std::size_t semi = object.state.semiAttributes.size();
            const std::size_t fifth = static_cast<std::size_t>(object.type->unknown[8]) | static_cast<std::size_t>(object.type->unknown[9]) << 16;
            ++tally[semi == fifth ? "semi = the entry's fifth value" : "semi != the entry's fifth value"];
            std::size_t labels = 0;
            const std::uint32_t semiGroup = code.semiGlobalGroup(type->group);
            if (const auto* r = semiGroup != 0 ? index.find({ts2::types::kStringTable, semiGroup, 0x100, 0}) : nullptr) {
                labels = ts2::str::parse(index.read(*r)).forLanguage(1).size();
            }
            ++tally[semi == labels ? "semi = the semi-global's string table 0x100" : "semi != the semi-global's string table 0x100"];
            const std::size_t rule = semiGroup != type->group ? labels : 0;
            ++tally[semi == rule ? "semi = labels of a semi-global, else 0: yes" : "semi = labels of a semi-global, else 0: no"];
            ++tally[fifth == rule ? "fifth = labels of a semi-global, else 0: yes" : "fifth = labels of a semi-global, else 0: no"];
            if (semi != labels) {
                ++tally["  semi " + std::to_string(semi) + " with labels " + std::to_string(labels) + " fifth " + std::to_string(fifth)];
                char group[24];
                std::snprintf(group, sizeof group, " 0x%08X", semiGroup);
                ++tally["  type " + type->definition.name + ", semi-global" + group];
            } else if (semi != 0) {
                char group[24];
                std::snprintf(group, sizeof group, " 0x%08X", semiGroup);
                ++tally["  ok type " + type->definition.name + ", semi-global" + group];
            }
            const auto semiData = static_cast<std::size_t>(static_cast<std::uint16_t>(object.state.data[0x4F]));
            ++tally[semi == semiData ? "semi = data 0x4F" : "semi != data 0x4F"];
            if (semi != semiData) {
                ++tally["  semi " + std::to_string(semi) + " with data 0x4F " + std::to_string(semiData)];
            }
        }
    }
    for (const auto& [what, n] : tally) {
        std::printf("%6d  %s\n", n, what.c_str());
    }
    return 0;
}

// Counts, over the Sims' current records in the lots given, the person state fields a save keeps
// from the loaded record: the picked interaction's picture, the eight values after it, the counts
// and the text, and the second motive set's zero pattern.
int surveyPersonExtras(int count, char** lots) {
    std::map<std::string, int> tally;
    for (int l = 0; l < count; ++l) {
        for (const auto& [key, entry] : savedObjectsOf(lots[l])) {
            const auto& [o, kind] = entry;
            if (!o.person || o.person->stateUnread) {
                continue;
            }
            const auto& s = o.person->state;
            char buf[256];
            std::snprintf(buf, sizeof buf, "picked %d %d %s", s.pickedIconIndex, s.pickedIconGuid, s.pickedIconObject == 0 ? "0" : s.pickedIconObject == 16711935 ? "0xFF00FF" : "object");
            ++tally[buf];
            std::string values = "values";
            for (const auto v : s.values) {
                values += v == 0 ? " 0" : v == 255 ? " 255" : " x";
            }
            ++tally[values];
            std::snprintf(buf, sizeof buf, "runs %zu entries %zu sub start %u value1 %u value2 %d listed %zu value3 %u value4 %u text %s",
                          s.runs.size(), s.entries.size(), s.subStartCount, s.value1, s.value2, s.list.size(), s.value3, s.value4,
                          s.text.empty() ? "empty" : "set");
            ++tally[buf];
            std::string motives = "motives2";
            for (const float m : o.person->motives2) {
                motives += m == 0 ? " 0" : " x";
            }
            ++tally[motives];
        }
    }
    for (const auto& [what, n] : tally) {
        std::printf("%6d  %s\n", n, what.c_str());
    }
    return 0;
}

// Counts, over the Sims' current records in the lots given, what their running, interrupted and sub
// interactions hold: blank (number 0) or not, every field of the blank ones, and the lines saved.
int surveyPersonStates(int count, char** lots) {
    std::map<std::string, int> tally;
    const auto describe = [](const ts2::xobj::SavedInteraction& i) {
        char buf[320];
        std::snprintf(buf, sizeof buf,
                      "v1 %d n %d actor %d target %d icon %d v2 %u index %d values %u %u %u %u prio %d tree %u v5 %g flags %d "
                      "lines %u %u %u id %d link %d name \"%s\" iconGuid %d iconIndex %d v6 %u",
                      i.value1, i.number, i.actor, i.target, i.iconObject, i.value2, i.index, i.values[0], i.values[1], i.values[2],
                      i.values[3], i.priority, i.tree, i.value5, i.flags, i.lineFlag3, i.lineFlag1, i.lineFlag2, i.id, i.linkId,
                      i.name.c_str(), i.iconGuid, i.iconIndex, i.value6);
        return std::string(buf);
    };
    for (int l = 0; l < count; ++l) {
        for (const auto& [key, entry] : savedObjectsOf(lots[l])) {
            const auto& [o, kind] = entry;
            if (!o.person || o.person->stateUnread) {
                continue;
            }
            const auto& s = o.person->state;
            ++tally[s.running.number == 0 ? "running blank" : "running set"];
            if (s.running.number == 0) {
                ++tally["  blank running: " + describe(s.running)];
            }
            ++tally[s.interrupted.number == 0 ? "interrupted blank" : "interrupted set"];
            ++tally[!s.runningSub ? "no running sub" : s.runningSub->number == 0 ? "running sub blank" : "running sub set"];
            if (s.runningSub && s.runningSub->number == 0) {
                ++tally["  blank sub: " + describe(*s.runningSub)];
            }
            for (const auto* i : {&s.running, &s.interrupted}) {
                ++tally["line " + std::to_string(i->index)];
            }
            // The fields of the interactions that are not blank, which the engine does not set.
            const auto setFields = [&](const ts2::xobj::SavedInteraction& i) {
                if (i.number != 0) {
                    ++tally["set flags " + std::to_string(i.flags) + " v1 " + std::to_string(i.value1) + " v2 " +
                            std::to_string(i.value2) + " lines " + std::to_string(i.lineFlag1) + "/" + std::to_string(i.lineFlag3) +
                            " v6 " + std::to_string(i.value6)];
                }
            };
            setFields(s.running);
            setFields(s.interrupted);
            for (const auto& q : s.queue) {
                setFields(q);
            }
            // Each interaction's flags by where it is and its priority, to tell what the game sets
            // when it makes one (a queued one has not started) from what running adds.
            const auto byPlace = [&](const char* place, const ts2::xobj::SavedInteraction& i) {
                if (i.number != 0) {
                    char key[96];
                    std::snprintf(key, sizeof key, "flags %s 0x%04X priority %d line %d", place, static_cast<unsigned>(i.flags),
                                  i.priority, i.index);
                    ++tally[key];
                }
            };
            byPlace("running", s.running);
            byPlace("interrupted", s.interrupted);
            for (const auto& q : s.queue) {
                byPlace("queued", q);
            }
            if (s.runningSub) {
                byPlace("sub", *s.runningSub);
            }
            for (const auto& q : s.subQueue) {
                byPlace("subqueued", q);
            }
            for (const auto& q : s.queue) {
                ++tally["queued line " + std::to_string(q.index)];
            }
            // One Sim's interaction numbers: running, the queue, interrupted, the sub ones.
            std::string numbers = "  numbers " + std::to_string(key.instance) + ": running " + std::to_string(s.running.number) +
                                  " | queue";
            for (const auto& q : s.queue) {
                numbers += " " + std::to_string(q.number);
            }
            numbers += " | interrupted " + std::to_string(s.interrupted.number);
            if (s.runningSub) {
                numbers += " | sub " + std::to_string(s.runningSub->number);
            }
            for (const auto& q : s.subQueue) {
                numbers += " " + std::to_string(q.number);
            }
            ++tally[numbers];
        }
    }
    for (const auto& [what, n] : tally) {
        std::printf("%6d  %s\n", n, what.c_str());
    }
    return 0;
}

// For each Sim saved on a lot, lists the person data values that differ from its Sim description
// in the neighborhood package, matched as the description sharing the most values.
int compareSimDescriptions(const char* lot, const char* hood) {
    std::vector<ts2::sdsc::SimDescription> descriptions;
    ts2::dbpf::Package package{hood};
    for (const auto& e : package.entries()) {
        if (e.key.type == ts2::sdsc::kType) {
            try {
                descriptions.push_back(ts2::sdsc::parse(package.read(e)));
            } catch (const std::exception&) {
            }
        }
    }
    for (const auto& [key, entry] : savedObjectsOf(lot)) {
        const auto& person = entry.first.person;
        if (!person) {
            continue;
        }
        const ts2::sdsc::SimDescription* best = nullptr;
        std::size_t bestSame = 0;
        for (const auto& d : descriptions) {
            std::size_t same = 0;
            for (std::size_t i = 0; i < std::min(d.personData.size(), person->personData.size()); ++i) {
                same += d.personData[i] == person->personData[i];
            }
            if (same > bestSame) {
                bestSame = same;
                best = &d;
            }
        }
        if (best == nullptr) {
            continue;
        }
        // Person data 0x6D, the current interaction's id, changes all the time, so a description
        // with the same non-zero value was written by the save that wrote the lot.
        constexpr std::size_t kCurrentInteractionId = 0x6D;
        const bool sameSave = person->personData.size() > kCurrentInteractionId && best->personData.size() > kCurrentInteractionId &&
                              person->personData[kCurrentInteractionId] != 0 &&
                              person->personData[kCurrentInteractionId] == best->personData[kCurrentInteractionId];
        std::printf("object %u neighbor %d: %zu of %zu values the same, lot %zu values%s\n", key.instance, best->neighborId, bestSame,
                    best->personData.size(), person->personData.size(), sameSave ? ", same save" : "");
        for (std::size_t i = 0; i < std::min(best->personData.size(), person->personData.size()); ++i) {
            if (best->personData[i] != person->personData[i]) {
                std::printf("  0x%02zX lot %d description %d\n", i, person->personData[i], best->personData[i]);
            }
        }
    }
    return 0;
}

// Remakes the records a game save added or changed for Sims made during play (descriptions, DNA,
// the DNA index, family members, family ties and the neighbor counter) from the neighborhood as it
// was before the save, given the made Sims, their families and ties as the game saved them, and
// compares each with the game's bytes. A description's relationship list is not compared.
int checkNewNeighborRecords(const char* before, const char* after) {
    const auto readFile = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const auto bodyOf = [](const ts2::dbpf::StoredResource& r) { return r.compressed ? ts2::qfs::decompress(r.bytes) : r.bytes; };
    ts2::dbpf::PackageImage was = ts2::dbpf::readImage(readFile(before));
    const ts2::dbpf::PackageImage now = ts2::dbpf::readImage(readFile(after));
    using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>;
    const auto keyOf = [](const ts2::dbpf::ResourceKey& k) { return Key{k.type, k.group, k.instance, k.instanceHigh}; };
    std::map<Key, std::vector<std::uint8_t>> gameBodies;
    for (const auto& r : now.resources) {
        gameBodies[keyOf(r.key)] = bodyOf(r);
    }
    auto hood = std::make_shared<ts2::sim::SavedNeighborhood>();
    std::set<std::int16_t> made;
    for (const auto& r : was.resources) {
        const auto body = bodyOf(r);
        if (r.key.type == ts2::sdsc::kType) {
            hood->neighborIds.insert(static_cast<std::int16_t>(r.key.instance));
            hood->neighbors[static_cast<std::int16_t>(r.key.instance)] = {};
        } else if (r.key.type == ts2::fami::kType) {
            const ts2::fami::Family f = ts2::fami::parse(body);
            hood->familyLots[r.key.instance] = f.lot;
            hood->familyMembers[r.key.instance] = f.members;
        } else if (r.key.type == ts2::famt::kType) {
            for (auto& s : ts2::famt::parse(body).sims) {
                hood->familyTies[s.sim] = s.ties;
            }
        } else if (r.key.type == ts2::lotrecords::kShortRecordType) {
            if (const auto counter = ts2::lotrecords::readShortRecord(body)) {
                hood->nextNeighborId = static_cast<std::int16_t>(*counter);
            }
        }
    }
    std::map<std::int16_t, ts2::sdsc::Record> madeRecords;
    for (const auto& [key, body] : gameBodies) {
        const auto [type, group, instance, instance2] = key;
        if (type == ts2::sdsc::kType && !hood->neighborIds.contains(static_cast<std::int16_t>(instance))) {
            if (auto record = ts2::sdsc::readRecord(body)) {
                madeRecords.emplace(static_cast<std::int16_t>(instance), std::move(*record));
            }
        }
    }
    ts2::sim::World world;
    world.setNeighborhood(hood);
    for (const auto& [id, record] : madeRecords) {
        ts2::sim::SavedNeighbor neighbor;
        neighbor.guid = record.guid;
        neighbor.personData.assign(record.personData.begin(), record.personData.end());
        if (const auto dna = gameBodies.find(Key{ts2::types::kSimDna, 0xFFFFFFFF, static_cast<std::uint32_t>(id), 0}); dna != gameBodies.end()) {
            neighbor.genes = ts2::sim::genomeFromProperties(ts2::propset::parse(dna->second));
        }
        const auto given = world.addNeighbor(std::move(neighbor));
        std::printf("made neighbor 0x%X: given id 0x%X\n", id, given ? *given : 0);
        made.insert(id);
    }
    for (const auto& [key, body] : gameBodies) {
        const auto [type, group, instance, instance2] = key;
        if (type == ts2::fami::kType && hood->familyMembers.contains(instance)) {
            for (const std::uint32_t guid : ts2::fami::parse(body).members) {
                world.addFamilyMember(instance, guid);
            }
        } else if (type == ts2::famt::kType) {
            for (const auto& s : ts2::famt::parse(body).sims) {
                if (made.contains(s.sim)) {
                    for (const auto& tie : s.ties) {
                        if (tie.type <= 1) {
                            world.setFamilyTie(tie.other, s.sim, static_cast<ts2::famt::TieType>(tie.type));
                        }
                    }
                }
            }
        }
    }
    const ts2::sim::HoodSaveCounts counts = ts2::sim::fillNeighborhoodPackage(was, {}, 0, world, {}, ts2::sim::NewNeighbors::Written);
    std::printf("added %d descriptions, %d DNA, %d family member lists, ties %s, counter %s\n", counts.newNeighbors, counts.newDna,
                counts.familyMembers, counts.familyTies ? "rewritten" : "kept", counts.neighborCounter ? "rewritten" : "kept");
    int differ = 0;
    for (const auto& r : was.resources) {
        const auto game = gameBodies.find(keyOf(r.key));
        const bool wanted = r.key.type == ts2::types::kSimDna || r.key.type == ts2::lotrecords::kDnaIndexType ||
                            r.key.type == ts2::fami::kType || r.key.type == ts2::famt::kType ||
                            r.key.type == ts2::lotrecords::kShortRecordType ||
                            (r.key.type == ts2::sdsc::kType && made.contains(static_cast<std::int16_t>(r.key.instance)));
        if (!wanted || game == gameBodies.end()) {
            continue;
        }
        std::vector<std::uint8_t> ours = bodyOf(r);
        std::vector<std::uint8_t> theirs = game->second;
        if (r.key.type == ts2::sdsc::kType) {
            auto a = ts2::sdsc::readRecord(ours), b = ts2::sdsc::readRecord(theirs);
            if (a && b) {
                a->relationships = b->relationships;
                ours = ts2::sdsc::writeRecord(*a);
            }
        }
        if (ours != theirs) {
            std::printf("  %08X:%X differs (ours %zu bytes, game %zu)\n", r.key.type, r.key.instance, ours.size(), theirs.size());
            ++differ;
        }
    }
    std::printf("%d of the records compared differ\n", differ);
    return differ == 0 ? 0 : 1;
}

// Checks the neighborhood save rules against a save the game made: each Sim the saved lot holds,
// applied to its description in `before`, should give its description in `after`, and the lot's
// description in `before` with the saved lot's info block should give the one in `after`.
int checkHoodSave(const char* lot, const char* before, const char* after, std::uint32_t lotId) {
    const auto descriptions = [](const char* path) {
        std::map<std::int16_t, ts2::sdsc::Record> out;
        ts2::dbpf::Package package{path};
        for (const auto& e : package.entries()) {
            if (e.key.type == ts2::sdsc::kType) {
                if (auto r = ts2::sdsc::readRecord(package.read(e))) {
                    out.emplace(static_cast<std::int16_t>(e.key.instance), std::move(*r));
                }
            }
        }
        return out;
    };
    const auto was = descriptions(before), now = descriptions(after);
    int same = 0, differ = 0;
    constexpr std::size_t kNeighborId = 0x1F;
    for (const auto& [key, entry] : savedObjectsOf(lot)) {
        const auto& person = entry.first.person;
        if (!person || person->personData.size() <= kNeighborId) {
            continue;
        }
        const std::int16_t id = person->personData[kNeighborId];
        const auto a = was.find(id), b = now.find(id);
        if (a == was.end() || b == now.end()) {
            continue;
        }
        const auto made = ts2::sim::savedDescriptionPersonData(a->second.personData, person->personData);
        std::printf("neighbor %d:", id);
        bool ok = true;
        for (std::size_t i = 0; i < made.size(); ++i) {
            if (made[i] != b->second.personData[i]) {
                std::printf(" 0x%02zX made %d game %d", i, made[i], b->second.personData[i]);
                ok = false;
            }
        }
        std::printf(ok ? " person data the same\n" : "\n");
        (ok ? same : differ)++;
    }
    std::printf("%d descriptions the same, %d differ\n", same, differ);
    std::optional<ts2::nhood::LotInfoRecord> info;
    {
        ts2::dbpf::Package package{lot};
        for (const auto& e : package.entries()) {
            if (e.key.type == ts2::nhood::kLotInfoType) {
                info = ts2::nhood::readLotInfo(package.read(e));
            }
        }
    }
    const auto lotDescription = [&](const char* path) -> std::optional<std::vector<std::uint8_t>> {
        ts2::dbpf::Package package{path};
        for (const auto& e : package.entries()) {
            if (e.key.type == ts2::nhood::kLotDescriptionType && e.key.instance == lotId) {
                return package.read(e);
            }
        }
        return std::nullopt;
    };
    const auto oldBytes = lotDescription(before), newBytes = lotDescription(after);
    if (info && oldBytes && newBytes) {
        if (auto record = ts2::nhood::readLotDescriptionRecord(*oldBytes)) {
            record->own = *info;
            std::printf("lot description %s\n", ts2::nhood::writeLotDescriptionRecord(*record) == *newBytes ? "the same" : "differs");
        }
    }
    return differ == 0 ? 0 : 1;
}

// Lists an object's scene record (0xFA1C39F7) in a lot: its model, materials, place, pose, blends,
// animations and locomotion sets.
int printSceneRecord(const char* lot, std::uint32_t id) {
    ts2::dbpf::Package package{lot};
    for (const auto& e : package.entries()) {
        if (e.key.type != ts2::types::kSceneObject || e.key.instance != id) {
            continue;
        }
        const auto record = ts2::cobject::readRecord(package.read(e));
        if (!record) {
            std::printf("scene record %u doesn't read\n", id);
            return 1;
        }
        const auto f = [](ts2::cobject::Word w) { float v; std::memcpy(&v, &w, 4); return v; };
        const auto& o = record->object;
        std::printf("scene record %u: chain %d, model '%s', place %g %g %g / %g %g %g %g\n", id, static_cast<int>(record->chain),
                    o.model.c_str(), f(o.place[0]), f(o.place[1]), f(o.place[2]), f(o.place[3]), f(o.place[4]), f(o.place[5]), f(o.place[6]));
        for (const auto& m : o.materials) {
            std::printf("  node '%s' value %u:", m.node.c_str(), m.value);
            for (const auto& [subset, material] : m.subsets) {
                std::printf(" %s=%s", subset.c_str(), material.c_str());
            }
            std::printf("\n");
        }
        std::printf("  %zu joints:", o.joints.size());
        for (const auto& j : o.joints) {
            std::printf("\n    %s %g %g %g / %g %g %g %g", j.name.c_str(), f(j.place[0]), f(j.place[1]), f(j.place[2]), f(j.place[3]),
                        f(j.place[4]), f(j.place[5]), f(j.place[6]));
        }
        std::printf("\n  %zu blends:", o.blends.size());
        for (const auto& b : o.blends) {
            std::printf(" %s/%s=%g", b.first.c_str(), b.second.c_str(), f(b.weight));
        }
        const auto& a = record->animatable;
        std::printf("\n  scale %g, %s places", f(a.scale), a.places ? "two" : "no");
        if (a.places) {
            for (std::size_t k = 0; k < a.places->size(); ++k) {
                std::printf("%s%g", k == 0 ? " (" : k == 7 ? " | " : " ", f((*a.places)[k]));
            }
            std::printf(")");
        }
        std::printf(", settings");
        for (const auto s : a.settings) {
            std::printf(" %08X", s);
        }
        std::printf(", place %g %g %g / %g %g %g %g\n", f(a.place[0]), f(a.place[1]), f(a.place[2]), f(a.place[3]), f(a.place[4]),
                    f(a.place[5]), f(a.place[6]));
        for (const auto& an : a.animations) {
            std::printf("  animation kind %u '%s' type '%s' resource '%s' state %d layer %d plays %d\n", static_cast<unsigned>(an.kind),
                        an.request.name.c_str(), an.request.type.c_str(), an.resource.c_str(), an.instance.state, an.instance.layer,
                        an.instance.plays);
            std::printf("    request handle %08X, IK object %d, words3 %08X %08X, event tree 0x%04X object %d type 0x%08X\n", an.request.flags,
                        static_cast<std::int16_t>(an.request.words1[5]), an.request.words3[0], an.request.words3[1], an.request.eventTree,
                        an.request.eventObject, an.request.eventObjectGuid);
            const auto& in = an.instance;
            std::printf("    ticks start %d stop %u saved %d, time %g (0x%08X), word1 %08X word3 %08X word6 %08X weight %g rate %g, %s curve, words5",
                        in.startTick, in.stopTick, in.savedTick, f(in.time), in.time, in.word1, in.word3, in.word6, f(in.weight),
                        f(an.request.rate), in.curve ? "a" : "no");
            for (const auto w : in.words5) {
                std::printf(" %08X", w);
            }
            std::printf("\n");
            if (in.curve) {
                std::printf("    curve value %d word %08X:", in.curve->value, in.curve->word);
                for (const auto& s : in.curve->segments) {
                    std::printf(" [kind %d %g %g length %g]", s.kind, f(s.values[0]), f(s.values[1]), f(s.length));
                }
                std::printf("\n");
            }
        }
        std::printf("  locomotion value %u, %zu sets:", record->locomotable.value, record->locomotable.sets.size());
        for (const auto& s : record->locomotable.sets) {
            std::printf(" [%u: %zu]", s.id, s.weights.size());
        }
        std::printf("\n");
        return 0;
    }
    std::printf("no scene record %u\n", id);
    return 1;
}

// Counts, over the Sims' scene records (chain Person) in the lots given, how many joints, material
// nodes, blends, animations and locomotion sets they hold, and their settings and locomotion value.
int surveySimScenes(int count, char** lots) {
    std::map<std::string, int> tally;
    for (int l = 0; l < count; ++l) {
        try {
            ts2::dbpf::Package package{lots[l]};
            for (const auto& e : package.entries()) {
                if (e.key.type != ts2::types::kSceneObject) {
                    continue;
                }
                const auto record = ts2::cobject::readRecord(package.read(e));
                if (!record) {
                    continue;
                }
                for (const auto& an : record->animatable.animations) {
                    if (an.kind == ts2::cobject::Animation::Kind::Clip) {
                        ++tally[an.resource == an.request.name + "_anim" ? "clip resource = name_anim" : "clip resource other: " + an.request.name + " -> " + an.resource];
                        if (const auto at = an.request.name.find("-blinkUnderlay"); at != std::string::npos) {
                            ++tally["blink clip prefix " + an.request.name.substr(0, at) + " on a " + an.request.type + " entry"];
                        }
                        const float time = std::bit_cast<float>(an.instance.time);
                        const auto elapsed = static_cast<float>(an.instance.savedTick - an.instance.startTick);
                        ++tally[std::string("clip time ") + (time == 0 ? "0" : time == elapsed ? "= saved - start" : time < elapsed ? "< saved - start" : "> saved - start") +
                                (an.instance.plays < 0 ? " (plays until stopped)" : "")];
                    }
                }
                if (record->chain != ts2::cobject::Chain::Person) {
                    continue;
                }
                const auto& o = record->object;
                std::size_t subsets = 0;
                for (const auto& m : o.materials) {
                    subsets += m.subsets.size();
                }
                ++tally["joints " + std::string(o.joints.empty() ? "none" : "some") + ", material nodes " +
                        std::string(o.materials.empty() ? "none" : "some") + ", subsets " + std::string(subsets == 0 ? "none" : "some") +
                        ", blends " + std::string(o.blends.empty() ? "none" : "some")];
                ++tally["model '" + o.model + "' places " + (record->animatable.places ? "two" : "none") + " locomotion value " +
                        std::to_string(record->locomotable.value) + " sets " + std::to_string(record->locomotable.sets.size())];
                ++tally["animations " + std::to_string(std::min<std::size_t>(record->animatable.animations.size(), 9))];
            }
        } catch (const std::exception&) {
        }
    }
    for (const auto& [what, n] : tally) {
        std::printf("%6d  %s\n", n, what.c_str());
    }
    return 0;
}

// Compares, for each Sim standing on a lot the game saved (in no slot), the scene place the engine
// gives it with the object place and the animated place of its saved scene record.
int checkSimPlaces(const std::filesystem::path& install, const std::filesystem::path& lotPath) {
    ts2::sim::LotSimulation sim(install, lotPath);
    ts2::dbpf::Package package{lotPath};
    std::map<std::uint32_t, ts2::cobject::Record> scenes;
    for (const auto& e : package.entries()) {
        if (e.key.type == ts2::types::kSceneObject) {
            if (auto r = ts2::cobject::readRecord(package.read(e))) {
                scenes.emplace(e.key.instance, std::move(*r));
            }
        }
    }
    const auto f = [](ts2::cobject::Word w) { return std::bit_cast<float>(w); };
    const auto show = [&](const char* what, const ts2::cobject::Place& p) {
        std::printf("    %-8s %g %g %g / %g %g %g %g\n", what, f(p[0]), f(p[1]), f(p[2]), f(p[3]), f(p[4]), f(p[5]), f(p[6]));
    };
    constexpr std::size_t kContainerField = 2;
    sim.world().nextAfter(0, [&](const ts2::sim::SimObject& o) {
        if (!o.isPerson() || o.data[kContainerField] > 0 || o.x == ts2::sim::kOffLot) {
            return false;
        }
        const float x = static_cast<float>(o.y) / 65536.0f, y = static_cast<float>(o.x) / 65536.0f;
        const auto ours = ts2::sim::standingScenePlace(o, sim.floorHeight(o.level, x, y));
        std::printf("Sim %d level %d:\n", o.id, o.level);
        show("ours", ours);
        if (const auto it = scenes.find(static_cast<std::uint32_t>(o.id)); it != scenes.end()) {
            show("object", it->second.object.place);
            show("animated", it->second.animatable.place);
        }
        return false;
    });
    return 0;
}

// Lists where two objects' saved records differ: object data, person data, attributes, semi
// attributes and temps, by index.
int diffObjects(const char* lot, std::uint32_t a, std::uint32_t b, const char* otherLot) {
    std::optional<ts2::xobj::SavedObject> first, second;
    for (const auto& [key, entry] : savedObjectsOf(lot)) {
        if (key.instance == a) {
            first = entry.first;
        } else if (key.instance == b && otherLot == nullptr) {
            second = entry.first;
        }
    }
    if (otherLot != nullptr) {
        for (const auto& [key, entry] : savedObjectsOf(otherLot)) {
            if (key.instance == b) {
                second = entry.first;
            }
        }
    }
    if (!first || !second) {
        std::printf("a record is missing\n");
        return 1;
    }
    const auto diff = [](const char* what, const auto& x, const auto& y) {
        const std::size_t n = std::max(x.size(), y.size());
        for (std::size_t i = 0; i < n; ++i) {
            const int vx = i < x.size() ? x[i] : 0, vy = i < y.size() ? y[i] : 0;
            if (vx != vy) {
                std::printf("  %s 0x%02zX: %d vs %d\n", what, i, vx, vy);
            }
        }
    };
    diff("data", first->data, second->data);
    diff("attribute", first->attributes, second->attributes);
    diff("semi", first->semiAttributes, second->semiAttributes);
    if (first->person && second->person) {
        diff("person", first->person->personData, second->person->personData);
        for (std::size_t i = 0; i < first->person->motives.size(); ++i) {
            if (first->person->motives[i] != second->person->motives[i]) {
                std::printf("  motive %zu: %g vs %g\n", i, first->person->motives[i], second->person->motives[i]);
            }
        }
    }
    return 0;
}

// Counts, over the saved stack frames in the lots given, how their parameter and local counts
// compare with what their trees declare.
int surveyFrameShapes(ResourceIndex& index, int count, char** lots) {
    ts2::sim::IndexCodeSource code(index);
    std::map<std::string, int> tally;
    for (int l = 0; l < count; ++l) {
        std::optional<ts2::objt::ObjectTypeTable> types;
        ts2::dbpf::Package package{lots[l]};
        for (const auto& e : package.entries()) {
            if (e.key.type == ts2::types::kObjectTypeTable) {
                try {
                    types = ts2::objt::parse(package.read(e));
                } catch (const std::exception&) {
                }
            }
        }
        if (!types) {
            continue;
        }
        for (const auto& [key, entry] : savedObjectsOf(lots[l])) {
            const auto& o = entry.first;
            for (const auto& f : o.tail ? o.tail->stack : std::vector<ts2::xobj::StackFrame>{}) {
                const auto* owner = types->find(static_cast<std::uint16_t>(f.codeOwner));
                const auto* type = owner != nullptr ? code.objectType(owner->guid) : nullptr;
                if (type == nullptr) {
                    ++tally["owner not found"];
                    continue;
                }
                const auto tree = static_cast<std::uint16_t>(f.treeId);
                const std::uint32_t group = tree < 0x1000 ? ts2::sim::kGlobalGroup : tree < 0x2000 ? type->group : code.semiGlobalGroup(type->group);
                const auto behavior = code.behavior(group, tree);
                if (!behavior) {
                    ++tally["tree not found"];
                    continue;
                }
                const auto compare = [](std::size_t saved, std::size_t declared) {
                    return saved == declared ? std::string("=") : saved < declared ? std::string("<") : std::string(">");
                };
                ++tally["params " + compare(f.params.size(), behavior->argCount) + " args, locals " + compare(f.locals.size(), behavior->localCount) +
                        " declared"];
            }
        }
    }
    for (const auto& [what, n] : tally) {
        std::printf("%7d  %s\n", n, what.c_str());
    }
    return 0;
}

// Counts, over every object on a lot the game saved that stands in no slot, whether its scene
// record's place is where the engine would put it: its position with x and y swapped, the floor
// height there and its facing (object data 1) about the up axis. Lists the first misses.
int checkObjectPlaces(const std::filesystem::path& install, const std::filesystem::path& lotPath) {
    ts2::sim::LotSimulation sim(install, lotPath);
    ts2::dbpf::Package package{lotPath};
    std::map<std::uint32_t, ts2::cobject::Record> scenes;
    for (const auto& e : package.entries()) {
        if (e.key.type == ts2::types::kSceneObject) {
            if (auto r = ts2::cobject::readRecord(package.read(e))) {
                scenes.emplace(e.key.instance, std::move(*r));
            }
        }
    }
    const auto f = [](ts2::cobject::Word w) { return std::bit_cast<float>(w); };
    std::map<std::string, int> tally;
    int listed = 0;
    constexpr std::size_t kContainerField = 2;
    sim.world().nextAfter(0, [&](const ts2::sim::SimObject& o) {
        const auto it = scenes.find(static_cast<std::uint32_t>(o.id));
        if (it != scenes.end() && o.isPerson() && o.data[kContainerField] > 0) {
            const auto& p = it->second.object.place;
            const auto& a = it->second.animatable.place;
            std::printf("  Sim %d in slot %d of object %d, at %g %g level %d: place %g %g %g / %g %g %g %g, animated %g %g %g / %g %g %g %g\n", o.id,
                        o.data[3], o.data[kContainerField], static_cast<float>(o.x) / 65536.0f, static_cast<float>(o.y) / 65536.0f, o.level, f(p[0]),
                        f(p[1]), f(p[2]), f(p[3]), f(p[4]), f(p[5]), f(p[6]), f(a[0]), f(a[1]), f(a[2]), f(a[3]), f(a[4]), f(a[5]), f(a[6]));
        }
        if (it == scenes.end() || o.data[kContainerField] > 0 || o.x == ts2::sim::kOffLot) {
            return false;
        }
        const float x = static_cast<float>(o.y) / 65536.0f, y = static_cast<float>(o.x) / 65536.0f;
        const auto ours = ts2::sim::standingScenePlace(o, sim.floorHeight(o.level, x, y));
        const auto& game = it->second.object.place;
        const bool xy = std::abs(f(ours[0]) - f(game[0])) < 0.01f && std::abs(f(ours[1]) - f(game[1])) < 0.01f;
        const bool z = std::abs(f(ours[2]) - f(game[2])) < 0.01f;
        // The same turn either way round.
        const float dot = f(ours[3]) * f(game[3]) + f(ours[4]) * f(game[4]) + f(ours[5]) * f(game[5]) + f(ours[6]) * f(game[6]);
        const bool turn = std::abs(std::abs(dot) - 1.0f) < 0.001f;
        const bool zero = f(game[0]) == 0 && f(game[1]) == 0 && f(game[2]) == 0;
        const std::string kind = o.isPerson() ? "Sim" : "object";
        ++tally[kind + (zero ? " place 0" : std::string(" xy ") + (xy ? "same" : "differs") + ", z " + (z ? "same" : "differs") + ", turn " + (turn ? "same" : "differs"))];
        if (!zero && (!xy || !z || !turn) && listed < 12) {
            ++listed;
            std::printf("  %s %d '%s' level %d: ours %g %g %g / %g %g, game %g %g %g / %g %g\n", kind.c_str(), o.id, o.definition.name.c_str(), o.level,
                        f(ours[0]), f(ours[1]), f(ours[2]), f(ours[5]), f(ours[6]), f(game[0]), f(game[1]), f(game[2]), f(game[5]), f(game[6]));
        }
        return false;
    });
    for (const auto& [what, n] : tally) {
        std::printf("%6d  %s\n", n, what.c_str());
    }
    return 0;
}

// Prints every field of one object's current record that the running object does not fill, and
// the sizes of those it does.
int printRecord(const char* lot, std::uint32_t id) {
    for (const auto& [key, entry] : savedObjectsOf(lot)) {
        if (key.instance != id) {
            continue;
        }
        const auto& [o, kind] = entry;
        const auto& h = o.header;
        std::printf("object %u version 0x%X: header reserved %g %g %g %g location %g %g level %d, values %d %g %d %d %g\n", id,
                    o.version, h.reserved[0], h.reserved[1], h.reserved[2], h.reserved[3], h.location[0], h.location[1], h.value1,
                    h.value2, h.value3, h.value4, h.value5, h.value6);
        std::printf("  %zu attributes, %zu semi attributes, temps", o.attributes.size(), o.semiAttributes.size());
        for (const auto t : o.temps) {
            std::printf(" %d", t);
        }
        std::printf("\n  token source");
        for (const auto t : o.tokenSource) {
            std::printf(" %d", t);
        }
        std::printf("\n");
        if (o.tail) {
            const auto& t = *o.tail;
            std::printf("  token guid 0x%08X flags %u values %u %u %u, %zu properties\n", t.token.guid, t.token.flags, t.token.value1,
                        t.token.value2, t.token.value3, t.token.properties.size());
            std::printf("  %zu arrays, %zu iterators, value1 %d, %zu strings, value2 %u, %zu frames, stack value %d\n", t.arrays.size(),
                        t.shorts.size(), t.value1, t.strings.size(), t.value2, t.stack.size(), t.stackValue);
            std::printf("  relationships v%d (%zu), slots marker %d extra %d (%zu values), effects flag %d v%d (%zu), %zu materials, "
                        "%zu timers, material block %zu bytes\n",
                        t.relationshipVersion, t.relationshipIds.size(), t.slots.marker, t.slots.extra, t.slots.values.size(),
                        t.effectFlag, t.effectVersion, t.effects.size(), t.materials.size(), t.timers.size(), t.materialBlock.size());
            std::printf("  slot values");
            for (const auto v : t.slots.values) {
                std::printf(" %d", v);
            }
            std::printf("\n  relationship ids");
            for (const auto r : t.relationshipIds) {
                std::printf(" %u", r);
            }
            std::printf("\n");
            for (const auto& f : t.stack) {
                std::printf("    frame object %d tree 0x%04X node %u values %d %d %d owner %d, %zu params %zu locals:", f.objectId,
                            static_cast<std::uint16_t>(f.treeId), f.node, f.value1, f.value2, f.value3, f.codeOwner, f.params.size(),
                            f.locals.size());
                for (const auto v : f.params) {
                    std::printf(" p%d", v);
                }
                for (const auto v : f.locals) {
                    std::printf(" l%d", v);
                }
                std::printf("\n");
            }
        }
        if (o.person) {
            const auto& p = *o.person;
            const auto& s = p.state;
            std::printf("  person v%d, %zu person data, motives2", p.version, p.personData.size());
            for (const float m : p.motives2) {
                std::printf(" %g", m);
            }
            std::printf("\n  state: %zu queued, %zu runs, %zu entries, sub start %u value1 %u, %s sub, %zu sub-queued, text '%s', "
                        "picked %d %d %d, value2 %d, %zu listed, value3 %u value4 %u, values",
                        s.queue.size(), s.runs.size(), s.entries.size(), s.subStartCount, s.value1, s.runningSub ? "a" : "no",
                        s.subQueue.size(), s.text.c_str(), s.pickedIconIndex, s.pickedIconGuid, s.pickedIconObject, s.value2, s.list.size(),
                        s.value3, s.value4);
            for (const auto v : s.values) {
                std::printf(" %u", v);
            }
            for (const auto& e : s.entries) {
                std::printf("\n    entry %d %g %g", e.key, e.value1, e.value2);
            }
            for (const auto& run : s.runs) {
                std::printf("\n    run object %d depth %d flag %d", run.object, run.depth, run.flag ? 1 : 0);
            }
            std::printf("\n  running tree 0x%04X line %d id %d number %d, interrupted tree 0x%04X line %d id %d link %d, name bytes",
                        s.running.tree, s.running.index, s.running.id, s.running.number, s.interrupted.tree, s.interrupted.index,
                        s.interrupted.id, s.interrupted.linkId);
            for (std::size_t i = 0; i < 16; ++i) {
                std::printf(" %02X", o.name[i]);
            }
            std::printf("\n");
        }
        std::printf("  %zu bytes left over\n", o.rest.size());
        return 0;
    }
    std::printf("object %u has no current record\n", id);
    return 1;
}

// A character file's start as the game made it: its name, GUID, names and template, the
// template's resources as installed, and its installed packs record.
struct GameCharacter {
    ts2::sim::NewCharacter character;
    std::vector<ts2::sim::TemplateResource> templ;
    std::vector<std::uint8_t> packs;
};

std::optional<GameCharacter> gameCharacter(ResourceIndex& index, const char* gameFile) {
    ts2::dbpf::Package game{gameFile};
    std::vector<std::uint8_t> definition, names;
    GameCharacter out;
    for (const auto& e : game.entries()) {
        if (e.key.type == ts2::types::kObjectDefinition) {
            definition = game.read(e);
        } else if (e.key.type == ts2::types::kCatalogString) {
            names = game.read(e);
        } else if (e.key.type == 0xEBFEE342) {
            out.packs = game.read(e);
        }
    }
    const auto field = [&](std::size_t i) { return static_cast<std::uint32_t>(definition[0x40 + 2 * i] | definition[0x41 + 2 * i] << 8); };
    const auto table = ts2::str::readRecord(names);
    if (definition.size() < 0x40 + 2 * 0x48 || !table || table->entries.size() < 3) {
        std::fprintf(stderr, "no object definition or name text in %s\n", gameFile);
        return std::nullopt;
    }
    ts2::sim::NewCharacter& character = out.character;
    character.fileName = std::filesystem::path(gameFile).stem().string();
    character.firstName = table->entries[0].value;
    character.surname = table->entries[2].value;
    character.guid = field(0x0E) | field(0x0F) << 16;
    character.templateGuid = field(0x46) | field(0x47) << 16;
    ts2::sim::IndexCodeSource code(index);
    const auto* type = code.objectType(character.templateGuid);
    if (type == nullptr) {
        std::fprintf(stderr, "template 0x%08X not installed\n", character.templateGuid);
        return std::nullopt;
    }
    for (const auto& r : index.resources()) {
        if (r.entry.key.group == type->group) {
            out.templ.push_back({r.entry.key, index.read(r)});
        }
    }
    return out;
}

// Starts a character file as the game does from the template named in a character file the game
// wrote, with that file's name, GUID and names, and compares each resource of ours with the
// game's. The look the game adds is listed, not compared.
int checkNewCharacter(ResourceIndex& index, const char* gameFile) {
    ts2::dbpf::Package game{gameFile};
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, std::vector<std::uint8_t>> theirs;
    for (const auto& e : game.entries()) {
        theirs[{e.key.type, e.key.instance, e.key.instanceHigh}] = game.read(e);
    }
    const auto made = gameCharacter(index, gameFile);
    if (!made) {
        return 1;
    }
    const ts2::dbpf::PackageImage ours = ts2::sim::newCharacterPackage(made->templ, made->character, made->packs);
    const ts2::dbpf::PackageImage reread = ts2::dbpf::readImage(ts2::dbpf::writeImage(ours));
    int same = 0, differ = 0;
    std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> madeKeys;
    for (const auto& r : reread.resources) {
        const auto key = std::tuple{r.key.type, r.key.instance, r.key.instanceHigh};
        madeKeys.insert(key);
        const auto body = r.compressed ? ts2::qfs::decompress(r.bytes) : r.bytes;
        const auto it = theirs.find(key);
        if (it == theirs.end() || r.key.group != 0xFFFFFFFF) {
            std::printf("  ours only %08X:%X\n", r.key.type, r.key.instance);
            ++differ;
        } else if (it->second != body) {
            std::printf("  %08X:%X differs (ours %zu bytes, game %zu)\n", r.key.type, r.key.instance, body.size(), it->second.size());
            ++differ;
        } else {
            ++same;
        }
    }
    std::map<std::uint32_t, int> look;
    for (const auto& [key, body] : theirs) {
        if (!madeKeys.contains(key)) {
            ++look[std::get<0>(key)];
        }
    }
    std::printf("%s from template 0x%08X: %d resources the same, %d differ. Not made (the look):", made->character.fileName.c_str(),
                made->character.templateGuid, same, differ);
    for (const auto& [t, n] : look) {
        std::printf(" %08X x%d", t, n);
    }
    std::printf("\n");
    return differ == 0 ? 0 : 1;
}

// Makes a newborn's look records from the genes and gender a game-born baby's age data names and the
// catalog parts the engine picks for them, and compares each with the game's.
int checkBabyLook(const char* install, const char* gameFile) {
    ts2::dbpf::Package game{gameFile};
    std::map<std::tuple<std::uint32_t, std::uint32_t>, std::vector<std::uint8_t>> theirs;
    for (const auto& e : game.entries()) {
        theirs[{e.key.type, e.key.instance}] = game.read(e);
    }
    const auto age = theirs.find({0xAC598EAC, 1});
    if (age == theirs.end()) {
        std::fprintf(stderr, "no age data in %s\n", gameFile);
        return 1;
    }
    const auto data = ts2::propset::parse(age->second);
    const auto value = [&](const char* key) { const auto it = data.find(key); return it != data.end() ? it->second : std::string{}; };
    ts2::scene::Newborn baby;
    baby.gender = static_cast<std::uint32_t>(std::stoul(value("gender"), nullptr, 0));
    baby.skinTone = value("skincolor");
    baby.hairTone = value("haircolor");
    baby.eyeColour = value("eyecolor");
    ts2::scene::Assets assets(install);
    const ts2::scene::BabyCatalog catalog = ts2::scene::readBabyCatalog(assets.skinPackages());
    const ts2::scene::NewbornOutfit outfit = ts2::scene::newbornOutfit(catalog, baby);
    constexpr std::uint32_t kHair = 1, kFace = 2, kBody = 8;
    if (!outfit.body || !outfit.skin.contains(kHair) || !outfit.skin.contains(kFace) || !outfit.skin.contains(kBody)) {
        std::fprintf(stderr, "the catalog has no full baby outfit for skin %s\n", baby.skinTone.c_str());
        return 1;
    }
    const auto key = [](const ts2::scenegraph::Link& l) { return ts2::dbpf::ResourceKey{l.type, l.group, l.instance, l.instanceHigh}; };
    ts2::sim::BabyLook look{baby.gender, baby.skinTone, baby.hairTone, baby.eyeColour, key(*outfit.body),
                            key(outfit.skin.at(kHair)), key(outfit.skin.at(kBody)), key(outfit.skin.at(kFace))};
    ts2::dbpf::PackageImage ours;
    ts2::sim::addBabyLookRecords(ours, look);
    int same = 0, differ = 0;
    for (const auto& r : ours.resources) {
        const auto body = r.compressed ? ts2::qfs::decompress(r.bytes) : r.bytes;
        const auto it = theirs.find({r.key.type, r.key.instance});
        if (it != theirs.end() && it->second == body) {
            ++same;
        } else {
            std::printf("  %08X:%X differs (ours %zu bytes, game %zu)\n", r.key.type, r.key.instance, body.size(),
                        it != theirs.end() ? it->second.size() : 0);
            ++differ;
        }
    }
    std::printf("%d look records the same, %d differ\n", same, differ);
    return differ == 0 ? 0 : 1;
}

// Blends a newborn's baby face from two parents' face shapes as the engine draws it, for each way
// its six face regions could come from either parent, and compares the result, as one offset and
// texture coordinate pair per triangle corner, with the baby face of the child's saved face shape.
// Parent B's shape comes from `packageB`, group `groupB` (an NPC's), or from a character file.
int fitBabyFace(const char* install, const char* child, const char* parentA, const char* packageB, std::uint32_t groupB) {
    const auto shapeIn = [](const char* path, std::optional<std::uint32_t> group) -> std::optional<ts2::faceshape::FaceShape> {
        ts2::dbpf::Package package{path};
        for (const auto& e : package.entries()) {
            if (e.key.type == ts2::faceshape::kType && (!group || e.key.group == *group)) {
                return ts2::faceshape::parse(package.read(e));
            }
        }
        return std::nullopt;
    };
    const auto childShape = shapeIn(child, std::nullopt);
    const auto a = shapeIn(parentA, std::nullopt);
    const auto b = shapeIn(packageB, groupB == 0 ? std::nullopt : std::optional<std::uint32_t>{groupB});
    if (!childShape || !a || !b) {
        std::fprintf(stderr, "a face shape is missing\n");
        return 1;
    }
    ts2::dbpf::Package childPackage{child};
    std::map<std::string, std::string> aged;
    for (const auto& e : childPackage.entries()) {
        if (e.key.type == 0xAC598EAC) {
            aged = ts2::propset::parse(childPackage.read(e));
        }
    }
    ts2::scene::Assets assets(install);
    ts2::scene::Newborn baby;
    baby.gender = static_cast<std::uint32_t>(std::stoul(aged["gender"], nullptr, 0));
    baby.skinTone = aged["skincolor"];
    const ts2::scene::BabyCatalog catalog = ts2::scene::readBabyCatalog(assets.skinPackages());
    const ts2::scene::NewbornOutfit outfit = ts2::scene::newbornOutfit(catalog, baby);
    if (!outfit.skin.contains(2)) {
        std::fprintf(stderr, "no baby face part\n");
        return 1;
    }
    const auto& facePart = outfit.skin.at(2);
    const auto props = assets.skinResource(facePart);
    const auto refs = assets.skinResource({0xAC506764, facePart.group, facePart.instance, facePart.instanceHigh});
    if (!props || !refs) {
        std::fprintf(stderr, "the baby face part can't be read\n");
        return 1;
    }
    const auto p = ts2::propset::parse(*props);
    const auto links = ts2::reflist::parse(*refs);
    const std::size_t shapeIndex = std::stoul(p.at("shapekeyidx"), nullptr, 0);
    const auto base = shapeIndex < links.size() ? ts2::scene::shapeMesh(assets, links[shapeIndex]) : std::nullopt;
    if (!base) {
        std::fprintf(stderr, "the baby face mesh can't be read\n");
        return 1;
    }
    // The corners of the face's triangle group, as the face shape lists them.
    const auto corners = [](const ts2::scenegraph::Mesh& mesh, std::vector<std::array<float, 3>>& pos, std::vector<std::array<float, 2>>& uv) {
        for (const auto& g : mesh.groups) {
            if (g.primitive != ts2::scenegraph::kTriangleList || g.link >= mesh.links.size()) {
                continue;
            }
            const auto xyz = ts2::scenegraph::vertexFloats(mesh, mesh.links[g.link], ts2::scenegraph::kPositions);
            const auto st = ts2::scenegraph::vertexFloats(mesh, mesh.links[g.link], ts2::scenegraph::kTextureCoordinates);
            for (const std::uint16_t i : g.indices) {
                pos.push_back({xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2]});
                uv.push_back({st[i * 2], st[i * 2 + 1]});
            }
            return;
        }
    };
    std::vector<std::array<float, 3>> basePos;
    std::vector<std::array<float, 2>> baseUv;
    corners(*base, basePos, baseUv);
    static constexpr std::array<const char*, 6> kRegions{"nose", "brow", "mouth", "eyes", "jaw", "ears"};
    auto data = std::make_shared<ts2::scene::NewbornFaceData>();
    for (const std::uint8_t gender : {std::uint8_t{0}, std::uint8_t{1}}) {
        const auto* saved = childShape->find(5, gender, 0);
        if (saved == nullptr) {
            continue;
        }
        int best = -1;
        std::size_t bestExact = 0;
        double bestWorst = 0;
        for (int combo = 0; combo < 64; ++combo) {
            std::vector<std::pair<std::string, bool>> picks;
            for (std::size_t r = 0; r < kRegions.size(); ++r) {
                const bool fromA = (combo >> r) & 1;
                picks.push_back({kRegions[r], fromA});
                if (std::string_view(kRegions[r]) == "eyes") {
                    picks.push_back({"cheekbones", fromA});
                } else if (std::string_view(kRegions[r]) == "mouth") {
                    picks.push_back({"cheek", fromA});
                }
            }
            ts2::scenegraph::Mesh mesh = *base;
            ts2::scene::newbornFaceEdit(assets, data, "fit", a, b, picks).apply(mesh);
            std::vector<std::array<float, 3>> pos;
            std::vector<std::array<float, 2>> uv;
            corners(mesh, pos, uv);
            if (pos.size() != saved->points.size()) {
                std::fprintf(stderr, "corner counts differ: ours %zu, saved %zu\n", pos.size(), saved->points.size());
                return 1;
            }
            std::size_t exact = 0;
            double worst = 0;
            for (std::size_t i = 0; i < pos.size(); ++i) {
                double err = 0;
                for (int k = 0; k < 3; ++k) {
                    err = std::max(err, static_cast<double>(std::abs(pos[i][k] - basePos[i][k] - saved->points[i].offset[k])));
                }
                exact += err == 0 ? 1 : 0;
                worst = std::max(worst, err);
            }
            if (best < 0 || exact > bestExact) {
                best = combo;
                bestExact = exact;
                bestWorst = worst;
            }
        }
        std::printf("baby face, gender %d: best picks", gender);
        for (std::size_t r = 0; r < kRegions.size(); ++r) {
            std::printf(" %s=%s", kRegions[r], ((best >> r) & 1) ? "A" : "B");
        }
        std::printf(": %zu of %zu corners exact, worst error %g\n", bestExact, saved->points.size(), bestWorst);
        {
            std::vector<std::pair<std::string, bool>> picks;
            for (std::size_t r = 0; r < kRegions.size(); ++r) {
                const bool fromA = (best >> r) & 1;
                picks.push_back({kRegions[r], fromA});
                if (std::string_view(kRegions[r]) == "eyes") {
                    picks.push_back({"cheekbones", fromA});
                } else if (std::string_view(kRegions[r]) == "mouth") {
                    picks.push_back({"cheek", fromA});
                }
            }
            ts2::scenegraph::Mesh mesh = *base;
            ts2::scene::newbornFaceEdit(assets, data, "fit", a, b, picks).apply(mesh);
            std::vector<std::array<float, 3>> pos;
            std::vector<std::array<float, 2>> uv;
            corners(mesh, pos, uv);
            std::map<int, int> buckets;  // error by power of ten
            int shown = 0;
            for (std::size_t i = 0; i < pos.size(); ++i) {
                double err = 0;
                for (int k = 0; k < 3; ++k) {
                    err = std::max(err, static_cast<double>(std::abs(pos[i][k] - basePos[i][k] - saved->points[i].offset[k])));
                }
                if (err > 0) {
                    ++buckets[static_cast<int>(std::floor(std::log10(err)))];
                    if (err > 1e-6 && shown++ < 4) {
                        std::printf("   corner %zu uv %.5f,%.5f ours %.7f %.7f %.7f saved %.7f %.7f %.7f\n", i, uv[i][0], uv[i][1],
                                    pos[i][0] - basePos[i][0], pos[i][1] - basePos[i][1], pos[i][2] - basePos[i][2],
                                    saved->points[i].offset[0], saved->points[i].offset[1], saved->points[i].offset[2]);
                    }
                }
            }
            for (const auto& [e, n] : buckets) {
                std::printf("   error 1e%d: %d corners\n", e, n);
            }
        }
    }
    return 0;
}

// The face regions each parent gave, from "region=A" or "region=B" arguments. The eyes take the
// cheekbones with them, and the mouth the cheeks, as a birth draws them.
ts2::sim::FacePicks facePicksOf(const std::vector<std::string>& pickArgs) {
    ts2::sim::FacePicks picks;
    for (const auto& arg : pickArgs) {
        const auto eq = arg.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string region = arg.substr(0, eq);
        const bool fromA = arg.substr(eq + 1) == "A";
        picks.push_back({region, fromA});
        if (region == "eyes") {
            picks.push_back({"cheekbones", fromA});
        } else if (region == "mouth") {
            picks.push_back({"cheek", fromA});
        }
    }
    return picks;
}

// Blends a newborn's face shape from its parents' as the engine writes it into the character file,
// with the face region picks given ("nose=A", "eyes=B", ...), and compares the record byte for byte
// with the child's saved one. Parent B's shape comes from `packageB`, group `groupB` (an NPC's), or
// from a character file when `groupB` is 0.
int checkFaceShape(const char* install, const char* child, const char* parentA, const char* packageB, std::uint32_t groupB,
                   const std::vector<std::string>& pickArgs) {
    const auto recordIn = [](const char* path, std::optional<std::uint32_t> group) -> std::optional<std::vector<std::uint8_t>> {
        ts2::dbpf::Package package{path};
        for (const auto& e : package.entries()) {
            if (e.key.type == ts2::faceshape::kType && (!group || e.key.group == *group)) {
                return package.read(e);
            }
        }
        return std::nullopt;
    };
    const auto theirs = recordIn(child, std::nullopt);
    const auto a = recordIn(parentA, std::nullopt);
    const auto b = recordIn(packageB, groupB == 0 ? std::nullopt : std::optional<std::uint32_t>{groupB});
    if (!theirs || !a || !b) {
        std::fprintf(stderr, "a face shape is missing\n");
        return 1;
    }
    const ts2::sim::FacePicks picks = facePicksOf(pickArgs);
    std::string skinTone;
    ts2::dbpf::ResourceKey theirKey;
    ts2::dbpf::Package childPackage{child};
    for (const auto& e : childPackage.entries()) {
        if (e.key.type == 0xAC598EAC) {
            skinTone = ts2::propset::parse(childPackage.read(e))["skincolor"];
        } else if (e.key.type == ts2::faceshape::kType) {
            theirKey = e.key;
        }
    }
    ts2::scene::Assets assets(install);
    const auto shape = ts2::scene::newbornFaceShape(assets, ts2::faceshape::parse(*a), ts2::faceshape::parse(*b), picks, skinTone);
    if (!shape) {
        std::fprintf(stderr, "no face shape made\n");
        return 1;
    }
    const auto ours = ts2::faceshape::write(*shape);
    ts2::dbpf::PackageImage image;
    ts2::sim::addFaceShapeRecord(image, ours);
    std::printf("record key %s the game's\n", image.resources[0].key == theirKey ? "is" : "is not");
    const auto saved = ts2::faceshape::parse(*theirs);
    const auto written = ts2::faceshape::parse(ours);
    std::size_t sameMeshes = 0;
    for (const auto& m : written.meshes) {
        const auto* other = saved.find(m.age, m.gender, m.group);
        bool same = other != nullptr && other->points.size() == m.points.size();
        for (std::size_t i = 0; same && i < m.points.size(); ++i) {
            same = m.points[i].offset == other->points[i].offset && m.points[i].uv == other->points[i].uv;
        }
        sameMeshes += same;
        if (!same) {
            std::printf("  mesh age %u gender %u group %u differs\n", m.age, m.gender, m.group);
        }
    }
    std::size_t firstDiff = 0;
    while (firstDiff < ours.size() && firstDiff < theirs->size() && ours[firstDiff] == (*theirs)[firstDiff]) {
        ++firstDiff;
    }
    // The grips of each age's model against the game's.
    std::uint32_t genderFlag = 0;
    std::map<std::uint32_t, std::vector<std::uint8_t>> models;
    for (const auto& e : childPackage.entries()) {
        if (e.key.type == 0xAC598EAC) {
            genderFlag = static_cast<std::uint32_t>(std::stoul(ts2::propset::parse(childPackage.read(e))["gender"], nullptr, 0));
        } else if (e.key.type == 0xE519C933) {
            models[e.key.instance] = childPackage.read(e);
        }
    }
    for (const bool single : {false, true}) {
    int gripsSame = 0, gripsDiffer = 0;
    for (const int age : {5, 0, 1, 2, 7, 3, 4}) {
        const auto grips = ts2::scene::newbornFaceGrips(assets, ts2::faceshape::parse(*a), ts2::faceshape::parse(*b), picks, skinTone, age, genderFlag,
                                                        ts2::sim::skeletonModelName(age), single);
        const auto it = models.find(ts2::hash::scenegraphKey("age" + std::to_string(age) + "_0_cres").instance);
        std::vector<ts2::scene::FaceGrip> game;
        if (it != models.end()) {
            for (const auto& block : ts2::rcol::read(it->second).blocks) {
                if (const auto* list = std::get_if<ts2::rcol::DataList>(&block); isGripList(list)) {
                    ts2::scene::FaceGrip g;
                    g.bone = list->value.items[0].text;
                    std::copy_n(list->value.items[1].floats.begin(), 3, g.translation.begin());
                    std::copy_n(list->value.items[2].floats.begin(), 4, g.rotation.begin());
                    game.push_back(g);
                }
            }
        }
        for (std::size_t i = 0; i < std::max(grips.size(), game.size()); ++i) {
            const bool same = i < grips.size() && i < game.size() && grips[i].bone == game[i].bone && grips[i].translation == game[i].translation &&
                              grips[i].rotation == game[i].rotation;
            same ? ++gripsSame : ++gripsDiffer;
            if (!same && !single) {
                const auto show = [](const char* who, const std::vector<ts2::scene::FaceGrip>& v, std::size_t j) {
                    if (j < v.size()) {
                        std::printf("    %s %s %.9g %.9g %.9g\n", who, v[j].bone.c_str(), v[j].translation[0], v[j].translation[1], v[j].translation[2]);
                    }
                };
                std::printf("  age %d grip %zu differs\n", age, i);
                show("ours", grips, i);
                show("game", game, i);
            }
        }
    }
    std::printf("%s precision grips: %d the same, %d differ\n", single ? "single" : "double", gripsSame, gripsDiffer);
    }
    // Each age's meshes: the positions against the game's mesh.
    std::map<std::uint32_t, std::vector<std::uint8_t>> meshes;
    for (const auto& e : childPackage.entries()) {
        if (e.key.type == 0xAC4F8687) {
            meshes[e.key.instance] = childPackage.read(e);
        }
    }
    for (const int age : {5, 0, 1, 2, 7, 3, 4}) {
        for (const bool far : {false, true}) {
            const std::string name = "age" + std::to_string(age) + "_0" + (far ? "LOD15" : "") + "_gmdc";
            const auto it = meshes.find(ts2::hash::scenegraphKey(name).instance);
            const auto vertices = ts2::scene::newbornFaceVertices(assets, ts2::faceshape::parse(*a), ts2::faceshape::parse(*b), picks, skinTone,
                                                                    static_cast<std::uint8_t>(age == 7 ? 3 : age), genderFlag == 1 ? 1 : 0, far);
            const auto* positions = vertices ? &vertices->positions : nullptr;
            if (it == meshes.end() || positions == nullptr) {
                std::printf("  %s: missing\n", name.c_str());
                continue;
            }
            const auto mesh = ts2::scenegraph::parseMesh(it->second);
            const auto* meshLink = firstGroupLink(mesh);
            if (meshLink == nullptr) {
                std::printf("  %s: the game's mesh has no group\n", name.c_str());
                continue;
            }
            const auto game = ts2::scenegraph::vertexFloats(mesh, *meshLink, ts2::scenegraph::kPositions);
            std::size_t same = 0;
            for (std::size_t i = 0; i < std::min(game.size(), positions->size()); ++i) {
                const bool eq = std::bit_cast<std::uint32_t>(game[i]) == std::bit_cast<std::uint32_t>((*positions)[i]);
                same += eq;
                if (!eq) {
                    std::printf("    float %zu (vertex %zu): ours %.9g (%08X) game %.9g (%08X)\n", i, i / 3, (*positions)[i],
                                std::bit_cast<std::uint32_t>((*positions)[i]), game[i], std::bit_cast<std::uint32_t>(game[i]));
                }
            }
            std::printf("  %s positions: %zu of %zu floats the same\n", name.c_str(), same, game.size());
            {
                const auto gameNormals = ts2::scenegraph::vertexFloats(mesh, *meshLink, ts2::scenegraph::kNormals);
                std::size_t exact = 0, close = 0;
                for (std::size_t i = 0; i < std::min(gameNormals.size(), vertices->normals.size()); ++i) {
                    exact += std::bit_cast<std::uint32_t>(gameNormals[i]) == std::bit_cast<std::uint32_t>(vertices->normals[i]);
                    close += std::fabs(gameNormals[i] - vertices->normals[i]) < 1e-6f;
                }
                std::printf("  %s normals: %zu exact, %zu within 1e-6, of %zu\n", name.c_str(), exact, close, gameNormals.size());
            }
        }
    }
    const bool identical = ours == *theirs;
    std::printf("%zu of %zu meshes the same, record %s (ours %zu bytes, game %zu", sameMeshes, written.meshes.size(),
                identical ? "byte for byte the same" : "differs", ours.size(), theirs->size());
    if (!identical) {
        std::printf(", first difference at byte %zu", firstDiff);
    }
    std::printf(")\n");
    return identical ? 0 : 1;
}

// Reads every model, shape, geometry node, mesh and material in the install's Sims3D packages and
// the packages given, writes each back, and counts those that come out byte for byte the same.
int checkSceneRecords(const char* install, const std::vector<std::string>& extra) {
    static constexpr std::array<std::uint32_t, 5> kTypes{0xE519C933, 0xFC6EB1F7, 0x7BA3838C, 0xAC4F8687, 0x49596978};
    std::vector<std::filesystem::path> packages;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(install, ec); !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".package" && it->path().parent_path().filename() == "Sims3D") {
            packages.push_back(it->path());
        }
    }
    for (const auto& p : extra) {
        if (std::filesystem::is_directory(p, ec)) {
            for (const auto& f : std::filesystem::directory_iterator(p, ec)) {
                if (f.path().extension() == ".package") {
                    packages.push_back(f.path());
                }
            }
        } else {
            packages.emplace_back(p);
        }
    }
    std::size_t same = 0, differ = 0, unread = 0;
    std::map<std::string, std::size_t> reasons;
    for (const auto& path : packages) {
        try {
            ts2::dbpf::Package package{path};
            for (const auto& e : package.entries()) {
                if (std::find(kTypes.begin(), kTypes.end(), e.key.type) == kTypes.end()) {
                    continue;
                }
                const auto bytes = package.read(e);
                try {
                    if (ts2::rcol::write(ts2::rcol::read(bytes)) == bytes) {
                        ++same;
                    } else {
                        if (++differ <= 5) {
                            std::printf("  differs: %s %08X:%08X:%08X\n", path.filename().string().c_str(), e.key.type, e.key.group, e.key.instance);
                        }
                    }
                } catch (const std::exception& ex) {
                    ++unread;
                    ++reasons[ex.what()];
                }
            }
        } catch (const std::exception& ex) {
            std::printf("  %s can't be read: %s\n", path.string().c_str(), ex.what());
        }
    }
    for (const auto& [why, n] : reasons) {
        std::printf("  %zu not read: %s\n", n, why.c_str());
    }
    std::printf("%zu packages: %zu records the same, %zu differ, %zu not read\n", packages.size(), same, differ, unread);
    return differ == 0 && unread == 0 ? 0 : 1;
}

// Makes a newborn's face nodes and shapes as the engine writes them into the character file and
// compares each with the game's record of the same key in `gameFile`. The values the game counts
// with time (the file's group number in the names, and the material names) are taken from the
// game's file.
int checkBabyModels(const char* install, const char* gameFile, const char* outFolder) {
    ts2::dbpf::Package game{gameFile};
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::uint8_t>> theirs;
    std::string tone, eyes, hair;
    std::uint32_t gender = 0;
    for (const auto& e : game.entries()) {
        theirs[{e.key.type, e.key.instance}] = game.read(e);
        if (e.key.type == 0xAC598EAC) {
            auto p = ts2::propset::parse(theirs[{e.key.type, e.key.instance}]);
            tone = p["skincolor"];
            eyes = p["eyecolor"];
            hair = p["haircolor"];
            gender = static_cast<std::uint32_t>(std::stoul(p["gender"], nullptr, 0));
        }
    }
    // The group number: the one in the game's names.
    std::uint32_t group = 0;
    for (const auto& [key, bytes] : theirs) {
        const std::string text(bytes.begin(), bytes.end());
        if (const auto at = text.find("#0x6f"); at != std::string::npos) {
            group = static_cast<std::uint32_t>(std::stoul(text.substr(at + 1, 10), nullptr, 16));
            break;
        }
    }
    ts2::scene::Assets assets(install);
    static constexpr std::array<int, 7> kAges{5, 0, 1, 2, 7, 3, 4};
    int same = 0, differ = 0;
    const auto compare = [&](std::uint32_t type, const std::string& name, const std::vector<std::uint8_t>& ours) {
        const auto key = ts2::hash::scenegraphKey(name);
        const auto it = theirs.find({type, key.instance});
        if (outFolder != nullptr) {
            std::error_code error;
            std::filesystem::create_directories(outFolder, error);
            writeBytes(std::filesystem::path(outFolder) / (name + ".bin"), ours);
        }
        if (it != theirs.end() && it->second == ours) {
            ++same;
        } else {
            ++differ;
            std::printf("  %s %s (ours %zu bytes, game %zu)\n", name.c_str(), it == theirs.end() ? "missing in the game's file" : "differs",
                        ours.size(), it != theirs.end() ? it->second.size() : 0);
        }
    };
    for (const int age : kAges) {
        for (const bool far : {false, true}) {
            const auto catalog = ts2::scene::baseFaceNode(assets, ts2::sim::baseFaceAgeFlag(age), gender, far, tone);
            if (!catalog) {
                std::printf("  age %d: no catalog face node\n", age);
                ++differ;
                continue;
            }
            const std::string name = "age" + std::to_string(age) + "_0" + (far ? "LOD15" : "") + "_gmnd";
            compare(0x7BA3838C, name, ts2::sim::newFaceNode(*catalog, age, far, group));
        }
        // The material the game's shape names (its counter is not reproducible).
        const std::string shapeName = "age" + std::to_string(age) + "_00_shpe";
        std::string material;
        if (const auto it = theirs.find({0xFC6EB1F7, ts2::hash::scenegraphKey(shapeName).instance}); it != theirs.end()) {
            const auto shape = ts2::rcol::read(it->second);
            if (const auto* s = shape.blocks.empty() ? nullptr : std::get_if<ts2::rcol::Shape>(&shape.blocks[0]); s != nullptr && !s->parts.empty()) {
                material = s->parts[0].material;
            }
        }
        compare(0xFC6EB1F7, shapeName, ts2::sim::newFaceShape(age, group, material));
        // The material: its counter (time-like) and brow family (a birth's draw) from the game's.
        if (const auto at = material.find("0x"); at != std::string::npos) {
            const auto counter = static_cast<std::uint32_t>(std::stoul(material.substr(at + 2), nullptr, 16));
            ts2::sim::FaceMaterialLook look;
            look.name = ts2::sim::faceMaterialName(counter, age, gender == 1 ? 1 : 0);
            const std::string materialName = look.name + "_txmt";
            if (const auto gameMaterial = theirs.find({0x49596978, ts2::hash::scenegraphKey(materialName).instance}); gameMaterial != theirs.end()) {
                const auto record = ts2::rcol::read(gameMaterial->second);
                for (const auto& [key, value] : materialProperties(record)) {
                    if (key == "cafOverlay1") {
                        look.browFamily = value.substr(0, value.find(','));
                    }
                }
            }
            look.eyeColour = eyes;
            look.skinTone = tone;
            look.hairTone = age == 4 ? "00000005-0000-0000-0000-000000000000" : hair;
            look.age = age;
            look.paramGender = gender == 1 ? 1 : 0;
            if (const auto parts = ts2::scene::newbornFaceMaterialParts(assets, ts2::sim::baseFaceAgeFlag(age), gender, tone, eyes, look.hairTone, look.browFamily)) {
                look.eyeTexture = parts->eyeTexture;
                look.browTexture = parts->browTexture;
                compare(0x49596978, materialName, ts2::sim::newFaceMaterial(parts->catalogMaterial, group, look));
            } else {
                std::printf("  age %d: no catalog face material\n", age);
                ++differ;
            }
        }
        // The model: the morphs are the base face mesh's, the grips (not known yet) the game's.
        std::vector<std::string> morphs;
        if (const auto node = ts2::scene::baseFaceNode(assets, ts2::sim::baseFaceAgeFlag(age), gender, false, tone)) {
            for (const auto& l : ts2::scenegraph::parseContainer(*node).links) {
                if (const auto bytes = l.type == 0xAC4F8687 ? assets.byKey(l) : std::nullopt) {
                    for (const auto& m : ts2::scenegraph::parseMesh(*bytes).morphs) {
                        if (!m.group.empty() || !m.name.empty()) {
                            morphs.push_back(m.group + "::" + m.name);
                        }
                    }
                }
            }
        }
        const std::string modelName = "age" + std::to_string(age) + "_0_cres";
        std::vector<ts2::sim::GripAdjust> grips;
        if (const auto it = theirs.find({0xE519C933, ts2::hash::scenegraphKey(modelName).instance}); it != theirs.end()) {
            for (const auto& block : ts2::rcol::read(it->second).blocks) {
                const auto* list = std::get_if<ts2::rcol::DataList>(&block);
                if (!isGripList(list)) {
                    continue;
                }
                ts2::sim::GripAdjust grip;
                grip.bone = list->value.items[0].text;
                std::copy_n(list->value.items[1].floats.begin(), 3, grip.translation.begin());
                std::copy_n(list->value.items[2].floats.begin(), 4, grip.rotation.begin());
                grips.push_back(grip);
            }
        }
        // The meshes, with the game's positions and normals (made by the face blend elsewhere).
        for (const bool far : {false, true}) {
            const auto node = ts2::scene::baseFaceNode(assets, ts2::sim::baseFaceAgeFlag(age), gender, far, tone);
            const std::string meshName = "age" + std::to_string(age) + "_0" + (far ? "LOD15" : "") + "_gmdc";
            const auto gameMesh = theirs.find({0xAC4F8687, ts2::hash::scenegraphKey(meshName).instance});
            for (const auto& l : node && gameMesh != theirs.end() ? ts2::scenegraph::parseContainer(*node).links : std::vector<ts2::scenegraph::Link>{}) {
                if (const auto bytes = l.type == 0xAC4F8687 ? assets.byKey(l) : std::nullopt) {
                    const auto gm = ts2::scenegraph::parseMesh(gameMesh->second);
                    if (const auto* link = firstGroupLink(gm)) {
                        compare(0xAC4F8687, meshName, ts2::sim::newFaceMesh(*bytes, age, far, group, ts2::scenegraph::vertexFloats(gm, *link, ts2::scenegraph::kPositions),
                                                                             ts2::scenegraph::vertexFloats(gm, *link, ts2::scenegraph::kNormals)));
                    }
                }
            }
        }
        if (const auto skeleton = assets.byName(0xE519C933, ts2::sim::skeletonModelName(age))) {
            compare(0xE519C933, modelName, ts2::sim::newFaceModel(*skeleton, age, group, grips, morphs));
        } else {
            std::printf("  age %d: no skeleton model\n", age);
            ++differ;
        }
    }
    std::printf("group 0x%08X: %d records the same, %d differ\n", group, same, differ);
    return differ == 0 ? 0 : 1;
}

// Compares a character file the engine wrote with one the game wrote (its resources by type and
// instance), resource by resource. Lists what differs and what only the game's holds.
int compareCharacterFile(const ts2::dbpf::PackageImage& made,
                         const std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, std::vector<std::uint8_t>>& theirs) {
    using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;
    const ts2::dbpf::PackageImage ours = ts2::dbpf::readImage(ts2::dbpf::writeImage(made));
    std::map<std::uint32_t, std::pair<int, int>> byType;  // same, differ
    std::set<Key> written;
    for (const auto& r : ours.resources) {
        const Key key{r.key.type, r.key.instance, r.key.instanceHigh};
        written.insert(key);
        const auto body = r.compressed ? ts2::qfs::decompress(r.bytes) : r.bytes;
        const auto it = theirs.find(key);
        if (it != theirs.end() && r.key.group == 0xFFFFFFFF && it->second == body) {
            ++byType[r.key.type].first;
        } else {
            ++byType[r.key.type].second;
            std::printf("  %08X:%08X %s (ours %zu bytes, game %zu)\n", r.key.type, r.key.instance,
                        it == theirs.end() ? "not in the game's file" : "differs", body.size(), it != theirs.end() ? it->second.size() : 0);
        }
    }
    int differ = 0;
    for (const auto& [type, counts] : byType) {
        std::printf("  %08X: %d the same, %d differ\n", type, counts.first, counts.second);
        differ += counts.second;
    }
    std::printf("Only in the game's file:");
    for (const auto& [key, body] : theirs) {
        if (!written.contains(key)) {
            std::printf(" %08X:%X", std::get<0>(key), std::get<1>(key));
        }
    }
    std::printf("\n%zu resources written, %d differ\n", ours.resources.size(), differ);
    return differ == 0 ? 0 : 1;
}

// Writes a newborn's whole character file as the engine writes it, from the birth's parents and
// face picks, and compares each resource with the game's file of that newborn. The values the game
// counts with time (the group number in names and the material name counter) and the eyebrow
// family the birth drew are taken from the game's file.
int checkNewbornFile(ResourceIndex& index, const char* install, const char* gameFile, const char* parentA, const char* packageB,
                     std::uint32_t groupB, const std::vector<std::string>& pickArgs) {
    const auto shapeIn = [](const char* path, std::optional<std::uint32_t> group) -> std::optional<ts2::faceshape::FaceShape> {
        ts2::dbpf::Package package{path};
        for (const auto& e : package.entries()) {
            if (e.key.type == ts2::faceshape::kType && (!group || e.key.group == *group)) {
                return ts2::faceshape::parse(package.read(e));
            }
        }
        return std::nullopt;
    };
    const auto a = shapeIn(parentA, std::nullopt);
    const auto b = shapeIn(packageB, groupB == 0 ? std::nullopt : std::optional<std::uint32_t>{groupB});
    const auto made = gameCharacter(index, gameFile);
    if (!a || !b || !made) {
        std::fprintf(stderr, "a parent's face shape or the file's start is missing\n");
        return 1;
    }
    using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;
    std::map<Key, std::vector<std::uint8_t>> theirs;
    ts2::dbpf::Package game{gameFile};
    for (const auto& e : game.entries()) {
        theirs[{e.key.type, e.key.instance, e.key.instanceHigh}] = game.read(e);
    }
    ts2::sim::NewbornLookParts parts;
    parts.parentA = *a;
    parts.parentB = *b;
    parts.picks = facePicksOf(pickArgs);
    // The look's genes and catalog parts, as the engine picks them for the genes.
    ts2::scene::Newborn baby;
    for (const auto& [key, body] : theirs) {
        if (std::get<0>(key) == 0xAC598EAC) {
            auto p = ts2::propset::parse(body);
            baby.gender = static_cast<std::uint32_t>(std::stoul(p["gender"], nullptr, 0));
            baby.skinTone = p["skincolor"];
            baby.hairTone = p["haircolor"];
            baby.eyeColour = p["eyecolor"];
        }
    }
    ts2::scene::Assets assets(install);
    const ts2::scene::BabyCatalog catalog = ts2::scene::readBabyCatalog(assets.skinPackages());
    const ts2::scene::NewbornOutfit outfit = ts2::scene::newbornOutfit(catalog, baby);
    constexpr std::uint32_t kHair = 1, kFace = 2, kBody = 8;
    if (!outfit.body || !outfit.skin.contains(kHair) || !outfit.skin.contains(kFace) || !outfit.skin.contains(kBody)) {
        std::fprintf(stderr, "the catalog has no full baby outfit for skin %s\n", baby.skinTone.c_str());
        return 1;
    }
    const auto link = [](const ts2::scenegraph::Link& l) { return ts2::dbpf::ResourceKey{l.type, l.group, l.instance, l.instanceHigh}; };
    parts.look = {baby.gender, baby.skinTone, baby.hairTone, baby.eyeColour, link(*outfit.body),
                  link(outfit.skin.at(kHair)), link(outfit.skin.at(kBody)), link(outfit.skin.at(kFace))};
    // The time-like values and the brow family, from the game's baby face shape and material.
    const auto sgKey = [](std::uint32_t type, const std::string& name) {
        const auto key = ts2::hash::scenegraphKey(name);
        return Key{type, key.instance, key.instanceHigh};
    };
    const auto shape = theirs.find(sgKey(0xFC6EB1F7, "age5_00_shpe"));
    if (shape == theirs.end()) {
        std::fprintf(stderr, "no baby face shape in %s\n", gameFile);
        return 1;
    }
    const std::string shapeText(shape->second.begin(), shape->second.end());
    const auto prefix = shapeText.find("#0x");
    const auto counterAt = shapeText.find("_0x");
    if (prefix == std::string::npos || counterAt == std::string::npos) {
        std::fprintf(stderr, "no group number or material counter in the baby face shape\n");
        return 1;
    }
    parts.group = static_cast<std::uint32_t>(std::stoul(shapeText.substr(prefix + 1, 10), nullptr, 16));
    parts.materialCounter = static_cast<std::uint32_t>(std::stoul(shapeText.substr(counterAt + 1), nullptr, 16));
    const std::string babyMaterial = ts2::sim::faceMaterialName(parts.materialCounter, 5, baby.gender == 1 ? 1 : 0) + "_txmt";
    if (const auto it = theirs.find(sgKey(0x49596978, babyMaterial)); it != theirs.end()) {
        const auto record = ts2::rcol::read(it->second);
        for (const auto& [key, value] : materialProperties(record)) {
            if (key == "cafOverlay1") {
                parts.browFamily = value.substr(0, value.find(','));
            }
        }
    }
    std::printf("group 0x%08X, material counter 0x%X, brow family %s\n", parts.group, parts.materialCounter, parts.browFamily.c_str());

    return compareCharacterFile(ts2::sim::newbornCharacterPackage(assets, made->templ, made->character, made->packs, parts), theirs);
}

// Writes the character file of a Sim Make New Character made, from the look in a character file
// the game wrote for one (its age data and lists, its material's eyebrow family, its names' group
// and counter) and the face template family given, and compares the two.
int checkMadeFile(ResourceIndex& index, const char* install, const char* gameFile, const std::string& family) {
    using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;
    auto made = gameCharacter(index, gameFile);
    if (!made) {
        std::fprintf(stderr, "the file's start is missing\n");
        return 1;
    }
    std::map<Key, std::vector<std::uint8_t>> theirs;
    ts2::dbpf::Package game{gameFile};
    for (const auto& e : game.entries()) {
        theirs[{e.key.type, e.key.instance, e.key.instanceHigh}] = game.read(e);
    }
    const auto aged = theirs.find({0xAC598EAC, 1, 0});
    const auto refs = theirs.find({0xAC506764, 1, 0});
    if (aged == theirs.end() || refs == theirs.end()) {
        std::fprintf(stderr, "no age data in %s\n", gameFile);
        return 1;
    }
    ts2::sim::MadeLookParts parts;
    parts.data = ts2::scene::readAgeData(ts2::propset::parse(aged->second), ts2::reflist::parse(refs->second));
    parts.faceFamily = family;
    for (const auto& [key, body] : theirs) {
        if (std::get<0>(key) == 0xFC6EB1F7) {
            const std::string text(body.begin(), body.end());
            const auto prefix = text.find("#0x");
            const auto counterAt = text.find("_0x");
            if (prefix != std::string::npos && counterAt != std::string::npos) {
                parts.group = static_cast<std::uint32_t>(std::stoul(text.substr(prefix + 1, 10), nullptr, 16));
                parts.materialCounter = static_cast<std::uint32_t>(std::stoul(text.substr(counterAt + 1), nullptr, 16));
            }
        } else if (std::get<0>(key) == 0x49596978) {
            const auto record = ts2::rcol::read(body);
            for (const auto& [name, value] : materialProperties(record)) {
                if (name == "cafOverlay0") {
                    parts.browFamily = value.substr(0, value.find(','));
                }
            }
        }
    }
    std::printf("group 0x%08X, material counter 0x%X, brow family %s\n", parts.group, parts.materialCounter, parts.browFamily.c_str());
    // Its names in every language, from the rows of the name tables its US English names are on.
    constexpr std::uint32_t kLiveText = 0x7F96C284;
    const bool female = parts.data.properties["gender"] == "1" || parts.data.properties["gender"] == "0x1";
    ts2::content::UiData text(install);
    const auto firstLists = text.languageTexts(kLiveText, female ? 0x97 : 0x96);
    const auto surnameLists = text.languageTexts(kLiveText, female ? 0xCC : 0x98);
    const auto rowOf = [](const std::vector<std::vector<std::string>>& lists, const std::string& name) {
        const auto it = lists.empty() ? std::vector<std::string>::const_iterator{} : std::find(lists[0].begin(), lists[0].end(), name);
        return lists.empty() || it == lists[0].end() ? std::size_t{0} : static_cast<std::size_t>(it - lists[0].begin());
    };
    const std::size_t firstRow = rowOf(firstLists, made->character.firstName), surnameRow = rowOf(surnameLists, made->character.surname);
    const auto firsts = ts2::sim::inEveryLanguage(firstLists, firstRow);
    const auto surnames = ts2::sim::inEveryLanguage(surnameLists, surnameRow);
    std::printf("names: first row %zu, surname row %zu\n", firstRow, surnameRow);
    for (std::size_t i = 0; i < firsts.size() && i < surnames.size(); ++i) {
        made->character.languageNames.emplace_back(firsts[i], surnames[i]);
    }
    ts2::scene::Assets assets(install);
    const auto catalog = ts2::scene::readOutfitCatalog(assets.skinCatalog(), assets.skinCatalogProducts());
    return compareCharacterFile(
        ts2::sim::madeCharacterPackage(assets, catalog.faceTemplates, made->templ, made->character, made->packs, parts), theirs);
}

// Makes a Sim of the age and gender of a game-made character file in outfit object `outfitGuid`
// (Make New Character's look, with the real catalog) and compares the hair of each outfit with the
// file's. The rest of the look is drawn at random, so only the hairs are compared.
int checkMadeHair(ResourceIndex& index, const char* install, const char* gameFile, std::uint32_t outfitGuid) {
    using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;
    std::map<Key, std::vector<std::uint8_t>> theirs;
    ts2::dbpf::Package game{gameFile};
    for (const auto& e : game.entries()) {
        theirs[{e.key.type, e.key.instance, e.key.instanceHigh}] = game.read(e);
    }
    const auto aged = theirs.find({0xAC598EAC, 1, 0});
    const auto refs = theirs.find({0xAC506764, 1, 0});
    if (aged == theirs.end() || refs == theirs.end()) {
        std::fprintf(stderr, "no age data in %s\n", gameFile);
        return 1;
    }
    const ts2::scene::AgeData file = ts2::scene::readAgeData(ts2::propset::parse(aged->second), ts2::reflist::parse(refs->second));
    // The outfit object's age data, each instance with its resource list.
    std::uint32_t group = 0;
    for (const auto& r : index.resources()) {
        if (r.entry.key.type == ts2::types::kObjectDefinition) {
            const auto bytes = index.read(r);
            if (bytes.size() > 64 + 2 * 0x10 && (bytes[64 + 2 * 0x0E] | bytes[65 + 2 * 0x0E] << 8 | bytes[64 + 2 * 0x0F] << 16 |
                                                 static_cast<std::uint32_t>(bytes[65 + 2 * 0x0F]) << 24) == outfitGuid) {
                group = r.entry.key.group;
            }
        }
    }
    std::vector<ts2::scene::AgeData> outfits;
    for (const auto& r : index.resources()) {
        if (r.entry.key.type == 0xAC598EAC && r.entry.key.group == group) {
            const auto* list = index.find({0xAC506764, group, r.entry.key.instance, r.entry.key.instanceHigh});
            outfits.push_back(ts2::scene::readAgeData(ts2::propset::parse(index.read(r)),
                                                      list ? ts2::reflist::parse(index.read(*list)) : std::vector<ts2::scenegraph::Link>{}));
        }
    }
    std::printf("outfit group 0x%08X, %zu age data\n", group, outfits.size());
    ts2::scene::Assets assets(install);
    const auto catalog = ts2::scene::readOutfitCatalog(assets.skinCatalog(), assets.skinCatalogProducts());
    ts2::scene::NewCharacter who;
    who.age = ts2::propset::number(file.properties, "age");
    who.gender = ts2::propset::number(file.properties, "gender");
    ts2::scene::OutfitRandom random(12345);
    const ts2::scene::MadeLook look = ts2::scene::makeLook(who, outfits, catalog, random);
    for (const auto& problem : look.problems) {
        std::printf("problem: %s\n", problem.c_str());
    }
    int differ = 0;
    for (int i = 0; i < 13; ++i) {
        const std::uint32_t key = (1u << i) << 16 | 1;
        const auto mine = look.data.lists.find(key);
        const auto game2 = file.lists.find(key);
        const auto show = [](const auto& lists, const auto& it) {
            return it == lists.end() || it->second.empty() ? std::string("none") : std::format("{:08X}", it->second.front().instance);
        };
        const std::string a = show(look.data.lists, mine), b = show(file.lists, game2);
        std::printf("  category 0x%04X: ours %s, game %s%s\n", 1u << i, a.c_str(), b.c_str(), a == b ? "" : "  <- differs");
        differ += a != b;
    }
    return differ == 0 ? 0 : 1;
}

// Compares the face shape a made Sim of face template `family` and skin tone `tone` gets with the one
// in a character file the game wrote for such a Sim, mesh by mesh, and the record's bytes.
int checkMadeShape(const char* install, const char* gameFile, const std::string& family, const std::string& tone) {
    ts2::dbpf::Package game{gameFile};
    std::optional<std::vector<std::uint8_t>> theirs;
    for (const auto& e : game.entries()) {
        if (e.key.type == ts2::faceshape::kType) {
            theirs = game.read(e);
        }
    }
    if (!theirs) {
        std::fprintf(stderr, "no face shape in %s\n", gameFile);
        return 1;
    }
    ts2::scene::Assets assets(install);
    const auto catalog = ts2::scene::readOutfitCatalog(assets.skinCatalog(), assets.skinCatalogProducts());
    const auto ours = ts2::scene::madeFaceShape(assets, catalog.faceTemplates, family, tone);
    if (!ours) {
        std::fprintf(stderr, "no face shape made\n");
        return 1;
    }
    const auto shape = ts2::faceshape::parse(*theirs);
    int differ = 0;
    for (const auto& m : ours->meshes) {
        const auto* g = shape.find(m.age, m.gender, m.group);
        if (g == nullptr || g->points.size() != m.points.size()) {
            std::printf("age %d gender %d group %d: %s\n", m.age, m.gender, m.group, g == nullptr ? "not in the game's" : "point counts differ");
            ++differ;
            continue;
        }
        int off = 0;
        float most = 0;
        std::size_t first = m.points.size();
        for (std::size_t i = 0; i < m.points.size(); ++i) {
            float d = 0;
            for (std::size_t k = 0; k < 3; ++k) {
                d = std::max(d, std::abs(m.points[i].offset[k] - g->points[i].offset[k]));
            }
            if (d > 1e-4f) {
                ++off;
                first = std::min(first, i);
            }
            most = std::max(most, d);
        }
        if (off > 0) {
            ++differ;
            std::printf("age %d gender %d group %d: %d of %zu points off, most %g, first corner %zu\n", m.age, m.gender, m.group, off,
                        m.points.size(), most, first);
        }
    }
    const auto bytes = ts2::faceshape::write(*ours);
    std::printf("%d meshes differ, record bytes %s (ours %zu, game %zu)\n", differ, bytes == *theirs ? "the same" : "differ", bytes.size(),
                theirs->size());
    return differ == 0 && bytes == *theirs ? 0 : 1;
}

// Lists, for each age and gender, the first catalog skin face part with the triangle groups of its
// mesh and their corner counts, and every face region set's name, ages and genders.
int listFaceBases(const char* install) {
    ts2::scene::Assets assets(install);
    std::vector<std::pair<ts2::scenegraph::Link, std::map<std::string, std::string>>> faces;
    assets.forEachSkinResource(0xEBCF3E27, [&](const ts2::scenegraph::Link& key, const std::vector<std::uint8_t>& bytes) {
        try {
            auto p = ts2::propset::parse(bytes);
            const auto number = [&](const char* k) { return p.contains(k) ? std::stoul(p[k], nullptr, 0) : 0ul; };
            if ((number("category") & 0x80) && (number("outfit") & 2) && (number("species") == 0 || (number("species") & 1))) {
                faces.push_back({key, std::move(p)});
            }
        } catch (const std::exception&) {
        }
    });
    for (const std::uint32_t age : {0x20u, 0x01u, 0x02u, 0x04u, 0x08u, 0x10u}) {
        for (const std::uint32_t gender : {1u, 2u}) {
            for (const auto& [key, p] : faces) {
                const auto number = [&](const char* k) { return p.contains(k) ? std::stoul(p.at(k), nullptr, 0) : 0ul; };
                if (!(number("age") & age) || !(number("gender") & gender)) {
                    continue;
                }
                const auto refs = assets.skinResource({0xAC506764, key.group, key.instance, key.instanceHigh});
                const auto links = refs ? ts2::reflist::parse(*refs) : std::vector<ts2::scenegraph::Link>{};
                const std::size_t shape = number("shapekeyidx");
                const auto mesh = shape < links.size() ? ts2::scene::shapeMesh(assets, links[shape]) : std::nullopt;
                std::printf("age 0x%02X gender %u: %s (%08X:%08X)", age, gender, p.contains("name") ? p.at("name").c_str() : "?", key.group,
                            key.instance);
                if (mesh) {
                    for (const auto& g : mesh->groups) {
                        std::printf(" [%s %zu]", g.name.c_str(), g.indices.size());
                    }
                }
                std::printf("\n");
                break;
            }
        }
    }
    assets.forEachSkinResource(0x8C93BF6C, [&](const ts2::scenegraph::Link& key, const std::vector<std::uint8_t>& bytes) {
        try {
            const auto p = ts2::propset::parse(bytes);
            const auto text = [&](const char* k) { const auto it = p.find(k); return it != p.end() ? it->second : std::string("-"); };
            std::printf("region %s age %s gender %s species %s (%08X:%08X)\n", text("name").c_str(), text("age").c_str(), text("gender").c_str(),
                        text("species").c_str(), key.group, key.instance);
        } catch (const std::exception& ex) {
            std::printf("region (%08X:%08X) unreadable: %s\n", key.group, key.instance, ex.what());
        }
    });
    return 0;
}

int lookupGuids(ResourceIndex& index, int count, char** guids) {
    ts2::sim::IndexCodeSource code(index);
    for (int i = 0; i < count; ++i) {
        const auto guid = static_cast<std::uint32_t>(std::stoul(guids[i], nullptr, 16));
        if (const auto* type = code.objectType(guid)) {
            const auto* master = code.multiTileMaster(*type);
            std::printf("0x%08X  group 0x%08X  master 0x%08X  %s\n", guid, type->group, master != nullptr ? master->definition.guid() : 0u,
                        type->definition.name.c_str());
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
                     "  simrun <install-folder> --progress [--hood <folder>] [--json <file>]\n"
                     "  simrun <install-folder | file.package> --run <group> <instance> [args...] [--trace]\n"
                     "  simrun <install-folder | file.package> --guid <hex-guid> [...]\n"
                     "  simrun <install-folder> --lot <lot.package> [ticks [--trace-tick <tick>] [--trace-object <id>] [--on-screen] [--pie <sim> [--pie-trace <object>]] [--queue <sim> <object> <choice> [--queue-at <tick>] [--queue-again <tick>] [--cancel <tick>]] [--ground <x> <y> <level> [--ground-later <tick> <x> <y> <level>]] [--go-here <sim> <tick> <x> <y> <level>]... [--portals] [--seed <n>] [--motive <sim> <motive> <value>]... [--camera <x> <y>]]\n"
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
        if (mode == "--progress") {
            std::shared_ptr<const ts2::sim::SavedNeighborhood> hood;
            std::string json;
            for (int i = 3; i + 1 < argc; ++i) {
                if (std::string_view(argv[i]) == "--hood") {
                    const std::filesystem::path folder(argv[++i]);
                    ts2::content::addCharacters(index, folder);
                    ts2::sim::IndexCodeSource code(index);
                    hood = ts2::sim::savedNeighborhood(ts2::content::loadNeighborhood(ts2::content::neighborhoodPackage(folder)), code);
                } else if (std::string_view(argv[i]) == "--json") {
                    json = argv[++i];
                }
            }
            return progress(index, hood, wantDefinitionsFor(argv[1]), scriptsFor(argv[1], index), json);
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
            std::optional<std::uint32_t> seed;
            std::vector<std::array<float, 3>> motives;
            std::optional<std::array<float, 2>> camera;
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
                } else if (std::string_view(argv[i]) == "--messages") {
                    showMessages = true;
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
                } else if (std::string_view(argv[i]) == "--motive" && i + 3 < argc && motives.size() < 64) {
                    motives.push_back({std::stof(argv[i + 1]), std::stof(argv[i + 2]), std::stof(argv[i + 3])});
                    i += 3;
                } else if (std::string_view(argv[i]) == "--camera" && i + 2 < argc) {
                    camera = std::array<float, 2>{std::stof(argv[i + 1]), std::stof(argv[i + 2])};
                    i += 2;
                } else if (std::string_view(argv[i]) == "--seed" && i + 1 < argc) {
                    seed = static_cast<std::uint32_t>(std::stoul(argv[++i]));
                } else if (std::string_view(argv[i]) == "--portals") {
                    portals = true;
                } else if (std::string_view(argv[i]) == "--save-compare" && i + 1 < argc) {
                    saveCompare = std::filesystem::path(argv[++i]);
                } else if (std::string_view(argv[i]) == "--save-to" && i + 1 < argc) {
                    saveTo = std::filesystem::path(argv[++i]);
                } else if (std::string_view(argv[i]) == "--save-folder" && i + 1 < argc) {
                    saveFolder = std::filesystem::path(argv[++i]);
                } else if (std::string_view(argv[i]) == "--baby-names" && i + 2 < argc) {
                    babyNames = std::pair{std::string(argv[i + 1]), std::string(argv[i + 2])};
                    i += 2;
                } else if (std::string_view(argv[i]) == "--last-hood" && i + 1 < argc) {
                    lastHood = static_cast<std::int16_t>(std::stoi(argv[++i]));
                } else if (std::string_view(argv[i]) == "--list-objects") {
                    listObjects = true;
                } else if (std::string_view(argv[i]) == "--saved-queues") {
                    savedQueues = true;
                } else if (std::string_view(argv[i]) == "--saved-animations") {
                    savedAnimations = true;
                } else if (std::string_view(argv[i]) == "--upgrade-check") {
                    upgradeCheck = true;
                } else if (std::string_view(argv[i]) == "--ground" && i + 3 < argc) {
                    groundAt = std::array<float, 3>{std::stof(argv[i + 1]), std::stof(argv[i + 2]), std::stof(argv[i + 3])};
                    i += 3;
                }
            }
            return runLot(index, std::filesystem::path(argv[1]), std::filesystem::path(argv[3]), ticks, traceTick, traceObject, onScreen,
                          personData, overlays, motion, pieSim, pieTrace, queueChoice, queueTick, queueAgainTick, cancelTick, groundAt, portals, groundLater, goHere, seed, motives, camera);
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
        if (mode == "--check-face-shape" && argc >= 7) {
            // simrun <install> --check-face-shape <child> <parentA> <packageB> <groupB> region=A|B...
            return checkFaceShape(argv[1], argv[3], argv[4], argv[5], static_cast<std::uint32_t>(std::stoul(argv[6], nullptr, 0)),
                                  std::vector<std::string>(argv + 7, argv + argc));
        }
        if (mode == "--check-scene-records") {
            // simrun <install> --check-scene-records [<package>...]
            return checkSceneRecords(argv[1], std::vector<std::string>(argv + 3, argv + argc));
        }
        if (mode == "--check-baby-models" && argc >= 4) {
            return checkBabyModels(argv[1], argv[3], argc >= 5 ? argv[4] : nullptr);
        }
        if (mode == "--face-bases") {
            return listFaceBases(argv[1]);
        }
        if (mode == "--fit-baby-face" && argc >= 7) {
            return fitBabyFace(argv[1], argv[3], argv[4], argv[5], static_cast<std::uint32_t>(std::stoul(argv[6], nullptr, 0)));
        }
        if (mode == "--check-baby-look" && argc >= 4) {
            return checkBabyLook(argv[1], argv[3]);
        }
        if (mode == "--check-new-character" && argc >= 4) {
            return checkNewCharacter(index, argv[3]);
        }
        if (mode == "--check-newborn-file" && argc >= 7) {
            // simrun <install> --check-newborn-file <game file> <parentA> <packageB> <groupB> region=A|B...
            return checkNewbornFile(index, argv[1], argv[3], argv[4], argv[5], static_cast<std::uint32_t>(std::stoul(argv[6], nullptr, 0)),
                                    std::vector<std::string>(argv + 7, argv + argc));
        }
        if (mode == "--check-made-hair" && argc >= 5) {
            // simrun <install> --check-made-hair <game file> <outfit GUID>
            return checkMadeHair(index, argv[1], argv[3], static_cast<std::uint32_t>(std::stoul(argv[4], nullptr, 0)));
        }
        if (mode == "--check-made-file" && argc >= 5) {
            // simrun <install> --check-made-file <game file> <face template family>
            return checkMadeFile(index, argv[1], argv[3], argv[4]);
        }
        if (mode == "--check-made-shape" && argc >= 6) {
            // simrun <install> --check-made-shape <game file> <face template family> <skin tone>
            return checkMadeShape(argv[1], argv[3], argv[4], argv[5]);
        }
        if (mode == "--guid") {
            return lookupGuids(index, argc - 3, argv + 3);
        }
        if (mode == "--check-hood-save" && argc >= 7) {
            return checkHoodSave(argv[3], argv[4], argv[5], static_cast<std::uint32_t>(std::stoul(argv[6], nullptr, 0)));
        }
        if (mode == "--check-new-neighbors" && argc >= 5) {
            return checkNewNeighborRecords(argv[3], argv[4]);
        }
        if (mode == "--compare-descriptions" && argc >= 5) {
            return compareSimDescriptions(argv[3], argv[4]);
        }
        if (mode == "--scene-record" && argc >= 5) {
            return printSceneRecord(argv[3], static_cast<std::uint32_t>(std::stoul(argv[4], nullptr, 0)));
        }
        if (mode == "--swap-resource" && argc >= 8) {
            // simrun <install> --swap-resource <lot> <donor lot> <type> <instance> <out>: the lot with
            // one resource taken from the donor as it is stored there, for A/B tests in the game.
            const auto type = static_cast<std::uint32_t>(std::stoul(argv[5], nullptr, 0));
            const auto instance = static_cast<std::uint32_t>(std::stoul(argv[6], nullptr, 0));
            const auto readAll = [](const char* path) {
                std::ifstream in(std::filesystem::path(path), std::ios::binary);
                return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
            };
            auto image = ts2::dbpf::readImage(readAll(argv[3]));
            const auto donor = ts2::dbpf::readImage(readAll(argv[4]));
            const auto match = [&](const ts2::dbpf::StoredResource& r) { return r.key.type == type && r.key.instance == instance; };
            const auto from = std::find_if(donor.resources.begin(), donor.resources.end(), match);
            const auto to = std::find_if(image.resources.begin(), image.resources.end(), match);
            if (from == donor.resources.end() || to == image.resources.end()) {
                std::printf("resource not in both packages\n");
                return 1;
            }
            to->bytes = from->bytes;
            to->compressed = from->compressed;
            to->uncompressedSize = from->uncompressedSize;
            const auto written = ts2::dbpf::writeImage(image);
            ts2::dbpf::verifyImage(written, image);
            if (!writeBytes(std::filesystem::path(argv[7]), written)) {
                return 1;
            }
            std::printf("wrote %zu bytes\n", written.size());
            return 0;
        }
        if (mode == "--model-joints" && argc >= 4) {
            // simrun <install> --model-joints <model>: the rest place of each joint of a model's
            // resource node ("<model>_cres"), as a saved scene record's joints are compared with.
            ts2::scene::Assets assets(argv[1]);
            const auto cres = assets.byName(0xE519C933, std::string(argv[3]) + "_cres");
            if (!cres) {
                std::printf("no resource node %s_cres\n", argv[3]);
                return 1;
            }
            const auto tree = ts2::scenegraph::parseResourceNode(*cres);
            std::map<int, std::string> joints;
            for (std::size_t i = 0; i < tree.nodes.size(); ++i) {
                const auto& n = tree.nodes[i];
                std::printf("%zu: kind %d '%s' joint %d %g %g %g / %g %g %g %g, children", i, static_cast<int>(n.kind), n.name.c_str(), n.joint,
                            n.translation[0], n.translation[1], n.translation[2], n.rotation[0], n.rotation[1], n.rotation[2], n.rotation[3]);
                for (const auto c : n.children) {
                    std::printf(" %u", c);
                }
                std::printf("\n");
                if (n.joint >= 0) {
                    joints[n.joint] = n.name;
                }
            }
            // Each mesh group's bones, by joint name, for the shapes the tree draws.
            for (const auto& n : tree.nodes) {
                for (const auto l : n.shapeLinks) {
                    const auto shpe = l < tree.container.links.size() ? assets.byKey(tree.container.links[l]) : std::nullopt;
                    if (!shpe) {
                        continue;
                    }
                    const auto shape = ts2::scenegraph::parseShape(*shpe);
                    for (const auto& node : shape.geometryNodes) {
                        const auto gmnd = assets.byName(0x7BA3838C, node);
                        for (const auto& link : gmnd ? ts2::scenegraph::parseContainer(*gmnd).links : decltype(ts2::scenegraph::parseContainer(*gmnd).links){}) {
                            const auto bytes = link.type == 0xAC4F8687 ? assets.byKey(link) : std::nullopt;
                            if (!bytes) {
                                continue;
                            }
                            for (const auto& g : ts2::scenegraph::parseMesh(*bytes).groups) {
                                std::printf("group '%s' bones:", g.name.c_str());
                                for (const auto b : g.bones) {
                                    std::printf(" %s", joints.contains(b) ? joints[b].c_str() : std::to_string(b).c_str());
                                }
                                std::printf("\n");
                            }
                        }
                    }
                }
            }
            return 0;
        }
        if (mode == "--sim-scenes") {
            return surveySimScenes(argc - 3, argv + 3);
        }
        if (mode == "--check-sim-places" && argc >= 4) {
            return checkSimPlaces(argv[1], argv[3]);
        }
        if (mode == "--find-start-tick" && argc >= 5) {
            // simrun <install> --find-start-tick <lot> <tick>: the scene record entries started at that tick.
            const auto tick = static_cast<std::int32_t>(std::stol(argv[4]));
            ts2::dbpf::Package package{argv[3]};
            for (const auto& e : package.entries()) {
                if (e.key.type != ts2::types::kSceneObject) {
                    continue;
                }
                if (const auto r = ts2::cobject::readRecord(package.read(e))) {
                    for (const auto& an : r->animatable.animations) {
                        if (an.instance.startTick == tick) {
                            std::printf("object %u chain %d: '%s' type '%s' plays %d time %g\n", e.key.instance, static_cast<int>(r->chain),
                                        an.request.name.c_str(), an.request.type.c_str(), an.instance.plays, std::bit_cast<float>(an.instance.time));
                        }
                    }
                }
            }
            return 0;
        }
        if (mode == "--diff-objects" && argc >= 6) {
            // simrun <install> --diff-objects <lot> <a> <b> [<lot of b>]
            return diffObjects(argv[3], static_cast<std::uint32_t>(std::stoul(argv[4], nullptr, 0)), static_cast<std::uint32_t>(std::stoul(argv[5], nullptr, 0)),
                               argc >= 7 ? argv[6] : nullptr);
        }
        if (mode == "--check-object-places" && argc >= 4) {
            return checkObjectPlaces(argv[1], argv[3]);
        }
        if (mode == "--record" && argc >= 5) {
            return printRecord(argv[3], static_cast<std::uint32_t>(std::stoul(argv[4], nullptr, 0)));
        }
        if (mode == "--group-count") {
            // How many resources of each type the install holds in each group given.
            for (int g = 3; g < argc; ++g) {
                const auto group = static_cast<std::uint32_t>(std::stoul(argv[g], nullptr, 0));
                std::map<std::uint32_t, int> byType;
                for (const auto& r : index.resources()) {
                    if (r.entry.key.group == group) {
                        ++byType[r.entry.key.type];
                    }
                }
                std::printf("group 0x%08X:", group);
                for (const auto& [type, n] : byType) {
                    std::printf(" %08X x%d", type, n);
                }
                std::printf("\n");
            }
            return 0;
        }
        if (mode == "--line-flags" && argc >= 4) {
            return printLineFlags(index, argv[3], argc - 4, argv + 4);
        }
        if (mode == "--frame-shapes") {
            return surveyFrameShapes(index, argc - 3, argv + 3);
        }
        if (mode == "--attribute-counts") {
            return checkAttributeCounts(index, argc - 3, argv + 3);
        }
        if (mode == "--person-extras") {
            return surveyPersonExtras(argc - 3, argv + 3);
        }
        if (mode == "--person-states") {
            return surveyPersonStates(argc - 3, argv + 3);
        }
        if (mode == "--save-fields") {
            return dumpSaveFields(argv[1], argc - 3, argv + 3);
        }
        if (mode == "--record-fields") {
            return surveyRecordFields(argc - 3, argv + 3);
        }
        if (mode == "--type-tables") {
            return checkTypeTables(index, argc - 3, argv + 3);
        }
        if (mode == "--check-clip-times" && argc >= 4) {
            // simrun <install> --check-clip-times <lot>: scene record entries whose time is past their clip's
            // length, which a load wraps in a loop that never ends for a clip of length 0.
            ts2::scene::Assets assets(argv[1]);
            ts2::scene::AnimationLibrary animations(assets);
            ts2::dbpf::Package package{argv[3]};
            int entries = 0, bad = 0;
            for (const auto& e : package.entries()) {
                if (e.key.type != ts2::types::kSceneObject) {
                    continue;
                }
                const auto r = ts2::cobject::readRecord(package.read(e));
                for (const auto& an : r ? r->animatable.animations : std::vector<ts2::cobject::Animation>{}) {
                    if (an.kind != ts2::cobject::Animation::Kind::Clip) {
                        continue;
                    }
                    ++entries;
                    const float time = std::bit_cast<float>(an.instance.time);
                    const auto clip = animations.find(an.request.name);
                    const float length = clip ? clip->duration : -1.0f;
                    if (time > 0 && (length <= 0 || time > length + 1.0f)) {
                        ++bad;
                        std::printf("  object %u '%s' time %g, clip length %g\n", e.key.instance, an.request.name.c_str(), time, length);
                    }
                }
            }
            std::printf("%d clip entries, %d with a time past their clip's length\n", entries, bad);
            return bad == 0 ? 0 : 1;
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
