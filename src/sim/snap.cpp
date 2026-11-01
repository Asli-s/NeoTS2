#include "sim/snap.hpp"

#include "diag/log.hpp"
#include "sim/idle_spot.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::sim {

namespace {
constexpr float kFixedPoint = 65536.0f;
constexpr float kLotLimit = 256;  // tiles, a lot is never wider
constexpr std::uint32_t kBoneSlot = 0x20000;  // slot entry flag word: placed by a bone of the model
constexpr std::uint32_t kFaceSlot = 1u << 12;
constexpr std::uint32_t kSkipRoomCheck = 1u << 11;
constexpr std::uint32_t kSkipWallCheck = 1u << 19;

float tiles(std::int32_t fixed) {
    return static_cast<float>(fixed) / kFixedPoint;
}

bool onLot(float x, float y) {
    return x >= 0 && y >= 0 && x < kLotLimit && y < kLotLimit;
}

// The slot's first allowed facing, 0 when it allows none.
int slotFacing(const slot::Entry& slot) {
    for (int direction = 0; direction < 8; ++direction) {
        if (slot.allowsFacing(direction)) {
            return direction;
        }
    }
    return 0;
}
constexpr float kPi = 3.14159265358979f;

// A bone's turn as facing steps of 45 degrees, rounded to the nearest.
int boneSteps(float yaw) {
    return static_cast<int>(std::lround(std::remainder(yaw, 2 * kPi) / (kPi / 4)));
}
} // namespace

bool isBoneSlot(const slot::Entry& slot) {
    return (slot.objectFlags & kBoneSlot) != 0;
}

std::optional<SnapTarget> routingSlotTarget(const SimObject& owner, const slot::Entry& slot, const std::optional<ModelBone>& bone) {
    const bool atBone = isBoneSlot(slot);
    if (atBone && (!bone || !std::isfinite(bone->yaw))) {
        return std::nullopt;
    }
    // The offset, (x, -y) or the bone's (x, y), turns with the owner's facing, 45 degrees a step,
    // facing 2 leaving it as it is. As in Wall in Front, that turn works on the world's axes, which are
    // the position's y and x. The game would also push the spot out by my radius, which stays 0.
    const float offsetX = atBone ? bone->x : slot.x, offsetY = atBone ? bone->y : -slot.y;
    const float angle = static_cast<float>(2 - owner.data[1]) * kPi / 4;
    const float c = std::cos(angle), s = std::sin(angle);
    const float worldX = offsetX * c - offsetY * s;
    const float worldY = offsetX * s + offsetY * c;
    const float x = tiles(owner.x) + worldY, y = tiles(owner.y) + worldX;
    // Far beyond any lot a position no longer fits the object's 16.16 numbers.
    constexpr float kFarLimit = 30000;
    if (!(std::abs(x) < kFarLimit && std::abs(y) < kFarLimit)) {
        return std::nullopt;
    }
    SnapTarget target;
    target.x = static_cast<std::int32_t>(std::lround(x * kFixedPoint));
    target.y = static_cast<std::int32_t>(std::lround(y * kFixedPoint));
    target.level = owner.level;
    if ((slot.routingFlags & kFaceSlot) != 0) {
        target.facing = atBone ? boneSlotFacing(owner, *bone) : static_cast<std::int16_t>((owner.data[1] + slotFacing(slot)) & 7);
    }
    target.checkRoom = (slot.routingFlags & kSkipRoomCheck) == 0;
    target.checkWalls = (slot.routingFlags & kSkipWallCheck) == 0;
    return target;
}

std::int16_t boneSlotFacing(const SimObject& owner, const ModelBone& bone) {
    return static_cast<std::int16_t>((owner.data[1] - boneSteps(bone.yaw)) & 7);
}

namespace {

// Whether (x, y) in tiles is on the lot, by the lot's own size where the room map knows it. The
// far edge itself counts as on the lot only with `withFarEdge`, as the placement test counts it.
// The room check tests the tile, so for it the far edge is off the lot.
bool inLot(const World& world, float x, float y, bool withFarEdge = false) {
    const auto& rooms = world.rooms();
    if (rooms.positionWidth() <= 0 || rooms.positionHeight() <= 0) {
        return onLot(x, y);
    }
    const float width = static_cast<float>(rooms.positionWidth()), height = static_cast<float>(rooms.positionHeight());
    if (withFarEdge) {
        return x >= 0 && y >= 0 && x <= width && y <= height;
    }
    return onLot(x, y) && x < width && y < height;
}

} // namespace

SnapRefusal snapRefusal(const World& world, const SimObject& object, const SnapTarget& target) {
    const float fromX = tiles(object.x), fromY = tiles(object.y), toX = tiles(target.x), toY = tiles(target.y);
    // Without the room check the placement test finds a spot off the lot (see placementCode).
    if (!inLot(world, toX, toY)) {
        return target.checkRoom ? SnapRefusal::OffLot : SnapRefusal::None;
    }
    if (target.checkRoom && (!onLot(fromX, fromY) || world.rooms().roomAtPosition(object.level - 1, fromX, fromY) !=
                                                         world.rooms().roomAtPosition(target.level - 1, toX, toY))) {
        return SnapRefusal::NotSameRoom;
    }
    const bool moves = object.x != target.x || object.y != target.y;
    if (target.checkWalls && moves && object.level == target.level && onLot(fromX, fromY) &&
        wallBetween(world, object.level - 1, fromX, fromY, toX, toY)) {
        return SnapRefusal::WallInTheWay;
    }
    return SnapRefusal::None;
}

void snapTo(World& world, SimObject& object, const SnapTarget& target) {
    object.x = target.x;
    object.y = target.y;
    object.level = target.level;
    if (onLot(tiles(target.x), tiles(target.y))) {
        object.tile = Tile{static_cast<std::uint8_t>(target.x >> 16), static_cast<std::uint8_t>(target.y >> 16),
                           static_cast<std::uint8_t>(target.level)};
        world.updateRoom(object);
    } else {
        object.tile.reset();
    }
    if (target.facing) {
        object.data[1] = *target.facing;
        object.heading.reset();
    }
    world.markFootprintChanged(object.id);
    moveContents(world, object);
    moveParts(world, object);
}

void moveContents(World& world, const SimObject& container, int depth) {
    constexpr int kDepthLimit = 8;  // slots inside slots, and a broken save can make a loop
    if (depth >= kDepthLimit) {
        return;
    }
    for (const std::int16_t id : container.slots) {
        SimObject* inside = id > 0 ? world.find(id) : nullptr;
        if (inside == nullptr || inside->data[2] != container.id) {
            continue;
        }
        inside->x = container.x;
        inside->y = container.y;
        inside->level = container.level;
        world.markFootprintChanged(inside->id);
        moveContents(world, *inside, depth + 1);
    }
}

std::vector<PartSpot> partSpots(const World& world, const SimObject& moved, const SnapTarget& at) {
    constexpr std::size_t kPartLevelField = 0x38;  // OBJD
    const auto& own = moved.definition;
    if (own.field(objd::field::kMultiTileMasterId) == 0 || own.isMultiTileMaster() || moved.data[kLeadTileField] <= 0) {
        return {};
    }
    const std::int16_t facing = at.facing.value_or(moved.data[1]);
    if ((facing & 1) != 0) {
        diag::warnOnce("multi-tile objects: parts don't follow a part turned diagonally yet");
        return {};
    }
    const std::int16_t lead = moved.data[kLeadTileField];
    const auto parts = world.idsIncludingRemoved([&](const SimObject& o) {
        return o.id != moved.id && !o.removed && o.data[kLeadTileField] == lead && o.definition.field(objd::field::kMultiTileMasterId) != 0;
    });
    const auto subIndex = [](const SimObject& o) { return o.definition.field(objd::field::kMultiTileSubIndex); };
    std::vector<PartSpot> spots;
    for (const std::int16_t id : parts) {
        const SimObject* part = world.find(id);
        if (part == nullptr) {
            continue;
        }
        // The offset from `moved`, a quarter turn for each two steps of facing.
        const int a = (subIndex(*part) >> 8) - (subIndex(moved) >> 8);
        const int b = (subIndex(*part) & 0xFF) - (subIndex(moved) & 0xFF);
        const int dx = facing == 0 ? a : facing == 2 ? -b : facing == 4 ? -a : b;
        const int dy = facing == 0 ? b : facing == 2 ? a : facing == 4 ? -b : -a;
        PartSpot spot{id, kOffLot, kOffLot, at.level + part->definition.field(kPartLevelField) - own.field(kPartLevelField), facing};
        const float x = tiles(at.x) + static_cast<float>(dx), y = tiles(at.y) + static_cast<float>(dy);
        if (at.x != kOffLot && at.y != kOffLot && onLot(x, y)) {
            spot.x = at.x + dx * 0x10000;
            spot.y = at.y + dy * 0x10000;
        }
        spots.push_back(spot);
    }
    return spots;
}

void moveParts(World& world, const SimObject& moved) {
    for (const PartSpot& spot : partSpots(world, moved, SnapTarget{moved.x, moved.y, moved.level, moved.data[1]})) {
        SimObject* part = world.find(spot.id);
        part->level = spot.level;
        part->data[1] = spot.facing;
        part->heading.reset();
        part->x = spot.x;
        part->y = spot.y;
        if (spot.x == kOffLot) {
            part->tile.reset();
        } else {
            part->tile = Tile{static_cast<std::uint8_t>(part->x >> 16), static_cast<std::uint8_t>(part->y >> 16),
                              static_cast<std::uint8_t>(part->level)};
            world.updateRoom(*part);
        }
        world.markFootprintChanged(part->id);
        moveContents(world, *part);
    }
}

std::optional<std::int16_t> placementCode(const World& world, const SimObject& object, const SnapTarget& target) {
    constexpr std::int16_t kOffLotCode = 2, kInTheWayCode = 0x0C;
    const auto& test = world.objectInTheWay();
    const auto codeAt = [&](const SimObject& placed, std::int32_t x, std::int32_t y, std::int32_t level,
                            std::int16_t facing) -> std::optional<std::int16_t> {
        if (x == kOffLot || y == kOffLot || !inLot(world, tiles(x), tiles(y), true)) {
            return kOffLotCode;
        }
        if (test && !placed.isPerson() && test(placed, tiles(x), tiles(y), level, facing)) {
            return kInTheWayCode;
        }
        return std::nullopt;
    };
    if (const auto code = codeAt(object, target.x, target.y, target.level, target.facing.value_or(object.data[1]))) {
        return code;
    }
    // A part landing off the lot is given as kOffLot by partSpots.
    for (const PartSpot& spot : partSpots(world, object, target)) {
        const SimObject* part = world.find(spot.id);
        if (part == nullptr) {
            continue;
        }
        if (const auto code = codeAt(*part, spot.x, spot.y, spot.level, spot.facing)) {
            return code;
        }
    }
    return std::nullopt;
}

} // namespace ts2::sim
