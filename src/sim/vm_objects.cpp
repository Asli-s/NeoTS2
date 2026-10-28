#include "sim/vm_execution.hpp"

#include <format>

namespace ts2::sim::vm_detail {

std::int32_t Execution::sleepTicks(const bhav::Instruction& in, Frame& f) {
    const auto param = static_cast<std::uint16_t>(in.operands[0] | in.operands[1] << 8);
    return slotAt(f.params, param, "param");
}

// Operands:
//   bytes 0-1  who (0 = me, 1 = stack object, 2 = nobody)
//   byte 2     bit 0 return immediately, bit 1 clear = clean up all
// A few object types can't be removed. Removing me without "return immediately" ends my run.
bool Execution::removeObject(const bhav::Instruction& in) {
    const auto& o = in.operands;
    const auto who = static_cast<std::int16_t>(o[0] | o[1] << 8);
    if (who < 0 || who > 2) {
        throw Stop{Outcome::Error, "illegal owner field in data reference", std::to_string(who)};
    }
    const std::int16_t id = who == 0 ? me().id : who == 1 ? stackObject() : 0;
    SimObject* object = world().findIncludingRemoved(id);
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
//   byte 1     flags: 0x01 fade time from Temp 1, 0x02 intensity from Temp 0
//   bytes 2-4  the object (owner, data)
//   bytes 5-6  fade time in ticks (0..300, else 10)
//   byte 7     intensity in percent (0..100, else 50)
//   byte 8     the light: 0xFF all, else an entry of the object's light name table
// Sets how strongly all of the object's lights shine. The light named and the fade time don't
// change what the game does with it.
bool Execution::changeLight(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    const std::int16_t id = read(o[2], static_cast<std::uint16_t>(o[3] | o[4] << 8), f);
    SimObject* target = world().find(id);
    if (target == nullptr) {
        throw Stop{Outcome::Error, "object not found for Change Light", std::to_string(id)};
    }
    int percent = (o[1] & 0x02) ? thread_.temps[0] : o[7];
    if (percent > 100 || percent < 0) {
        percent = 50;
    }
    if (o[8] != 0xFF) {
        const auto name = code_.string(me().group, kLightNameTable, o[8]);
        if (!name || name->empty()) {
            throw Stop{Outcome::Error, "Missing Light Name", std::to_string(o[8])};
        }
    }
    target->lightStrength = static_cast<float>(percent * 0.01);
    return true;
}

// Operands:
//   bytes 0-1   material: entry of string table 0x88 (or my temp 0, flag 0x10)
//   byte 2      flags (kMeshGroupFromSource ... kMeshGroupSemiGlobal)
//   bytes 3-4   mesh group: entry of string table 0x87 (or my temp 1, flag 0x20, -1 = whole model)
//   byte 5, 6-7 the object to change
//   byte 8, 9-10 the source object whose tables flags 0x01 and 0x08 read, otherwise the tree's owner's
//               (an own material comes from the target's or source's recolor first, see materialNames)
//   byte 13     0x01 the material is a TV channel (its video isn't played yet). Snapshot, portrait
//               and painting modes (0x02-0x20) are not done yet
// A mesh group name that is missing or empty means the whole model. Always true.
bool Execution::changeMaterial(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    if (o[13] & ~kMaterialIsTvChannel) {
        throw Stop{Outcome::Unsupported, "Change Material mode " + hex(o[13], 2) + " not implemented", ""};
    }
    const std::uint8_t flags = o[2];
    SimObject* target = world().find(read(o[5], static_cast<std::uint16_t>(o[6] | o[7] << 8), f));
    if (target == nullptr) {
        throw Stop{Outcome::Error, "could not find object reference", "Change Material"};
    }
    auto sourceGroup = [&](bool fromSource) {
        if (!fromSource) {
            return f.codeOwner;
        }
        const SimObject* source = world().find(read(o[8], static_cast<std::uint16_t>(o[9] | o[10] << 8), f));
        if (source == nullptr) {
            throw Stop{Outcome::Error, "could not find object reference", "Change Material source"};
        }
        return source->group;
    };
    // Global, semi-global (none when the object has no semi-global code) or the object's own table.
    auto name = [&](std::uint32_t group, bool global, bool semiGlobal, std::uint16_t table, std::size_t index) {
        if (global) {
            group = kGlobalGroup;
        } else if (semiGlobal) {
            const std::uint32_t semi = code_.semiGlobalGroup(group);
            if (semi == group) {
                return std::string();
            }
            group = semi;
        }
        return code_.string(group, table, index).value_or(std::string());
    };

    const std::size_t materialIndex = (flags & kMaterialIndexInTemp0) ? static_cast<std::uint16_t>(thread_.temps[0])
                                                                      : static_cast<std::size_t>(o[0] | o[1] << 8);
    std::string material;
    if (!(flags & (kMaterialGlobal | kMaterialSemiGlobal))) {
        const SimObject* recolored = (flags & kMaterialFromSource)
                                         ? world().find(read(o[8], static_cast<std::uint16_t>(o[9] | o[10] << 8), f))
                                         : target;
        if (recolored != nullptr && materialIndex + 1 < recolored->materialNames.size()) {
            material = recolored->materialNames[materialIndex + 1];
        }
    }
    if (material.empty()) {
        material = name(sourceGroup(flags & kMaterialFromSource), flags & kMaterialGlobal, flags & kMaterialSemiGlobal,
                        kMaterialNameTable, materialIndex);
    }
    if (material.empty()) {
        throw Stop{Outcome::Error, "Missing Material / Mesh Group Name", std::to_string(materialIndex)};
    }
    const std::uint32_t meshGroupOwner = sourceGroup(flags & kMeshGroupFromSource);
    const int meshGroupIndex = (flags & kMeshGroupIndexInTemp1) ? thread_.temps[1] : (o[3] | o[4] << 8);
    const std::string meshGroup = meshGroupIndex < 0 ? std::string()
                                                     : name(meshGroupOwner, flags & kMeshGroupGlobal, flags & kMeshGroupSemiGlobal,
                                                            kMeshGroupNameTable, static_cast<std::size_t>(meshGroupIndex));
    setMaterial(*target, {meshGroup, material});
    return true;
}

// Operands:
//   bytes 0-1   sound: entry of string table 0x4132, of my tree's own code (0-9999), the global
//               code (10000-19999) or the semi-global code (20000 and up). Negative: the table is
//               that of -id, the entry the variable in bytes 6, 7-8
//   byte 4      flags: 0x02 the stack object is the source (unless the byte is 0xFF), 0x40 stop
// Hidden objects (object data 0x22 bit 0x02) and Sims off the lot make no sound. Always true.
bool Execution::soundEvent(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    const auto id = static_cast<std::int16_t>(o[0] | o[1] << 8);
    const int base = std::abs(static_cast<int>(id));
    const std::uint32_t group = base >= 20000 ? code_.semiGlobalGroup(f.codeOwner) : base >= 10000 ? kGlobalGroup : f.codeOwner;
    const int index = id < 0 ? read(o[6], static_cast<std::uint16_t>(o[7] | o[8] << 8), f)
                             : base - (base >= 20000 ? 20000 : base >= 10000 ? 10000 : 0);
    const auto& events = world().soundEvents();
    if (!events || index < 0) {
        return true;
    }
    const auto name = code_.string(group, kSoundNameTable, static_cast<std::size_t>(index));
    if (!name || name->empty()) {
        return true;
    }
    std::int16_t source = (o[4] & kSoundFromStackObject) && o[4] != 0xFF ? stackObject() : me().id;
    if (source == 0) {
        source = me().id;
    }
    if (const SimObject* object = world().find(source)) {
        if ((object->data[kHiddenFlags] & 0x02) || (object->isPerson() && object->x == kOffLot)) {
            return true;
        }
    }
    events({*name, source, (o[4] & kSoundStop) != 0});
    return true;
}

// Cinematic: asks the camera to play a cinematic with the actors listed in object array 0 of an
// object, usually a "Temp Array" object. True when the cinematic starts, and it never waits.
// Operands:
//   bytes 0-1   cinematic: entry (counted from 1) of string table 0x97, or with byte 5 bit 0x10
//               the variable in bytes 6, 7-8
//   byte 5      flags: 0x20 the global names, 0x40 the semi-global ones, else the tree's own
//   byte 9      the variable holding the actors' object, data in bytes 10-11
// An object that doesn't exist or has no object arrays is an error. The engine plays no cinematics
// yet, so none starts and this is false. The scripts then go on without one (a wedding shows the
// Sim in a picture-in-picture instead).
bool Execution::cinematic(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    const std::int16_t actorsId = read(o[9], static_cast<std::uint16_t>(o[10] | o[11] << 8), f);
    const SimObject* actors = world().find(actorsId);
    if (actors == nullptr) {
        throw Stop{Outcome::Error, "could not find object reference", std::to_string(actorsId)};
    }
    if (actors->arrays.empty()) {
        throw Stop{Outcome::Error, "bad object array", "0"};
    }
    if (o[5] & kCinematicIndexInVariable) {
        read(o[6], static_cast<std::uint16_t>(o[7] | o[8] << 8), f);  // read as the game does, for its errors
    }
    return false;
}

// Dialog. Operands: byte 5 the type, byte 7 bit 0: show it and go on, bits 4-6: the temp holding a
// notification's id (or a yes-no-cancel answer). Text notifications get a new id in that temp and
// go on. Stop is true when the notification was shown (and removes it), Modify and Append when it
// is shown. Tutorial popups and hints go on. Other dialogs wait a tick and are answered with their
// first button (true). Pickers need the stack object to have object arrays. Nothing is shown yet.
std::optional<bool> Execution::dialog(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    const std::uint8_t type = o[5];
    std::int16_t& id = thread_.temps[(o[7] >> 4) & 0x07];
    switch (type) {
    case kTextNotification:   id = world().showNotification(); return true;
    case kStopNotification:   return world().hideNotification(id);
    case kModifyNotification:
    case kAppendNotification: return world().notificationShown(id);
    case kHint:
    case kTutorialNext:
    case kTutorialNextModal:  return true;
    default: break;
    }
    if (o[7] & 0x01) {
        return true;
    }
    if (f.dialogOpen) {
        f.dialogOpen = false;
        return true;
    }
    if (type == kTutorialTasks) {
        return true;
    }
    if (std::find(std::begin(kPickerDialogs), std::end(kPickerDialogs), type) != std::end(kPickerDialogs)) {
        const SimObject* stack = world().find(stackObject());
        if (stack == nullptr || stack->arrays.empty()) {
            throw Stop{Outcome::Error, "bad object array", "Dialog picker"};
        }
    }
    f.dialogOpen = true;
    return std::nullopt;
}

// Camera Control: true at once while the camera is busy. Otherwise it marks the camera busy
// (Camera Busy := bytes 4-5, or temp 0 with byte 8 bit 0x20) and waits a tick. Then, with the
// stack object off the lot it is true, else it waits one more tick and is true. The camera
// doesn't move yet.
std::optional<bool> Execution::cameraControl(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    if (f.cameraStep == 0) {
        if (globals_[global::kCameraBusy] != 0) {
            return true;
        }
        globals_[global::kCameraBusy] = (o[8] & 0x20) ? thread_.temps[0] : static_cast<std::int16_t>(o[4] | o[5] << 8);
        globals_[global::kCameraStarted] = 1;
        f.cameraStep = 1;
        return std::nullopt;
    }
    if (f.cameraStep == 1) {
        const SimObject* stack = world().find(stackObject());
        if (stack == nullptr) {
            f.cameraStep = 0;
            throw Stop{Outcome::Error, "stack object does not exist", std::to_string(stackObject())};
        }
        if (stack->x != kOffLot && stack->y != kOffLot) {
            f.cameraStep = 2;
            return std::nullopt;
        }
    }
    f.cameraStep = 0;
    return true;
}

void Execution::setMaterial(SimObject& object, MaterialChange change) {
    if (!object.materialChanges.empty() && object.materialChanges.back() == change) {
        return;  // some objects set the same material every few ticks
    }
    auto& changes = object.materialChanges;
    if (change.group.empty()) {
        changes.clear();
    } else {
        std::erase_if(changes, [&](const MaterialChange& c) { return c.group == change.group; });
    }
    changes.push_back(std::move(change));
    ++object.materialVersion;
}

// The centre of the tile next to `sim` the way (its direction + `turn`) points, facing back at it
// (direction + 4 + `turn`), on its level, as Create Object placement 9 puts things: the game
// turns a step of (0, -1) by even directions only, so an odd one is `sim`'s own tile. Nothing
// off the lot.
std::optional<Execution::CreateSpot> Execution::tileBeside(const SimObject& sim, std::int16_t turn) {
    if (sim.x == kOffLot || sim.y == kOffLot) {
        return std::nullopt;
    }
    const int way = (sim.data[kDirectionField] + turn) & 7;
    constexpr int kStepX[] = {0, 0, 1, 0, 0, 0, -1, 0}, kStepY[] = {-1, 0, 0, 0, 1, 0, 0, 0};
    const int x = static_cast<int>(std::floor(sim.x / 65536.0)) + kStepX[way];
    const int y = static_cast<int>(std::floor(sim.y / 65536.0)) + kStepY[way];
    if (!onLotTile(sim.level, x, y)) {
        return std::nullopt;
    }
    CreateSpot spot{x << 16 | 0x8000, y << 16 | 0x8000, sim.level,
                    Tile{static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y), static_cast<std::uint8_t>(sim.level)}};
    spot.facing = static_cast<std::int16_t>((sim.data[kDirectionField] + 4 + turn) & 7);
    return spot;
}

bool Execution::onLotTile(std::int32_t level, int x, int y) {
    constexpr int kMaxTile = 255;
    if (x < 0 || y < 0 || x > kMaxTile || y > kMaxTile) {
        return false;
    }
    const RoomMap& rooms = world().rooms();
    return rooms.width() == 0 || rooms.roomAtPosition(level - 1, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f) != RoomMap::kNoRoom;
}

// Create Object's tile tests on the spot's tile (flags byte 5): 0x08 refuses a tile with any object
// on it, 0x01 one with an object of the same type.
bool Execution::tileRefuses(const CreateSpot& spot, std::uint8_t flags, std::uint32_t guid) {
    if ((flags & (kCreateDoNotDuplicate | kCreateFailIfTileOccupied)) == 0) {
        return false;
    }
    const Tile tile = spot.tile.value_or(Tile{static_cast<std::uint8_t>(spot.x >> 16), static_cast<std::uint8_t>(spot.y >> 16),
                                              static_cast<std::uint8_t>(spot.level)});
    bool occupied = false, duplicate = false;
    world().nextAfter(std::numeric_limits<std::int16_t>::min(), [&](const SimObject& o) {
        if (!o.removed && o.tile == tile) {
            occupied = true;
            duplicate |= o.definition.guid() == guid;
        }
        return false;
    });
    return ((flags & kCreateFailIfTileOccupied) && occupied) || ((flags & kCreateDoNotDuplicate) && duplicate);
}

// The centre of the tile in front of `ref` (one tile along its facing), on its level. None when that
// tile is off the lot or in a different room from `ref`'s, as the game checks before placing.
std::optional<Execution::CreateSpot> Execution::tileInFront(const SimObject* ref) {
    constexpr std::size_t kRoomField = 0x1D;
    constexpr int kMaxTile = 255;
    constexpr double kPi = 3.14159265358979;
    if (ref == nullptr || ref->x == kOffLot || ref->y == kOffLot) {
        return std::nullopt;
    }
    // The facing turns the world's +y. The world's x and y are the route's y and x.
    const double angle = static_cast<double>(2 - (ref->data[kDirectionField] & 7)) * kPi / 4;
    const int x = static_cast<int>(std::floor(ref->x / 65536.0)) + static_cast<int>(std::lround(std::cos(angle)));
    const int y = static_cast<int>(std::floor(ref->y / 65536.0)) + static_cast<int>(std::lround(-std::sin(angle)));
    if (x < 0 || y < 0 || x > kMaxTile || y > kMaxTile) {
        return std::nullopt;
    }
    const RoomMap& rooms = world().rooms();
    if (rooms.width() > 0) {
        const std::int32_t room = rooms.roomAtPosition(ref->level - 1, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
        if (room == RoomMap::kNoRoom || room != ref->data[kRoomField]) {
            return std::nullopt;
        }
    }
    return CreateSpot{x << 16 | 0x8000, y << 16 | 0x8000, ref->level,
                      Tile{static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y), static_cast<std::uint8_t>(ref->level)}};
}

// Operands:
//   bytes 0-3  GUID
//   byte 4     where to create the object
//   byte 5     flags
//   byte 6     local
//   byte 9     slot
//   byte 10    more flags
// With flag 0x04 the object is the Sim of the neighbor whose id is in the stack object (the GUID
// and flags 0x40 and 0x80 are not used). The new object becomes the stack object.
// Placements:
// - 1 "on top of me" and 2 "in my hand": both into my slot 0, the game treats them alike, and my
//   tile must pass the tile tests.
// - 4 and 0xA: into slot byte 9 of the stack object or of the object in temp 0 (false when there
//   is none, its tile must pass the tile tests).
// - 3: in front of the stack object (see tileInFront).
// - 9: next to me (see tileBeside, the turn in local byte 10).
// - "out of world".
// - "below object in local" or "in param 0": at the exact place of the object whose id is there,
//   false when there is none.
// - 5 "underneath me": at my exact place and level. Off the lot it is not supported.
// The spot's tile must pass the tile tests (see tileRefuses).
// Not done yet: the game's test that the object fits at the spot or in the slot (here it always
// fits, and a slot that is missing or full is reported as not implemented), and placement 9's
// wall test.
bool Execution::createObject(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    const std::uint8_t flags = o[5];
    if ((flags & kCreateFromNeighbor) && !world().neighbors()) {
        throw Stop{Outcome::Unsupported, "Create Object from a neighbor needs the neighborhood", ""};
    }
    if (!(flags & kCreateFromNeighbor) && (flags & kCreateFromInventoryToken)) {
        throw Stop{Outcome::Unsupported, "Create Object from an inventory token", ""};
    }
    if ((flags & kCreatePassObjectIds) && (flags & kCreatePassTemp0)) {
        throw Stop{Outcome::Error, "more than one passing value specified", ""};
    }

    std::uint32_t guid = readGuid(o);
    if (flags & kCreateFromNeighbor) {
        // The neighbor whose id is the stack object: its Sim's type. No such neighbor is false.
        const auto neighborGuid = world().neighborGuid(stackObject());
        if (!neighborGuid) {
            return false;
        }
        guid = *neighborGuid;
    } else if (flags & kCreateGuidFromTemps) {
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
    std::optional<CreateSpot> spot;
    SimObject* container = nullptr;
    std::size_t slot = 0;
    if (o[4] == kPlaceOnTopOfMe || o[4] == kPlaceInMyHand) {
        container = &me();
    } else if (o[4] == kPlaceInStackObjectSlot || o[4] == kPlaceInSlotOfObjectInTemp0) {
        container = world().find(o[4] == kPlaceInStackObjectSlot ? stackObject() : thread_.temps[0]);
        if (container == nullptr) {
            return false;
        }
        slot = o[9];
    }
    if (container != nullptr) {
        if (slot >= container->slots.size() || container->slots[slot] != 0) {
            const std::string where = "slot " + std::to_string(slot) + " of " + std::to_string(container->id);
            const SimObject* held = slot < container->slots.size() ? world().find(container->slots[slot]) : nullptr;
            throw Stop{Outcome::Unsupported, "Create Object into a slot that isn't free (the game's fit test)",
                       held != nullptr                       ? where + " held by " + std::to_string(held->id) + " " + held->definition.name
                       : slot >= container->slots.size() ? where + " missing"
                                                             : where + " held by " + std::to_string(container->slots[slot]) + " (gone)"};
        }
        if (container->x == kOffLot || container->y == kOffLot) {
            return false;
        }
        spot = CreateSpot{container->x, container->y, container->level,
                          Tile{static_cast<std::uint8_t>(container->x >> 16), static_cast<std::uint8_t>(container->y >> 16),
                               static_cast<std::uint8_t>(container->level)}};
    } else if (o[4] == kPlaceInFrontOfStackObject) {
        spot = tileInFront(world().find(stackObject()));
        if (!spot) {
            return false;
        }
    } else if (o[4] == kPlaceBesideMe) {
        spot = tileBeside(me(), slotAt(f.locals, o[10], "local"));
        if (!spot) {
            return false;
        }
    } else if (o[4] == kPlaceBelowObjectInLocal || o[4] == kPlaceBelowObjectInParam0) {
        if (o[4] == kPlaceBelowObjectInParam0 && (f.code == nullptr || f.code->argCount == 0)) {
            throw Stop{Outcome::Error, "param out of range", "0"};  // the tree takes no params
        }
        const std::int16_t id = o[4] == kPlaceBelowObjectInParam0 ? slotAt(f.params, 0, "param")
                                                                   : slotAt(f.locals, static_cast<std::int8_t>(o[6]), "local");
        const SimObject* below = id != 0 ? world().find(id) : nullptr;
        if (below == nullptr || below->removed || below->x == kOffLot || below->y == kOffLot) {
            return false;
        }
        spot = CreateSpot{below->x, below->y, below->level, below->tile};
    } else if (o[4] == kPlaceUnderneathMe) {
        const SimObject& self = me();
        if (self.x == kOffLot || self.y == kOffLot) {
            throw Stop{Outcome::Unsupported, "Create Object underneath an object off the lot", ""};
        }
        spot = CreateSpot{self.x, self.y, self.level,
                          Tile{static_cast<std::uint8_t>(self.x >> 16), static_cast<std::uint8_t>(self.y >> 16),
                               static_cast<std::uint8_t>(self.level)}};
    } else if (o[4] != kPlaceOutOfWorld) {
        throw Stop{Outcome::Unsupported, "Create Object placement not implemented", hex(o[4], 2)};
    }
    if (spot && tileRefuses(*spot, flags, type->definition.guid())) {
        return false;
    }
    if ((o[10] & kCreateCopyMaterials) && world().find(thread_.temps[5]) == nullptr) {
        throw Stop{Outcome::Error, "copy-materials source in temp 5 does not exist",
                   std::to_string(thread_.temps[5])};
    }

    SimObject& created = world().create(type->group, type->definition, type->containerSlots);
    if (container != nullptr) {
        putInSlot(created, *container, static_cast<std::int16_t>(slot));  // a byte, checked against the slots
    } else if (spot) {
        created.x = spot->x;
        created.y = spot->y;
        created.level = spot->level;
        created.tile = spot->tile;
        if (spot->facing) {
            created.data[kDirectionField] = *spot->facing;
        }
        world().updateRoom(created);
    }
    if (startCreated) {
        startCreated(created, *type);
    }
    passToMain(created, flags);
    stackObject() = created.id;
    return true;
}

// Flag 0x10 gives the new object's Main my temp 0 as its first parameter. Flag 0x02 gives it my id
// and my stack object. Main has run up to its first wait by now, and a Main without parameters
// gets no value.
void Execution::passToMain(SimObject& created, std::uint8_t flags) {
    if (!(flags & (kCreatePassObjectIds | kCreatePassTemp0)) || created.thread.frames.empty()) {
        return;
    }
    Frame& main = created.thread.frames.front();
    const std::int16_t value = (flags & kCreatePassTemp0) ? thread_.temps[0] : me().id;
    if (main.code && main.code->argCount > 0 && !main.params.empty()) {
        main.params[0] = value;
    }
    if (flags & kCreatePassObjectIds) {
        main.stackObjectId = stackObject();
    }
}

// Byte 4 bit 0 means "ignore in release". Otherwise a non-zero condition is an error.
bool Execution::breakPoint(const bhav::Instruction& in, Frame& f) {
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

// The value is the high half of a 32-bit random number times the range. Range 0 writes 0, except
// into a motive, which it leaves as it is.
bool Execution::randomNumber(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    const auto destData = static_cast<std::uint16_t>(o[0] | o[1] << 8);
    const auto rangeData = static_cast<std::uint16_t>(o[4] | o[5] << 8);
    const std::int16_t range = read(o[6], rangeData, f);
    if (range == 0) {
        const bool motive = o[2] == owner::kMyMotives || o[2] == owner::kStackObjectMotives ||
                            o[2] == owner::kStackObjectMotiveByTemp;
        if (!motive) {
            write(o[2], destData, f, 0);
        }
        return true;
    }
    const auto scaled = static_cast<std::uint64_t>(static_cast<std::uint32_t>(rng_())) *
                        static_cast<std::uint32_t>(static_cast<std::int32_t>(range));
    write(o[2], destData, f, static_cast<std::int16_t>(scaled >> 32));
    return true;
}

// Operands: bytes 0-1 whose (0 mine, 1 the stack object's), bytes 2-3 what (0 graphic, 1 lighting,
// 2 room score). The game only checks them: it updates all three itself when objects change.
bool Execution::refresh(const bhav::Instruction& in) {
    const auto& o = in.operands;
    const auto whose = static_cast<std::int16_t>(o[0] | o[1] << 8);
    const auto what = static_cast<std::int16_t>(o[2] | o[3] << 8);
    if (whose != 0 && whose != 1) {
        throw Stop{Outcome::Error, "bad parameter to the refresh primitive", std::to_string(whose)};
    }
    if (whose == 1 && world().find(stackObject()) == nullptr) {
        throw Stop{Outcome::Error, "no stack object present when required", std::to_string(stackObject())};
    }
    if (what < 0 || what > 2) {
        throw Stop{Outcome::Error, "bad parameter to the refresh primitive", std::to_string(what)};
    }
    return true;
}

// Runs a Lua script. Entry [bytes 2-3] (counted from 1) of string table 0x130 names it, looked up
// in the code owner's group when byte 4 bit 1 is set, else in its semi-global group with bit 2,
// else in the global group. With byte 4 bit 0 the script is the one of that name. Without it the
// entry's description is the script's Lua source (kept as "<name>_0x<group>" once compiled). With
// byte 4 bit 3 the script gets three parameters (owner, data) from bytes 6-8, 9-11 and 12-14. The
// primitive's result is the script's return value. The other bits of byte 4 and byte 5 are not used.
bool Execution::luaScript(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    LuaRuntime* scripts = world().scripts();
    if (scripts == nullptr) {
        throw Stop{Outcome::Unsupported, "Lua scripts are not loaded", ""};
    }
    const std::uint8_t flags = o[4];
    const auto index = static_cast<std::uint16_t>(o[2] | o[3] << 8);
    const std::uint32_t group = (flags & 0x02) ? f.codeOwner : (flags & 0x04) ? code_.semiGlobalGroup(f.codeOwner) : kGlobalGroup;
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
    const auto table = static_cast<std::uint16_t>(o[0] | o[1] << 8);
    const bool ran = (flags & 0x01) != 0
                         ? scripts->run(*name, call)
                         : scripts->runSource(std::format("{}_0x{:08x}", *name, group),
                                              code_.stringDescription(group, table, index - 1u).value_or(""), call);
    if (ran) {
        return call.returnValue;
    }
    if (!call.unsupported.empty()) {
        throw Stop{Outcome::Unsupported, "script function not implemented: " + call.unsupported, *name};
    }
    // The game logs a script that raised an object error and the tree goes on, the primitive false.
    if (call.errorCode != 0) {
        ++scripts->scriptErrors;
        scripts->lastScriptError = *name + " " + hex(static_cast<std::uint16_t>(call.errorCode), 2);
        return false;
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

// Message: hands six values to objects' message handlers (OBJf entry 0x24), each run at once on my
// stack, as me, with the receiver as stack object and the message as its args. The handlers'
// results don't count. Operands:
//   byte 15, 1-2  message value 0 (owner, data), value 1 is my stack object, 2 is byte 8,
//                 3 and 4 are (byte 9, 10-11) and (byte 12, 13-14), 5 is my temp 0
//   byte 4        flags: bit 1 one receiver, the object (byte 5, 6-7), true. Bit 0 the room
//                 filter takes (byte 5, 6-7) instead of my room
//   byte 3        who receives otherwise: 0 Sims with person data 0x20 = 0, 2 and 4 with 0x20 = 2,
//                 1, 3 and 5 every Sim, 6 every object that is not a Sim, 7 every object
//   byte 0        of those, 0 the ones in my room, 1 on my level, 3 and 4 a tile test (not
//                 known yet), anything else all of them
// A broadcast is true when anyone received it (a receiver without a handler counts).
bool Execution::message(const bhav::Instruction& in, Frame& f) {
    constexpr std::size_t kMessageHandlerEntry = 0x24;
    constexpr std::size_t kSimKindPersonData = 0x20;
    const auto& o = in.operands;
    const auto data = [&](std::size_t at) { return static_cast<std::uint16_t>(o[at] | o[at + 1] << 8); };
    const std::array<std::int16_t, 6> words{read(o[15], data(1), f), stackObject(), static_cast<std::int16_t>(o[8]),
                                            read(o[9], data(10), f), read(o[12], data(13), f), thread_.temps[0]};
    const auto send = [&](SimObject& receiver) {
        const ObjectType* type = code_.objectType(receiver.definition.guid());
        const auto* functions = type ? code_.objectFunctions(*type) : nullptr;
        if (functions == nullptr || kMessageHandlerEntry >= functions->size() || (*functions)[kMessageHandlerEntry].action == 0) {
            return;
        }
        const RunResult r = runOnMe(type->group, (*functions)[kMessageHandlerEntry].action, receiver.id, words);
        switch (r.outcome) {
        case Outcome::True:
        case Outcome::False:
            return;
        case Outcome::Unsupported:
        case Outcome::MissingCode:
            throw Stop{r.outcome, r.reason, r.detail};
        default:
            // As for any tree run at once: a handler that waits or fails is an error on my stack.
            me().failedRun = (r.outcome == Outcome::Sleeping ? "a message handler waited" : r.reason + " " + r.detail) + " in " +
                             hex(r.group, 8) + " " + hex(r.instance, 4) + " at " + std::to_string(r.pc);
            return;
        }
    };
    const std::uint8_t flags = o[4];
    if (flags & 0x02) {
        const std::int16_t id = read(o[5], data(6), f);
        SimObject* receiver = world().find(id);
        if (receiver == nullptr || receiver->removed) {
            throw Stop{Outcome::Error, "message receiver does not exist", std::to_string(id)};
        }
        send(*receiver);
        return true;
    }
    const std::uint8_t who = o[3];
    const std::uint8_t where = o[0];
    if (where == 3 || where == 4) {
        throw Stop{Outcome::Unsupported, "Message tile filter not implemented", hex(where, 2)};
    }
    const std::int16_t room = flags & 0x01 ? read(o[5], data(6), f) : me().data[kRoomDataField];
    const std::int32_t level = me().level;
    // The receivers are chosen before any handler runs, which may add or remove objects.
    std::vector<std::int16_t> receivers;
    world().nextAfter(0, [&](const SimObject& s) {
        if (s.removed) {
            return false;
        }
        const bool person = s.isPerson();
        const std::int16_t kind = person ? s.personData[kSimKindPersonData] : 0;
        const bool chosen = who == 7 ? true
                          : who == 6 ? !person
                          : !person  ? false
                          : who == 0 ? kind == 0
                          : who == 2 || who == 4 ? kind == 2
                                                 : true;
        const bool near = where == 0 ? s.data[kRoomDataField] == room : where == 1 ? s.level == level : true;
        if (chosen && near) {
            receivers.push_back(s.id);
        }
        return false;
    });
    bool sent = false;
    for (const std::int16_t id : receivers) {
        if (SimObject* receiver = world().find(id); receiver != nullptr && !receiver->removed) {
            send(*receiver);
            sent = true;
        }
    }
    return sent;
}

// On Timer: sets, changes or removes a timer on the stack object (see object_timer.hpp). Operands:
//   bytes 0-1    ticks until it runs out (or my temp 1 with flag bit 3)
//   bytes 3-4    the tree it runs
//   byte 5       flags: bit 0 the timer gets this tree's params instead of the three values,
//                bit 1 it repeats, bit 2 (change) its countdown starts over, bit 3 see above
//   byte 6, 7-8 / 9, 10-11 / 12, 13-14  the three values (owner, data)
//   byte 15      0 set (the new timer's id goes to my temp 0), 1 change, else remove the
//                timer whose id is in my temp 0
// Always true.
bool Execution::onTimer(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    const std::uint8_t flags = o[5];
    SimObject& target = stack();
    const std::int32_t time = flags & 0x08 ? thread_.temps[1] : static_cast<std::uint16_t>(o[0] | o[1] << 8);
    const auto tree = static_cast<std::uint16_t>(o[3] | o[4] << 8);
    auto& timers = target.timers;
    const auto byId = [&] {
        const auto id = static_cast<std::uint16_t>(thread_.temps[0]);
        return std::find_if(timers.begin(), timers.end(), [id](const ObjectTimer& t) { return t.id == id; });
    };
    if (o[15] == 1) {
        if (const auto timer = byId(); timer != timers.end()) {
            timer->tree = tree;
            timer->interval = time;
            timer->repeats = (flags & 0x02) != 0;
            if (flags & 0x04) {
                timer->countdown = time;
            }
        }
        return true;
    }
    if (o[15] != 0) {
        if (const auto timer = byId(); timer != timers.end()) {
            timers.erase(timer);
        }
        return true;
    }
    if (timers.size() >= kMaxObjectTimers) {
        throw Stop{Outcome::Error, "too many timers on one object", std::to_string(target.id)};
    }
    ObjectTimer timer;
    timer.countdown = time;
    timer.interval = time;
    timer.tree = tree;
    timer.repeats = (flags & 0x02) != 0;
    timer.setter = me().id;
    if (flags & 0x01) {
        std::copy_n(f.params.begin(), std::min(f.params.size(), timer.args.size()), timer.args.begin());
    } else {
        timer.args[0] = read(o[6], static_cast<std::uint16_t>(o[7] | o[8] << 8), f);
        timer.args[1] = read(o[9], static_cast<std::uint16_t>(o[10] | o[11] << 8), f);
        timer.args[2] = read(o[12], static_cast<std::uint16_t>(o[13] | o[14] << 8), f);
    }
    // Ids are handed out in turn, skipping 0 and any still in use (the game's source of ids is
    // not known yet: PR-25).
    do {
        ++target.lastTimerId;
    } while (target.lastTimerId == 0 || std::any_of(timers.begin(), timers.end(), [&](const ObjectTimer& t) { return t.id == target.lastTimerId; }));
    timer.id = target.lastTimerId;
    timers.push_back(timer);
    thread_.temps[0] = static_cast<std::int16_t>(timer.id);
    return true;
}

// Operands:
//   bytes 0-3  GUID
//   bytes 4-5  object id (data), byte 6 its owner
//   byte 7     flags: 0x04 "does this GUID exist", 0x02 neighbor id, 0x01 compare the GUID the
//              object's type was cloned from (OBJD fields 0x46-0x47) instead of its own
// A missing object is false, not an error.
// Notify the Stack Object out of Idle: when the stack object's script waits at a Sleep or an Idle
// for Input, its param 0 (the ticks left, as those trees use them) becomes 0, so a wait counted
// there ends. Anything else is left alone. Always true.
bool Execution::notifyOutOfIdle() {
    SimObject& target = stack();
    if (target.thread.frames.empty()) {
        return true;
    }
    Frame& waiting = target.thread.frames.back();
    if (!waiting.code || waiting.pc >= waiting.code->instructions.size()) {
        return true;
    }
    const auto& at = waiting.code->instructions[waiting.pc];
    if ((at.opcode != kSleepOpcode && at.opcode != kIdleForInputOpcode) || waiting.params.empty()) {
        return true;
    }
    waiting.params[0] = 0;
    if (at.opcode == kSleepOpcode && (at.operands[0] | at.operands[1] << 8) == 0) {
        target.thread.sleepTicks = 0;
    }
    return true;
}

bool Execution::testObjectType(const bhav::Instruction& in, Frame& f) {
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
    const auto data = static_cast<std::uint16_t>(o[4] | o[5] << 8);
    const SimObject* object = world().find(read(o[6], data, f));
    if (object == nullptr) {
        return false;
    }
    if (flags & kTestOriginalGuid) {
        const std::uint32_t original = object->definition.field(kOriginalGuidLow) |
                                       static_cast<std::uint32_t>(object->definition.field(kOriginalGuidHigh)) << 16;
        return original == guid && code_.objectType(original) != nullptr;
    }
    return object->definition.guid() == guid;
}

} // namespace ts2::sim::vm_detail
