#include "sim/find_location.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::sim {

namespace {
constexpr std::int32_t kLotLimit = 256;  // tiles, a lot is never wider
constexpr std::size_t kPlacementFlagsField = 8;
constexpr std::int16_t kIgnoredByPreferEmpty = 0x06;
constexpr std::size_t kFacingRuleField = 0x17;
constexpr std::uint16_t kNoDiagonalFacingDefinitionField = 0x0A;

constexpr int kDefaultWalk = 6;

struct Offset {
    std::int32_t along = 0;   // added to y
    std::int32_t across = 0;  // added to x
};

// The step around a ring shared by the walks that start on a diagonal.
Offset aroundRing(Offset p) {
    const std::int32_t a = p.along, b = p.across;
    if (a <= -b && a < b) {
        return {a, b - 1};
    }
    if (a <= b) {
        if (a > -b) {
            return {a - 1, b};
        }
    } else if (a >= -b) {
        return {a, b + 1};
    }
    return a >= b && a < -b ? Offset{a + 1, b} : p;
}

// One step of the game's walk out from the start tile, ring by ring. `first` is the direction of the
// first step (0 is -y, 2 is +x, odd ones diagonal). Each ring then goes round the same way. The
// default walk is direction 6: (0,0) (0,-1) (1,-1) (1,0) (1,1) (0,1) (-1,1) (-1,0) (-1,-1) (-1,-2)
// ... as (along, across).
Offset nextInWalk(int first, Offset p) {
    const std::int32_t a = p.along, b = p.across;
    const bool atStart = a == 0 && b == 0;
    switch (first) {
    case 0:
        if (a <= b && a >= -b) return {a - 1, b};
        if (a <= -b && a < b) return {a, b - 1};
        if (a > b && a >= -b) return {a, b + 1};
        return a >= b && a < -b ? Offset{a + 1, b} : p;
    case 2:
        if (a >= b && a >= -b) return {a, b + 1};
        if (a <= -b && a < b) return {a, b - 1};
        if (a <= b && a > -b) return {a - 1, b};
        return a >= b && a < -b ? Offset{a + 1, b} : p;
    case 4:
        if (a >= b && a <= -b) return {a + 1, b};
        if (a <= -b && a < b) return {a, b - 1};
        if (a > b) return a >= -b ? Offset{a, b + 1} : p;
        return a > -b ? Offset{a - 1, b} : p;
    case 1:
    case 3:
    case 5:
    case 7: {
        // Odd directions begin at the diagonal, then go round, stepping out a ring at its last tile.
        static constexpr Offset kDiagonal[4] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
        const Offset d = kDiagonal[first / 2];
        if (atStart) {
            return d;
        }
        const Offset q = aroundRing(p);
        const bool ringEnd = d.across > 0 ? q.across >= 0 : q.across <= 0;
        return ringEnd && q.across * d.across == q.along * d.along ? Offset{q.along + d.along, q.across + d.across} : q;
    }
    default:
        if (a <= -b) return a <= b ? Offset{a, b - 1} : (a < -b ? Offset{a + 1, b} : Offset{a, b + 1});
        if (a <= b) return {a - 1, b};
        return a < -b ? (a > b ? Offset{a + 1, b} : p) : Offset{a, b + 1};
    }
}

bool holdsSomethingElse(const World& world, const SimObject& placed, const Tile& tile) {
    return world.nextAfter(0, [&](const SimObject& s) {
        return s.id != placed.id && s.tile == tile && (s.data[kPlacementFlagsField] & kIgnoredByPreferEmpty) == 0;
    }) != nullptr;
}

std::optional<std::int16_t> pickFacing(const SimObject& placed, const LocationSearch& search, int firstTurn) {
    const bool diagonalsAllowed = placed.definition.field(kNoDiagonalFacingDefinitionField) == 0 &&
                                  placed.data[kFacingRuleField] < 2;
    for (int i = 0; i < 8; ++i) {
        const int turn = (firstTurn + i) & 7;
        const int facing = (search.referenceFacing + turn) & 7;
        if (((search.allowedFacings >> turn) & 1) != 0 && (diagonalsAllowed || (facing & 1) == 0)) {
            return static_cast<std::int16_t>(facing);
        }
    }
    return std::nullopt;
}
// The direction (0-7, 0 is -y, 2 is +x) from the start point to a tile's centre: the nearest of the
// eight, split at tan(22.5 degrees).
int directionTo(const LocationSearch& search, std::int32_t x, std::int32_t y) {
    constexpr double kTan22_5 = 0.4142135679721832;
    const double dx = x + 0.5 - search.startX / 65536.0, dy = y + 0.5 - search.startY / 65536.0;
    const double ax = std::fabs(dx), ay = std::fabs(dy);
    int dir = 2;
    if (ax * kTan22_5 <= ay) {
        dir = kTan22_5 * ay <= ax ? 3 : 4;
    }
    if (dx < 0) {
        dir = dir == 2 ? 6 : dir == 3 ? 5 : dir;
    }
    if (dy < 0) {
        dir = dir == 3 ? 1 : dir == 4 ? 0 : dir == 5 ? 7 : dir;
    }
    return dir;
}
} // namespace

std::optional<FoundLocation> findLocation(const World& world, const SimObject& placed, const LocationSearch& search,
                                          SimRandom& rng) {
    const RoomMap& rooms = world.rooms();
    const std::int32_t width = rooms.positionWidth() > 0 ? std::min(rooms.positionWidth(), kLotLimit) : kLotLimit;
    const std::int32_t height = rooms.positionHeight() > 0 ? std::min(rooms.positionHeight(), kLotLimit) : kLotLimit;
    if (width < 3 || height < 3) {
        return std::nullopt;
    }
    std::int32_t startX = std::clamp(search.startX >> 16, 1, width - 2);
    std::int32_t startY = std::clamp(search.startY >> 16, 1, height - 2);
    const auto roomOf = [&](std::int32_t x, std::int32_t y) {
        return rooms.roomAtPosition(search.level - 1, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
    };
    const std::int32_t startRoom = roomOf(startX, startY);

    // The game gives up after 8 lot widths of tiles off the lot in a row. Every on-lot tile is seen
    // well before the walk's rings pass the lot.
    const std::int32_t offLotLimit = 8 * std::max(width, height);
    std::int32_t offLotInARow = 0;
    const int first = search.startDirection >= 0 ? search.startDirection & 7 : kDefaultWalk;
    Offset at{};
    bool restarted = false;
    for (std::int64_t step = 0; offLotInARow < offLotLimit; ++step) {
        if (step > 0 && !restarted) {
            const Offset next = nextInWalk(first, at);
            if ((next.along == at.along && next.across == at.across) || step > 4LL * kLotLimit * kLotLimit) {
                break;
            }
            at = next;
        }
        restarted = false;
        const std::int32_t x = startX + at.across, y = startY + at.along;
        if (x < 0 || y < 0 || x >= width || y >= height) {
            ++offLotInARow;
            continue;
        }
        offLotInARow = 0;
        const Tile tile{static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y), static_cast<std::uint8_t>(search.level)};
        if (search.preferEmpty && holdsSomethingElse(world, placed, tile)) {
            continue;
        }
        if (search.sameRoom && roomOf(x, y) != startRoom) {
            continue;
        }
        FoundLocation found{x * 65536 + 32768, y * 65536 + 32768, std::nullopt};
        if (search.allowedFacings != 0) {
            const int firstTurn = static_cast<int>((static_cast<std::uint64_t>(rng()) * 8) >> 32);
            found.facing = pickFacing(placed, search, firstTurn);
            if (!found.facing) {
                continue;
            }
        }
        // With a start direction, a start tile that isn't that way from the start point gives way to
        // the tile in front of it: the walk starts again there.
        if (search.startDirection >= 0 && at.along == 0 && at.across == 0 && directionTo(search, x, y) != first) {
            static constexpr Offset kFront[8] = {{-1, 0}, {-1, 1}, {0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, -1}};
            const std::int32_t frontX = std::clamp((search.startX >> 16) + kFront[first].across, 1, width - 2);
            const std::int32_t frontY = std::clamp((search.startY >> 16) + kFront[first].along, 1, height - 2);
            if (frontX != startX || frontY != startY) {
                startX = frontX;
                startY = frontY;
                restarted = true;
                continue;
            }
        }
        return found;
    }
    return std::nullopt;
}

} // namespace ts2::sim
