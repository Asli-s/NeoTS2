#include "sim/snap.hpp"

#include "sim/idle_spot.hpp"

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
        const int turn = atBone ? -boneSteps(bone->yaw) : slotFacing(slot);
        target.facing = static_cast<std::int16_t>((owner.data[1] + turn) & 7);
    }
    target.checkRoom = (slot.routingFlags & kSkipRoomCheck) == 0;
    target.checkWalls = (slot.routingFlags & kSkipWallCheck) == 0;
    return target;
}

SnapRefusal snapRefusal(const World& world, const SimObject& object, const SnapTarget& target) {
    const float fromX = tiles(object.x), fromY = tiles(object.y), toX = tiles(target.x), toY = tiles(target.y);
    if (!onLot(toX, toY) || (target.checkRoom && (!onLot(fromX, fromY) || world.rooms().roomAtPosition(object.level - 1, fromX, fromY) !=
                                                                              world.rooms().roomAtPosition(target.level - 1, toX, toY)))) {
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
}

} // namespace ts2::sim
