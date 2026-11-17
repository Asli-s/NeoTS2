// What the files of the lot save share: lot_save.cpp fills a lot's package from the running lot,
// and the lot_save_*.cpp files build its records by topic.
#pragma once

#include "formats/cobject_record.hpp"
#include "formats/dbpf.hpp"
#include "formats/dbpf_writer.hpp"
#include "formats/objm.hpp"
#include "formats/objt.hpp"
#include "formats/qfs.hpp"
#include "formats/resource_types.hpp"
#include "formats/xobj.hpp"
#include "sim/lot_save.hpp"
#include "sim/world.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ts2::sim::lot_save_detail {

// A resource's bytes as the records read them.
inline std::vector<std::uint8_t> bodyOf(const dbpf::StoredResource& resource) {
    return resource.compressed ? qfs::decompress(resource.bytes) : resource.bytes;
}

// Which kind of record a resource type holds, for the types that hold a saved object.
inline std::optional<xobj::RecordKind> resourceKind(std::uint32_t type) {
    switch (type) {
    case types::kSavedObject: return xobj::RecordKind::Object;
    case types::kSavedObjectWithMaterial: return xobj::RecordKind::ObjectWithMaterial;
    case types::kSavedPerson: return xobj::RecordKind::Person;
    default: return std::nullopt;
    }
}

// Which resource type holds an object type's saved state, as a lot's loader looks for it.
inline std::uint32_t wantedType(const objt::Entry& entry) {
    return entry.kind == objt::kPersonKind ? types::kSavedPerson
           : (entry.flag & 1)              ? types::kSavedObjectWithMaterial
                                           : types::kSavedObject;
}

// Whether the key is the lot's object map. Instance 2 of the same type holds the object groups.
inline bool isObjectMap(const dbpf::ResourceKey& key) {
    return key.type == types::kObjectMap && key.instance == 1;
}

// An object's entry in the lot's object type table, found by the definition it runs.
inline std::optional<std::int32_t> typeIndexOf(const objt::ObjectTypeTable& types, const SimObject& object) {
    for (const objt::Entry& entry : types.entries) {
        if (entry.guid == object.definition.guid()) {
            return static_cast<std::int32_t>(entry.index);
        }
    }
    return std::nullopt;
}

// The lot's object map as it was loaded, which says which type each object's record holds.
inline std::optional<objm::MapRecord> mapOf(const dbpf::PackageImage& package) {
    for (const dbpf::StoredResource& resource : package.resources) {
        if (isObjectMap(resource.key)) {
            return objm::readMapRecord(bodyOf(resource));
        }
    }
    return std::nullopt;
}

// The lot's object type table, which the object map names its objects through.
inline std::optional<objt::ObjectTypeTable> typesOf(const dbpf::PackageImage& package) {
    for (const dbpf::StoredResource& resource : package.resources) {
        if (resource.key.type == types::kObjectTypeTable) {
            return objt::parse(bodyOf(resource));
        }
    }
    return std::nullopt;
}

// The handles a save gives its Sims' clips, which the game finds a clip by. An overlay's is
// 0x60000000 and a Sim clip's 0x40000000 with an 8-bit serial from bit 21, both with bit 0 for a
// mirrored clip. The serials come from a counter each Sim's scene record saves as its first setting,
// which a load takes back. `last` is the last serial given out, and `saved` what the records save:
// past every serial this save gives out.
// `savedTick` is the clock the lot's clips were saved at, for an entry built from an empty one.
struct ClipHandles {
    std::uint32_t last = 0;
    std::uint32_t saved = 0;
    std::int32_t savedTick = 0;
};

// What the steps of one save share as they fill the package.
struct SaveState {
    dbpf::PackageImage& package;
    const World& world;
    const TypeOwners& owners;
    // The lot's object type table with the types added since it loaded on its end. Empty when the
    // lot holds none, and then no object or scene records, map or table are written.
    std::optional<objt::ObjectTypeTable> types;
    // Whether the lot is being moved from the shipped version, and the version its records read at.
    bool upgrading = false;
    std::uint32_t version = xobj::kWrittenVersion;
    // The type table entry the loaded object map gives each object, by id.
    std::unordered_map<std::int32_t, std::int32_t> typeByObject;
    // Where a record of each type the lot was saved with lies, by type table entry.
    std::unordered_map<std::uint16_t, std::size_t> templates;
    // Objects given a record by this save, which the object map lists.
    std::unordered_set<std::int32_t> done;
    // Types with an object whose stack or record stays as it was loaded (see typeTableRecord).
    std::unordered_set<std::uint32_t> stale;
    SaveCounters counters;
    // An animation of one of the lot's Sims, for an object whose record holds none to build from.
    const cobject::Animation* anyAnimation = nullptr;
    ClipHandles clipHandles;
    LotSaveCounts counts;
};

// An object the loaded object map lists, as the running lot has it now.
struct ListedObject {
    const SimObject* object = nullptr;  // null when the object at that id is gone or a new one
    const objt::Entry* entry = nullptr;  // the type table entry the map gives it
};

// The object on the running lot that continues the one the loaded object map lists at `id` (see
// continuingObject). Empty for an id the map does not list or a lot with no type table.
inline ListedObject listedObject(const SaveState& save, std::int32_t id) {
    const auto listed = save.typeByObject.find(id);
    if (listed == save.typeByObject.end() || !save.types) {
        return {};
    }
    return {continuingObject(save.world, *save.types, listed->first, listed->second),
            save.types->find(static_cast<std::uint16_t>(listed->second))};
}

// `record` without what belongs to the object it was written for, for a record being started from
// another object's. Everything else it holds is what the game writes for objects of that type.
xobj::SavedObject asTemplate(const xobj::SavedObject& record);

// Reads an object's record at the lot's version. A record of the shipped version is moved to the
// version the game writes, which the object map it is listed in is written at. Throws for a record
// that does not read or cannot be moved.
xobj::SavedObject readSaved(const dbpf::StoredResource& resource, std::uint32_t version, xobj::RecordKind kind);

// The frames an object's scripts are suspended in, as the record keeps them. Empty when a frame's
// code has no entry in the lot's object type table, which is how a frame names its owner.
std::optional<std::vector<xobj::StackFrame>> stackOf(const xobj::SavedObject& loaded, const SimObject& object,
                                                     const TypeOwners& owners);

// Gives each object made since the lot loaded a record of its own, started from a record of its
// type, or for an ordinary object with none, from the record the game writes for an object just
// made. An object whose type the lot's table does not hold is left out, as the object map has no
// way to name it either. Needs `save.types`.
void addNewObjects(SaveState& save);

// A Sim's interactions as it has them now. Each keeps the saved interaction it was loaded from,
// found by its id, with that one's number. One made since starts as madeInteraction gives it and
// takes the next number of the counter the game numbers interactions by, `counters.lastNumber`. One with
// nothing running gets the blank interaction, and one with no sub-queue interaction running gets
// none, as the game writes them. Nested ones waiting go to the sub-queue, the others to the queue.
// The interrupted interaction, which the engine does not keep, stays only while the Sim still runs
// the one it was loaded running, and is blank otherwise. A blank keeps the id the Sim gave it, and
// one the save makes takes the next of `counters.lastLinkId`.
void saveQueue(xobj::PersonState& state, const SimObject& sim, SaveCounters& counters, const AutonomyTunings& tunings);

// The blank interaction the game saves for a Sim with nothing in that place: no number, actor,
// target, line or tree, no picture, flags 16, and an id of its own that is also its link.
xobj::SavedInteraction blankInteraction(std::uint16_t id);

// The object's scene record as the game saves it from a running lot: the clips and overlays it is
// playing, each carrying how far into it the object has got. An animation it has stopped playing is
// left empty, as the game leaves one it does not keep, and what it is attached to stays as it was.
// An object whose record holds no animation to build one from takes `otherwise`, an animation of
// another object of the lot, and without one it keeps what it was loaded with.
// With `handles`, each clip gets its handle from them and a Sim's record saves their counter.
cobject::Record sceneRecord(const cobject::Record& loaded, const SimObject& object, const World& world, double nowMs,
                            const cobject::Animation* otherwise, ClipHandles* handles = nullptr);

// A new Sim's scene record, started from `another` Sim's: its place where it stands (or at the origin
// while it is in a slot), its animations as sceneRecord gives them, and the settings `another` holds,
// but for the clip handle counter, which `handles` give. A load only matches the pose, materials and
// blends by name to the Sim's body, so they stay empty rather than take `another`'s. Its pair of
// places is as simPlacePair gives it. Its locomotion keeps `another`'s value with no sets.
cobject::Record newSimSceneRecord(const cobject::Record& another, const SimObject& sim, const World& world, double nowMs,
                                  const cobject::Animation* otherwise, float floorHeight, const TypeOwners& owners,
                                  ClipHandles* handles = nullptr);

// A Sim's pair of places, the record's place `place` and then where it is in the world: a standing
// Sim's place twice, and for one in a slot where the slot's bone holds it (owners.drawnPlace). None
// for one in a slot without a drawn place.
std::optional<std::array<cobject::Word, 14>> simPlacePair(const SimObject& sim, const cobject::Place& place, const TypeOwners& owners);

// Checks that every object the map will list has a record of the kind its type asks for, which
// reads at the version the map is written at. The game reads that record as the object's, so a
// missing or unreadable one breaks its load. Objects the lot was loaded with keep the record they
// had unless the lot is being upgraded, so only those new to the map are checked otherwise.
// Throws, before anything is written, when one fails.
void checkListedRecords(const dbpf::PackageImage& package, const objm::MapRecord& map, const objt::ObjectTypeTable& types,
                        const std::unordered_map<std::int32_t, std::int32_t>& loadedTypes, bool upgrading);

// The lot's object type table as a save writes it: each entry keeps its place, and an installed
// type gets the values it has now in place of those it had when the lot was last saved, as the game
// writes them. Otherwise the next load finds the type changed and resets every object of it. A type
// in `stale` keeps the values it was loaded with: one of its objects keeps the stack it was loaded
// with, whose places in its trees only hold for the trees as they were, so the game resets it. The
// fifth value, the semi-global attribute count, is not compared, so it stays.
objt::ObjectTypeTable typeTableRecord(const objt::ObjectTypeTable& loaded, const TypeOwners& owners,
                                      const std::unordered_set<std::uint32_t>& stale);

} // namespace ts2::sim::lot_save_detail
