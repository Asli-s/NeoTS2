#include "sim/vm_execution.hpp"

namespace ts2::sim::vm_detail {

// Operands:
//   byte 0     mode: 1 takes the object out of its container's slot, leaves it where it is and is
//              true (the game does more for multi-tile objects and Sims, not known yet), 0 searches the reference's room,
//              then any room, 2 only the room, 7 by the reference's routing slot, 8 by a global
//              slot (bytes 10-11). Any other mode is not kept yet
//   byte 1     local holding the reference object (flag 0x01)
//   byte 2     flags: 0x01 reference from the local, 0x02 don't prefer empty tiles, 0x04 tiles the
//              player can edit (not checked yet), 0x08 both objects from bytes 4-9 (else the stack
//              object is placed by me), 0x40 start in front of the reference object, and 0x10, 0x20 and
//              0x80 not kept yet
//   byte 3     facings not allowed, relative to the reference object's (bit 0 the same, ...)
//   bytes 4-6  the object to place (owner, data)
//   bytes 7-9  the reference object
bool Execution::findLocationFor(const bhav::Instruction& in, Frame& f) {
    constexpr std::uint8_t kReferenceInLocal = 0x01, kDontPreferEmpty = 0x02, kUserEditable = 0x04,
                           kObjectsInOperands = 0x08, kInFrontOfReference = 0x40;
    const auto& o = in.operands;
    const bool inOperands = (o[2] & kObjectsInOperands) != 0;
    const std::int16_t placedId = inOperands ? read(o[4], static_cast<std::uint16_t>(o[5] | o[6] << 8), f) : stackObject();
    SimObject* placed = world().find(placedId);
    if (placed == nullptr) {
        throw Stop{Outcome::Error, "no object to place", std::to_string(placedId)};
    }
    if (o[0] == 1) {
        takeOutOfSlot(*placed);
        return true;
    }
    const bool bySlot = o[0] == 7 || o[0] == 8;
    if (!bySlot && ((o[0] != 0 && o[0] != 2) || (o[2] & ~(kReferenceInLocal | kDontPreferEmpty | kUserEditable | kObjectsInOperands | kInFrontOfReference)) != 0)) {
        throw Stop{Outcome::Unsupported, "Find Location For form not implemented", hex(o[0], 2) + " " + hex(o[2], 2)};
    }
    std::int16_t refId = me().id;
    if (inOperands) {
        refId = read(o[7], static_cast<std::uint16_t>(o[8] | o[9] << 8), f);
    } else if (o[2] & kReferenceInLocal) {
        if (o[1] >= f.locals.size()) {
            throw Stop{Outcome::Error, "local index out of range", std::to_string(o[1])};
        }
        refId = f.locals[o[1]];
    }
    const SimObject* ref = world().find(refId);
    if (ref == nullptr) {
        throw Stop{Outcome::Error, "no reference object to place by", std::to_string(refId)};
    }
    if (bySlot) {
        return placeBySlot(o, *placed, *ref);
    }
    LocationSearch search;
    search.startX = ref->x;
    search.startY = ref->y;
    search.level = ref->level;
    search.referenceFacing = ref->data[kDirectionField];
    search.allowedFacings = static_cast<std::uint8_t>(~o[3]);
    search.preferEmpty = (o[2] & kDontPreferEmpty) == 0;
    if (o[2] & kInFrontOfReference) {
        search.startDirection = ref->data[kDirectionField] & 7;
    }
    std::optional<FoundLocation> found = findLocation(world(), *placed, search, rng_);
    if (!found && o[0] == 0) {
        search.sameRoom = false;
        found = findLocation(world(), *placed, search, rng_);
    }
    if (!found) {
        return false;
    }
    snapTo(world(), *placed, SnapTarget{found->x, found->y, ref->level, found->facing});
    return true;
}

std::vector<std::optional<ModelBone>> Execution::slotJoints(const SimObject& owner, const slot::Entry& entry, std::uint32_t group) {
    return sim::slotJoints(world(), code_, owner, entry, group);
}

// The facing Snap gives the joint of bone slot `entry` whose spot is (x, y), as routing faces a Sim
// at the joint it reached.
std::optional<int> Execution::jointFacing(const SimObject& owner, const slot::Entry& entry,
                                      const std::vector<std::optional<ModelBone>>& joints, float x, float y) {
    for (const auto& joint : joints) {
        const auto target = joint ? routingSlotTarget(owner, entry, joint) : std::nullopt;
        if (target && std::fabs(static_cast<float>(target->x) / 65536.0f - x) < 1e-3f &&
            std::fabs(static_cast<float>(target->y) / 65536.0f - y) < 1e-3f) {
            return target->facing;
        }
    }
    return std::nullopt;
}

// The slot's spots by the reference, best first: the first with a facing the object may take wins.
// The game would also test that the object fits there, which isn't done yet.
bool Execution::placeBySlot(const std::array<std::uint8_t, 16>& o, SimObject& placed, const SimObject& ref) {
    constexpr std::uint32_t kSlotKindBits = (1u << 21) | (1u << 25);
    const auto index = static_cast<std::int16_t>(o[10] | o[11] << 8);
    const std::vector<slot::Entry>* slots = &code_.globalSlots();
    std::uint32_t group = kGlobalGroup;
    if (o[0] == 7) {
        const ObjectType* type = code_.objectType(ref.definition.guid());
        slots = type != nullptr ? &type->routingSlots : nullptr;
        group = type != nullptr ? type->group : 0;
    }
    if (slots == nullptr || index < 0 || static_cast<std::size_t>(index) >= slots->size()) {
        throw Stop{Outcome::Error, "invalid routing slot specified", std::to_string(index)};
    }
    const slot::Entry& entry = (*slots)[static_cast<std::size_t>(index)];
    if ((entry.routingFlags & kSlotKindBits) == kSlotKindBits) {
        throw Stop{Outcome::Unsupported, "Find Location For slot kind not implemented", hex(entry.routingFlags, 8)};
    }
    if (!world().placementSpots()) {
        throw Stop{Outcome::Unsupported, "placing by a slot needs the lot's route grid", ""};
    }
    const bool diagonalsAllowed = placed.definition.field(kNoDiagonalFacingField) == 0 && placed.data[kFacingRuleField] < 2;
    const auto mayFace = [&](int facing) { return diagonalsAllowed || (facing & 1) == 0; };
    const auto allowedTurns = static_cast<std::uint8_t>(~o[3]);
    const auto joints = slotJoints(ref, entry, group);
    for (const auto& [x, y] : world().placementSpots()(placed, ref, entry, joints)) {
        std::optional<int> facing = isBoneSlot(entry) ? jointFacing(ref, entry, joints, x, y) : slotGoalFacing(ref, entry, x, y);
        if (facing && !mayFace(*facing)) {
            continue;
        }
        if (!facing) {
            const int first = static_cast<int>((static_cast<std::uint64_t>(static_cast<std::uint32_t>(rng_())) * 8) >> 32);
            for (int i = 0; i < 8 && !facing; ++i) {
                const int turn = (first + i) & 7, dir = (turn + ref.data[kDirectionField]) & 7;
                if (((allowedTurns >> turn) & 1) != 0 && mayFace(dir)) {
                    facing = dir;
                }
            }
            if (!facing) {
                continue;
            }
        }
        const auto gx = static_cast<std::int32_t>(std::lround(x * 65536.0f)), gy = static_cast<std::int32_t>(std::lround(y * 65536.0f));
        snapTo(world(), placed, SnapTarget{gx, gy, ref.level, static_cast<std::int16_t>(*facing)});
        return true;
    }
    return false;
}

// Operands:
//   bytes 0-1    the slot number, or (mode 0) the param holding it
//   bytes 2-3    mode: 0 slot number in a param, 3 slot number (from temp 0 with byte 4 bit 3),
//                4 a global slot numbered the same way, while 1 into the stack object and 2 in front of
//                it are not kept yet
//   byte 4       0x02 ask a Sim in the way to move, 0x08 slot number in temp 0, 0x10 test only
//   bytes 10-11  1: put my own slot (temp 1) there instead of me (not kept yet)
// Moves me to a routing slot of the stack object, its routing slots counted from 0, and is true.
// It is false when a check refuses, my object data 0x34 then saying why (2: not the same room). Moving
// takes me out of what I was in: a Sim standing up from a seat is no longer in it (data 2).
bool Execution::snap(const bhav::Instruction& in, Frame& f) {
    constexpr std::uint8_t kSlotInTemp0 = 0x08, kTestOnly = 0x10;
    const auto& o = in.operands;
    const auto number = static_cast<std::int16_t>(o[0] | o[1] << 8);
    const auto mode = static_cast<std::int16_t>(o[2] | o[3] << 8);
    SimObject& self = me();
    self.data[kPrimitiveResultField] = 0;
    const SimObject* owner = world().find(stackObject());
    if (owner == nullptr) {
        throw Stop{Outcome::Error, "stack object does not exist", std::to_string(stackObject())};
    }
    if (mode == 1 || mode == 2) {
        throw Stop{Outcome::Unsupported, "Snap form not implemented", hex(static_cast<std::uint16_t>(mode), 2)};
    }
    // Bytes 10-11 == 1: the checks are made as if I came from the stack object's routing slot in
    // temp 1, not from where I stand.
    const bool fromSlot = (o[10] | o[11] << 8) == 1;
    std::int16_t slot = number;
    if (mode == 0) {
        slot = slotAt(f.params, number, "param");
    } else if (mode == 3 || mode == 4) {
        slot = (o[4] & kSlotInTemp0) ? thread_.temps[0] : number;
    } else {
        throw Stop{Outcome::Error, "invalid snapping mode", std::to_string(mode)};
    }
    const ObjectType* type = code_.objectType(owner->definition.guid());
    const std::vector<slot::Entry>* slots = mode == 4 ? &code_.globalSlots() : type != nullptr ? &type->routingSlots : nullptr;
    if (slots == nullptr || slot < 0 || static_cast<std::size_t>(slot) >= slots->size()) {
        throw Stop{Outcome::Error, "slot number out of range", std::to_string(slot)};
    }
    const slot::Entry& entry = (*slots)[static_cast<std::size_t>(slot)];
    const std::uint32_t boneGroup = mode == 4 ? kGlobalGroup : type->group;
    const auto target = routingSlotTarget(*owner, entry, isBoneSlot(entry) ? slotBone(*owner, boneGroup, entry) : std::nullopt);
    if (!target) {
        throw Stop{Outcome::Unsupported, "Snap slot's bone not found", std::to_string(slot)};
    }
    std::optional<SimObject> fromSpot;  // me as if at that slot (only its place is read)
    if (fromSlot) {
        const std::int16_t index = thread_.temps[1];
        if (type == nullptr || index < 0 || static_cast<std::size_t>(index) >= type->routingSlots.size()) {
            throw Stop{Outcome::Error, "slot number out of range", std::to_string(index)};
        }
        const slot::Entry& fromEntry = type->routingSlots[static_cast<std::size_t>(index)];
        const auto spot = routingSlotTarget(*owner, fromEntry, isBoneSlot(fromEntry) ? slotBone(*owner, type->group, fromEntry) : std::nullopt);
        if (!spot) {
            throw Stop{Outcome::Unsupported, "Snap slot's bone not found", std::to_string(index)};
        }
        fromSpot = self;
        fromSpot->x = spot->x;
        fromSpot->y = spot->y;
    }
    const SnapRefusal refusal = snapRefusal(world(), fromSpot ? *fromSpot : self, *target);
    if (refusal == SnapRefusal::NotSameRoom) {
        self.data[kPrimitiveResultField] = 2;
    }
    if (refusal != SnapRefusal::None) {
        return false;
    }
    if ((o[4] & kTestOnly) == 0) {
        leaveContainer(self);
        snapTo(world(), self, *target);
    }
    return true;
}

// Takes an object out of what it is in: out of the container's slot when it holds one there, else
// only its container (data 2) cleared, as a seat sets nothing more.
void Execution::leaveContainer(SimObject& object) {
    constexpr std::size_t kContainerField = 2, kSlotField = 3;
    if (object.data[kContainerField] <= 0) {
        return;
    }
    const SimObject* container = world().find(object.data[kContainerField]);
    const auto slot = static_cast<std::size_t>(object.data[kSlotField]);
    if (container != nullptr && slot < container->slots.size() && container->slots[slot] == object.id) {
        takeOutOfSlot(object);
    } else {
        object.data[kContainerField] = 0;
    }
}

// The bone a slot sits at: the first name in entry `bone` of `group`'s "Model - Bones" string
// table, found in the object's model.
std::optional<ModelBone> Execution::slotBone(const SimObject& object, std::uint32_t group, const slot::Entry& entry) {
    constexpr std::uint16_t kBoneNamesTable = 0x90;
    const auto names = entry.bone >= 0 ? code_.string(group, kBoneNamesTable, static_cast<std::size_t>(entry.bone)) : std::nullopt;
    if (!names || !world().modelBones()) {
        return std::nullopt;
    }
    return modelJoint(object, slotJointNames(*names).front());
}

std::optional<ModelBone> Execution::modelJoint(const SimObject& object, const std::string& name) {
    return sim::modelJoint(world(), object, name);
}

// Snap Into: puts an object into a container's slot (a Sim's hand, a counter's top). Operands:
//   byte 0, 1-2  the object (owner, data)
//   byte 3, 4-5  the container (owner, data)
//   byte 6       the slot, or with flag bit 0 my temp 0
//   byte 11      flags: bit 0 see above, bit 1 only test, bit 2 reset the object's root bones
// False when it can't go there, my data 0x34 then saying why (the game's code + 2): 0x11 a
// multi-tile object, 0x0E no such slot, 0x0C the slot holds another object or the object holds
// the container, 0x0F container data 0x1C below object data 0x1B. Not run yet: the slot's
// height mask (0x25), the container's own say (its OBJf entry 0x34) and the bone reset (PR-27).
// The object leaves its old slot and the world's tiles, and takes the container's place,
// level and facing. Data 2 and 3 name the container and slot.
bool Execution::snapInto(const bhav::Instruction& in, Frame& f) {
    constexpr std::size_t kSnapResultField = 0x34, kSnapOtherField = 0x36;
    constexpr std::size_t kContainerField = 2, kCapacityField = 0x1C, kSizeField = 0x1B;
    const auto& o = in.operands;
    const std::uint8_t flags = o[11];
    me().data[kSnapResultField] = 0;
    me().data[kSnapOtherField] = 0;
    const std::int16_t objectId = read(o[0], static_cast<std::uint16_t>(o[1] | o[2] << 8), f);
    const std::int16_t containerId = read(o[3], static_cast<std::uint16_t>(o[4] | o[5] << 8), f);
    SimObject* object = world().find(objectId);
    SimObject* container = world().find(containerId);
    if (object == nullptr || container == nullptr || object->removed || container->removed) {
        throw Stop{Outcome::Error, "could not find object reference", std::to_string(object == nullptr ? objectId : containerId)};
    }
    const std::int16_t slot = (flags & 0x01) ? thread_.temps[0] : static_cast<std::int16_t>(o[6]);
    const auto refuse = [&](std::int16_t code) {
        me().data[kSnapResultField] = static_cast<std::int16_t>(code + 2);
        return false;
    };
    if (object->definition.field(objd::field::kMultiTileMasterId) != 0) {
        return refuse(0x11);
    }
    if (slot < 0 || static_cast<std::size_t>(slot) >= container->slots.size()) {
        return refuse(0x0E);
    }
    const std::int16_t held = container->slots[static_cast<std::size_t>(slot)];
    if (held != 0 && held != object->id) {
        return refuse(0x0C);
    }
    // The object can't go into itself or into anything it holds.
    int depth = 0;
    for (const SimObject* c = container; c != nullptr && depth < 16; ++depth) {
        if (c->id == object->id) {
            return refuse(0x0C);
        }
        c = c->data[kContainerField] > 0 ? world().find(c->data[kContainerField]) : nullptr;
    }
    if (container->data[kCapacityField] < object->data[kSizeField]) {
        return refuse(0x0F);
    }
    if (flags & 0x02) {
        return true;
    }
    putInSlot(*object, *container, slot);
    return true;
}

// Takes an object out of the slot it is in, if any.
void Execution::takeOutOfSlot(SimObject& object) {
    constexpr std::size_t kContainerField = 2, kSlotField = 3;
    if (object.data[kContainerField] > 0) {
        if (SimObject* old = world().find(object.data[kContainerField])) {
            const auto oldSlot = static_cast<std::size_t>(object.data[kSlotField]);
            if (oldSlot < old->slots.size() && old->slots[oldSlot] == object.id) {
                old->slots[oldSlot] = 0;
            }
        }
    }
    object.data[kContainerField] = 0;
    object.data[kSlotField] = -1;
}

// Puts an object into a container's slot (checked by the caller): out of its old slot and the
// world's tiles, at the container's place and level, facing the slot's way (the container's
// facing turned as the slot's bone is, to the nearest compass direction). A stair part's slots
// face up or down the stair this way.
void Execution::putInSlot(SimObject& object, SimObject& container, std::int16_t slot) {
    constexpr std::size_t kContainerField = 2, kSlotField = 3;
    takeOutOfSlot(object);
    container.slots[static_cast<std::size_t>(slot)] = object.id;
    object.data[kContainerField] = container.id;
    object.data[kSlotField] = slot;
    object.x = container.x;
    object.y = container.y;
    object.level = container.level;
    object.tile.reset();
    object.data[kDirectionField] = container.data[kDirectionField];
    const ObjectType* type = code_.objectType(container.definition.guid());
    if (type != nullptr && static_cast<std::size_t>(slot) < type->containerSlotEntries.size()) {
        if (const auto bone = slotBone(container, type->group, type->containerSlotEntries[static_cast<std::size_t>(slot)])) {
            object.data[kDirectionField] = compassDirection(compassHeading(container.data[kDirectionField] & 7) + bone->yaw);
        }
    }
    object.heading.reset();
    world().markFootprintChanged(object.id);
}

} // namespace ts2::sim::vm_detail
