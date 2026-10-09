#include "sim/vm.hpp"

#include "sim/lua_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace ts2::sim {

namespace {

constexpr std::uint16_t kSleepOpcode = 0x0000;
constexpr std::uint16_t kGenericSimsCallOpcode = 0x0001;
constexpr std::uint16_t kRandomNumberOpcode = 0x0008;
constexpr std::uint16_t kBreakPointOpcode = 0x000F;
constexpr std::uint16_t kSetToNextOpcode = 0x001F;
constexpr std::uint16_t kSetMotiveChangeOpcode = 0x001D;
constexpr std::uint16_t kWantSatisfyOpcode = 0x007C;
constexpr std::uint16_t kGetDirectionToOpcode = 0x000C;
constexpr std::uint16_t kGetDistanceToOpcode = 0x000B;
constexpr std::uint16_t kPushInteractionOpcode = 0x000D;
constexpr std::uint16_t kIdleForInputOpcode = 0x0011;
constexpr std::uint16_t kActionStringOpcode = 0x0032;
constexpr std::uint16_t kManageInventoryOpcode = 0x0033;
constexpr std::uint16_t kLuaScriptOpcode = 0x007E;
constexpr std::uint16_t kLuaScriptNames = 0x130;
constexpr std::uint16_t kActionStringTable = 0x12E;
// Object data whose bit 0x40 Idle for Input sets while interactions are waiting.
constexpr std::size_t kIdleFlagsField = 0x08;
constexpr std::int16_t kInteractionsWaitingBit = 0x40;
// Where Push Interaction reports why it failed (object data "primitive result").
constexpr std::size_t kPrimitiveResultField = 0x34;
constexpr std::size_t kPriorityPersonData = 0x21;
// Autonomy's choice: the object (person data) and the interaction number (object data).
constexpr std::size_t kBestActionObjectPersonData = 0x35;
constexpr std::size_t kTreeTableEntryField = 0x14;
constexpr std::size_t kMaxQueuedInteractions = 7;
// A level counts as this many tiles in distances.
constexpr float kLevelHeight = 10.0f;
// OBJD field a category want compares with an object's.
constexpr std::size_t kWantCategoryField = 0x2F;
// Set Motive Change rates are per Sim hour; the game keeps them per tick (1,800 ticks an hour).
constexpr float kTicksPerSimHour = 1800.0f;
constexpr std::uint16_t kCreateObjectOpcode = 0x002A;
constexpr std::uint16_t kRunTreeByNameOpcode = 0x001C;
constexpr std::uint16_t kRelationshipOpcode = 0x001A;
constexpr std::uint32_t kTestOwnTypeGuid = 0x4C7CAB2B;
constexpr std::uint16_t kRemoveObjectOpcode = 0x0012;
constexpr std::uint8_t kRemoveReturnImmediately = 0x01;
// Objects the game refuses to remove (a fixed list).
constexpr std::uint32_t kProtectedGuids[] = {
    0xCC7B08F8, 0x6C7B0905, 0xCC203477, 0x865A6812, 0x81E6BEF9, 0xD20C0AEE, 0x39CCF441,
};
constexpr std::uint8_t kTestAlternateDefinition = 0x01;
constexpr std::uint8_t kTestNeighborId = 0x02;
constexpr std::uint8_t kTestGuidExists = 0x04;
constexpr std::uint8_t kRelationshipFailIfMissing = 0x01;
constexpr std::uint8_t kRelationshipNeighborIds = 0x02;
constexpr std::uint8_t kRelationshipWrite = 0x04;
constexpr std::uint8_t kRelationshipFlag8 = 0x08;
constexpr std::uint16_t kRoomOutside = 1;
constexpr std::uint16_t kRoomLevel = 2;
constexpr std::uint16_t kRoomArea = 3;
constexpr std::uint16_t kRoomLayer1 = 7;
constexpr std::uint8_t kRunInMyStack = 0x00;
constexpr std::uint8_t kPushOntoMyStack = 0x02;
constexpr std::uint8_t kRunTypeTreeFromTemps = 0x03;
constexpr std::uint16_t kTreeNameTable = 0x12F;

constexpr std::uint8_t kPlaceOutOfWorld = 0x06;
constexpr std::uint8_t kCreateDoNotDuplicate = 0x01;
constexpr std::uint8_t kCreatePassObjectIds = 0x02;
constexpr std::uint8_t kCreateFromNeighbor = 0x04;
constexpr std::uint8_t kCreateFailIfTileOccupied = 0x08;
constexpr std::uint8_t kCreatePassTemp0 = 0x10;
constexpr std::uint8_t kCreateFromInventoryToken = 0x40;
constexpr std::uint8_t kCreateGuidFromTemps = 0x80;
constexpr std::uint8_t kCreateCopyMaterials = 0x02;
constexpr std::uint16_t kTemplateObjectType = 0x10;
constexpr std::uint16_t kNotCreatableObjectType = 0x11;
constexpr std::uint16_t kTestObjectTypeOpcode = 0x0020;
constexpr std::uint16_t kArrayOperationOpcode = 0x0076;
// Array Operation operations (byte 1). Sorting: 0x0C puts the highest first, 0x0D the lowest first.
constexpr std::uint8_t kArrayClear = 0;
constexpr std::uint8_t kArraySize = 1;
constexpr std::uint8_t kArrayResize = 2;
constexpr std::uint8_t kArrayFill = 3;
constexpr std::uint8_t kArrayInsertFront = 4;
constexpr std::uint8_t kArrayInsertBack = 5;
constexpr std::uint8_t kArrayInsertAtIterator = 6;
constexpr std::uint8_t kArrayRemoveFront = 7;
constexpr std::uint8_t kArrayRemoveBack = 8;
constexpr std::uint8_t kArrayRemoveAtIterator = 9;
constexpr std::uint8_t kArrayFind = 10;
constexpr std::uint8_t kArraySwap = 11;
constexpr std::uint8_t kArraySortDescending = 12;
constexpr std::uint8_t kArraySortAscending = 13;

// Generic Sims Calls with no effect on simulation state: camera, UI, preloading and 3D sync.
// 0x21 Update Footprint only informs the path planner, which does not exist yet; revisit with routing.
constexpr std::uint8_t kNoOpGenericCalls[] = {
    0x01, 0x02, 0x03, 0x12, 0x18, 0x1D, 0x1E, 0x1F, 0x21, 0x28, 0x29, 0x2A, 0x2C, 0x33,
};

// Generic call 0x41 starts the global "Init - Object" tree; the game returns true here
// (false would be an error exit).
constexpr std::uint8_t kInitObjectGenericCall = 0x41;

constexpr std::uint8_t kSearchObject = 0x00;
constexpr std::uint8_t kSearchPerson = 0x01;
constexpr std::uint8_t kSearchNonPerson = 0x02;
constexpr std::uint8_t kSearchObjectOfType = 0x04;
constexpr std::uint8_t kVerifyStackObject = 0x1A;
constexpr std::uint8_t kVerifyNeighborInStackObject = 0x1D;
constexpr std::uint8_t kVerifySimInStackObject = 0x1E;
constexpr std::uint8_t kSearchAnySpecies = 0x28;
constexpr std::uint8_t kSearchNonHuman = 0x29;
// Search 0x0C: objects whose category (object data 0x3B) equals temp 0.
constexpr std::uint8_t kSearchCategoryInTemp0 = 0x0C;
constexpr std::size_t kCategoryField = 0x3B;
constexpr std::uint8_t kSearchSpecies1Or2 = 0x2A;
constexpr std::uint8_t kSearchSpecies1 = 0x2B;
constexpr std::uint8_t kSearchSpecies2 = 0x2C;
constexpr std::uint8_t kSearchSpecies3 = 0x2D;

// Person data the species searches test (0 = human).
constexpr std::size_t kSpeciesPersonData = 0xBA;
constexpr std::size_t kLifeStagePersonData = 0x3A;
// Searches skip objects with bit 0x1 or 0x4 set in object data 0x38, or with a non-zero object
// data 0x6B. Set to Next flag bits 0 and 2 turn these filters off.
constexpr std::size_t kSearchSkipFlagsField = 0x38;
constexpr std::size_t kSearchSkipField = 0x6B;
// Search 0x08: objects on the same tile. A target with bit 0x2000 set in object data 0x2A is an
// error.
constexpr std::uint8_t kSearchSameTile = 0x08;
constexpr std::size_t kSameTileRefusedField = 0x2A;
// Search 0x03: the parts of a multi-tile object, which all hold the lead object's id in object
// data 0x13.
constexpr std::uint8_t kSearchMultiTilePart = 0x03;
// Search 0x12: like 0x04, with the GUID in temps 0 (low) and 1 (high).
constexpr std::uint8_t kSearchObjectOfTypeInTemps = 0x12;
// Search 0x09: the objects next to the object in a local, in the same room (object data 0x1D).
constexpr std::uint8_t kSearchAdjacent = 0x09;
constexpr std::size_t kRoomDataField = 0x1D;
// Searches that walk the neighborhood's neighbor ids instead of objects, by species.
constexpr std::uint8_t kSearchHumanNeighbor = 0x05;
constexpr std::uint8_t kSearchAnyNeighbor = 0x2E;
constexpr std::uint8_t kSearchNonHumanNeighbor = 0x2F;
constexpr std::uint8_t kSearchSpecies1Or2Neighbor = 0x30;
constexpr std::uint8_t kSearchSpecies1Neighbor = 0x31;
constexpr std::uint8_t kSearchSpecies2Neighbor = 0x32;
constexpr std::uint8_t kSearchSpecies3Neighbor = 0x33;
constexpr std::size_t kLeadTileObjectField = 0x13;
constexpr std::int16_t kSameTileRefusedBit = 0x2000;

struct Stop {
    Outcome outcome;
    std::string reason;
    std::string detail;
};

std::string hex(std::uint32_t value, int digits) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%0*X", digits, value);
    return text;
}


template <typename Container>
auto& slotAt(Container& values, std::int64_t index, const char* what) {
    if (index < 0 || static_cast<std::size_t>(index) >= values.size()) {
        throw Stop{Outcome::Error, std::string(what) + " index out of range", std::to_string(index)};
    }
    return values[static_cast<std::size_t>(index)];
}

std::int16_t flagBit(std::int16_t flag) {
    if (flag < 1 || flag > 16) {
        throw Stop{Outcome::Error, "flag number out of range", std::to_string(flag)};
    }
    return static_cast<std::int16_t>(1 << (flag - 1));
}

class Execution {
public:
    Execution(CodeSource& code, std::array<std::int16_t, kGlobalCount>& globals,
              ThreadState& thread, World* world, SimObject* me, std::mt19937& rng)
        : code_(code), globals_(globals), thread_(thread), world_(world), me_(me), rng_(rng) {}

    // Runs a tree to the end at once on a scratch thread, as `who` with the given stack object
    // (the game runs check trees this way). Set by the VM.
    std::function<RunResult(SimObject& who, std::uint32_t codeGroup, std::uint16_t tree, std::int16_t stackObject,
                            std::span<const std::int16_t> args)>
        runNow;

    // Private and semi-global code and constants come from the frame's code owner.
    std::uint32_t groupFor(std::uint16_t id, std::uint32_t codeOwner) {
        if (id < 0x1000) {
            return kGlobalGroup;
        }
        if (id < 0x2000) {
            return codeOwner;
        }
        return code_.semiGlobalGroup(codeOwner);
    }

    struct NamedCall {
        std::uint32_t group = 0;
        std::uint16_t instance = 0;
        std::shared_ptr<const bhav::Behavior> code;
        std::uint32_t codeOwner = 0;
        std::array<std::int16_t, 4> args{};
        std::optional<std::int16_t> stackObject;  // the new frame's, when it isn't the caller's
    };

    // Run Tree by Name. The tree name comes from string table 0x12F. Modes (byte 5):
    //   0 run in my stack, 2 push onto my stack as an action: one of the stack object's trees
    //   3: one of the trees of the object type whose GUID is in temps 0 (low) and 1 (high), run
    //      with the stack object (or me, without one) as its stack object
    // The tree is looked up in the object's group, then its semi-global group, then the global one.
    // Mode 1 (run in the stack object's stack) is not implemented.
    std::optional<NamedCall> namedTree(const bhav::Instruction& in, Frame& f) {
        const auto& o = in.operands;
        const std::uint8_t mode = o[5];
        if (mode != kRunInMyStack && mode != kPushOntoMyStack && mode != kRunTypeTreeFromTemps) {
            throw Stop{Outcome::Unsupported, "Run Tree by Name mode " + hex(mode, 2) + " not implemented", ""};
        }
        const std::uint8_t flags = o[2];
        const std::size_t nameIndex = o[4] | ((flags & 0x40) ? 0x100 : 0);
        std::uint32_t treeGroup = 0;
        std::optional<std::int16_t> frameStackObject;
        if (mode == kRunTypeTreeFromTemps) {
            const std::uint32_t guid = static_cast<std::uint32_t>(static_cast<std::uint16_t>(thread_.temps[0])) |
                                       static_cast<std::uint32_t>(static_cast<std::uint16_t>(thread_.temps[1])) << 16;
            const ObjectType* type = code_.objectType(guid);
            if (type == nullptr) {
                throw Stop{Outcome::Error, "you sent me a crappy GUID, please fix it", hex(guid, 8)};
            }
            treeGroup = type->group;
            frameStackObject = world().find(stackObject()) != nullptr ? stackObject() : me().id;
        } else {
            treeGroup = stack().group;
        }
        const std::uint32_t tableGroup = (flags & 0x01) ? kGlobalGroup
                                       : (flags & 0x02) ? (mode == kRunTypeTreeFromTemps ? treeGroup : stack().group)
                                                        : f.codeOwner;
        const auto name = code_.string(tableGroup, kTreeNameTable, nameIndex);
        if (!name || name->empty()) {
            return std::nullopt;
        }

        NamedCall call;
        call.stackObject = frameStackObject;
        for (const std::uint32_t group : {treeGroup, code_.semiGlobalGroup(treeGroup), kGlobalGroup}) {
            if (const auto instance = code_.behaviorByName(group, *name)) {
                call.group = group;
                call.instance = *instance;
                call.code = code_.behavior(group, *instance);
                break;
            }
        }
        if (!call.code) {
            return std::nullopt;
        }
        call.codeOwner = treeGroup;

        if (flags & 0x20) {
            std::copy_n(f.params.begin(), std::min<std::size_t>(4, f.params.size()), call.args.begin());
        } else if (flags & 0x10) {
            for (std::size_t i = 0; i < 3; ++i) {
                const auto data = static_cast<std::uint16_t>(o[7 + 3 * i] | o[8 + 3 * i] << 8);
                call.args[i] = read(o[6 + 3 * i], data, f);
            }
        }
        return call;
    }

    std::int16_t read(std::uint8_t owner, std::uint16_t data, Frame& f) {
        switch (owner) {
        case owner::kMyAttribute:              return slotAt(me().attributes, data, "attribute");
        case owner::kStackObjectAttribute:     return slotAt(stack().attributes, data, "attribute");
        case owner::kMySemiAttribute:          return slotAt(me().semiAttributes, data, "semi-attribute");
        case owner::kStackObjectSemiAttribute: return slotAt(stack().semiAttributes, data, "semi-attribute");
        case owner::kMyData:                   return slotAt(me().data, data, "object data");
        case owner::kStackObjectData:          return slotAt(stack().data, data, "object data");
        case owner::kMyMotives:                return static_cast<std::int16_t>(motive(me(), data));
        case owner::kStackObjectMotives:       return static_cast<std::int16_t>(motive(stack(), data));
        case owner::kStackObjectMotiveByTemp:
            return static_cast<std::int16_t>(motive(stack(), slotAt(thread_.temps, data, "temp")));
        case owner::kMyPersonData:                return personData(me(), data);
        case owner::kStackObjectPersonData:       return personData(stack(), data);
        case owner::kMyPersonDataByTemp:          return personData(me(), tempValue(data));
        case owner::kStackObjectPersonDataByTemp: return personData(stack(), tempValue(data));
        case owner::kStackObjectDefinition:
            return static_cast<std::int16_t>(
                slotAt(stack().definition.fields, data, "object definition field"));
        case owner::kGlobal:        return slotAt(globals_, data, "global");
        case owner::kLiteral:       return static_cast<std::int16_t>(data);
        case owner::kTemp:          return slotAt(thread_.temps, data, "temp");
        case owner::kParam:         return slotAt(f.params, data, "param");
        case owner::kStackObjectId: return stackObject();
        case owner::kTempByTemp:
            return slotAt(thread_.temps, slotAt(thread_.temps, data, "temp"), "temp[temp]");
        case owner::kLocal:         return slotAt(f.locals, data, "local");
        case owner::kConstant:      return constant(data, f.codeOwner);
        case owner::kConstantByTemp: return constantByTemp(data, f.codeOwner);
        case owner::kRoomByTemp:     return roomField(thread_.temps[0], data);
        case owner::kNeighborInStackObject: return neighborField(stackObject(), data);
        case owner::kNeighborPersonData:    return neighborPersonData(data);
        case owner::kNeighborObjectDefinition: return neighborDefinitionField(data);
        case owner::kMySlot:                return slotValue(me(), data);
        case owner::kStackObjectSlot:       return slotValue(stack(), data);
        case owner::kMyArrayIterator:            return arrayValue(me(), data, false);
        case owner::kStackObjectArrayIterator:   return arrayValue(stack(), data, false);
        case owner::kMyArrayAtIterator:          return arrayValue(me(), data, true);
        case owner::kStackObjectArrayAtIterator: return arrayValue(stack(), data, true);
        default:
            throw Stop{Outcome::Unsupported, "data owner not implemented", hex(owner, 2)};
        }
    }

    // Room fields: 0 ambient light, 1 outside, 2 level, 3 area (counted in quarter tiles), 4 pool,
    // 5 windows. Fields 1 and 7 are the room's region in room layers 0 and 1 (0 = outdoors).
    std::int16_t roomField(std::int16_t roomId, std::uint16_t field) {
        const RoomInfo* room = world().rooms().room(roomId);
        if (room == nullptr) {
            throw Stop{Outcome::Unsupported, "room not known", std::to_string(roomId)};
        }
        switch (field) {
        case kRoomOutside:
        case kRoomLayer1: {
            const std::int32_t region = room->layerRegion[field == kRoomOutside ? 0 : 1];
            if (region == RoomMap::kNoRoom) {
                throw Stop{Outcome::Unsupported, "room has no region in that layer", std::to_string(roomId)};
            }
            return static_cast<std::int16_t>(region);
        }
        case kRoomLevel:   return static_cast<std::int16_t>(room->level + 1);
        case kRoomArea:    return static_cast<std::int16_t>(room->area());
        default:
            throw Stop{Outcome::Unsupported, "room field not implemented", std::to_string(field)};
        }
    }

    // Slot owners: the object in container slot [data], 0 when the slot is empty.
    std::int16_t slotValue(SimObject& object, std::uint16_t data) {
        const auto slot = static_cast<std::int16_t>(data);
        if (slot < 0 || static_cast<std::size_t>(slot) >= object.slots.size()) {
            throw Stop{Outcome::Error, "slot number out of range", std::to_string(slot)};
        }
        return object.slots[static_cast<std::size_t>(slot)];
    }

    // Object array owners: the iterator of array [index], or the element it points at.
    std::int16_t& arrayValue(SimObject& object, std::uint16_t data, bool elementAtIterator) {
        const auto index = static_cast<std::int16_t>(data);
        if (index < 0 || static_cast<std::size_t>(index) >= object.arrays.size()) {
            throw Stop{Outcome::Error, "bad object array", std::to_string(index)};
        }
        std::int16_t& iterator = object.arrayIterators[static_cast<std::size_t>(index)];
        if (!elementAtIterator) {
            return iterator;
        }
        auto& a = object.arrays[static_cast<std::size_t>(index)];
        if (iterator < 0 || static_cast<std::size_t>(iterator) >= a.size()) {
            throw Stop{Outcome::Error, "array iterator does not exist", std::to_string(iterator)};
        }
        return a[static_cast<std::size_t>(iterator)];
    }

    // "Neighbor in stack object" fields, for the neighbor whose id is in the stack object (0 for an
    // unknown neighbor or a field above 10): 0 the neighbor's Sim on this lot, 2 age, 3 and 4 always 0.
    // Fields 1 and 5-10 come from the neighbor's household and family and are not implemented.
    std::int16_t neighborField(std::int16_t neighborId, std::uint16_t field) {
        if (!world().neighbors()) {
            throw Stop{Outcome::Unsupported, "neighbor fields need the neighborhood", std::to_string(field)};
        }
        const SavedNeighbor* n = world().neighbor(neighborId);
        if (n == nullptr || field > 10) {
            return 0;
        }
        switch (field) {
        case 0: {
            const SimObject* sim = world().nextAfter(std::numeric_limits<std::int16_t>::min(),
                                                     [n](const SimObject& s) {
                                                         return s.isPerson() && s.definition.guid() == n->guid;
                                                     });
            return sim ? sim->id : 0;
        }
        case 2:
            return n->personData.size() > kLifeStagePersonData ? n->personData[kLifeStagePersonData] : 0;
        case 3:
        case 4:
            return 0;
        default:
            throw Stop{Outcome::Unsupported, "neighbor field not implemented", std::to_string(field)};
        }
    }

    // "Neighbor's object definition": field [data] of the object definition of the neighbor whose
    // id is in the stack object.
    std::int16_t neighborDefinitionField(std::uint16_t data) {
        if (!world().neighbors()) {
            throw Stop{Outcome::Unsupported, "neighbor fields need the neighborhood", std::to_string(data)};
        }
        const SavedNeighbor* neighbor = world().neighbor(stackObject());
        if (neighbor == nullptr) {
            throw Stop{Outcome::Error, "missing neighbor for data access", std::to_string(stackObject())};
        }
        const ObjectType* type = code_.objectType(neighbor->guid);
        if (type == nullptr || data >= type->definition.fields.size()) {
            throw Stop{Outcome::Error, "index into object definition out of range", std::to_string(data)};
        }
        return static_cast<std::int16_t>(type->definition.fields[data]);
    }

    // "Neighbor's person data": the stack object holds a neighbor id.
    std::int16_t& neighborPersonData(std::uint16_t data) {
        if (!world().neighbors()) {
            throw Stop{Outcome::Unsupported, "neighbor person data needs the neighborhood", std::to_string(data)};
        }
        std::vector<std::int16_t>* values = world().neighborPersonData(stackObject());
        if (values == nullptr) {
            throw Stop{Outcome::Error, "missing neighbor for data access", std::to_string(stackObject())};
        }
        const auto index = static_cast<std::int16_t>(data);
        if (index < 0 || static_cast<std::size_t>(index) >= values->size()) {
            throw Stop{Outcome::Error, "person data field is out of range", std::to_string(index)};
        }
        return (*values)[static_cast<std::size_t>(index)];
    }

    void write(std::uint8_t owner, std::uint16_t data, Frame& f, std::int16_t value) {
        switch (owner) {
        case owner::kMyAttribute:              slotAt(me().attributes, data, "attribute") = value; return;
        case owner::kStackObjectAttribute:     slotAt(stack().attributes, data, "attribute") = value; return;
        case owner::kMySemiAttribute:          slotAt(me().semiAttributes, data, "semi-attribute") = value; return;
        case owner::kStackObjectSemiAttribute: slotAt(stack().semiAttributes, data, "semi-attribute") = value; return;
        case owner::kMyData:                   slotAt(me().data, data, "object data") = value; return;
        case owner::kStackObjectData:          slotAt(stack().data, data, "object data") = value; return;
        case owner::kMyMotives:          setMotive(me(), data, value); return;
        case owner::kStackObjectMotives: setMotive(stack(), data, value); return;
        case owner::kStackObjectMotiveByTemp:
            setMotive(stack(), slotAt(thread_.temps, data, "temp"), value);
            return;
        case owner::kMyPersonData:                personData(me(), data) = value; return;
        case owner::kStackObjectPersonData:       personData(stack(), data) = value; return;
        case owner::kMyPersonDataByTemp:          personData(me(), tempValue(data)) = value; return;
        case owner::kStackObjectPersonDataByTemp: personData(stack(), tempValue(data)) = value; return;
        case owner::kStackObjectDefinition:
            // Game code writes these (e.g. "Function - Load"); each object keeps its own copy.
            slotAt(stack().definition.fields, data, "object definition field") =
                static_cast<std::uint16_t>(value);
            return;
        case owner::kGlobal:        slotAt(globals_, data, "global") = value; return;
        case owner::kTemp:          slotAt(thread_.temps, data, "temp") = value; return;
        case owner::kParam:         slotAt(f.params, data, "param") = value; return;
        case owner::kStackObjectId: stackObject() = value; return;
        case owner::kTempByTemp:
            slotAt(thread_.temps, slotAt(thread_.temps, data, "temp"), "temp[temp]") = value;
            return;
        case owner::kLocal:         slotAt(f.locals, data, "local") = value; return;
        case owner::kLiteral:
        case owner::kConstant:
        case owner::kConstantByTemp:
            throw Stop{Outcome::Error, "write to a read-only operand", hex(owner, 2)};
        case owner::kRoomByTemp:
            throw Stop{Outcome::Unsupported, "writing room fields not implemented", hex(owner, 2)};
        case owner::kNeighborInStackObject:
            throw Stop{Outcome::Unsupported, "writing neighbor fields not implemented", hex(owner, 2)};
        case owner::kNeighborPersonData: neighborPersonData(data) = value; return;
        case owner::kNeighborObjectDefinition:
            throw Stop{Outcome::Unsupported, "writing a neighbor's object definition not implemented", hex(owner, 2)};
        case owner::kMySlot:
        case owner::kStackObjectSlot:
            throw Stop{Outcome::Unsupported, "writing slots not implemented", hex(owner, 2)};
        case owner::kMyArrayIterator:            arrayValue(me(), data, false) = value; return;
        case owner::kStackObjectArrayIterator:   arrayValue(stack(), data, false) = value; return;
        case owner::kMyArrayAtIterator:          arrayValue(me(), data, true) = value; return;
        case owner::kStackObjectArrayAtIterator: arrayValue(stack(), data, true) = value; return;
        default:
            throw Stop{Outcome::Unsupported, "data owner not implemented", hex(owner, 2)};
        }
    }

    // Expression doesn't use operand byte 4 or bytes 8-15.
    bool expression(const bhav::Instruction& in, Frame& f) {
        const auto x = bhav::decodeExpression(in);
        auto lhs = [&] { return read(x.lhsOwner, x.lhsData, f); };
        auto rhs = [&] { return read(x.rhsOwner, x.rhsData, f); };
        auto set = [&](int value) {
            const auto result = static_cast<std::int16_t>(value);
            write(x.lhsOwner, x.lhsData, f, result);
            return result;
        };
        auto nonZero = [](std::int16_t value) {
            if (value == 0) {
                throw Stop{Outcome::Error, "division by zero", ""};
            }
            return value;
        };

        switch (x.op) {
        case op::kGreater:             return lhs() > rhs();
        case op::kLess:                return lhs() < rhs();
        case op::kEquals:              return lhs() == rhs();
        case op::kGreaterOrEqual:      return lhs() >= rhs();
        case op::kLessOrEqual:         return lhs() <= rhs();
        case op::kNotEqual:            return lhs() != rhs();
        case op::kAdd:                 set(lhs() + rhs()); return true;
        case op::kSubtract:            set(lhs() - rhs()); return true;
        case op::kAssign:              set(rhs()); return true;
        case op::kMultiply:            set(lhs() * rhs()); return true;
        case op::kDivide:              { const auto d = nonZero(rhs()); set(lhs() / d); return true; }
        case op::kModulo:              { const auto d = nonZero(rhs()); set(lhs() % d); return true; }
        case op::kAnd:                 set(lhs() & rhs()); return true;
        case op::kOr:                  set(lhs() | rhs()); return true;
        case op::kXor:                 set(lhs() ^ rhs()); return true;
        case op::kAbs:                 set(std::abs(rhs())); return true;
        case op::kIsFlagSet:           return (lhs() & flagBit(rhs())) != 0;
        case op::kSetFlag:             set(lhs() | flagBit(rhs())); return true;
        case op::kClearFlag:           set(lhs() & ~flagBit(rhs())); return true;
        case op::kIncrementAndLess:    return set(lhs() + 1) < rhs();
        case op::kDecrementAndGreater: return set(lhs() - 1) > rhs();
        default:
            throw Stop{Outcome::Unsupported, "expression operator not implemented", hex(x.op, 2)};
        }
    }

    bool primitive(const bhav::Instruction& in, Frame& f) {
        switch (in.opcode) {
        case bhav::kExpressionOpcode: return expression(in, f);
        case kSetToNextOpcode:        return setToNext(in, f);
        case kSetMotiveChangeOpcode:  return setMotiveChange(in, f);
        case kWantSatisfyOpcode:      return wantSatisfy(in, f);
        case kGetDirectionToOpcode:   return getDirectionTo(in, f);
        case kGetDistanceToOpcode:    return getDistanceTo(in, f);
        case kPushInteractionOpcode:  return pushInteraction(in, f);
        case kActionStringOpcode:     return actionString(in, f);
        case kManageInventoryOpcode:  return manageInventory(in, f);
        case kLuaScriptOpcode:        return luaScript(in, f);
        case kTestObjectTypeOpcode:   return testObjectType(in, f);
        case kRandomNumberOpcode:     return randomNumber(in, f);
        case kGenericSimsCallOpcode:  return genericSimsCall(in);
        case kCreateObjectOpcode:     return createObject(in, f);
        case kRelationshipOpcode:     return relationship(in, f);
        case kRemoveObjectOpcode:     return removeObject(in);
        case kBreakPointOpcode:       return breakPoint(in, f);
        case kArrayOperationOpcode:   return arrayOperation(in, f);
        default:
            throw Stop{Outcome::Unsupported, "primitive not implemented", hex(in.opcode, 4)};
        }
    }

    // Operands:
    //   byte 1      operation
    //   byte 2      whose arrays (0 = mine, else the stack object's)
    //   bytes 3-4   array
    //   byte 5, 6-7 value (owner, data)
    //   byte 8, 9-10 iterator (owner, data)
    // Only "find" can return false.
    bool arrayOperation(const bhav::Instruction& in, Frame& f) {
        const auto& o = in.operands;
        SimObject* object = &me();
        if (o[2] != 0) {
            object = world().find(stackObject());
            if (object == nullptr) {
                throw Stop{Outcome::Error, "stack number out of range", std::to_string(stackObject())};
            }
        }
        const auto arrayIndex = static_cast<std::int16_t>(o[3] | o[4] << 8);
        if (arrayIndex < 0 || static_cast<std::size_t>(arrayIndex) >= object->arrays.size()) {
            throw Stop{Outcome::Error, "bad object array", std::to_string(arrayIndex)};
        }
        std::vector<std::int16_t>& a = object->arrays[static_cast<std::size_t>(arrayIndex)];
        const std::uint8_t valueOwner = o[5];
        const auto valueData = static_cast<std::uint16_t>(o[6] | o[7] << 8);
        const std::uint8_t iteratorOwner = o[8];
        const auto iteratorData = static_cast<std::uint16_t>(o[9] | o[10] << 8);
        auto iterator = [&]() {
            const std::int16_t i = read(iteratorOwner, iteratorData, f);
            if (i < 0 || static_cast<std::size_t>(i) >= a.size()) {
                throw Stop{Outcome::Error, "array iterator does not exist", std::to_string(i)};
            }
            return static_cast<std::size_t>(i);
        };
        auto requireElements = [&]() {
            if (a.empty()) {
                throw Stop{Outcome::Error, "array iterator does not exist", "empty array"};
            }
        };

        switch (o[1]) {
        case kArrayClear:
            a.clear();
            return true;
        case kArraySize:
            write(valueOwner, valueData, f, static_cast<std::int16_t>(a.size()));
            return true;
        case kArrayResize: {
            const std::int16_t size = read(valueOwner, valueData, f);
            if (size < 0) {
                throw Stop{Outcome::Unsupported, "array resize to a negative size", std::to_string(size)};
            }
            a.resize(static_cast<std::size_t>(size), 0);
            return true;
        }
        case kArrayFill:
            std::fill(a.begin(), a.end(), read(valueOwner, valueData, f));
            return true;
        case kArrayInsertFront:
            a.insert(a.begin(), read(valueOwner, valueData, f));
            return true;
        case kArrayInsertBack:
            a.push_back(read(valueOwner, valueData, f));
            return true;
        case kArrayInsertAtIterator: {
            const std::int16_t value = read(valueOwner, valueData, f);
            a.insert(a.begin() + static_cast<std::ptrdiff_t>(iterator()), value);
            return true;
        }
        case kArrayRemoveFront:
            requireElements();
            a.erase(a.begin());
            return true;
        case kArrayRemoveBack:
            requireElements();
            a.pop_back();
            return true;
        case kArrayRemoveAtIterator:
            a.erase(a.begin() + static_cast<std::ptrdiff_t>(iterator()));
            return true;
        case kArrayFind: {
            const std::int16_t value = read(valueOwner, valueData, f);
            const std::int16_t start = read(iteratorOwner, iteratorData, f);
            if (start < 0 || static_cast<std::size_t>(start) >= a.size()) {
                return false;
            }
            const auto it = std::find(a.begin() + start, a.end(), value);
            if (it == a.end()) {
                return false;
            }
            write(iteratorOwner, iteratorData, f, static_cast<std::int16_t>(it - a.begin()));
            return true;
        }
        case kArraySwap: {
            const std::int16_t first = read(valueOwner, valueData, f);
            if (first < 0 || static_cast<std::size_t>(first) >= a.size()) {
                throw Stop{Outcome::Error, "array iterator does not exist", std::to_string(first)};
            }
            std::swap(a[static_cast<std::size_t>(first)], a[iterator()]);
            return true;
        }
        case kArraySortDescending:
            std::stable_sort(a.begin(), a.end(), std::greater<>());
            return true;
        case kArraySortAscending:
            std::stable_sort(a.begin(), a.end());
            return true;
        default:
            throw Stop{Outcome::Error, "unknown array operation", std::to_string(o[1])};
        }
    }

    static bool genericSimsCall(const bhav::Instruction& in) {
        const std::uint8_t call = in.operands[0];
        if (std::find(std::begin(kNoOpGenericCalls), std::end(kNoOpGenericCalls), call) !=
            std::end(kNoOpGenericCalls)) {
            return true;
        }
        if (call == kInitObjectGenericCall) {
            return true;
        }
        throw Stop{Outcome::Unsupported, "Generic Sims Call not implemented", hex(call, 2)};
    }

    std::int32_t sleepTicks(const bhav::Instruction& in, Frame& f) {
        const auto param = static_cast<std::uint16_t>(in.operands[0] | in.operands[1] << 8);
        return slotAt(f.params, param, "param");
    }

    // Operands:
    //   bytes 0-1  who (0 = me, 1 = stack object, 2 = nobody)
    //   byte 2     bit 0 return immediately; bit 1 clear = clean up all
    // A few object types can't be removed. Removing me without "return immediately" ends my run.
    bool removeObject(const bhav::Instruction& in) {
        const auto& o = in.operands;
        const auto who = static_cast<std::int16_t>(o[0] | o[1] << 8);
        if (who < 0 || who > 2) {
            throw Stop{Outcome::Error, "illegal owner field in data reference", std::to_string(who)};
        }
        const std::int16_t id = who == 0 ? me().id : who == 1 ? stackObject() : 0;
        SimObject* object = world().find(id);
        if (object == nullptr) {
            return false;
        }
        const std::uint32_t guid = object->definition.guid();
        if (std::find(std::begin(kProtectedGuids), std::end(kProtectedGuids), guid) !=
            std::end(kProtectedGuids)) {
            return false;
        }
        world().remove(*object);
        if (object == me_ && (o[2] & kRemoveReturnImmediately) == 0) {
            throw Stop{Outcome::MeRemoved, "", ""};
        }
        return true;
    }

    // Operands:
    //   byte 0     variable
    //   byte 1     flags: 0x01 fail if the variable doesn't exist, 0x02 neighbor ids, 0x04 write
    //   bytes 2-4  object A (owner, data)
    //   bytes 5-7  object B
    //   bytes 8-10 value
    // Missing variables are created unless flag 0x01 is set.
    bool relationship(const bhav::Instruction& in, Frame& f) {
        const auto& o = in.operands;
        const std::uint8_t variable = o[0];
        const std::uint8_t flags = o[1];
        if (flags & kRelationshipFlag8) {
            throw Stop{Outcome::Unsupported, "Relationship flag 0x08 not implemented", ""};
        }
        const std::int16_t from = read(o[2], static_cast<std::uint16_t>(o[3] | o[4] << 8), f);
        const std::int16_t to = read(o[5], static_cast<std::uint16_t>(o[6] | o[7] << 8), f);
        std::vector<std::int16_t>* found = nullptr;
        if (flags & kRelationshipNeighborIds) {
            // A and B are neighbor ids: use A's list towards B.
            const auto& neighbors = world().neighbors();
            if (!neighbors) {
                throw Stop{Outcome::Unsupported, "Relationship with neighbor ids needs the neighborhood", ""};
            }
            if (!neighbors->contains(from) || !neighbors->contains(to)) {
                throw Stop{Outcome::Error, "neighbor not found", std::to_string(from) + " " + std::to_string(to)};
            }
            found = &world().neighborRelationship(from, to);
        } else {
            if (world().find(from) == nullptr || world().find(to) == nullptr) {
                throw Stop{Outcome::Error, "could not find object reference",
                           std::to_string(from) + " " + std::to_string(to)};
            }
            found = &world().relationship(from, to);
        }

        std::vector<std::int16_t>& values = *found;
        if (values.size() <= variable) {
            if (flags & kRelationshipFailIfMissing) {
                return false;
            }
            values.resize(variable + 1u, 0);
        }
        const auto valueData = static_cast<std::uint16_t>(o[9] | o[10] << 8);
        if (flags & kRelationshipWrite) {
            values[variable] = read(o[8], valueData, f);
        } else {
            write(o[8], valueData, f, values[variable]);
        }
        return true;
    }

    // Operands:
    //   bytes 0-3  GUID
    //   byte 4     where to create the object
    //   byte 5     flags
    //   byte 6     local
    //   byte 9     slot
    //   byte 10    more flags
    // The new object becomes the stack object. Only "out of world" placement is implemented.
    bool createObject(const bhav::Instruction& in, Frame&) {
        const auto& o = in.operands;
        const std::uint8_t flags = o[5];
        if (flags & kCreateFromNeighbor) {
            throw Stop{Outcome::Unsupported, "Create Object from a neighbor needs the neighborhood", ""};
        }
        if (flags & kCreateFromInventoryToken) {
            throw Stop{Outcome::Unsupported, "Create Object from an inventory token", ""};
        }
        if ((flags & kCreatePassObjectIds) && (flags & kCreatePassTemp0)) {
            throw Stop{Outcome::Error, "more than one passing value specified", ""};
        }

        std::uint32_t guid = readGuid(o);
        if (flags & kCreateGuidFromTemps) {
            guid = static_cast<std::uint16_t>(thread_.temps[0]) |
                   static_cast<std::uint32_t>(static_cast<std::uint16_t>(thread_.temps[1])) << 16;
        }
        const ObjectType* type = code_.objectType(guid);
        if (type == nullptr) {
            return false;
        }
        const std::uint16_t objectKind = type->definition.field(objd::field::kType);
        if (objectKind == kTemplateObjectType) {
            throw Stop{Outcome::Error, "trying to create instance of template object", hex(guid, 8)};
        }
        if (objectKind == kNotCreatableObjectType) {
            return false;
        }
        if (o[4] != kPlaceOutOfWorld) {
            throw Stop{Outcome::Unsupported, "Create Object placement not implemented", hex(o[4], 2)};
        }
        if ((o[10] & kCreateCopyMaterials) && world().find(thread_.temps[5]) == nullptr) {
            throw Stop{Outcome::Error, "copy-materials source in temp 5 does not exist",
                       std::to_string(thread_.temps[5])};
        }

        SimObject& created = world().create(type->group, type->definition, type->containerSlots);
        stackObject() = created.id;
        return true;
    }

    // Byte 4 bit 0 means "ignore in release"; otherwise a non-zero condition is an error.
    bool breakPoint(const bhav::Instruction& in, Frame& f) {
        const auto& o = in.operands;
        if (o[4] & 1) {
            return true;
        }
        const auto data = static_cast<std::uint16_t>(o[0] | o[1] << 8);
        const auto conditionOwner = static_cast<std::uint16_t>(o[2] | o[3] << 8);
        if (conditionOwner > 0xFF) {
            throw Stop{Outcome::Error, "illegal owner field in data reference", hex(conditionOwner, 4)};
        }
        if (read(static_cast<std::uint8_t>(conditionOwner), data, f) != 0) {
            throw Stop{Outcome::Error, "break point hit", ""};
        }
        return true;
    }

    // Range 0 writes nothing; otherwise the value is the high half of a 32-bit random number times
    // the range.
    bool randomNumber(const bhav::Instruction& in, Frame& f) {
        const auto& o = in.operands;
        const auto destData = static_cast<std::uint16_t>(o[0] | o[1] << 8);
        const auto rangeData = static_cast<std::uint16_t>(o[4] | o[5] << 8);
        const std::int16_t range = read(o[6], rangeData, f);
        if (range == 0) {
            return true;
        }
        const auto scaled = static_cast<std::uint64_t>(static_cast<std::uint32_t>(rng_())) *
                            static_cast<std::uint32_t>(static_cast<std::int32_t>(range));
        write(o[2], destData, f, static_cast<std::int16_t>(scaled >> 32));
        return true;
    }

    // Operands:
    //   bytes 0-3    GUID
    //   byte 4       search type; bit 0x80 = explicit target
    //   byte 5, 7    target (owner, index)
    //   byte 8       filter flags
    //   bytes 9-10   object data field that must equal bytes 11-12 (with flag bit 1)
    bool setToNext(const bhav::Instruction& in, Frame& f) {
        const auto& o = in.operands;
        const std::uint32_t guid = readGuid(o);
        const std::uint8_t searchType = o[4] & 0x7F;
        std::uint8_t targetOwner = o[5];
        std::uint16_t targetData = o[7];
        if ((o[4] & 0x80) == 0) {
            targetOwner = owner::kStackObjectId;
            targetData = 0;
        }

        switch (searchType) {
        case kVerifyStackObject:
            return world().find(read(targetOwner, targetData, f)) != nullptr;
        case kVerifySimInStackObject: {
            const SimObject* object = world().find(read(targetOwner, targetData, f));
            return object != nullptr && object->isPerson();
        }
        case kVerifyNeighborInStackObject: {
            // The target holds a neighbor id; true when the neighborhood has that neighbor.
            const auto& neighbors = world().neighbors();
            if (!neighbors) {
                throw Stop{Outcome::Unsupported, "Set to Next needs the neighborhood", hex(searchType, 2)};
            }
            return neighbors->contains(read(targetOwner, targetData, f));
        }
        case kSearchSameTile:
            return nextOnSameTile(o, targetOwner, targetData, f);
        case kSearchMultiTilePart:
            return nextMultiTilePart(targetOwner, targetData, f);
        case kSearchAdjacent:
            return nextAdjacent(o, targetOwner, targetData, f);
        default:
            break;
        }

        const std::uint8_t flags = o[8];
        const bool requireField = (flags & 0x02) != 0;
        const auto fieldIndex = static_cast<std::uint16_t>(o[9] | o[10] << 8);
        const auto fieldValue = static_cast<std::int16_t>(o[11] | o[12] << 8);
        if (requireField && fieldIndex >= kObjectDataCount) {
            throw Stop{Outcome::Error, "object data index out of range", std::to_string(fieldIndex)};
        }
        auto passesFilters = [&](const SimObject& s) {
            if ((flags & 0x01) == 0 && (s.data[kSearchSkipFlagsField] & 0x05) != 0) {
                return false;
            }
            if ((flags & 0x04) == 0 && s.data[kSearchSkipField] != 0) {
                return false;
            }
            return !requireField || s.data[fieldIndex] == fieldValue;
        };

        if (searchType == kSearchHumanNeighbor || (searchType >= kSearchAnyNeighbor && searchType <= kSearchSpecies3Neighbor)) {
            return nextNeighbor(searchType, passesFilters, targetOwner, targetData, f);
        }

        auto speciesIs = [](auto accept) {
            return [accept](const SimObject& s) {
                return s.isPerson() && accept(s.personData[kSpeciesPersonData]);
            };
        };
        std::function<bool(const SimObject&)> matches;
        switch (searchType) {
        case kSearchObject:        matches = [](const SimObject&) { return true; }; break;
        case kSearchNonPerson:     matches = [](const SimObject& s) { return !s.isPerson(); }; break;
        case kSearchObjectOfType:
        case kSearchObjectOfTypeInTemps: {
            const std::uint32_t type = searchType == kSearchObjectOfType
                                           ? guid
                                           : static_cast<std::uint32_t>(static_cast<std::uint16_t>(thread_.temps[0])) |
                                                 static_cast<std::uint32_t>(static_cast<std::uint16_t>(thread_.temps[1])) << 16;
            // An unknown object type is false and leaves the target as it is.
            if (code_.objectType(type) == nullptr) {
                return false;
            }
            matches = [type](const SimObject& s) { return s.definition.guid() == type; };
            break;
        }
        case kSearchPerson:        matches = speciesIs([](std::int16_t v) { return v == 0; }); break;
        case kSearchAnySpecies:    matches = speciesIs([](std::int16_t) { return true; }); break;
        case kSearchNonHuman:      matches = speciesIs([](std::int16_t v) { return v != 0; }); break;
        case kSearchSpecies1Or2:   matches = speciesIs([](std::int16_t v) { return v == 1 || v == 2; }); break;
        case kSearchSpecies1:      matches = speciesIs([](std::int16_t v) { return v == 1; }); break;
        case kSearchSpecies2:      matches = speciesIs([](std::int16_t v) { return v == 2; }); break;
        case kSearchSpecies3:      matches = speciesIs([](std::int16_t v) { return v == 3; }); break;
        case kSearchCategoryInTemp0: {
            const std::int16_t category = thread_.temps[0];
            matches = [category](const SimObject& s) { return s.data[kCategoryField] == category; };
            break;
        }
        default:
            throw Stop{Outcome::Unsupported, "Set to Next search type not implemented",
                       hex(searchType, 2)};
        }

        SimObject* next = world().nextAfter(read(targetOwner, targetData, f), [&](const SimObject& s) {
            return matches(s) && passesFilters(s);
        });
        if (next == nullptr) {
            write(targetOwner, targetData, f, 0);
            return false;
        }
        write(targetOwner, targetData, f, next->id);
        return true;
    }

    // Next neighbor id after the target whose species fits the search. A neighbor whose Sim is on
    // the lot must also pass the search filters; one whose Sim isn't here always qualifies.
    // Running out leaves the target at 0 and is false.
    template <typename Filters>
    bool nextNeighbor(std::uint8_t searchType, const Filters& passesFilters, std::uint8_t targetOwner,
                      std::uint16_t targetData, Frame& f) {
        const auto* ids = world().neighbors();
        if (ids == nullptr) {
            throw Stop{Outcome::Unsupported, "Set to Next needs the neighborhood", hex(searchType, 2)};
        }
        auto speciesFits = [searchType](std::int16_t species) {
            switch (searchType) {
            case kSearchHumanNeighbor:       return species == 0;
            case kSearchAnyNeighbor:         return true;
            case kSearchNonHumanNeighbor:    return species != 0;
            case kSearchSpecies1Or2Neighbor: return species == 1 || species == 2;
            case kSearchSpecies1Neighbor:    return species == 1;
            case kSearchSpecies2Neighbor:    return species == 2;
            case kSearchSpecies3Neighbor:    return species == 3;
            default:                         return false;
            }
        };
        for (auto it = ids->upper_bound(read(targetOwner, targetData, f)); it != ids->end(); ++it) {
            const auto* personData = world().neighborPersonData(*it);
            if (personData == nullptr || !speciesFits((*personData)[kSpeciesPersonData])) {
                continue;
            }
            const SavedNeighbor* neighbor = world().neighbor(*it);
            const SimObject* sim = world().nextAfter(std::numeric_limits<std::int16_t>::min(), [&](const SimObject& s) {
                return s.isPerson() && s.definition.guid() == neighbor->guid;
            });
            if (sim == nullptr || passesFilters(*sim)) {
                write(targetOwner, targetData, f, *it);
                return true;
            }
        }
        write(targetOwner, targetData, f, 0);
        return false;
    }

    // Next object beside the object in local [byte 6], in its room: the first object on the tile
    // at +x, then +y, -x and -y, continuing after the side the target is on. Running out is false
    // and leaves the target as it is.
    bool nextAdjacent(const std::array<std::uint8_t, 16>& o, std::uint8_t targetOwner, std::uint16_t targetData,
                      Frame& f) {
        const std::uint8_t local = o[6];
        if (local >= f.locals.size()) {
            throw Stop{Outcome::Error, "stack number out of range", std::to_string(local)};
        }
        const SimObject* reference = world().find(f.locals[local]);
        if (reference == nullptr) {
            throw Stop{Outcome::Error, "reference to tree parameter when no parameter exists",
                       std::to_string(f.locals[local])};
        }
        if (!reference->tile) {
            return false;
        }
        const Tile origin = *reference->tile;
        auto sign = [](int v) { return (v > 0) - (v < 0); };
        int dx = 1;
        int dy = 0;
        bool advance = false;
        if (const SimObject* current = world().find(read(targetOwner, targetData, f)); current && current->tile) {
            dx = sign(current->tile->x - origin.x);
            dy = sign(current->tile->y - origin.y);
            advance = true;
        }
        for (;;) {
            if (advance) {
                if (dx >= 1) {
                    dx = 0;
                    dy = 1;
                } else if (dx < 0) {
                    dx = 0;
                    dy = -1;
                } else if (dy < 1) {
                    return false;
                } else {
                    dx = -1;
                    dy = 0;
                }
            }
            advance = true;
            const Tile side{static_cast<std::uint8_t>(origin.x + dx), static_cast<std::uint8_t>(origin.y + dy),
                            origin.level};
            // The game takes the first object of the tile's own list; we use the lowest id.
            const SimObject* candidate = world().nextAfter(std::numeric_limits<std::int16_t>::min(),
                                                           [&](const SimObject& s) { return s.tile == side; });
            if (candidate != nullptr && candidate->data[kRoomDataField] == reference->data[kRoomDataField]) {
                write(targetOwner, targetData, f, candidate->id);
                return true;
            }
        }
    }

    // Set Motive Change (Sims only). Byte 3 bit 0 clears all of my motive changes. Otherwise it adds a
    // change for motive byte 2: a rate per Sim hour (owner byte 0, data bytes 4-5) up to a limit
    // (owner byte 1, data bytes 6-7).
    bool setMotiveChange(const bhav::Instruction& in, Frame& f) {
        const auto& o = in.operands;
        SimObject& sim = me();
        if (!sim.isPerson()) {
            throw Stop{Outcome::Unsupported, "Set Motive Change on an object that is not a person", ""};
        }
        if (o[3] & 0x01) {
            sim.motiveChanges.clear();
            return true;
        }
        const std::uint8_t motive = o[2];
        constexpr std::array<std::uint8_t, 9> kChangeableMotives{0, 1, 5, 6, 7, 8, 9, 14, 15};
        if (std::find(kChangeableMotives.begin(), kChangeableMotives.end(), motive) == kChangeableMotives.end()) {
            throw Stop{Outcome::Error, "motive number out of range", std::to_string(motive)};
        }
        const std::int16_t rate = read(o[0], static_cast<std::uint16_t>(o[4] | o[5] << 8), f);
        const std::int16_t limit = read(o[1], static_cast<std::uint16_t>(o[6] | o[7] << 8), f);
        sim.motiveChanges.push_back({motive, rate / kTicksPerSimHour, static_cast<float>(limit)});
        return true;
    }

    // Runs a Lua script: entry [bytes 2-3] (counted from 1) of string table 0x130 names it, from the
    // global group or, with byte 4 bit 1, the code owner's. With byte 4 bit 3 the script gets three
    // parameters (owner, data) from bytes 6-8, 9-11 and 12-14. The primitive is the script's return
    // value. The other bits of byte 4 and byte 5 are not used.
    bool luaScript(const bhav::Instruction& in, Frame& f) {
        const auto& o = in.operands;
        LuaRuntime* scripts = world().scripts();
        if (scripts == nullptr) {
            throw Stop{Outcome::Unsupported, "Lua scripts are not loaded", ""};
        }
        const std::uint8_t flags = o[4];
        const auto index = static_cast<std::uint16_t>(o[2] | o[3] << 8);
        const std::uint32_t group = (flags & 0x02) ? f.codeOwner : kGlobalGroup;
        const auto name = index == 0 ? std::nullopt : code_.string(group, static_cast<std::uint16_t>(o[0] | o[1] << 8), index - 1u);
        if (!name || name->empty()) {
            throw Stop{Outcome::Error, "Lua script name not found", std::to_string(index)};
        }
        ScriptCall call;
        call.hasParams = (flags & 0x08) != 0;
        if (call.hasParams) {
            for (std::size_t i = 0; i < 3; ++i) {
                call.params[i] = read(o[6 + 3 * i], static_cast<std::uint16_t>(o[7 + 3 * i] | o[8 + 3 * i] << 8), f);
            }
        }
        call.temps = &thread_.temps;
        call.locals = &f.locals;
        call.treeParams = &f.params;
        call.stackObject = stackObject();
        call.runningObject = me_ != nullptr ? me_->id : 0;
        call.runningOnObject = f.runningOnId;
        call.world = &world();
        call.code = &code_;
        call.codeOwner = f.codeOwner;
        call.globals = globals_.data();
        call.globalCount = globals_.size();
        if (scripts->run(*name, call)) {
            return call.returnValue;
        }
        if (!call.unsupported.empty()) {
            throw Stop{Outcome::Unsupported, "script function not implemented: " + call.unsupported, *name};
        }
        if (call.errorCode != 0) {
            throw Stop{Outcome::Error, "object error running script", *name + " " + hex(static_cast<std::uint16_t>(call.errorCode), 2)};
        }
        // Most other script errors come from parts of the game's script API the engine lacks:
        // name the missing global when Lua does.
        const auto quote = call.luaError.find("global `");
        if (quote != std::string::npos) {
            const auto end = call.luaError.find('\'', quote);
            throw Stop{Outcome::Unsupported, "script function not implemented: " + call.luaError.substr(quote + 8, end - quote - 8), *name};
        }
        throw Stop{Outcome::Unsupported, "script error: " + call.luaError, *name};
    }

    // Add/Change the Action String: what the queue shows for my running interaction. Always true.
    // Byte 9 zero sets the text: entry [bytes 14-15] (byte 2 bit 4: [temp 0]) of string table
    // 0x12E, from the code owner's group, then its semi-global group, then the global one (byte 2
    // bit 2: only the global one, bit 3: only the semi-global one); a missing text is an error.
    // Otherwise it sets the icon: of the object (owner byte 11, data bytes 12-13), or with byte 2
    // bit 5 of the object type with GUID bytes 5-8 (bit 6: temps 2 and 3). The text's tokens and
    // the icon index (byte 10, bit 7: temp 1) are not used.
    // Manage Inventory. Byte 0: the inventory (bits 0-2: global, lot, family, neighbor, game-wide;
    // bit 3: counted tokens), its id from owner byte 1 / data 2-3; byte 4 the operation; bytes 5-8
    // a GUID (0: the stack object's, for the operations that need one). Byte 9 with byte 0 bits
    // 4-5 gives a flags value and mask for singular tokens ((flags & mask) == value; marked tokens
    // never match), and the kind of a counted token. Owner byte 10 / data 11-12 is an index
    // (counted from 1), owner byte 13 / data 14-15 a value or the result, owner byte 6 / data 7-8 a
    // second index.
    bool manageInventory(const bhav::Instruction& in, Frame& f) {
        const auto& o = in.operands;
        const std::uint8_t op = o[4];
        SimObject& self = me();
        bool counted = (o[0] & 0x08) != 0;
        int kind = o[0] & 0x07;
        std::int16_t id = 0;
        if (counted || op != 9) {
            id = read(o[1], word(o, 2), f);
        } else {
            kind = self.tokenSource.kind;
            id = self.tokenSource.id;
            counted = self.tokenSource.counted;
        }
        Inventory* inventory = kind <= 4 ? world().inventory(static_cast<InventoryKind>(kind), id) : nullptr;
        if (inventory == nullptr) {
            throw Stop{Outcome::Error, "inventory not found", std::to_string(kind) + " " + std::to_string(id)};
        }

        std::uint32_t guid = static_cast<std::uint32_t>(o[5] | o[6] << 8 | o[7] << 16) | static_cast<std::uint32_t>(o[8]) << 24;
        const bool needsGuid = counted ? (op == 0 || op == 2 || op == 4 || op == 6 || op == 7)
                                       : (op == 0 || op == 2 || op == 3);
        if (guid == 0 && needsGuid) {
            const SimObject* stackObject = world().find(this->stackObject());
            if (stackObject == nullptr) {
                throw Stop{Outcome::Error, "stack object not found for inventory", std::to_string(this->stackObject())};
            }
            guid = stackObject->definition.guid();
        }
        const auto index = [&] { return static_cast<std::int16_t>(read(o[10], word(o, 11), f) - 1); };
        const auto value = [&] { return read(o[13], word(o, 14), f); };
        const auto result = [&](std::int32_t v) { write(o[13], word(o, 14), f, checkedResult(v)); };
        return counted ? countedTokens(*inventory, op, guid, o, index, value, result, f, kind, id)
                       : singularTokens(*inventory, op, guid, o, index, value, result, f, kind, id);
    }

    static std::uint16_t word(const std::array<std::uint8_t, 16>& o, std::size_t at) {
        return static_cast<std::uint16_t>(o[at] | o[at + 1] << 8);
    }

    // Results outside 16 bits are error 0x14 in the game.
    static std::int16_t checkedResult(std::int32_t v) {
        if (v < std::numeric_limits<std::int16_t>::min() || v > std::numeric_limits<std::int16_t>::max()) {
            throw Stop{Outcome::Error, "data value out of range", std::to_string(v)};
        }
        return static_cast<std::int16_t>(v);
    }

    using TokenList = std::vector<std::shared_ptr<InventoryToken>>;

    // Remembers where the object's token came from, for writing it back (operation 9).
    void copyToObject(SimObject& self, const InventoryToken& t, int kind, std::int16_t id, bool counted,
                      std::size_t at) {
        self.token = t;
        self.tokenSource = {static_cast<std::int16_t>(kind), id, counted, static_cast<std::int16_t>(at), t.flags};
    }

    static std::int16_t property(const InventoryToken& t, std::int16_t i) {
        return i >= 0 && static_cast<std::size_t>(i) < t.properties.size() ? t.properties[static_cast<std::size_t>(i)] : 0;
    }
    // Setting a property past the end first fills the list with zeros.
    static void setProperty(InventoryToken& t, std::int16_t i, std::int16_t v) {
        if (i < 0) {
            return;
        }
        if (static_cast<std::size_t>(i) >= t.properties.size()) {
            t.properties.resize(static_cast<std::size_t>(i) + 1, 0);
        }
        t.properties[static_cast<std::size_t>(i)] = v;
    }

    // The search singular-token operations use: forward from the 1-based position `start` (so the
    // last result continues the search) or backward before it; unmarked tokens whose flags match
    // and whose category is 0, and with a GUID, of that object type.
    static std::optional<std::size_t> findToken(const TokenList& tokens, std::optional<std::uint32_t> guid,
                                                std::int16_t start, std::uint16_t value, std::uint16_t mask,
                                                bool backward) {
        auto matches = [&](const InventoryToken& t) {
            return (!guid || t.guid == *guid) && (t.flags & 0x04) == 0 && (value & mask) == (t.flags & mask) &&
                   t.category == 0;
        };
        const auto n = static_cast<std::int32_t>(tokens.size());
        if (!backward) {
            for (std::int32_t i = std::max<std::int32_t>(start, 0); i < n; ++i) {
                if (matches(*tokens[static_cast<std::size_t>(i)])) {
                    return static_cast<std::size_t>(i);
                }
            }
            return std::nullopt;
        }
        const std::int32_t from = (start - 1 >= 0 && start - 1 < n) ? start - 2 : n - 1;
        for (std::int32_t i = from; i >= 0; --i) {
            if (matches(*tokens[static_cast<std::size_t>(i)])) {
                return static_cast<std::size_t>(i);
            }
        }
        return std::nullopt;
    }

    template <typename Index, typename Value, typename Result>
    bool singularTokens(Inventory& inventory, std::uint8_t op, std::uint32_t guid, const std::array<std::uint8_t, 16>& o,
                        Index index, Value value, Result result, Frame& f, int kind, std::int16_t id) {
        TokenList& tokens = inventory.singular;
        SimObject& self = me();
        const auto flagsValue = static_cast<std::uint16_t>((o[9] & 0x03) << 3 | (o[0] >> 4 & 0x03));
        const auto flagsMask = static_cast<std::uint16_t>((o[9] & 0x7C) >> 2);
        const bool backward = (o[0] & 0x80) != 0;
        auto at = [&](std::int16_t i) -> InventoryToken* {
            return i >= 0 && static_cast<std::size_t>(i) < tokens.size() ? tokens[static_cast<std::size_t>(i)].get() : nullptr;
        };
        switch (op) {
        case 0:  // add a token of the GUID with the flags value
            if (tokens.size() > 0x7FFE) {
                return false;
            }
            tokens.push_back(std::make_shared<InventoryToken>(InventoryToken{guid, flagsValue, 0, 0, 0, {}}));
            return true;
        case 1: {  // remove the token at the index
            const std::int16_t i = index();
            if (at(i) == nullptr) {
                return false;
            }
            tokens.erase(tokens.begin() + i);
            return true;
        }
        case 2:  // remove every matching token of the GUID
            return std::erase_if(tokens, [&](const auto& t) {
                       return t->guid == guid && (t->flags & 0x04) == 0 && (flagsValue & flagsMask) == (t->flags & flagsMask) &&
                              t->category == 0;
                   }) != 0;
        case 3:    // find a token of the GUID
        case 12: {  // find a token by flags only
            const auto start = static_cast<std::int16_t>(read(o[10], word(o, 11), f));
            const auto found = findToken(tokens, op == 3 ? std::optional{guid} : std::nullopt, start, flagsValue, flagsMask, backward);
            if (!found) {
                return false;
            }
            result(static_cast<std::int32_t>(*found) + 1);
            return true;
        }
        case 4: {  // push a value onto the token's properties
            InventoryToken* t = at(index());
            if (t == nullptr) {
                return false;
            }
            t->properties.push_back(value());
            return true;
        }
        case 5: {  // pop the token's last property
            InventoryToken* t = at(index());
            if (t == nullptr || t->properties.empty()) {
                return false;
            }
            const std::int16_t last = t->properties.back();
            t->properties.pop_back();
            result(last);
            return true;
        }
        case 6: {  // copy the token into the object's token
            const std::int16_t i = index();
            InventoryToken* t = at(i);
            if (t == nullptr) {
                return false;
            }
            copyToObject(self, *t, kind, id, false, static_cast<std::size_t>(i));
            return true;
        }
        case 7: {  // read a property of the object's token
            const std::int16_t i = index();
            if (i < 0 || static_cast<std::size_t>(i) >= self.token.properties.size()) {
                return false;
            }
            result(self.token.properties[static_cast<std::size_t>(i)]);
            return true;
        }
        case 8: {  // set a property of the object's token
            const std::int16_t i = index();
            if (i < 0 || static_cast<std::size_t>(i) >= self.token.properties.size()) {
                return false;
            }
            self.token.properties[static_cast<std::size_t>(i)] = value();
            return true;
        }
        case 9: {  // write the object's token back where it came from
            InventoryToken* t = at(self.tokenSource.index);
            if (t == nullptr) {
                return false;
            }
            if (t->value2 != self.token.value2) {
                if (self.token.value2 == 0) {
                    throw Stop{Outcome::Error, "object token has no object link", ""};
                }
                if (t->value2 != 0) {
                    return false;
                }
                throw Stop{Outcome::Error, "inventory token has no object link", ""};
            }
            *t = self.token;
            return true;
        }
        case 10:  // how many tokens there are
            result(static_cast<std::int32_t>(tokens.size()));
            return true;
        case 11: {  // add a copy of the object's token, with the flags it was copied with
            if (self.token.value2 != 0) {
                throw Stop{Outcome::Error, "object token has an object link", ""};
            }
            auto copy = std::make_shared<InventoryToken>(self.token);
            copy->flags = self.tokenSource.flags;
            tokens.push_back(std::move(copy));
            return true;
        }
        case 13: {  // count the tokens matching the flags
            const auto n = std::count_if(tokens.begin(), tokens.end(), [&](const auto& t) {
                return (t->flags & 0x04) == 0 && (flagsValue & flagsMask) == (t->flags & flagsMask) && t->category == 0;
            });
            result(static_cast<std::int32_t>(n));
            return true;
        }
        case 14: {  // set property [index] of the token at the second index
            InventoryToken* t = at(static_cast<std::int16_t>(read(o[6], word(o, 7), f) - 1));
            const std::int16_t i = index();
            const std::int16_t v = value();
            if (t == nullptr) {
                return false;
            }
            setProperty(*t, i, v);
            return true;
        }
        case 15: {  // read property [index] of the token at the second index (0 past the end)
            InventoryToken* t = at(static_cast<std::int16_t>(read(o[6], word(o, 7), f) - 1));
            const std::int16_t i = index();
            if (t == nullptr) {
                return false;
            }
            result(property(*t, i));
            return true;
        }
        default:
            throw Stop{Outcome::Unsupported, "Manage Inventory operation not implemented", hex(op, 2)};
        }
    }

    // Counted tokens are kept per GUID and kind (property 1, from byte 9); property 0 is the count.
    template <typename Index, typename Value, typename Result>
    bool countedTokens(Inventory& inventory, std::uint8_t op, std::uint32_t guid, const std::array<std::uint8_t, 16>& o,
                       Index index, Value value, Result result, Frame& f, int kind, std::int16_t id) {
        TokenList& tokens = inventory.counted;
        SimObject& self = me();
        const std::uint8_t tokenKind = o[9];
        const auto newFlags = static_cast<std::uint16_t>((o[0] & 0x70) >> 4);
        auto find = [&](std::uint32_t g, std::int16_t k) -> std::optional<std::size_t> {
            for (std::size_t i = 0; i < tokens.size(); ++i) {
                if (tokens[i]->guid == g && property(*tokens[i], 1) == k) {
                    return i;
                }
            }
            return std::nullopt;
        };
        auto add = [&](std::uint32_t g, std::int16_t k, std::uint16_t flags, std::int16_t n) {
            if (n < 1) {
                return false;
            }
            if (const auto i = find(g, k)) {
                auto& t = *tokens[*i];
                setProperty(t, 0, static_cast<std::int16_t>(std::min<std::int32_t>(property(t, 0) + n, 0x7FFF)));
            } else {
                tokens.push_back(std::make_shared<InventoryToken>(InventoryToken{g, flags, 0, 0, 0, {n, k}}));
            }
            return true;
        };
        auto remove = [&](std::uint32_t g, std::int16_t k, std::int16_t n) {
            if (n < 1) {
                return false;
            }
            const auto i = find(g, k);
            if (!i) {
                return false;
            }
            auto& t = *tokens[*i];
            const std::int32_t left = property(t, 0) - n;
            if (left > 0) {
                setProperty(t, 0, static_cast<std::int16_t>(left));
            } else {
                tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(*i));
            }
            return true;
        };
        auto at = [&](std::int16_t i) -> InventoryToken* {
            return i >= 0 && static_cast<std::size_t>(i) < tokens.size() ? tokens[static_cast<std::size_t>(i)].get() : nullptr;
        };
        switch (op) {
        case 0:  // add [value] of the GUID and kind
            return add(guid, tokenKind, newFlags, value());
        case 1: {  // add [value] to the token at the index
            const InventoryToken* t = at(index());
            const std::int16_t n = value();
            if (t == nullptr) {
                return false;
            }
            add(t->guid, property(*t, 1), newFlags, n);
            return true;
        }
        case 2:  // remove [value] of the GUID and kind
            return remove(guid, tokenKind, value());
        case 3: {  // remove [value] from the token at the index
            const InventoryToken* t = at(index());
            const std::int16_t n = value();
            if (t == nullptr) {
                return false;
            }
            remove(t->guid, property(*t, 1), n);
            return true;
        }
        case 4: {  // remove the token of the GUID and kind
            const auto i = find(guid, tokenKind);
            if (!i) {
                return false;
            }
            tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(*i));
            return true;
        }
        case 5: {  // remove the token at the index
            const std::int16_t i = index();
            if (i < 0) {
                throw Stop{Outcome::Error, "inventory index out of range", std::to_string(i)};
            }
            if (at(i) == nullptr) {
                return false;
            }
            tokens.erase(tokens.begin() + i);
            return true;
        }
        case 6: {  // find the token of the GUID and kind: its count, and its position into the index
            const auto i = find(guid, tokenKind);
            if (!i) {
                return false;
            }
            result(property(*tokens[*i], 0));
            write(o[10], word(o, 11), f, checkedResult(static_cast<std::int32_t>(*i) + 1));
            return true;
        }
        case 7: {  // copy the token of the GUID and kind into the object's token
            const auto i = find(guid, tokenKind);
            if (!i) {
                return false;
            }
            copyToObject(self, *tokens[*i], kind, id, true, *i);
            return true;
        }
        case 8: {  // copy the token at the index into the object's token
            const std::int16_t i = index();
            const InventoryToken* t = at(i);
            if (t == nullptr) {
                return false;
            }
            copyToObject(self, *t, kind, id, true, static_cast<std::size_t>(i));
            return true;
        }
        case 9: {  // the next token of the kind after the 1-based position at the index: its position
                   // goes back to the index, its GUID (low, then high) to the value and the slot after it
            const auto start = static_cast<std::int32_t>(read(o[10], word(o, 11), f));
            for (std::size_t i = static_cast<std::size_t>(std::max(start, 0)); i < tokens.size(); ++i) {
                if (property(*tokens[i], 1) == static_cast<std::int16_t>(tokenKind)) {
                    write(o[10], word(o, 11), f, checkedResult(static_cast<std::int32_t>(i) + 1));
                    write(o[13], word(o, 14), f, static_cast<std::int16_t>(tokens[i]->guid & 0xFFFF));
                    write(o[13], static_cast<std::uint16_t>(word(o, 14) + 1), f, static_cast<std::int16_t>(tokens[i]->guid >> 16));
                    return true;
                }
            }
            return false;
        }
        case 10:  // how many counted tokens there are
            result(static_cast<std::int32_t>(tokens.size()));
            return true;
        default:
            throw Stop{Outcome::Unsupported, "Manage Inventory operation not implemented", "counted " + hex(op, 2)};
        }
    }

    bool actionString(const bhav::Instruction& in, Frame& f) {
        const auto& o = in.operands;
        SimObject& self = me();
        const std::uint8_t flags = o[2];
        if (o[9] == 0) {
            const auto index = static_cast<std::uint16_t>((flags & 0x10) ? thread_.temps[0] : (o[14] | o[15] << 8));
            std::vector<std::uint32_t> groups;
            if (flags & 0x04) {
                groups = {kGlobalGroup};
            } else if (flags & 0x08) {
                groups = {code_.semiGlobalGroup(f.codeOwner)};
            } else {
                groups = {f.codeOwner, code_.semiGlobalGroup(f.codeOwner), kGlobalGroup};
            }
            for (const auto group : groups) {
                if (const auto text = code_.string(group, kActionStringTable, index); text && !text->empty()) {
                    self.actionString = *text;
                    return true;
                }
            }
            throw Stop{Outcome::Error, "missing action string in table", std::to_string(index)};
        }
        if (flags & 0x20) {
            const std::uint32_t guid =
                (flags & 0x40) ? (static_cast<std::uint32_t>(static_cast<std::uint16_t>(thread_.temps[2])) |
                                  static_cast<std::uint32_t>(static_cast<std::uint16_t>(thread_.temps[3])) << 16)
                               : static_cast<std::uint32_t>(o[5] | o[6] << 8 | o[7] << 16) |
                                     static_cast<std::uint32_t>(o[8]) << 24;
            if (guid == 0) {
                throw Stop{Outcome::Error, "you sent me a crappy GUID, please fix it", "0"};
            }
            self.actionIconGuid = guid;
            self.actionIconObject = 0;
            return true;
        }
        const std::int16_t id = read(o[11], static_cast<std::uint16_t>(o[12] | o[13] << 8), f);
        if (world().find(id) == nullptr) {
            throw Stop{Outcome::Error, "could not find object reference", std::to_string(id)};
        }
        self.actionIconObject = id;
        self.actionIconGuid = 0;
        return true;
    }

    struct IdleResult {
        enum class Kind { True, Wait, Start } kind = Kind::True;
        QueuedInteraction interaction;
    };

    // Idle for Input (Sims only). Operands:
    //   bytes 0-1  the parameter that counts down the ticks left to wait
    //   bytes 2-3  non-zero: a waiting interaction may start
    //   byte 4     bit 0: don't wait, just start the first waiting interaction if there is one
    // Waiting: with nothing queued the count goes down by one each tick and the primitive is true
    // at 0. With interactions waiting it sets bit 0x40 of my object data 8 and starts the first one
    // (or is true when none may start). A started interaction runs its action tree on my stack with
    // its object as stack object; afterwards the primitive is true.
    IdleResult idleForInput(const bhav::Instruction& in, Frame& f) {
        const auto& o = in.operands;
        SimObject& sim = me();
        if (!sim.isPerson()) {
            throw Stop{Outcome::Unsupported, "Idle for Input on an object that is not a person", ""};
        }
        auto startFirst = [&sim] {
            IdleResult r{IdleResult::Kind::Start, sim.queue.front()};
            sim.queue.erase(sim.queue.begin());
            return r;
        };
        if (o[4] & 0x01) {
            return sim.queue.empty() ? IdleResult{} : startFirst();
        }
        const auto counter = static_cast<std::int16_t>(o[0] | o[1] << 8);
        if (counter < 0 || static_cast<std::size_t>(counter) >= f.params.size()) {
            throw Stop{Outcome::Error, "stack number out of range", std::to_string(counter)};
        }
        if (sim.queue.empty()) {
            std::int16_t& ticks = f.params[static_cast<std::size_t>(counter)];
            if (ticks == 0) {
                sim.data[kIdleFlagsField] &= static_cast<std::int16_t>(~kInteractionsWaitingBit);
                return IdleResult{};
            }
            --ticks;
            return IdleResult{IdleResult::Kind::Wait, {}};
        }
        sim.data[kIdleFlagsField] |= kInteractionsWaitingBit;
        const bool mayStart = (o[2] | o[3]) != 0;
        return mayStart ? startFirst() : IdleResult{};
    }

    // Push Interaction: adds interaction N of an object's interaction table to the stack object's
    // queue. Operands:
    //   byte 0       interaction number (byte 3 bit 4: from owner byte 5, data bytes 6-7)
    //   byte 1       the temp (byte 3 bit 1: the local) holding the object
    //   byte 2       priority: 0 my own ("Priority" person data), 1 high (100), 2 low (20), 3 medium (50)
    //   byte 14      bit 0: pass my parameters as its arguments; bit 1: take the object and the number
    //                from autonomy's choice (my person data 0x35, my object data 0x14)
    // On failure my object data 0x34 says why: 1 no priority, 2 the test tree said no, 3 the queue
    // is full. The other flag bits (icons, joining, messages) are not implemented.
    bool pushInteraction(const bhav::Instruction& in, Frame& f) {
        const auto& o = in.operands;
        SimObject& self = me();
        self.data[kPrimitiveResultField] = 0;
        SimObject& sim = stack();
        if (!sim.isPerson()) {
            throw Stop{Outcome::Error, "object referred to must be a person", std::to_string(sim.id)};
        }
        const bool fromAutonomy = (o[14] & 0x02) != 0;
        std::int16_t objectId = 0;
        if (fromAutonomy) {
            objectId = personData(self, kBestActionObjectPersonData);
        } else if (o[3] & 0x02) {
            if (o[1] >= f.locals.size()) {
                throw Stop{Outcome::Error, "local out of range", std::to_string(o[1])};
            }
            objectId = f.locals[o[1]];
        } else {
            if (o[1] >= kTempCount) {
                throw Stop{Outcome::Error, "stack number out of range", std::to_string(o[1])};
            }
            objectId = thread_.temps[o[1]];
        }
        const SimObject* object = world().find(objectId);
        if (object == nullptr) {
            throw Stop{Outcome::Error, "could not find object reference", std::to_string(objectId)};
        }
        const std::int16_t index = (o[3] & 0x10) ? read(o[5], static_cast<std::uint16_t>(o[6] | o[7] << 8), f)
                                   : fromAutonomy ? self.data[kTreeTableEntryField]
                                                  : static_cast<std::int16_t>(o[0]);
        // Objects without their own interaction table use their semi-global group's (Sims use
        // PersonGlobals').
        const auto tableId = object->definition.field(objd::field::kInteractionTableId);
        auto table = code_.interactionTable(object->group, tableId);
        if (table == nullptr) {
            table = code_.interactionTable(code_.semiGlobalGroup(object->group), tableId);
        }
        if (table == nullptr) {
            throw Stop{Outcome::Error, "tree table not found", hex(object->group, 8)};
        }
        if (index < 0 || static_cast<std::size_t>(index) >= table->interactions.size()) {
            throw Stop{Outcome::Error, "reference to tree table entry that does not exist", std::to_string(index)};
        }
        const ttab::Interaction& entry = table->interactions[static_cast<std::size_t>(index)];

        std::int16_t priority = 0;
        switch (o[2]) {
        case 0:
            priority = self.isPerson() ? self.personData[kPriorityPersonData] : 0;
            if (self.isPerson() && priority == 0) {
                self.data[kPrimitiveResultField] = 1;
                return false;
            }
            break;
        case 1: priority = 100; break;
        case 2: priority = 20; break;
        case 3: priority = 50; break;
        default:
            throw Stop{Outcome::Error, "unknown interaction priority", std::to_string(o[2])};
        }

        QueuedInteraction queued;
        queued.object = object->id;
        queued.group = object->group;
        queued.index = static_cast<std::uint16_t>(index);
        queued.action = entry.action;
        queued.priority = priority;
        if (o[14] & 0x01) {
            std::copy_n(f.params.begin(), std::min<std::size_t>(4, f.params.size()), queued.args.begin());
        }

        if (entry.test != 0) {
            const RunResult check = runNow(sim, object->group, entry.test, object->id, queued.args);
            if (check.outcome == Outcome::False) {
                self.data[kPrimitiveResultField] = 2;
                return false;
            }
            if (check.outcome == Outcome::Sleeping) {
                throw Stop{Outcome::Unsupported, "interaction test tree waited", hex(entry.test, 4)};
            }
            if (check.outcome != Outcome::True) {
                throw Stop{check.outcome, "interaction test tree: " + check.reason, check.detail};
            }
        }
        if (sim.queue.size() >= kMaxQueuedInteractions) {
            self.data[kPrimitiveResultField] = 3;
            return false;
        }
        sim.queue.push_back(queued);
        return true;
    }

    // Get Distance To: the distance from an object to the stack object, in tiles (a level counts as
    // 10), into a temp. Operands:
    //   bytes 0-1  temp index (0-7)
    //   byte 2     bit 0: the object is (owner byte 3, data bytes 4-5); else my object data 0x0B
    //   byte 6     bit 1: in hundredths of a tile
    bool getDistanceTo(const bhav::Instruction& in, Frame& f) {
        const auto& o = in.operands;
        const auto temp = static_cast<std::int16_t>(o[0] | o[1] << 8);
        if (temp < 0 || temp >= static_cast<std::int16_t>(kTempCount)) {
            throw Stop{Outcome::Error, "temporary reference out of range", std::to_string(temp)};
        }
        const std::int16_t fromId = (o[2] & 0x01) ? read(o[3], static_cast<std::uint16_t>(o[4] | o[5] << 8), f)
                                                  : read(owner::kMyData, 0x0B, f);
        const SimObject* from = world().find(fromId);
        const SimObject* to = world().find(stackObject());
        if (from == nullptr || to == nullptr) {
            throw Stop{Outcome::Error, "could not find object reference",
                       std::to_string(from == nullptr ? fromId : stackObject())};
        }
        const float dx = static_cast<float>(from->x) / 65536.0f - static_cast<float>(to->x) / 65536.0f;
        const float dy = static_cast<float>(from->y) / 65536.0f - static_cast<float>(to->y) / 65536.0f;
        const float dz = static_cast<float>(to->level - from->level) * kLevelHeight;
        float distance = std::sqrt(dz * dz + dx * dx + dy * dy);
        if (o[6] & 0x02) {
            distance *= 100.0f;
        }
        thread_.temps[static_cast<std::size_t>(temp)] = static_cast<std::int16_t>(static_cast<int>(distance));
        return true;
    }

    // Get Direction To: the direction from an object to the stack object. Operands:
    //   bytes 0-1, byte 2  result (data, owner)
    //   byte 4             bit 0: the object is (owner byte 5, data bytes 6-7); else my object data 0x0B
    //   byte 8             bit 1: in degrees (0-359), else one of 8 directions (0-7)
    bool getDirectionTo(const bhav::Instruction& in, Frame& f) {
        const auto& o = in.operands;
        const bool explicitObject = (o[4] & 0x01) != 0;
        const std::int16_t fromId = explicitObject ? read(o[5], static_cast<std::uint16_t>(o[6] | o[7] << 8), f)
                                                   : read(owner::kMyData, 0x0B, f);
        const SimObject* from = world().find(fromId);
        if (from == nullptr) {
            throw Stop{Outcome::Error, "could not find object reference", std::to_string(fromId)};
        }
        const SimObject& to = stack();
        const float dx = static_cast<float>(to.x) / 65536.0f - static_cast<float>(from->x) / 65536.0f;
        const float dy = static_cast<float>(to.y) / 65536.0f - static_cast<float>(from->y) / 65536.0f;
        std::int16_t result = 0;
        if (o[8] & 0x02) {
            constexpr float kDegreesPerRadian = 360.0f / 6.2831853f;
            const auto degrees = static_cast<int>(std::atan2(dy, dx) * kDegreesPerRadian + 90.0f);
            result = static_cast<std::int16_t>((degrees + 360) % 360);
        } else {
            result = octant(dx, dy);
        }
        write(o[2], static_cast<std::uint16_t>(o[0] | o[1] << 8), f, result);
        return true;
    }

    // One of 8 directions for a vector, split at 22.5 degrees from each axis: 0 is -y, 2 +x, 4 +y,
    // 6 -x, with the diagonals between.
    static std::int16_t octant(float dx, float dy) {
        constexpr float kTan22_5 = 0.41421354f;
        const float ax = std::fabs(dx);
        const float ay = std::fabs(dy);
        int direction = 2;
        if (ax * kTan22_5 <= ay) {
            direction = kTan22_5 * ay <= ax ? 3 : 4;
        }
        if (dx < 0) {
            direction = direction == 2 ? 6 : direction == 3 ? 5 : direction;
        }
        if (dy < 0) {
            direction = direction == 3 ? 1 : direction == 4 ? 0 : direction == 5 ? 7 : direction;
        }
        return static_cast<std::int16_t>(direction);
    }

    // Want Satisfy: satisfies the Sim's current wants that match. Operands:
    //   byte 0, 1-2  the Sim (owner, data): an object id, or a neighbor id with flag bit 0
    //   bytes 3-6    want GUID
    //   byte 7, 8-9  argument (owner, data); what it is depends on the want
    //   byte 10, 11-12 value (owner, data)
    //   byte 13      flags: bit 0 the Sim is a neighbor id, bit 1 a Sim argument is a neighbor id
    // True when at least one want matched. Matched wants leave the Sim's list; the aspiration
    // points and memories they give are not implemented.
    bool wantSatisfy(const bhav::Instruction& in, Frame& f) {
        const auto& o = in.operands;
        if (!world().neighbors() || !world().hasWantDefinitions()) {
            throw Stop{Outcome::Unsupported, "Want Satisfy needs the neighborhood and the want definitions", ""};
        }
        const std::uint8_t flags = o[13];
        const std::int16_t sim = toNeighbor(read(o[0], static_cast<std::uint16_t>(o[1] | o[2] << 8), f), flags & 0x01);
        const std::uint32_t guid = static_cast<std::uint32_t>(o[3] | o[4] << 8 | o[5] << 16) | static_cast<std::uint32_t>(o[6]) << 24;
        const content::WantDefinition* def = world().wantDefinition(guid);
        if (def == nullptr) {
            throw Stop{Outcome::Error, "data number out of range", hex(guid, 8)};
        }
        const std::int16_t argument = read(o[7], static_cast<std::uint16_t>(o[8] | o[9] << 8), f);
        const std::int16_t value = read(o[10], static_cast<std::uint16_t>(o[11] | o[12] << 8), f);
        if (!def->simArrayTree.empty()) {
            throw Stop{Outcome::Unsupported, "Want Satisfy for wants with a Sim list tree", def->simArrayTree};
        }

        // The request the Sim's wants are compared with.
        swaf::Want request;
        request.id = static_cast<std::uint16_t>(sim);
        request.guid = guid;
        request.type = static_cast<std::uint8_t>(def->argument);
        request.value = value;
        switch (def->argument) {
        case content::WantArgument::None:
            request.value = argument;
            break;
        case content::WantArgument::Sim:
            request.argument = toNeighbor(argument, flags & 0x02);
            break;
        case content::WantArgument::Object:
        case content::WantArgument::Category:
            // Category wants are satisfied with an object; its type decides the category.
            request.type = static_cast<std::uint8_t>(content::WantArgument::Object);
            request.argument = argument == 0 ? 0 : static_cast<std::int32_t>(objectGuid(argument));
            break;
        case content::WantArgument::Skill:
            request.argument = argument;
            break;
        case content::WantArgument::Career:
        case content::WantArgument::Badge:
            request.argument = static_cast<std::int32_t>(objectGuid(argument));
            break;
        }

        auto& wants = world().neighborWants(sim);
        const auto matched = std::remove_if(wants.begin(), wants.end(), [&](const swaf::Want& w) {
            return wantMatches(w, request, def->op);
        });
        const bool satisfied = matched != wants.end();
        wants.erase(matched, wants.end());
        return satisfied;
    }

    // A Sim reference as a neighbor id: an object id turned into its Sim's neighbor id, or a
    // neighbor id checked to exist. Not a Sim (or an unknown neighbor) is an error.
    std::int16_t toNeighbor(std::int16_t id, bool isNeighborId) {
        std::int16_t neighbor = 0;
        if (isNeighborId) {
            neighbor = world().neighbors()->contains(id) ? id : 0;
        } else {
            const SimObject* object = world().find(id);
            if (object == nullptr) {
                throw Stop{Outcome::Error, "could not find object reference", std::to_string(id)};
            }
            neighbor = world().neighborIdOf(*object);
        }
        if (neighbor == 0) {
            throw Stop{Outcome::Error, "object referred to must be a person", std::to_string(id)};
        }
        return neighbor;
    }

    std::uint32_t objectGuid(std::int16_t id) {
        const SimObject* object = world().find(id);
        if (object == nullptr) {
            throw Stop{Outcome::Error, "could not find object reference", std::to_string(id)};
        }
        return object->definition.guid();
    }

    // Whether a Sim's want is satisfied by the request. An argument of 0 in the want accepts any.
    bool wantMatches(const swaf::Want& want, const swaf::Want& request, content::WantOperator op) const {
        if (want.id != request.id || want.guid != request.guid) {
            return false;
        }
        auto compare = [op](std::int32_t got, std::int32_t wanted) {
            switch (op) {
            case content::WantOperator::Equals:              return got == wanted;
            case content::WantOperator::LessThan:            return got < wanted;
            case content::WantOperator::GreaterThan:         return got > wanted;
            case content::WantOperator::LessThanOrEquals:    return got <= wanted;
            case content::WantOperator::GreaterThanOrEquals: return got >= wanted;
            case content::WantOperator::None:                return true;
            }
            return true;
        };
        using content::WantArgument;
        switch (static_cast<WantArgument>(want.type)) {
        case WantArgument::None:
            break;
        case WantArgument::Sim:
            if (want.argument != 0 && want.argument != request.argument) {
                return false;
            }
            break;
        case WantArgument::Object:
            if (want.argument != 0 && request.argument != 0 && want.argument != request.argument) {
                return false;
            }
            break;
        case WantArgument::Category:
            if (want.argument != 0) {
                if (request.type == static_cast<std::uint8_t>(WantArgument::Object)) {
                    const ObjectType* type = code_.objectType(static_cast<std::uint32_t>(request.argument));
                    if (type == nullptr || type->definition.field(kWantCategoryField) != want.argument) {
                        return false;
                    }
                } else if (request.type == static_cast<std::uint8_t>(WantArgument::Category)) {
                    return want.argument == request.argument;
                }
            }
            break;
        case WantArgument::Skill:
        case WantArgument::Career:
        case WantArgument::Badge: {
            const bool argumentOk = want.argument == 0 || want.argument == request.argument;
            return want.value == 0 ? argumentOk : argumentOk && want.value == request.value;
        }
        default:
            return false;
        }
        return compare(request.value, want.value);
    }

    // Next part of the target's multi-tile object, in falling id order and wrapping around (the
    // target itself when it is the only part). Not a multi-tile object: false, and the target becomes 0.
    bool nextMultiTilePart(std::uint8_t targetOwner, std::uint16_t targetData, Frame& f) {
        const std::int16_t targetId = read(targetOwner, targetData, f);
        const SimObject* target = world().find(targetId);
        if (target == nullptr || target->definition.field(objd::field::kMultiTileMasterId) == 0) {
            write(targetOwner, targetData, f, 0);
            return false;
        }
        const std::int16_t lead = target->data[kLeadTileObjectField];
        const SimObject* next = world().previousAround(targetId, [&](const SimObject& s) {
            return s.id == targetId || s.data[kLeadTileObjectField] == lead;
        });
        write(targetOwner, targetData, f, next->id);
        return true;
    }

    // Next object on the target's tile, wrapping around (the target itself when it is alone).
    // The game keeps its own order per tile, which isn't saved; we use id order.
    bool nextOnSameTile(const std::array<std::uint8_t, 16>& o, std::uint8_t targetOwner,
                        std::uint16_t targetData, Frame& f) {
        const std::int16_t targetId = read(targetOwner, targetData, f);
        const SimObject* target = targetId != 0 ? world().find(targetId) : nullptr;
        if (target == nullptr || !target->tile) {
            return false;
        }
        const std::uint8_t flags = o[8];
        const auto fieldIndex = static_cast<std::uint16_t>(o[9] | o[10] << 8);
        const auto fieldValue = static_cast<std::int16_t>(o[11] | o[12] << 8);
        auto passesFilters = [&](const SimObject& s) {
            if ((flags & 0x01) == 0 && (s.data[kSearchSkipFlagsField] & 0x05) != 0) {
                return false;
            }
            // The game reads a field past the end as 0 here.
            const std::int16_t value = fieldIndex < kObjectDataCount ? s.data[fieldIndex] : 0;
            return (flags & 0x02) == 0 || value == fieldValue;
        };
        if (!passesFilters(*target)) {
            return false;
        }
        if (target->data[kSameTileRefusedField] & kSameTileRefusedBit) {
            throw Stop{Outcome::Error, "could not find object reference", std::to_string(targetId)};
        }
        const Tile tile = *target->tile;
        const SimObject* next = world().nextAround(targetId, [&](const SimObject& s) {
            return s.id == targetId || (s.tile == tile && passesFilters(s));
        });
        write(targetOwner, targetData, f, next->id);
        return true;
    }

    // Operands:
    //   bytes 0-3  GUID
    //   bytes 4-5  object id (data), byte 6 its owner
    //   byte 7     flags: 0x04 "does this GUID exist", 0x02 neighbor id, 0x01 alternate definition
    // A missing object is false, not an error.
    bool testObjectType(const bhav::Instruction& in, Frame& f) {
        const auto& o = in.operands;
        const std::uint32_t guid = readGuid(o);
        const std::uint8_t flags = o[7];
        if (guid == kTestOwnTypeGuid) {
            throw Stop{Outcome::Unsupported, "Test Object Type with the special 'own type' GUID", ""};
        }
        if (flags & kTestGuidExists) {
            if (code_.objectType(guid) == nullptr) {
                return false;
            }
            thread_.temps[0] = static_cast<std::int16_t>(guid & 0xFFFF);
            thread_.temps[1] = static_cast<std::int16_t>(guid >> 16);
            return true;
        }
        if (flags & kTestNeighborId) {
            throw Stop{Outcome::Unsupported, "Test Object Type with a neighbor id needs the neighborhood", ""};
        }
        if (flags & kTestAlternateDefinition) {
            throw Stop{Outcome::Unsupported, "Test Object Type flag 0x01 not implemented", ""};
        }
        const auto data = static_cast<std::uint16_t>(o[4] | o[5] << 8);
        const SimObject* object = world().find(read(o[6], data, f));
        return object != nullptr && object->definition.guid() == guid;
    }

    std::array<std::int16_t, 4> callArgs(const bhav::Instruction& in, Frame& f,
                                         std::size_t calleeArgCount) {
        const auto& o = in.operands;
        std::array<std::int16_t, 4> args{};
        if (o[12] == 1) {
            // Four (owner, 16-bit data) triples, flagged by byte 12.
            // Slots beyond the callee's argument count can hold stale bytes, so skip them.
            for (std::size_t i = 0; i < std::min<std::size_t>(4, calleeArgCount); ++i) {
                const auto data = static_cast<std::uint16_t>(o[i * 3 + 1] | o[i * 3 + 2] << 8);
                args[i] = read(o[i * 3], data, f);
            }
        } else if (std::all_of(o.begin(), o.begin() + 8, [](std::uint8_t v) { return v == 0xFF; })) {
            std::copy_n(thread_.temps.begin(), 4, args.begin());
        } else {
            for (std::size_t i = 0; i < 4; ++i) {
                args[i] = static_cast<std::int16_t>(o[i * 2] | o[i * 2 + 1] << 8);
            }
        }
        return args;
    }

private:
    // Each frame has its own stack object; operands read the current frame's.
    std::int16_t& stackObject() { return thread_.frames.back().stackObjectId; }

    SimObject& me() {
        if (me_ == nullptr) {
            throw Stop{Outcome::Unsupported, "run has no 'me' object", ""};
        }
        return *me_;
    }

    World& world() {
        if (world_ == nullptr) {
            throw Stop{Outcome::Unsupported, "run has no world", ""};
        }
        return *world_;
    }

    static std::uint32_t readGuid(const std::array<std::uint8_t, 16>& o) {
        return static_cast<std::uint32_t>(o[0]) | static_cast<std::uint32_t>(o[1]) << 8 |
               static_cast<std::uint32_t>(o[2]) << 16 | static_cast<std::uint32_t>(o[3]) << 24;
    }

    std::int16_t tempValue(std::uint16_t tempIndex) {
        return slotAt(thread_.temps, tempIndex, "temp");
    }

    // Motives are floats in the game, clamped to -100..100 on every write.
    static float& motive(SimObject& object, std::int64_t index) {
        if (!object.isPerson()) {
            throw Stop{Outcome::Error, "motives on an object that is not a person",
                       std::to_string(object.id)};
        }
        if (index < 0 || static_cast<std::size_t>(index) >= kMotiveCount) {
            throw Stop{Outcome::Error, "motive number out of range", std::to_string(index)};
        }
        return object.motives[static_cast<std::size_t>(index)];
    }

    static void setMotive(SimObject& object, std::int64_t index, std::int16_t value) {
        motive(object, index) = std::clamp(static_cast<float>(value), kMotiveMin, kMotiveMax);
    }

    static std::int16_t& personData(SimObject& object, std::int64_t index) {
        if (!object.isPerson()) {
            throw Stop{Outcome::Error, "person data on an object that is not a person",
                       std::to_string(object.id)};
        }
        return slotAt(object.personData, index, "person data");
    }

    SimObject& stack() {
        SimObject* object = world().find(stackObject());
        if (object == nullptr) {
            throw Stop{Outcome::Error, "stack object does not exist", std::to_string(stackObject())};
        }
        return *object;
    }

    // Constant references: bits 13-14 pick the range (own, semi-global, global; 3 is invalid), bits
    // 7-12 plus bit 15 the table, bits 0-6 the value.
    std::int16_t constant(std::uint16_t data, std::uint32_t codeOwner) {
        const std::uint32_t range = (data >> 13) & 3;
        if (range == 3) {
            throw Stop{Outcome::Error, "invalid constant", hex(data, 4)};
        }
        const std::uint32_t table = ((data >> 7) & 0x3F) | ((data >> 9) & 0x40);
        return slotAt(constantTable(range * 0x80 + table, 0x80, codeOwner), data & 0x7F, "constant");
    }

    // "Constants [temp]": data >> 3 picks the table, temp 0 the value.
    std::int16_t constantByTemp(std::uint16_t data, std::uint32_t codeOwner) {
        return slotAt(constantTable(data >> 3, 0x400, codeOwner), thread_.temps[0], "constant");
    }

    // Table ids by range: the code owner's own tables (0x1000+), its semi-global tables (0x2000+) and
    // the global tables (0x100+).
    const std::vector<std::int16_t>& constantTable(std::uint32_t id, std::uint32_t rangeSize,
                                                   std::uint32_t codeOwner) {
        std::uint32_t group = kGlobalGroup;
        std::uint32_t instance = 0x0100 + (id - 2 * rangeSize);
        if (id < rangeSize) {
            group = codeOwner;
            instance = 0x1000 + id;
        } else if (id < 2 * rangeSize) {
            group = code_.semiGlobalGroup(codeOwner);
            instance = 0x2000 + (id - rangeSize);
        }
        const std::vector<std::int16_t>* values =
            code_.constants(group, static_cast<std::uint16_t>(instance));
        if (values == nullptr) {
            throw Stop{Outcome::MissingCode, "constant table not found",
                       hex(group, 8) + " " + hex(instance, 4)};
        }
        return *values;
    }

    CodeSource& code_;
    std::array<std::int16_t, kGlobalCount>& globals_;
    ThreadState& thread_;
    World* world_;
    SimObject* me_;
    std::mt19937& rng_;
};

} // namespace

const char* outcomeName(Outcome outcome) {
    switch (outcome) {
    case Outcome::True:        return "returned true";
    case Outcome::False:       return "returned false";
    case Outcome::Error:       return "error";
    case Outcome::Unsupported: return "unsupported";
    case Outcome::StepLimit:   return "step limit";
    case Outcome::DepthLimit:  return "depth limit";
    case Outcome::MissingCode: return "missing code";
    case Outcome::Sleeping:    return "sleeping";
    case Outcome::MeRemoved:   return "me was removed";
    }
    return "?";
}

Vm::Vm(CodeSource& code, Limits limits, World* world)
    : code_(code), limits_(limits), world_(world) {}

RunResult Vm::run(ThreadState& thread, std::uint32_t objectGroup, std::uint16_t entry,
                  std::span<const std::int16_t> args) {
    return execute(thread, objectGroup, nullptr, &entry, args);
}

RunResult Vm::run(SimObject& me, std::uint16_t entry, std::span<const std::int16_t> args) {
    return execute(me.thread, me.group, &me, &entry, args);
}

RunResult Vm::run(SimObject& me, std::uint32_t codeGroup, std::uint16_t entry,
                  std::span<const std::int16_t> args) {
    return execute(me.thread, codeGroup, &me, &entry, args);
}

RunResult Vm::resume(SimObject& me) {
    return execute(me.thread, me.thread.codeGroup, &me, nullptr, {});
}

RunResult Vm::resume(ThreadState& thread, std::uint32_t objectGroup) {
    return execute(thread, objectGroup, nullptr, nullptr, {});
}

RunResult Vm::execute(ThreadState& thread, std::uint32_t objectGroup, SimObject* me,
                      const std::uint16_t* entry, std::span<const std::int16_t> args) {
    Execution ex(code_, globals, thread, world_, me, rng);
    ex.runNow = [this](SimObject& who, std::uint32_t codeGroup, std::uint16_t tree, std::int16_t stackObject,
                       std::span<const std::int16_t> callArgs) {
        constexpr int kMaxNestedRuns = 8;
        if (nestedRuns_ >= kMaxNestedRuns) {
            RunResult r;
            r.outcome = Outcome::DepthLimit;
            r.reason = "too many nested check trees";
            return r;
        }
        ThreadState scratch;
        scratch.stackObjectId = stackObject;
        scratch.codeGroup = codeGroup;
        ++nestedRuns_;
        RunResult r = execute(scratch, codeGroup, &who, &tree, callArgs);
        --nestedRuns_;
        return r;
    };
    std::vector<Frame>& frames = thread.frames;
    RunResult result;

    auto checkDepth = [&] {
        if (frames.size() >= limits_.maxDepth) {
            throw Stop{Outcome::DepthLimit, "call depth limit reached",
                       std::to_string(limits_.maxDepth)};
        }
    };

    auto resolve = [&](std::uint16_t instance, std::uint32_t codeOwner) {
        checkDepth();
        const std::uint32_t group = ex.groupFor(instance, codeOwner);
        auto code = code_.behavior(group, instance);
        if (!code) {
            throw Stop{Outcome::MissingCode, "BHAV not found", hex(group, 8) + " " + hex(instance, 4)};
        }
        return std::pair{group, std::move(code)};
    };

    auto push = [&](std::uint32_t group, std::shared_ptr<const bhav::Behavior> code,
                    std::uint16_t instance, std::span<const std::int16_t> callArgs,
                    std::uint32_t codeOwner) {
        Frame frame;
        frame.group = group;
        frame.codeOwner = codeOwner;
        frame.instance = instance;
        frame.params.assign(std::max<std::size_t>(4, code->argCount), 0);
        std::copy_n(callArgs.begin(), std::min(callArgs.size(), frame.params.size()),
                    frame.params.begin());
        frame.locals.assign(code->localCount, 0);
        frame.stackObjectId = frames.empty() ? thread.stackObjectId : frames.back().stackObjectId;
        if (!frames.empty()) {
            frame.runningOnId = frames.back().runningOnId;
        } else if (me != nullptr) {
            frame.runningOnId = codeOwner != me->group && thread.stackObjectId != 0 ? thread.stackObjectId : me->id;
        }
        frame.code = std::move(code);
        frames.push_back(std::move(frame));
    };

    // Follows the exit of the current instruction, unwinding returns. True when the run ended.
    auto followBranch = [&](bool branch) {
        const bhav::Instruction& done = frames.back().code->instructions[frames.back().pc];
        std::uint16_t target = branch ? done.trueTarget : done.falseTarget;
        while (true) {
            if (target == bhav::kTargetError) {
                throw Stop{Outcome::Error, "reached an error exit", ""};
            }
            if (target != bhav::kTargetTrue && target != bhav::kTargetFalse) {
                frames.back().pc = target;
                return false;
            }
            const bool returned = target == bhav::kTargetTrue || frames.back().callerContinuesTrue;
            if (frames.size() == 1) {
                thread.stackObjectId = frames.back().stackObjectId;
            }
            frames.pop_back();
            if (frames.empty()) {
                result.outcome = returned ? Outcome::True : Outcome::False;
                return true;
            }
            const Frame& caller = frames.back();
            const bhav::Instruction& callSite = caller.code->instructions[caller.pc];
            target = returned ? callSite.trueTarget : callSite.falseTarget;
        }
    };

    try {
        if (entry != nullptr) {
            if (thread.isRunning()) {
                throw Stop{Outcome::Error, "thread is already running a behavior", ""};
            }
            thread.codeGroup = objectGroup;
            auto [group, code] = resolve(*entry, objectGroup);
            push(group, std::move(code), *entry, args, objectGroup);
        } else if (!thread.isRunning()) {
            throw Stop{Outcome::Error, "nothing to resume", ""};
        } else if (thread.pendingResult) {
            const bool pending = *thread.pendingResult;
            thread.pendingResult.reset();
            thread.sleepTicks = 0;
            if (followBranch(pending)) {
                return result;
            }
        }

        while (true) {
            if (result.steps >= limits_.maxSteps) {
                throw Stop{Outcome::StepLimit, "step limit reached", std::to_string(limits_.maxSteps)};
            }
            Frame& f = frames.back();
            if (f.code->instructions.empty()) {
                throw Stop{Outcome::Error, "BHAV has no instructions", ""};
            }
            if (f.pc >= f.code->instructions.size()) {
                throw Stop{Outcome::Error, "jump to a missing instruction", std::to_string(f.pc)};
            }
            const bhav::Instruction& in = f.code->instructions[f.pc];
            ++result.steps;
            if (onStep) {
                onStep({frames.size() - 1, f.group, f.instance, f.pc, in});
            }

            if (in.opcode >= 0x0100) {
                const std::uint32_t codeOwner = f.codeOwner;
                auto [group, code] = resolve(in.opcode, codeOwner);
                const auto callArgs = ex.callArgs(in, f, code->argCount);
                push(group, std::move(code), in.opcode, callArgs, codeOwner);
                continue;
            }
            if (in.opcode == kRunTreeByNameOpcode) {
                auto call = ex.namedTree(in, f);
                if (!call) {
                    if (followBranch(false)) {
                        return result;
                    }
                    continue;
                }
                checkDepth();
                push(call->group, std::move(call->code), call->instance, call->args, call->codeOwner);
                if (call->stackObject) {
                    frames.back().stackObjectId = *call->stackObject;
                }
                continue;
            }
            if (in.opcode == kIdleForInputOpcode) {
                const auto idle = ex.idleForInput(in, f);
                if (idle.kind == Execution::IdleResult::Kind::Wait) {
                    // Try again next tick: the same instruction runs when the thread resumes.
                    thread.sleepTicks = 1;
                    result.outcome = Outcome::Sleeping;
                    break;
                }
                if (idle.kind == Execution::IdleResult::Kind::True) {
                    if (followBranch(true)) {
                        return result;
                    }
                    continue;
                }
                const QueuedInteraction& started = idle.interaction;
                auto [group, code] = resolve(started.action, started.group);
                push(group, std::move(code), started.action, started.args, started.group);
                frames.back().stackObjectId = started.object;
                frames.back().runningOnId = started.object;
                frames.back().callerContinuesTrue = true;
                continue;
            }
            if (in.opcode == kSleepOpcode) {
                thread.sleepTicks = ex.sleepTicks(in, f);
                thread.pendingResult = true;
                result.outcome = Outcome::Sleeping;
                break;
            }
            if (followBranch(ex.primitive(in, f))) {
                return result;
            }
        }
    } catch (const Stop& stop) {
        result.outcome = stop.outcome;
        result.reason = stop.reason;
        result.detail = stop.detail;
    } catch (const std::exception& ex2) {
        result.outcome = Outcome::Error;
        result.reason = "exception";
        result.detail = ex2.what();
    }

    if (!frames.empty()) {
        result.group = frames.back().group;
        result.instance = frames.back().instance;
        result.pc = frames.back().pc;
    }
    if (result.outcome != Outcome::Sleeping) {
        frames.clear();
        thread.pendingResult.reset();
        thread.sleepTicks = 0;
    }
    return result;
}

} // namespace ts2::sim