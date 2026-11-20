#include "sim/lot_save.hpp"

#include "formats/objd.hpp"
#include "formats/srel.hpp"

#include "sim/lot_save_detail.hpp"
#include "sim/lot_simulation.hpp"
#include "sim/selection.hpp"
#include "sim/simulator_ids.hpp"
#include "sim/type_signature.hpp"
#include "sim/want_refill.hpp"
#include "sim/world.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ts2::sim {

using namespace lot_save_detail;

simi::Record simulatorRecord(const simi::Record& loaded, std::span<const std::int16_t> globals, std::uint32_t tickRandomState) {
    simi::Record record = loaded;
    std::copy_n(globals.begin(), std::min(globals.size(), record.globals.size()), record.globals.begin());
    record.tickRandomState = tickRandomState;
    return record;
}

simi::Record simulatorRecord(const simi::Record& loaded, LotSimulation& simulation) {
    return simulatorRecord(loaded, simulation.globals(), simulation.vm().rng.state());
}

bool canSaveLot(const dbpf::PackageImage& package) {
    bool simulator = false, map = false;
    for (const dbpf::StoredResource& resource : package.resources) {
        if (resource.key.type == simi::kType && simi::readRecord(bodyOf(resource))) {
            simulator = true;
        } else if (isObjectMap(resource.key) && objm::readMapRecord(bodyOf(resource))) {
            map = true;
        }
    }
    return simulator && map;
}

namespace {

// A Sim made this run whose character file this save doesn't write would be left out of the
// neighborhood, so the game would load a Sim on the lot with nothing behind it. Throws when the lot
// holds one.
void refuseNewSims(const World& world, const std::set<std::int16_t>& written) {
    world.nextAfter(0, [&](const SimObject& object) {
        const std::int16_t neighbor = world.neighborIdOf(object);
        if (object.isPerson() && world.addedNeighbors().contains(neighbor) && !written.contains(neighbor)) {
            throw std::runtime_error("a Sim made since the lot loaded other than a newborn cannot be saved yet, as NeoTS2 "
                                     "does not write its character file yet");
        }
        return false;
    });
}

// The type table entry the loaded object map gives each object, by id.
std::unordered_map<std::int32_t, std::int32_t> loadedTypeByObject(const std::optional<objm::MapRecord>& loadedMap) {
    std::unordered_map<std::int32_t, std::int32_t> typeByObject;
    if (loadedMap) {
        for (const auto& [id, type] : loadedMap->objects) {
            typeByObject.emplace(id, type);
        }
    }
    return typeByObject;
}

// One clip of the lot's own, for an object whose record holds nothing to build an animation from.
std::optional<cobject::Animation> anySimClip(const dbpf::PackageImage& package) {
    std::optional<cobject::Animation> anyAnimation;
    for (const dbpf::StoredResource& resource : package.resources) {
        if (anyAnimation || resource.key.type != types::kSceneObject) {
            continue;
        }
        if (const auto record = cobject::readRecord(bodyOf(resource))) {
            for (const cobject::Animation& a : record->animatable.animations) {
                if (a.kind == cobject::Animation::Kind::Clip && a.request.type == "sim") {
                    anyAnimation = a;
                    break;
                }
            }
        }
    }
    return anyAnimation;
}

// The clip handle counter goes on from the highest the lot's records saved, and the records save it
// past one new serial for each Sim on the lot, the most this save gives out, as a Sim plays one Sim
// clip at a time. The second counter stays at the highest saved.
ClipHandles clipHandlesFrom(const dbpf::PackageImage& package, const World& world) {
    ClipHandles handles;
    for (const dbpf::StoredResource& resource : package.resources) {
        if (resource.key.type != types::kSceneObject) {
            continue;
        }
        const auto record = cobject::readRecord(bodyOf(resource));
        if (!record) {
            continue;
        }
        if (record->chain != cobject::Chain::Object) {
            handles.last = std::max(handles.last, record->animatable.settings[0]);
            handles.second = std::max(handles.second, record->animatable.settings[1]);
        }
        for (const cobject::Animation& a : record->animatable.animations) {
            if (a.kind == cobject::Animation::Kind::Clip) {
                handles.savedTick = std::max(handles.savedTick, a.instance.savedTick);
            }
        }
    }
    std::uint32_t sims = 0;
    world.nextAfter(0, [&](const SimObject& object) {
        sims += object.isPerson() ? 1 : 0;
        return false;
    });
    handles.saved = handles.last + sims;
    return handles;
}

// The counters new interactions written for Sims take their ids and numbers from. The game numbers
// interactions from the highest number its saved Sims hold. Ids go on from `lastLinkId`, the last
// one the simulator's id counter gave out.
SaveCounters countersFromSavedSims(const dbpf::PackageImage& package, std::uint16_t lastLinkId, std::uint32_t version) {
    SaveCounters counters;
    counters.lastLinkId = lastLinkId;
    for (const dbpf::StoredResource& resource : package.resources) {
        if (resource.key.type != types::kSavedPerson) {
            continue;
        }
        try {
            const xobj::SavedObject sim = xobj::parse(bodyOf(resource), version, xobj::RecordKind::Person);
            if (sim.person) {
                const auto take = [&](const xobj::SavedInteraction& i) {
                    counters.lastNumber = std::max(counters.lastNumber, i.number);
                };
                const auto& s = sim.person->state;
                take(s.running);
                take(s.interrupted);
                for (const auto* list : {&s.queue, &s.subQueue, &s.list}) {
                    for (const auto& i : *list) {
                        take(i);
                    }
                }
                if (s.runningSub) {
                    take(*s.runningSub);
                }
            }
        } catch (const std::exception&) {
        }
    }
    return counters;
}

// Where a record of each type the lot was saved with lies, for an object made since it loaded.
// What the game wrote for another object of its type is what it writes for the type, so a new
// object's record starts from one and takes the state of the object it is for.
std::unordered_map<std::uint16_t, std::size_t> recordOfEachType(const dbpf::PackageImage& package, const objt::ObjectTypeTable& types,
                                                                const std::unordered_map<std::int32_t, std::int32_t>& typeByObject) {
    std::unordered_map<std::uint16_t, std::size_t> templates;
    for (std::size_t at = 0; at < package.resources.size(); ++at) {
        const dbpf::ResourceKey& key = package.resources[at].key;
        const auto listed = typeByObject.find(static_cast<std::int32_t>(key.instance));
        if (listed == typeByObject.end() || !resourceKind(key.type)) {
            continue;
        }
        if (const objt::Entry* entry = types.find(static_cast<std::uint16_t>(listed->second));
            entry != nullptr && wantedType(*entry) == key.type) {
            templates.try_emplace(entry->index, at);
        }
    }
    return templates;
}

// The scene record of an object the lot was loaded with, as it is playing now. Empty for an
// object that is gone or a record that does not read.
std::optional<std::vector<std::uint8_t>> rewrittenScene(SaveState& save, const dbpf::StoredResource& resource,
                                                        const cobject::Animation* anyAnimation) {
    const SimObject* object = listedObject(save, static_cast<std::int32_t>(resource.key.instance)).object;
    if (object == nullptr) {
        return std::nullopt;
    }
    const auto loaded = cobject::readRecord(bodyOf(resource));
    if (!loaded) {
        return std::nullopt;
    }
    cobject::Record record = sceneRecord(*loaded, *object, save.world, save.world.clockMs(), anyAnimation, &save.clipHandles);
    // A Sim's place is where it is now: where it stands, or the origin while it hangs in a slot, as
    // the game writes it. The load puts the Sim's body there.
    if (object->isPerson() && record.chain == cobject::Chain::Person) {
        record.object.place = simScenePlace(*object, save.owners.floorHeight);
        if (const auto places = simPlacePair(*object, record.object.place, save.owners)) {
            record.animatable.places = places;
        }
        // The joints are the skeleton's own root ("<name>_objt"), at that place too, then the skeleton's
        // joints in the pose the Sim is drawn in. A load poses the drawn Sim so. Stale joints drew it
        // where and as it was saved before, and none (a newborn's first record) left it in no pose.
        if (auto drawn = save.owners.drawnJoints ? save.owners.drawnJoints(object->id) : std::nullopt) {
            const auto root = std::find_if(record.object.joints.begin(), record.object.joints.end(),
                                           [](const cobject::PosedJoint& j) { return j.name.ends_with("_objt"); });
            std::string rootName = root != record.object.joints.end() ? root->name : object->definition.name + "_objt";
            drawn->insert(drawn->begin(), cobject::PosedJoint{std::move(rootName), {}});
            record.object.joints = std::move(*drawn);
        }
        for (cobject::PosedJoint& joint : record.object.joints) {
            if (joint.name.ends_with("_objt")) {
                joint.place = record.object.place;
            }
        }
    } else if (!object->isPerson() && record.chain == cobject::Chain::Animatable) {
        // An object a clip has posed keeps that pose, as the game saves its joints as they are drawn:
        // a plumbob raised over a skill meter stays raised. Its own root joint stays as loaded, and
        // one a record lacks is put first at the record's place, as every record the game writes
        // starts with it.
        auto& joints = record.object.joints;
        const auto root = std::find_if(joints.begin(), joints.end(), [](const cobject::PosedJoint& j) { return j.name.ends_with("_objt"); });
        const cobject::PosedJoint own = root != joints.end() ? *root : cobject::PosedJoint{object->definition.name + "_objt", record.object.place};
        if (auto drawn = save.owners.drawnJoints ? save.owners.drawnJoints(object->id) : std::nullopt) {
            drawn->insert(drawn->begin(), own);
            joints = std::move(*drawn);
        } else if (root == joints.end()) {
            joints.insert(joints.begin(), own);
        }
        putDrawnMorphs(record.object, object->id, save.owners);
    }
    std::vector<std::uint8_t> written = cobject::writeRecord(record);
    ++save.counts.scenes;
    return written;
}

// The saved record of an object the lot was loaded with, as it is now. Empty for a resource that
// holds no saved object and for a record left as loaded.
std::optional<std::vector<std::uint8_t>> rewrittenObject(SaveState& save, const dbpf::StoredResource& resource) {
    const auto kind = resourceKind(resource.key.type);
    if (!kind) {
        ++save.counts.keptAsLoaded;
        return std::nullopt;
    }
    // A record is written only for an object the lot was loaded with, whose record this is. An
    // object made since can land on an id whose old record is still in the package, even one the
    // map lists for an object gone since, and its state does not belong there. It gets a record of
    // its own as a new object.
    const ListedObject listed = listedObject(save, static_cast<std::int32_t>(resource.key.instance));
    if (listed.object == nullptr || listed.entry == nullptr || wantedType(*listed.entry) != resource.key.type) {
        ++save.counts.objectsKept;
        ++save.counts.keptAsLoaded;
        return std::nullopt;
    }
    try {
        const xobj::SavedObject loaded = readSaved(resource, save.version, *kind);
        const xobj::SavedObject ours = objectRecord(loaded, *listed.object, save.owners, &save.counters);
        if (loaded.tail && !listed.object->thread.frames.empty() && !stackOf(loaded, *listed.object, save.owners)) {
            ++save.counts.stacksKept;
            save.stale.insert(listed.entry->guid);
        }
        std::vector<std::uint8_t> written = xobj::write(ours, *kind);
        ++save.counts.objects;
        save.done.insert(static_cast<std::int32_t>(resource.key.instance));
        return written;
    } catch (const std::exception& ex) {
        if (save.upgrading) {
            throw std::runtime_error(std::string("an object of a lot the game has never saved could not be "
                                                 "written at the version the game writes now: ") +
                                     ex.what());
        }
        ++save.counts.keptAsLoaded;
        save.stale.insert(listed.entry->guid);
        return std::nullopt;
    }
}

// Writes the simulator, the scene records, the live camera's view and the record of every object
// the lot was loaded with. The object map and the type table are left for last.
void rewriteLoadedRecords(SaveState& save, std::span<const std::int16_t> globals, std::uint32_t tickRandomState,
                          const LotCameraView* view, const cobject::Animation* anyAnimation) {
    const std::uint32_t liveCamera = view ? savedcamera::instanceOf(view->rig.name) : 0;
    for (dbpf::StoredResource& resource : save.package.resources) {
        std::optional<std::vector<std::uint8_t>> written;
        if (resource.key.type == simi::kType) {
            if (const auto loaded = simi::readRecord(bodyOf(resource))) {
                written = simi::writeRecord(simulatorRecord(*loaded, globals, tickRandomState));
                ++save.counts.simulator;
            }
        } else if (isObjectMap(resource.key) && save.types) {
            // Written last, once it is known which new objects got a record.
            continue;
        } else if (resource.key.type == types::kObjectTypeTable && save.types) {
            // Written last, once it is known which objects kept the stacks they were loaded with.
            continue;
        } else if (resource.key.type == types::kSceneObject) {
            written = rewrittenScene(save, resource, anyAnimation);
        } else if (view && resource.key.type == savedcamera::kType && resource.key.instance == liveCamera) {
            if (const auto loaded = savedcamera::readRecord(bodyOf(resource))) {
                written = savedcamera::writeRecord(savedcamera::viewRecord(*loaded, view->rig, view->state));
                ++save.counts.camera;
            }
        }
        if (!written) {
            written = rewrittenObject(save, resource);
            if (!written) {
                continue;
            }
        }
        resource = dbpf::storeResource(resource.key, *written);
    }
}

// Writes the object map, listing the objects that got a record. Needs `save.types`.
void writeObjectMap(SaveState& save) {
    for (dbpf::StoredResource& resource : save.package.resources) {
        if (!isObjectMap(resource.key)) {
            continue;
        }
        if (const auto loaded = objm::readMapRecord(bodyOf(resource))) {
            const objm::MapRecord map = objectMapRecord(*loaded, *save.types, save.world, &save.done);
            checkListedRecords(save.package, map, *save.types, save.typeByObject, save.upgrading);
            resource = dbpf::storeResource(resource.key, objm::writeMapRecord(map));
            ++save.counts.objectMap;
        } else {
            ++save.counts.keptAsLoaded;
        }
    }
}

// Writes the object type table with the types' values as they are now. Needs `save.types`.
void writeTypeTable(SaveState& save) {
    for (dbpf::StoredResource& resource : save.package.resources) {
        if (resource.key.type != types::kObjectTypeTable) {
            continue;
        }
        if (!save.owners.savedValues.empty() || !save.owners.newTypes.empty()) {
            resource = dbpf::storeResource(resource.key, objt::write(typeTableRecord(*save.types, save.owners, save.stale)));
            ++save.counts.typeTable;
        } else {
            ++save.counts.keptAsLoaded;
        }
    }
}

// Gives each object the save wrote a relationship record towards each object it lists (see
// savedRelationships), as the game saves an object's relationships with it. Records no object lists
// stay as loaded.
void writeRelationships(SaveState& save) {
    std::unordered_map<std::uint32_t, std::size_t> loaded;
    for (std::size_t i = 0; i < save.package.resources.size(); ++i) {
        const dbpf::ResourceKey& key = save.package.resources[i].key;
        if (key.type == srel::kType && key.group == 0xFFFFFFFF && key.instanceHigh == 0) {
            loaded.emplace(key.instance, i);
        }
    }
    std::vector<std::int32_t> ids(save.done.begin(), save.done.end());
    std::sort(ids.begin(), ids.end());
    for (const std::int32_t id : ids) {
        const SimObject* object = save.world.find(static_cast<std::int16_t>(id));
        if (object == nullptr) {
            continue;
        }
        for (const std::uint32_t to : savedRelationships(save.world, *object)) {
            const std::vector<std::int16_t>* values = save.world.relationshipIfAny(object->id, static_cast<std::int16_t>(to));
            const auto instance = static_cast<std::uint32_t>(static_cast<std::uint16_t>(object->id)) << 16 | (to & 0xFFFF);
            const std::vector<std::uint8_t> body = srel::writeRecord({{values->begin(), values->end()}});
            const dbpf::ResourceKey key{srel::kType, 0xFFFFFFFF, instance, 0};
            if (const auto at = loaded.find(instance); at != loaded.end()) {
                save.package.resources[at->second] = dbpf::storeResource(key, body);
            } else {
                save.package.resources.push_back(dbpf::storeResource(key, body));
                save.package.dataOrder.push_back(save.package.resources.size() - 1);
                loaded.emplace(instance, save.package.resources.size() - 1);
            }
            ++save.counts.relationships;
        }
    }
}

} // namespace

std::vector<std::uint32_t> savedRelationships(const World& world, const SimObject& object) {
    std::vector<std::uint32_t> ids;
    for (const std::uint32_t to : object.relationshipTable) {
        const std::vector<std::int16_t>* values = world.relationshipIfAny(object.id, static_cast<std::int16_t>(to));
        if (values != nullptr && !values->empty()) {
            ids.push_back(to);
        }
    }
    return ids;
}

LotSaveCounts fillLotPackage(dbpf::PackageImage& package, const World& world, std::span<const std::int16_t> globals,
                             std::uint32_t tickRandomState, const TypeOwners& owners, const LotCameraView* view) {
    refuseNewSims(world, owners.writtenNewSims);
    // Records list the relationships the save writes records for.
    TypeOwners withLists = owners;
    if (!withLists.relationshipList) {
        withLists.relationshipList = [&world](const SimObject& object) { return savedRelationships(world, object); };
    }
    SaveState save{package, world, withLists};
    save.types = typesOf(package);
    // Types of objects made since the lot loaded that its table does not hold yet go on its end.
    if (save.types) {
        save.types->entries.insert(save.types->entries.end(), owners.newTypes.begin(), owners.newTypes.end());
        save.counts.typesAdded = static_cast<int>(owners.newTypes.size());
    }
    const auto loadedMap = mapOf(package);
    // A lot the game has never saved holds its objects at the shipped version, and every one still
    // listed must be written at the version the map is written at, or the game misreads it.
    save.upgrading = loadedMap && loadedMap->upgraded;
    save.version = save.upgrading ? xobj::kShippedVersion : xobj::kWrittenVersion;
    save.typeByObject = loadedTypeByObject(loadedMap);
    const std::optional<cobject::Animation> anyAnimation = anySimClip(package);
    save.anyAnimation = anyAnimation ? &*anyAnimation : nullptr;
    save.clipHandles = clipHandlesFrom(package, world);
    // Ids the save has to make go on from the simulator's id counter, without moving it, as the
    // game's save leaves its globals as they are.
    save.counters = countersFromSavedSims(package, static_cast<std::uint16_t>(static_cast<std::uint16_t>(globals[kIdCounterGlobal]) - 1),
                                          save.version);
    if (save.types) {
        save.templates = recordOfEachType(package, *save.types, save.typeByObject);
    }
    rewriteLoadedRecords(save, globals, tickRandomState, view, anyAnimation ? &*anyAnimation : nullptr);
    if (save.types) {
        addNewObjects(save);
        writeRelationships(save);
        writeObjectMap(save);
        writeTypeTable(save);
    }
    save.counts.lastLinkId = save.counters.lastLinkId;
    return save.counts;
}

LotSaveCounts fillLotPackage(dbpf::PackageImage& package, LotSimulation& simulation, const LotCameraView* view,
                             const std::set<std::int16_t>& writtenNewSims,
                             std::function<std::optional<cobject::Place>(std::int16_t)> drawnPlace,
                             std::function<std::optional<std::vector<cobject::PosedJoint>>(std::int16_t)> drawnJoints,
                             std::function<std::vector<xobj::Effect>(std::int16_t)> runningEffects,
                             std::function<std::optional<std::vector<cobject::NamedWeight>>(std::int16_t)> drawnMorphs) {
    // The game's queue reads the names as soon as a Sim is selected, even while paused, so a save
    // right after a switch sees 0x80 gone.
    markShownNames(simulation.world(), simulation.globals());
    // Each Sim's waiting want refill runs before its wants are saved, as the game's save runs it.
    prepareWantsForSave({simulation.world(), simulation.code(), simulation.vm(), simulation.installedPacks()});
    TypeOwners owners;
    if (const auto types = typesOf(package)) {
        for (const objt::Entry& entry : types->entries) {
            if (const ObjectType* type = simulation.code().objectType(entry.guid)) {
                const auto index = static_cast<std::int16_t>(entry.index);
                owners.indexByGroup.emplace(type->group, index);
                owners.groupByIndex.emplace(index, type->group);
                owners.savedValues.emplace(entry.guid, savedTypeValues(simulation.code(), *type));
            }
        }
    }
    owners.madeAsLoaded = simulation.madeAsLoaded();
    owners.tunings = AutonomyTunings::from(simulation.code().tunings());
    owners.writtenNewSims = writtenNewSims;
    owners.drawnPlace = std::move(drawnPlace);
    // The model name as the game keeps it: the one a script gave, else the row of the type's model
    // names that object data 0x00 gives, or OBJD field 0x48 when that is 0, with "_cres" cut off.
    owners.modelName = [&simulation](const SimObject& object) {
        constexpr std::uint16_t kModelNames = 0x85;
        std::string model = object.model;
        if (model.empty()) {
            const auto own = static_cast<std::uint16_t>(object.data[0]);
            const std::uint16_t row = own != 0 ? own : object.definition.field(objd::field::kDefaultModel);
            model = row != 0 ? simulation.code().string(object.group, kModelNames, row).value_or(std::string{}) : std::string{};
        }
        constexpr std::string_view kSuffix = "_cres";
        if (model.size() >= kSuffix.size()) {
            std::string tail = model.substr(model.size() - kSuffix.size());
            std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (tail == kSuffix) {
                model.resize(model.size() - kSuffix.size());
            }
        }
        return model;
    };
    owners.drawnJoints = std::move(drawnJoints);
    owners.runningEffects = std::move(runningEffects);
    owners.drawnMorphs = std::move(drawnMorphs);
    // A Sim on a stage stands above its storey's floor. The platform grid counts the ground as level 0.
    owners.floorHeight = [&simulation](std::int32_t level, float x, float y) {
        return simulation.floorHeight(level, x, y) + simulation.world().standingLift(level - 1, x, y);
    };
    addNewTypes(package, simulation, owners);
    const LotSaveCounts counts =
        fillLotPackage(package, simulation.world(), simulation.globals(), simulation.vm().rng.state(), owners, view);
    return counts;
}

} // namespace ts2::sim
