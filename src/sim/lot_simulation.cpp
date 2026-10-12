#include "sim/lot_simulation.hpp"

#include "content/install.hpp"
#include "content/lot.hpp"
#include "content/neighborhood.hpp"
#include "content/scripts.hpp"
#include "content/wants.hpp"
#include "formats/resource_types.hpp"
#include "sim/lua_runtime.hpp"
#include "sim/object_reset.hpp"

#include <algorithm>
#include <optional>
#include <set>
#include <vector>

namespace ts2::sim {

namespace {

// Scripts may run this many primitives in one tick before they are stopped.
constexpr std::size_t kMaxStepsPerRun = 10000;

// The install's object packages and the lot's neighborhood's character files.
content::ResourceIndex loadCode(const std::filesystem::path& install, const std::filesystem::path& lot) {
    content::ResourceIndex index;
    for (const auto& path : content::objectLoadOrder(content::findProducts(install))) {
        index.addPackage(path);
    }
    content::addCharacters(index, content::neighborhoodOfLot(lot));
    return index;
}

std::shared_ptr<LuaRuntime> loadScripts(const std::filesystem::path& install, CodeSource& code) {
    const auto package = content::objectScriptsPackage(install);
    if (!package) {
        return nullptr;
    }
    const auto loaded = content::loadObjectScripts(*package);
    auto runtime = std::make_shared<LuaRuntime>();
    runtime->installedPacks = content::installedPacksMask(content::findProducts(install));
    runtime->load(loaded.modules, loaded.scripts, &code);
    return runtime;
}

} // namespace

LotSimulation::LotSimulation(const std::filesystem::path& install, const std::filesystem::path& lot)
    : index_(loadCode(install, lot)), code_(index_), vm_(code_, {kMaxStepsPerRun, 64}, &world_) {
    const content::Lot saved = content::loadLot(lot);
    report_ = buildWorld(saved, code_, world_);
    std::copy_n(saved.globals.begin(), std::min(saved.globals.size(), vm_.globals.size()), vm_.globals.begin());
    const auto hood = content::neighborhoodPackage(content::neighborhoodOfLot(lot));
    if (std::filesystem::exists(hood)) {
        const content::Neighborhood loaded = content::loadNeighborhood(hood);
        findHousehold(loaded, lot);
        world_.setNeighborhood(savedNeighborhood(loaded));
        findCharacterFiles();
    }
    if (const auto wants = content::wantsPackage(install)) {
        world_.setWantDefinitions(std::make_shared<content::WantDefinitions>(content::loadWantDefinitions(*wants)));
    }
    world_.setScripts(loadScripts(install, code_));
    startObjects();
}

void LotSimulation::findHousehold(const content::Neighborhood& hood, const std::filesystem::path& lot) {
    const auto number = content::lotNumber(lot);
    if (!number) {
        return;
    }
    for (const auto& [instance, family] : hood.families) {
        if (family.lot != *number || family.members.empty()) {
            continue;
        }
        for (const std::uint32_t guid : family.members) {
            const SimObject* sim = world_.nextAfter(0, [&](const SimObject& o) { return o.isPerson() && o.definition.guid() == guid; });
            if (sim != nullptr) {
                household_.push_back(sim->id);
            }
        }
        return;
    }
}

std::vector<std::uint8_t> LotSimulation::simImage(std::int16_t sim, std::uint32_t instance) {
    const SimObject* object = world_.find(sim);
    const auto* image = object ? index_.find({types::kImage, object->group, instance}) : nullptr;
    return image ? index_.read(*image) : std::vector<std::uint8_t>{};
}

std::optional<std::filesystem::path> LotSimulation::characterFile(std::int16_t neighbor) const {
    const SavedNeighbor* n = world_.neighbor(neighbor);
    const auto it = n ? characterFiles_.find(n->guid) : characterFiles_.end();
    return it != characterFiles_.end() ? std::optional<std::filesystem::path>(it->second) : std::nullopt;
}

void LotSimulation::findCharacterFiles() {
    characterFiles_.clear();
    std::vector<content::ResourceIndex::Resource> definitions;
    for (const auto& r : index_.resources()) {
        if (r.entry.key.type == types::kObjectDefinition && index_.packagePath(r.package).parent_path().filename() == "Characters") {
            definitions.push_back(r);
        }
    }
    for (const auto& r : definitions) {
        try {
            characterFiles_.emplace(objd::parse(index_.read(r)).guid(), index_.packagePath(r.package));
        } catch (const std::exception&) {
        }
    }
}

// What the game does with each object once the lot is loaded: it runs the object's Load function, and
// an object whose type changed since the save is reset, runs Init and then its Reset function. The
// parts of a multi-tile object are all reset before any of them runs Init. Object functions run to
// the end there and then, each within the usual step limit.
void LotSimulation::startObjects() {
    constexpr std::uint16_t kLoad = 2, kInit = 0, kReset = 0x2B;  // OBJf entries
    const std::set<std::int16_t> changed(report_.changedType.begin(), report_.changedType.end());
    std::vector<std::int16_t> ids;
    world_.nextAfter(0, [&](const SimObject& o) {
        ids.push_back(o.id);
        return false;
    });
    // Runs object function `entry` of object `id`, if it still exists and has one.
    auto runFunction = [&](std::int16_t id, std::uint16_t entry) {
        SimObject* me = world_.find(id);
        const ObjectType* type = me ? code_.objectType(me->definition.guid()) : nullptr;
        const auto* functions = type ? code_.objectFunctions(*type) : nullptr;
        if (functions == nullptr || entry >= functions->size() || (*functions)[entry].action == 0) {
            return;
        }
        note(vm_.runOnce(*me, type->group, (*functions)[entry].action), id);
    };
    // Init and Reset after a reset, then the two placement fields Init may not set.
    auto finishReset = [&](std::int16_t id) {
        runFunction(id, kInit);
        SimObject* me = world_.find(id);
        if (const ObjectType* type = me ? code_.objectType(me->definition.guid()) : nullptr) {
            finishSingleTileReset(*me, *type);
        }
        runFunction(id, kReset);
    };
    std::vector<std::int16_t> resetParts;  // of multi-tile objects
    for (const std::int16_t id : ids) {
        runFunction(id, kLoad);
        SimObject* object = world_.find(id);
        const ObjectType* type = object ? code_.objectType(object->definition.guid()) : nullptr;
        if (type == nullptr || !changed.contains(id)) {
            continue;
        }
        resetObject(*object, *type, code_);
        ++resets_;
        if (isMultiTilePart(*type)) {
            resetParts.push_back(id);
        } else {
            finishReset(id);
        }
    }
    for (const std::int16_t id : resetParts) {
        finishReset(id);
    }
    world_.purge();
}

void LotSimulation::note(const RunResult& r, std::int16_t object) {
    if (r.outcome == Outcome::True || r.outcome == Outcome::False || r.outcome == Outcome::Sleeping) {
        return;
    }
    std::string why = outcomeName(r.outcome);
    why += r.reason.empty() ? "" : ": " + r.reason;
    why += r.detail.empty() ? "" : " (" + r.detail + ")";
    Stop& stop = stops_[why];
    if (stop.count++ == 0) {
        stop.first = r;
        stop.object = object;
        stop.tick = ticks_;
    }
}

void LotSimulation::tick() {
    world_.advanceClock(kNormalTickMs);
    std::vector<std::int16_t> ids;
    world_.nextAfter(0, [&](const SimObject& o) {
        ids.push_back(o.id);
        return false;
    });
    for (const std::int16_t id : ids) {
        SimObject* object = world_.find(id);
        if (object == nullptr || !object->thread.isRunning()) {
            continue;
        }
        if (object->thread.sleepTicks > 0) {
            --object->thread.sleepTicks;
            continue;
        }
        const RunResult r = vm_.resume(*object);
        if (r.outcome != Outcome::Sleeping) {
            note(r, id);
        }
    }
    world_.purge();
    advanceClock(vm_.globals);
    ++ticks_;
}

} // namespace ts2::sim
