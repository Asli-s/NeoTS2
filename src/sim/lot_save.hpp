#pragma once

#include "formats/camera_config.hpp"
#include "formats/cobject_record.hpp"
#include "formats/dbpf_writer.hpp"
#include "formats/objm.hpp"
#include "formats/objt.hpp"
#include "formats/saved_camera.hpp"
#include "formats/simi.hpp"
#include "formats/xobj.hpp"
#include "sim/find_best_interaction.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace ts2::sim {

class LotSimulation;
class World;

// The lot's simulator record (SIMI) as the game saves it from a running lot: the simulator's 60
// globals and the state its ticks draw random numbers from, as they are now. The rest of the record
// (values whose meaning is not known yet and the random state used outside the ticks) is kept from
// `loaded`, the record the lot was loaded with.
simi::Record simulatorRecord(const simi::Record& loaded, std::span<const std::int16_t> globals, std::uint32_t tickRandomState);
simi::Record simulatorRecord(const simi::Record& loaded, LotSimulation& simulation);

// The lot's object map (ObjM instance 1) as the game saves it from a running lot: every object with
// its entry in the lot's object type table, and the ticks each one's script still sleeps. Objects
// keep the place the loaded record gave them, objects that are gone drop out, and objects the record
// doesn't list yet go in front, the newest first. An object whose type the table doesn't hold is
// left out, as the lot has no way to name it, and so is a new object not in `recorded` when that is
// given, as the game would read whatever record its id holds. The wait table is built again from
// the objects listed. The scripts waiting for a message are kept from `loaded`, unless it was read
// at the shipped version, which saves none, and then those of the objects listed are written. The
// rest of the record, such as what keeps build and buy mode closed, is kept from `loaded`.
objm::MapRecord objectMapRecord(const objm::MapRecord& loaded, const objt::ObjectTypeTable& types, const World& world,
                                const std::unordered_set<std::int32_t>* recorded = nullptr);
objm::MapRecord objectMapRecord(const objm::MapRecord& loaded, const objt::ObjectTypeTable& types, LotSimulation& simulation);

struct SimObject;

// Which entry of the lot's object type table holds each object group's code, and which group each
// entry holds, for the stack frames that name the object whose code they run by its entry. Several
// entries can hold one group, so a frame keeps the entry it was saved with when that entry still
// holds its group.
struct TypeOwners {
    std::unordered_map<std::uint32_t, std::int16_t> indexByGroup;
    std::unordered_map<std::int16_t, std::uint32_t> groupByIndex;
    // The values the type table saves of each installed type the table names, by GUID, as
    // savedTypeValues gives them. A load resets every object of a type whose saved values differ.
    std::unordered_map<std::uint32_t, std::array<std::uint32_t, 4>> savedValues;
    // Entries for types of objects made since the lot loaded that its table does not hold, which a
    // save adds to the end of the table (see addNewTypes).
    std::vector<objt::Entry> newTypes;
    // Objects the lot made as it loaded rather than in play (see LotSimulation::madeAsLoaded).
    std::unordered_set<std::int16_t> madeAsLoaded;
    // The game's autonomy tunings, for the attenuation it saves with a new interaction.
    AutonomyTunings tunings;
    // Neighbors made since the neighborhood loaded whose character files this save writes. Only
    // those can be saved on the lot, as the game loads a Sim through its file.
    std::set<std::int16_t> writtenNewSims;
    // The height a Sim stands at (x, y) in the scene's tiles on a storey (1 = ground), higher on a
    // stage than on the floor. Without it a Sim stands at height 0.
    std::function<float(std::int32_t, float, float)> floorHeight;
    // Where a Sim is drawn on the lot (its position, then its rotation as a quaternion), by object id,
    // which for one in a slot is where the slot's bone holds it. Without it, or with nothing for a
    // Sim, such a Sim keeps the pair of places it was loaded with.
    std::function<std::optional<cobject::Place>(std::int16_t)> drawnPlace;
    // A Sim's skeleton joints as drawn now, by object id: each named joint below the skeleton's own
    // root in the skeleton's order, with its place relative to its parent joint. Without it, or with
    // nothing for a Sim, the Sim's joints keep the pose they were loaded with.
    std::function<std::optional<std::vector<cobject::PosedJoint>>(std::int16_t)> drawnJoints;
    // The effects an object is playing now, by object id, as its record saves them. Without it the
    // record keeps the effects it was loaded with.
    std::function<std::vector<xobj::Effect>(std::int16_t)> runningEffects;
    // The objects an object's record lists relationships towards (see savedRelationships). Without
    // it the record lists the object's whole relationship table.
    std::function<std::vector<std::uint32_t>(const SimObject&)> relationshipList;
};

// The objects `object` saves relationships towards: those in its relationship table it holds
// variables towards. One a failed lookup left empty is left out, as the game's saves hold none.
std::vector<std::uint32_t> savedRelationships(const World& world, const SimObject& object);

// The object on the running lot that continues the one the object map lists at `id` with the type
// at `typeIndex`: the object at that id now, when its GUID is that type's. Ids are reused, so an
// object of another type there is a new one. Null for an id out of range.
const SimObject* continuingObject(const World& world, const objt::ObjectTypeTable& types, std::int32_t id,
                                  std::int32_t typeIndex);

// The counters a save gives new interactions their ids and numbers from: the simulator's id counter
// and the number the game numbers interactions from.
struct SaveCounters {
    std::uint16_t lastLinkId = 0;
    std::int32_t lastNumber = 0;
};

// The object's saved record as the game writes it from a running lot: its attributes, its object
// data, its position and level, the values its scripts left in its temporary registers and its
// token, the arrays and container slots it keeps, and a Sim's motives and person data. The stack
// its scripts are suspended in is written when every frame's code has an entry in the table, since
// that is how a frame names the object whose code it runs. A Sim's interactions are written as it
// has them now, new ones taking ids and numbers from `counters` (from 0 when none are given).
// Everything else, the parts of the record whose meaning is not known yet among them, is kept from
// `loaded`.
xobj::SavedObject objectRecord(const xobj::SavedObject& loaded, const SimObject& object, const TypeOwners& owners,
                               SaveCounters* counters = nullptr);

// The lot camera as the player left it, for the view saved with the lot.
struct LotCameraView {
    savedcamera::ViewState state;
    camera::Config rig;  // the camera file the view follows, for its zoom levels and orientations
};

// Whether the lot holds the records a save writes, at the version the game writes or at the
// version the game ships lots at, which a save upgrades. A lot last saved by another older version
// of the game holds records NeoTS2 does not write yet.
bool canSaveLot(const dbpf::PackageImage& package);

// Gives `owners` an entry for each type of an object on the running lot that the lot's type table
// does not hold yet, numbered after the table's last, as the game adds them when it saves. Only
// types whose objects get a record are added: not multi-tile types, whose objects are saved with a
// material block (XMTO), and Sims only when their character files are written (writtenNewSims).
void addNewTypes(const dbpf::PackageImage& package, LotSimulation& simulation, TypeOwners& owners);

// What a save put into a lot's package, for the log and the tests.
struct LotSaveCounts {
    int simulator = 0;   // simulator records written
    int objectMap = 0;   // object maps written
    int typesAdded = 0;  // types added to the object type table
    int typeTable = 0;   // object type tables written
    int camera = 0;      // cameras whose views were written
    int objects = 0;     // objects whose saved state was written
    int scenes = 0;      // objects whose scene record was written
    int objectsAdded = 0;  // objects made since the load given a record of their own
    int relationships = 0; // relationship records written, one for each object an object lists
    // New objects left out: the lot's table holds no entry for their type, there is no record to
    // start theirs from, or their record does not write.
    int objectsUnnamed = 0;
    int objectsRemade = 0;   // objects left out, the lot making them again as it loads
    int objectsKept = 0;   // objects left as loaded, having gone from the lot
    int stacksKept = 0;    // objects whose stack was left as loaded, a frame's code having no entry
    int objectsFresh = 0;  // new objects with no record of their type to start from, given an object just made's
    int keptAsLoaded = 0;  // resources written back as the lot was loaded with them
    std::uint16_t lastLinkId = 0;  // the simulator's id counter once the save gave out its ids
};

// Where a standing Sim is in its scene record: its position with the lot's x and y swapped (the
// scene's x is the position's y), at height `floorHeight`, turned about the up axis by the heading it
// is drawn at.
cobject::Place standingScenePlace(const SimObject& sim, float floorHeight);

// Where a Sim is in its scene record: at the origin while it is in an object's slot, whose bone
// carries it, else where it stands (standingScenePlace) at the floor height `floorHeight` gives.
cobject::Place simScenePlace(const SimObject& sim, const std::function<float(std::int32_t, float, float)>& floorHeight);

// Puts into `package` the records NeoTS2 can build from the running lot: the simulator's state, the
// object map, every object the lot was loaded with, and the live camera's view when one is given.
// Every other resource stays as the lot was loaded with it, so saving a lot never loses what NeoTS2
// does not fill yet. An object made since the lot loaded gets a record of its own, started from
// one of its type (see addNewObjects).
LotSaveCounts fillLotPackage(dbpf::PackageImage& package, const World& world, std::span<const std::int16_t> globals,
                             std::uint32_t tickRandomState, const TypeOwners& owners, const LotCameraView* view);
// `writtenNewSims` are the Sims made this run whose character files the same save writes. A lot
// holding any other Sim made this run is refused with std::runtime_error.
LotSaveCounts fillLotPackage(dbpf::PackageImage& package, LotSimulation& simulation, const LotCameraView* view,
                             const std::set<std::int16_t>& writtenNewSims = {},
                             std::function<std::optional<cobject::Place>(std::int16_t)> drawnPlace = {},
                             std::function<std::optional<std::vector<cobject::PosedJoint>>(std::int16_t)> drawnJoints = {},
                             std::function<std::vector<xobj::Effect>(std::int16_t)> runningEffects = {});

} // namespace ts2::sim
