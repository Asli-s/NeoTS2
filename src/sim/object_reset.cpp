#include "sim/object_reset.hpp"

#include "sim/find_location.hpp"
#include "sim/snap.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>

namespace ts2::sim {

namespace {

// The game's objects have this many data fields. The engine keeps a few more, which a reset leaves.
constexpr std::size_t kGameDataCount = 0x72;
static_assert(kGameDataCount <= kObjectDataCount);

// Object data a reset keeps.
constexpr std::array<std::size_t, 19> kKept = {
    0x01,  // direction
    0x02,  // container
    0x03,  // slot in the container
    0x13,  // multi-tile lead
    0x15, 0x18,
    0x1D,  // room
    0x29, 0x2D, 0x2E, 0x2F, 0x37, 0x43, 0x4F, 0x50, 0x51, 0x69, 0x6A, 0x6B,
};

// Object data a reset sets to a fixed value.
struct Fixed {
    std::size_t field;
    std::int16_t value;
};
constexpr std::array<Fixed, 7> kFixed = {{
    {0x17, 2}, {0x39, 0xFF}, {0x47, 0}, {0x48, 0xFF}, {0x49, 0xFF}, {0x4A, 0xFF}, {0x4B, 0xFF},
}};

// Object data a reset takes from the object's OBJD: {data field, OBJD field}.
struct FromDefinition {
    std::size_t field;
    std::size_t definitionField;
};
constexpr std::array<FromDefinition, 2> kFromDefinition = {{{0x3A, 0x31}, {0x4E, 0x0D}}};
constexpr std::size_t kNicenessField = 0x07, kNicenessDefinitionField = 0x2D;
// ... and, when OBJD field 0x0C is set, these too.
constexpr std::array<FromDefinition, 4> kFromDefinitionIf0C = {{{0x04, 0x06}, {0x05, 0x03}, {0x0D, 0x05}, {0x2A, 0x04}}};

constexpr std::uint16_t kMainFunction = 1;  // OBJf entry
constexpr std::size_t kMainArgs = 4;        // Main starts with four zero arguments

std::int16_t definitionField(const ObjectType& type, std::size_t field) {
    return static_cast<std::int16_t>(type.definition.field(field));
}

// A frame running tree `tree` from the top, or none when the tree can't be found.
std::optional<Frame> startFrame(CodeSource& code, const ObjectType& type, std::uint16_t tree, std::int16_t objectId) {
    const std::uint32_t group = tree < 0x1000   ? kGlobalGroup
                                : tree < 0x2000 ? type.group
                                                : code.semiGlobalGroup(type.group);
    auto behavior = code.behavior(group, tree);
    if (!behavior) {
        return std::nullopt;
    }
    Frame f;
    f.group = group;
    f.codeOwner = type.group;
    f.instance = tree;
    f.params.assign(std::max<std::size_t>(kMainArgs, behavior->argCount), 0);
    f.locals.assign(behavior->localCount, 0);
    f.runningOnId = objectId;
    f.code = std::move(behavior);
    return f;
}

} // namespace

void restartMain(SimObject& object, const ObjectType& type, CodeSource& code) {
    object.thread.frames.clear();
    object.runDepths.clear();
    object.thread.pendingResult.reset();
    object.thread.sleepTicks = 0;
    object.thread.stackObjectId = 0;
    const auto* functions = code.objectFunctions(type);
    if (functions != nullptr && kMainFunction < functions->size() && (*functions)[kMainFunction].action != 0) {
        if (auto frame = startFrame(code, type, (*functions)[kMainFunction].action, object.id)) {
            object.thread.frames.push_back(std::move(*frame));
        }
    }
}

void resetObject(SimObject& object, const ObjectType& type, CodeSource& code) {
    const auto saved = object.data;
    std::fill_n(object.data.begin(), kGameDataCount, std::int16_t{0});
    for (const std::size_t field : kKept) {
        object.data[field] = saved[field];
    }
    object.data[kObjectIdField] = object.id;
    for (const auto& f : kFixed) {
        object.data[f.field] = f.value;
    }
    for (const auto& f : kFromDefinition) {
        object.data[f.field] = definitionField(type, f.definitionField);
    }
    // A multi-tile object's parts take their niceness from the object's master type.
    const ObjectType* master = isMultiTilePart(type) ? code.multiTileMaster(type) : nullptr;
    object.data[kNicenessField] = definitionField(master ? *master : type, kNicenessDefinitionField);
    if (type.definition.field(0x0C) != 0) {
        for (const auto& f : kFromDefinitionIf0C) {
            object.data[f.field] = definitionField(type, f.definitionField);
        }
    }
    // The temps and the token reference go, the token itself stays.
    object.thread.temps.fill(0);
    object.tokenSource = {};

    restartMain(object, type, code);
    notePausedRun(object);
}

void notePausedRun(SimObject& object) {
    constexpr std::size_t kRunsWhilePausedField = 0x3C;
    object.runsWhilePaused = object.data[kRunsWhilePausedField] != 0;
}

namespace {

constexpr std::size_t kSittingPersonData = 0x00;

// Whether `sim` may go back into slot `slot` of `container`, by the checks Snap Into makes. The
// slot's allowed kinds and the container's own say are not checked yet, as in Snap Into.
bool fitsSlot(const World& world, const SimObject& sim, const SimObject& container, std::int16_t slot) {
    constexpr std::size_t kContainerField = 2, kCapacityField = 0x1C, kSizeField = 0x1B;
    constexpr int kDepthLimit = 16;  // containers inside containers, and a broken save can make a loop
    if (slot < 0 || static_cast<std::size_t>(slot) >= container.slots.size()) {
        return false;
    }
    const std::int16_t held = container.slots[static_cast<std::size_t>(slot)];
    if (held != 0 && held != sim.id) {
        return false;
    }
    int depth = 0;
    for (const SimObject* c = &container; c != nullptr && depth < kDepthLimit; ++depth) {
        if (c->id == sim.id) {
            return false;
        }
        c = c->data[kContainerField] > 0 ? world.find(c->data[kContainerField]) : nullptr;
    }
    return container.data[kCapacityField] >= sim.data[kSizeField];
}

} // namespace

void placeAfterReset(World& world, SimObject& sim, SimRandom& rng) {
    constexpr std::size_t kContainerField = 2, kSlotField = 3, kDirectionField = 1;
    if (sim.removed || sim.x == kOffLot || sim.y == kOffLot) {
        return;
    }
    const std::int16_t slot = sim.data[kSlotField];
    SimObject* container = sim.data[kContainerField] > 0 ? world.find(sim.data[kContainerField]) : nullptr;
    const bool heldBySlot = container != nullptr && slot >= 0 && static_cast<std::size_t>(slot) < container->slots.size() &&
                            container->slots[static_cast<std::size_t>(slot)] == sim.id;
    const SnapTarget here{sim.x, sim.y, sim.level, std::nullopt};
    takeOutOfSlot(world, sim);
    if (sim.personData[kSittingPersonData] == 0 && sim.personData[kSwimmingPersonData] == 0) {
        if (container != nullptr && fitsSlot(world, sim, *container, slot)) {
            sim.data[kContainerField] = container->id;
            sim.data[kSlotField] = slot;
            if (heldBySlot) {
                container->slots[static_cast<std::size_t>(slot)] = sim.id;
            }
            return;
        }
        if (container == nullptr && !placementCode(world, sim, SnapTarget{sim.x, sim.y, sim.level, sim.data[kDirectionField]}, true)) {
            return;
        }
    }
    LocationSearch search;
    search.startX = sim.x;
    search.startY = sim.y;
    search.level = sim.level;
    search.preferEmpty = false;
    std::optional<FoundLocation> found = findLocation(world, sim, search, rng);
    if (!found) {
        search.sameRoom = false;
        found = findLocation(world, sim, search, rng);
    }
    snapTo(world, sim, found ? SnapTarget{found->x, found->y, sim.level, std::nullopt} : here);
}

void resetSim(World& world, SimObject& sim, const ObjectType& type, CodeSource& code, SimRandom& rng) {
    constexpr std::size_t kResetClearedField = 0x22;
    constexpr std::size_t kSocialMenuObjectPersonData = 0x3B;
    placeAfterReset(world, sim, rng);
    sim.queue.clear();
    sim.actionString.clear();
    sim.actionIconGuid = 0;
    sim.actionIconIndex = 0;
    sim.actionIconObject = 0;
    sim.running.reset();
    sim.runningSub.reset();
    sim.animation.reset();
    sim.ending.reset();
    sim.overlays.clear();
    sim.turning.reset();
    sim.walking.reset();
    sim.walkTail.reset();
    sim.postRoute.reset();
    sim.reaching.reset();
    sim.data[kResetClearedField] = 0;
    for (const std::size_t index : {kSittingPersonData, kSocialMenuObjectPersonData}) {
        if (index < sim.personData.size()) {
            sim.personData[index] = 0;
        }
    }
    restartMain(sim, type, code);
    notePausedRun(sim);
}

bool isMultiTilePart(const ObjectType& type) {
    return type.definition.field(objd::field::kMultiTileMasterId) != 0;
}

void finishSingleTileReset(SimObject& object, const ObjectType& type) {
    // Two placement fields come from the OBJD when it sets either.
    if (type.definition.field(0x6A) != 0 || type.definition.field(0x6B) != 0) {
        object.data[0x2A] = definitionField(type, 0x6A);
        object.data[0x52] = definitionField(type, 0x6B);
    }
    notePausedRun(object);
}

} // namespace ts2::sim
