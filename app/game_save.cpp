// Saving a lot. Yes on the quit box's save question (quitting, or leaving the lot) writes what Buy and
// Build mode changed into the lot's package: its floors, its terrain paint, its walls built and taken
// down and their coverings, the objects bought, moved and sold (doors and windows too), and the money
// those took, in the lot's simulator state and the household's family record in the neighborhood
// package. What happened in Live mode isn't saved yet: the log says so. Each package is written again
// whole and checked before it replaces the old one, the first time keeping the file as the game last
// wrote it beside it (".neots2bak").
#include "game.hpp"

#include "content/lot_save.hpp"
#include "content/neighborhood.hpp"
#include "diag/log.hpp"
#include "formats/objm.hpp"
#include "formats/resource_types.hpp"
#include "scene/lot_parts.hpp"
#include "sim/object_save.hpp"
#include "sim/type_signature.hpp"
#include "sim/vm_execution.hpp"
#include "sim/wall_save.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

namespace app {

namespace {

namespace fs = std::filesystem;

// The lot packages a new object's scene record may come from: the lot's own first, then the rest of
// its neighborhood's, then other neighborhoods'.
std::vector<fs::path> templateLots(const fs::path& lotFile) {
    std::vector<fs::path> out{lotFile};
    const fs::path lots = lotFile.parent_path();
    std::error_code error;
    auto addFolder = [&](const fs::path& folder) {
        for (const auto& entry : fs::directory_iterator(folder, error)) {
            if (entry.path().extension() == ".package" && entry.path() != lotFile) {
                out.push_back(entry.path());
            }
        }
    };
    addFolder(lots);
    for (const auto& hood : fs::directory_iterator(lots.parent_path().parent_path(), error)) {
        if (hood.is_directory() && hood.path() != lots.parent_path()) {
            addFolder(hood.path() / "Lots");
        }
    }
    return out;
}

// What a moved object's record takes: its place, data and slots.
ts2::xobj::SavedObject placeNow(const ts2::sim::SimObject& o) {
    ts2::xobj::SavedObject s;
    s.header.location = {static_cast<float>(o.x) / 65536.0f, static_cast<float>(o.y) / 65536.0f};
    s.header.value1 = o.level;
    std::copy_n(o.data.begin(), std::min(o.data.size(), s.data.size()), s.data.begin());
    s.tail.emplace();
    s.tail->slots.marker = -1;
    s.tail->slots.values = o.slots;
    return s;
}

} // namespace

void Game::touchObject(std::int16_t object) {
    if (!lotLife_ || object == 0) {
        return;
    }
    edits_.objects = true;
    auto& world = lotLife_->simulation().world();
    for (const auto part : lotLife_->partsOf(object)) {
        edits_.touched.insert(part);
        if (const auto* o = world.find(part); o != nullptr && o->data[ts2::sim::vm_detail::kContainerData] != 0) {
            edits_.touched.insert(o->data[ts2::sim::vm_detail::kContainerData]);
        }
    }
    edits_.touched.insert(object);
}

bool Game::saveLot() {
    if (!inLot_ || !lotLife_ || !buildLot_ || lotFile_.empty()) {
        return false;
    }
    if (lotRan_) {
        ts2::diag::warning(std::format("saving lot {}: what happened in Live mode can't be saved yet and is left as it was", enteredLot_));
    }
    const bool walls = edits_.walls || edits_.coverings;
    if (!edits_.floors && !edits_.paint && !walls && edits_.touched.empty()) {
        ts2::diag::step(std::format("saving lot {}: nothing that can be saved yet was changed", enteredLot_));
        return true;
    }
    try {
        ts2::content::LotSaveParts parts;
        parts.floors = edits_.floors;
        parts.terrainPaint = edits_.paint;
        ts2::content::LotChanges changes;
        std::vector<ts2::dbpf::ResourceKey> removed;
        bool wallsSaved = false, objectsSaved = edits_.touched.empty();
        std::set<std::int16_t> unsavedObjects;
        {
            ts2::dbpf::Package original(lotFile_);
            if (walls) {
                auto wallChanges = ts2::sim::wallChanges(original, *buildLot_);
                for (const auto& problem : wallChanges.problems) {
                    ts2::diag::warning(std::format("saving lot {}: {}; the walls and their coverings are left as they were", enteredLot_, problem));
                }
                wallsSaved = wallChanges.problems.empty();
                if (wallsSaved) {
                    ts2::diag::info(std::format("saving lot {}: {} walls built, {} taken down", enteredLot_, wallChanges.built, wallChanges.removed));
                }
                changes.resources = std::move(wallChanges.resources);
            }
            if (!edits_.touched.empty()) {
                auto objectChanges = saveObjects(original);
                for (const auto& problem : objectChanges.problems) {
                    ts2::diag::warning(std::format("saving lot {}: {}; it is left as it was", enteredLot_, problem));
                }
                objectsSaved = objectChanges.problems.empty();
                ts2::diag::info(std::format("saving lot {}: {} objects bought, {} moved, {} sold", enteredLot_, objectChanges.bought, objectChanges.moved,
                                            objectChanges.sold));
                changes.resources.insert(changes.resources.end(), std::make_move_iterator(objectChanges.resources.begin()),
                                         std::make_move_iterator(objectChanges.resources.end()));
                removed = std::move(objectChanges.removed);
                unsavedObjects = std::move(objectChanges.failed);
            }
            // The money of the objects saved; what those left as they were took stays unsaved with them.
            std::int32_t objectFunds = 0;
            for (const auto& [object, funds] : edits_.objectFunds) {
                if (!unsavedObjects.contains(object)) {
                    objectFunds += funds;
                }
            }
            parts.fundsChange = (parts.floors ? edits_.floorFunds : 0) + (wallsSaved ? edits_.coveringFunds + edits_.wallFunds : 0) + objectFunds;
            auto rest = ts2::content::lotChanges(original, *buildLot_, parts);
            changes.resources.insert(changes.resources.end(), std::make_move_iterator(rest.resources.begin()), std::make_move_iterator(rest.resources.end()));
            changes.problems = std::move(rest.problems);
            changes.funds = rest.funds;
        }
        for (const auto& problem : changes.problems) {
            ts2::diag::warning(std::format("saving lot {}: {}", enteredLot_, problem));
        }
        if (!changes.resources.empty() || !removed.empty()) {
            const auto report = ts2::content::rewritePackage(lotFile_, changes.resources, removed);
            ts2::diag::step(std::format("saved lot {}: {} resources written, {} added, {} removed, {} kept{}", enteredLot_, report.replaced, report.added,
                                        report.removed, report.copied,
                                        report.madeBackup ? ", the old package kept as " + report.backup.filename().string() : ""));
        } else {
            ts2::diag::step(std::format("saving lot {}: it was already saved so", enteredLot_));
        }
        if (changes.funds) {
            const auto hoodFile = ts2::content::neighborhoodPackage(ts2::content::neighborhoodOfLot(lotFile_));
            std::vector<ts2::content::ReplacedResource> family;
            {
                ts2::dbpf::Package hood(hoodFile);
                family = ts2::content::familyFundsChanges(hood, static_cast<std::int32_t>(enteredLot_), *changes.funds);
            }
            if (!family.empty()) {
                ts2::content::rewritePackage(hoodFile, family);
            }
            ts2::diag::step(std::format("saved lot {}: the household has {} (family records written: {})", enteredLot_, *changes.funds, family.size()));
        }
        // Saved: what was saved now counts from here.
        edits_.floors = edits_.paint = false;
        edits_.floorFunds = 0;
        if (wallsSaved) {
            edits_.walls = edits_.coverings = false;
            edits_.coveringFunds = edits_.wallFunds = 0;
        }
        // Objects left as they were stay to save, with their money; the rest count from here.
        std::erase_if(edits_.touched, [&](std::int16_t object) { return !unsavedObjects.contains(object); });
        std::erase_if(edits_.objectFunds, [&](const auto& entry) { return !unsavedObjects.contains(entry.first); });
        edits_.objects = !edits_.touched.empty();
        return changes.problems.empty() && (!walls || wallsSaved) && objectsSaved;
    } catch (const std::exception& ex) {
        ts2::diag::error(std::format("saving lot {} failed: {}", enteredLot_, ex.what()));
        return false;
    }
}

ts2::sim::ObjectSave Game::saveObjects(ts2::dbpf::Package& original) {
    auto& sim = lotLife_->simulation();
    auto& world = sim.world();
    auto& code = sim.code();
    // The objects as saved, and what held each.
    std::optional<ts2::objm::ObjectMap> map;
    std::optional<ts2::objt::ObjectTypeTable> savedTypes;
    for (const auto& e : original.entries()) {
        if (e.key.type == ts2::types::kObjectMap && e.key.instance == 1) {
            map = ts2::objm::parse(original.read(e));
        } else if (e.key.type == ts2::types::kObjectTypeTable) {
            savedTypes = ts2::objt::parse(original.read(e));
        }
    }
    if (!map) {
        return {{}, {}, {"objects: the lot has no object map"}};
    }
    std::set<std::int16_t> ids = edits_.touched;
    for (const auto& e : original.entries()) {
        const bool state = e.key.type == ts2::types::kSavedObject || e.key.type == ts2::types::kSavedObjectWithMaterial;
        if (state && edits_.touched.contains(static_cast<std::int16_t>(e.key.instance))) {
            try {
                const auto kind = e.key.type == ts2::types::kSavedObjectWithMaterial ? ts2::xobj::RecordKind::ObjectWithMaterial : ts2::xobj::RecordKind::Object;
                const auto saved = ts2::xobj::parse(original.read(e), map->version, kind);
                if (saved.data[ts2::sim::vm_detail::kContainerData] != 0) {
                    ids.insert(saved.data[ts2::sim::vm_detail::kContainerData]);
                }
            } catch (const std::exception&) {
                // read again when it is saved, which says what is wrong
            }
        }
    }

    ts2::sim::ObjectChanges changes;
    std::map<std::uint32_t, std::optional<ts2::sim::SceneTemplate>> templates;
    std::vector<fs::path> lots;
    std::vector<std::string> problems;
    std::set<std::int16_t> failed;
    for (const auto id : ids) {
        const auto* o = world.find(id);
        const bool here = o != nullptr && !o->removed;
        const auto savedType = map->typeIndexByObject.find(static_cast<std::uint32_t>(id));
        const bool saved = savedType != map->typeIndexByObject.end();
        // An id the lot saved that now names an object of another type: that one went, this one came.
        const ts2::objt::Entry* savedEntry = saved && savedTypes ? savedTypes->find(static_cast<std::uint16_t>(savedType->second)) : nullptr;
        const bool replaced = here && savedEntry != nullptr && savedEntry->guid != o->definition.guid();
        if (saved && (!here || replaced)) {
            changes.sold.push_back(id);
            if (!here) {
                continue;
            }
        }
        if (saved && !replaced) {
            changes.moved.push_back({id, o->definition.guid(), placeNow(*o), 0});
        } else if (here) {
            if (o->isPerson()) {
                continue;
            }
            const std::uint32_t guid = o->definition.guid();
            if (!templates.contains(guid)) {
                if (lots.empty()) {
                    lots = templateLots(lotFile_);
                }
                templates[guid] = ts2::sim::findSceneTemplate(lots, guid);
            }
            const auto& scene = templates[guid];
            if (!scene) {
                problems.push_back(std::format("object {} bought: no saved lot has an object of its type ({:08x}) to follow", id, guid));
                failed.insert(id);
                continue;
            }
            ts2::sim::BoughtObject b;
            b.scene = *scene;
            b.now.id = id;
            b.now.guid = guid;
            // As the saved object of its type, with what NeoTS2 keeps of it now.
            auto& s = b.now.state;
            s = scene->state;
            s.header.location = {static_cast<float>(o->x) / 65536.0f, static_cast<float>(o->y) / 65536.0f};
            s.header.value1 = o->level;
            for (std::size_t i = 0; i < s.attributes.size() && i < o->attributes.size(); ++i) {
                s.attributes[i] = o->attributes[i];
            }
            for (std::size_t i = 0; i < s.semiAttributes.size() && i < o->semiAttributes.size(); ++i) {
                s.semiAttributes[i] = o->semiAttributes[i];
            }
            s.temps = o->thread.temps;
            std::copy_n(o->data.begin(), std::min(o->data.size(), s.data.size()), s.data.begin());
            const auto& ts = o->tokenSource;
            s.tokenSource = {ts.kind, ts.id, ts.counted, ts.index, ts.guidLow, ts.guidHigh, ts.flags, ts.propertyCount};
            auto& t = *s.tail;
            t.token = {o->token.guid, o->token.flags, o->token.value1, o->token.value2, o->token.category, o->token.properties};
            t.arrays = o->arrays;
            t.shorts = o->arrayIterators;
            t.shorts.resize(t.arrays.size(), 0);
            t.strings = o->materialNames;
            t.relationshipIds.clear();
            if (t.slots.marker >= 0) {
                t.slots.marker = -1;
            }
            t.slots.values = o->slots;
            t.effectsSaved = 0;
            t.effects.clear();
            t.materials.clear();
            t.timers.clear();
            // Its thread, frame by frame from the bottom.
            t.stack.clear();
            t.stackValue = 0;
            const auto& frames = o->thread.frames;
            bool owners = true;
            for (std::size_t k = 0; k < frames.size(); ++k) {
                const auto& f = frames[k];
                ts2::xobj::StackFrame sf;
                sf.objectId = f.stackObjectId;
                sf.treeId = static_cast<std::int16_t>(f.instance);
                sf.node = f.pc;
                sf.params = f.params;
                sf.locals = f.locals;
                if (f.code) {
                    sf.params.resize(std::min<std::size_t>(sf.params.size(), f.code->argCount));
                    sf.locals.resize(std::min<std::size_t>(sf.locals.size(), f.code->localCount));
                }
                sf.value1 = id;
                sf.value2 = f.runningOnId != 0 ? f.runningOnId : id;
                const bool above = k + 1 < frames.size() && (frames[k + 1].callerIdlesAgain || frames[k + 1].callerContinuesTrue);
                sf.value3 = above ? 1 : 0;
                t.stack.push_back(std::move(sf));
                // The type whose code the frame runs: the object's own, or another's on the lot.
                std::uint32_t owner = f.codeOwner == o->group ? guid : 0;
                if (owner == 0) {
                    for (const auto other : world.idsIncludingRemoved([&](const ts2::sim::SimObject& x) { return x.group == f.codeOwner; })) {
                        owner = world.find(other) != nullptr ? world.find(other)->definition.guid() : 0;
                        break;
                    }
                }
                owners = owners && owner != 0;
                b.frameOwners.push_back(owner);
            }
            if (!owners) {
                problems.push_back(std::format("object {} bought: its script runs code of a type that isn't on the lot", id));
                failed.insert(id);
                continue;
            }
            if (!frames.empty() && frames.back().code && frames.back().pc < frames.back().code->instructions.size() &&
                frames.back().code->instructions[frames.back().pc].opcode == ts2::sim::vm_detail::kSleepOpcode && o->thread.sleepTicks > 0) {
                b.now.waitTicks = static_cast<std::int16_t>(std::min(o->thread.sleepTicks - 1, 0x7FFF));
            }
            changes.bought.push_back(std::move(b));
        }
    }
    changes.newType = [&](std::uint32_t guid) -> std::optional<ts2::objt::Entry> {
        const auto* type = code.objectType(guid);
        if (type == nullptr) {
            return std::nullopt;
        }
        // Its objects' semi-attribute count and record kind, as a saved object of the type has them.
        if (!templates.contains(guid)) {
            if (lots.empty()) {
                lots = templateLots(lotFile_);
            }
            templates[guid] = ts2::sim::findSceneTemplate(lots, guid);
        }
        const auto& saved = templates[guid];
        if (!saved) {
            return std::nullopt;
        }
        return ts2::sim::typeTableEntry(code, *type, saved->state.semiAttributes.size(), saved->kind == ts2::xobj::RecordKind::ObjectWithMaterial,
                                        lotLife_->objectName(guid));
    };
    const ts2::scene::Heights heights(*buildLot_);
    changes.floorHeight = [&](std::int32_t storey, float x, float y) {
        return heights.tile(ts2::content::gridLevel(*buildLot_, storey), x, y);
    };
    auto result = ts2::sim::objectChanges(original, changes);
    result.problems.insert(result.problems.begin(), problems.begin(), problems.end());
    result.failed.insert(failed.begin(), failed.end());
    return result;
}

} // namespace app
