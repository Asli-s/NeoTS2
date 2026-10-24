#include "sim/route_goals.hpp"

#include "sim/object_footprint.hpp"
#include "sim/snap.hpp"

#include <cctype>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <map>

namespace ts2::sim {

namespace {

constexpr float kFixedPoint = 65536.0f;
constexpr float kTan22_5 = 0.41421357f;
constexpr float kPiQuarter = 0.78539816f;
constexpr float kBaseScore = 100, kOpenSide = 25, kOwnSpot = 20, kNearWallCost = 10, kDoorCost = 5, kRandomRange = 20;
constexpr float kSimInTheWayCost = 30;
constexpr float kNearWall = 0.4f, kAgainstWall = 0.3f;
constexpr std::uint32_t kRandomScore = 1u << 13;
constexpr std::uint32_t kNoOwnSpot = 1u << 18;
constexpr std::uint32_t kNoWallSides = 1u << 19;
constexpr std::uint32_t kAbsoluteDirections = 1u << 9;
constexpr std::uint32_t kAnyDirection = 1u << 20;
constexpr float kMaxDistance = 32, kMinStep = 0.0625f;
constexpr float kDistanceWeight = 16, kSidewaysWeight = 0.001f, kUnlistedDirection = 0.01f;
constexpr float kFloorCostWeight = 10;
constexpr int kMostTried = 100, kMostGoals = 48, kMoreWithSeats = 16;
// The seat sweep's whole-tile step and slack, the wedge's tolerance, and the seat goals' sideways weight.
constexpr float kSeatStep = 1, kSeatSlack = 1, kWedgeTolerance = 0.001f, kSeatSidewaysWeight = 0.01f;
// A slot whose goal is its own spot (flag bits 21 and 25), not its joints. Such seat slots aren't offered yet.
constexpr std::uint32_t kKindSlot = 1u << 21;
// Object data 2: the object a Sim sits in.
constexpr std::size_t kContainerField = 2;

float tiles(std::int32_t fixed) { return static_cast<float>(fixed) / kFixedPoint; }

// base + to - from in 32-bit two's complement, as the game's integers wrap. Saved positions can be
// anything, so the sum must not overflow a signed int.
std::int32_t wrappedSum(std::int32_t base, std::int32_t to, std::int32_t from) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(base) + static_cast<std::uint32_t>(to) - static_cast<std::uint32_t>(from));
}

// `at` moved by `offset` tiles in 16.16 units, the offset cut toward zero first.
float shifted(float at, float offset) {
    // Both must fit 16.16 before they are cut, or the conversion to an integer is undefined.
    const auto fits = [](float v) { return std::isfinite(v) && std::fabs(v) < kMaxDistance * 64; };
    if (!fits(at) || !fits(offset)) {
        return at + offset;
    }
    const float sum = static_cast<float>(static_cast<std::int32_t>(at * kFixedPoint)) + static_cast<float>(static_cast<std::int32_t>(offset * kFixedPoint));
    return sum / kFixedPoint;
}

// The game's 8-way direction of (a, b): a along its second axis (object x), b along its first
// (object y). Direction 0 is toward lower b, 2 toward higher a, 4 higher b, 6 lower a.
int direction8(float a, float b) {
    if (std::fabs(b) < std::fabs(a) * kTan22_5) {
        return a > 0 ? 2 : 6;
    }
    if (std::fabs(a) < std::fabs(b) * kTan22_5) {
        return b > 0 ? 4 : 0;
    }
    return a > 0 ? (b > 0 ? 3 : 1) : (b > 0 ? 5 : 7);
}

// The directions of the owner's tile with a wall on that side, turned back by `turn` (the owner's
// facing), each widened to its neighbours. The game never reports the side toward
// direction 6.
std::uint8_t walledDirections(const WallTiles& walls, int level, int x, int y, int turn) {
    const std::uint8_t sides = walls.at(level, x, y);
    std::uint8_t mask = 0;
    if (sides & WallTiles::kLowY) mask |= 1u << 0;
    if (sides & WallTiles::kHighX) mask |= 1u << 2;
    if (sides & WallTiles::kHighY) mask |= 1u << 4;
    std::uint8_t widened = 0;
    for (int d = 0; d < 8; ++d) {
        if (mask & (1u << d)) {
            const int t = ((d - turn) % 8 + 8) % 8;
            widened |= static_cast<std::uint8_t>(1u << ((t + 7) % 8) | 1u << t | 1u << ((t + 1) % 8));
        }
    }
    return widened;
}

// The game's straight test from the owner to a goal, its wall and door parts: the cost it adds, or
// nothing when a wall is across the way or right against the goal, or a Sim can't stand there
// (object coordinates). The walls are tested first, as the game does. `misses` notes what failed.
// `owner` is kept from blocking the spot. `ownerSim` is the owner when another Sim on it doesn't count,
// and `simTerm` false leaves Sims out.
std::optional<float> straightTest(const SlotGoalWorld& world, int wallLevel, float fromX, float fromY, float toX, float toY, std::int16_t owner,
                                  std::int16_t ownerSim = -1, bool simTerm = true, GoalFit fit = GoalFit::Square,
                                  GoalMisses* misses = nullptr) {
    const auto standing = [&] {
        if (!world.canStand || world.canStand(wallLevel, toX, toY, owner, fit)) {
            return true;
        }
        // Something in the way where there is floor. A spot with no floor is not a miss of either kind.
        if (misses != nullptr && (!world.hasFloor || world.hasFloor(wallLevel, toX, toY))) {
            misses->object = true;
        }
        return false;
    };
    if (world.placing) {
        return standing() ? std::optional<float>{0.0f} : std::nullopt;
    }
    const float nearest = wallDistanceOnTile(*world.walls, wallLevel, toX, toY);
    if (wallCrosses(*world.walls, wallLevel, fromX, fromY, toX, toY) || nearest < kAgainstWall) {
        if (misses != nullptr) {
            misses->wall = true;
        }
        return std::nullopt;
    }
    if (!standing()) {
        return std::nullopt;
    }
    float cost = nearest < kNearWall ? kNearWallCost : 0;
    if (world.doorOnTile && world.doorOnTile(wallLevel, static_cast<int>(std::floor(toX)), static_cast<int>(std::floor(toY)))) {
        cost += kDoorCost;
    }
    if (simTerm && world.simNear && world.simNear(wallLevel, toX, toY, ownerSim)) {
        cost += kSimInTheWayCost;
    }
    return cost;
}

} // namespace

namespace {

// Whether a route item blocks a Sim: `owner`'s own object items and dynamic items `intersects` lets
// through don't.
bool blocksSim(const RouteItem& item, std::uint32_t owner, const RouteIntersects& intersects) {
    const bool own = item.kind == RouteItemKind::Object && item.owner == owner;
    const bool letThrough = item.dynamic && intersects && intersects(item.owner).has_value();
    return !own && !letThrough && item.kind != RouteItemKind::Area && item.kind != RouteItemKind::Solid && item.type >= 2;
}

} // namespace

bool canStandAt(const RouteGrid& grid, float x, float y, std::uint32_t owner, const RouteIntersects& intersects, GoalFit fit) {
    const int leaf = std::isfinite(x) && std::isfinite(y) ? grid.leafAt(x, y) : -1;
    if (leaf < 0) {
        return false;
    }
    bool floor = false;
    for (const int i : grid.cells()[static_cast<std::size_t>(leaf)].items) {
        const RouteItem& item = grid.items()[static_cast<std::size_t>(i)];
        floor |= item.makesFloor;
        if (blocksSim(item, owner, intersects) && (fit == GoalFit::Point || item.kind != RouteItemKind::Object)) {
            return false;
        }
    }
    if (!floor || fit == GoalFit::Point) {
        return floor;
    }
    return !objectInTheWayOfSim(grid, x, y, owner, intersects, fit);
}

std::int16_t noGoalReason(const GoalMisses& misses) {
    constexpr std::int16_t kNoValidGoals = 6, kDestinationOccupied = 7, kWallInTheWay = 9;
    return misses.object ? kDestinationOccupied : misses.wall ? kWallInTheWay : kNoValidGoals;
}

bool floorAt(const RouteGrid& grid, float x, float y) {
    const int leaf = std::isfinite(x) && std::isfinite(y) ? grid.leafAt(x, y) : -1;
    if (leaf < 0) {
        return false;
    }
    const auto& items = grid.cells()[static_cast<std::size_t>(leaf)].items;
    return std::ranges::any_of(items, [&](int i) { return grid.items()[static_cast<std::size_t>(i)].makesFloor; });
}

std::optional<std::uint32_t> objectInTheWayOfSim(const RouteGrid& grid, float x, float y, std::uint32_t owner,
                                                 const RouteIntersects& intersects, GoalFit fit) {
    constexpr float kSimHalfSize = 0.1f;   // a Sim's square, as the game places it at a goal
    constexpr float kClearance = 0.3f;     // how far a wide sweep's goal stays from objects
    constexpr float kSampleStep = 1.0f / 16;  // finer than the smallest leaf, so no leaf is missed
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return std::nullopt;
    }
    const auto square = placeFootprint({{-kSimHalfSize, -kSimHalfSize, kSimHalfSize, kSimHalfSize}}, x, y, 0);
    if (square.empty()) {
        return std::nullopt;
    }
    const RouteRect& sim = square.front();
    // Strict: footprints that only touch don't overlap.
    const auto overlaps = [&](const RouteRect& r) { return r.x0 < sim.x1 && r.x1 > sim.x0 && r.y0 < sim.y1 && r.y1 > sim.y0; };
    const auto near = [&](const RouteRect& r) {
        const bool alongX = x >= r.x0 && x <= r.x1, alongY = y >= r.y0 && y <= r.y1;
        if ((alongX && y >= r.y0 - kClearance && y <= r.y1 + kClearance) || (alongY && x >= r.x0 - kClearance && x <= r.x1 + kClearance)) {
            return true;
        }
        const float dx = std::max({r.x0 - x, x - r.x1, 0.0f}), dy = std::max({r.y0 - y, y - r.y1, 0.0f});
        return dx * dx + dy * dy < kClearance * kClearance;
    };
    // The objects' footprints round the spot, each once.
    const float reach = fit == GoalFit::Clearance ? kClearance : 0.0f;
    const float x0 = std::min(sim.x0, x - reach), x1 = std::max(sim.x1, x + reach);
    const float y0 = std::min(sim.y0, y - reach), y1 = std::max(sim.y1, y + reach);
    // Whole counts of samples, so a spot far off the lot can't loop without end.
    const int across = static_cast<int>(std::ceil((x1 - x0) / kSampleStep)), down = static_cast<int>(std::ceil((y1 - y0) / kSampleStep));
    std::vector<int> seen;
    for (int i = 0; i <= across; ++i) {
        for (int j = 0; j <= down; ++j) {
            const int at = grid.leafAt(std::min(x0 + static_cast<float>(i) * kSampleStep, x1), std::min(y0 + static_cast<float>(j) * kSampleStep, y1));
            if (at < 0) {
                continue;
            }
            for (const int k : grid.cells()[static_cast<std::size_t>(at)].items) {
                const RouteItem& item = grid.items()[static_cast<std::size_t>(k)];
                if (item.kind != RouteItemKind::Object || item.padding || !blocksSim(item, owner, intersects) ||
                    std::ranges::find(seen, k) != seen.end()) {
                    continue;
                }
                seen.push_back(k);
                if (fit == GoalFit::Square || item.exactFootprint ? overlaps(item.rect) : near(item.rect)) {
                    return item.owner;
                }
            }
        }
    }
    return std::nullopt;
}

int eighthToward(float dx, float dy) { return direction8(dx, dy); }

float floorCostAt(const RouteGrid& grid, float x, float y) {
    const int leaf = grid.leafAt(x, y);
    if (leaf < 0) {
        return 1;
    }
    const auto p = grid.passage(grid.cells()[static_cast<std::size_t>(leaf)]);
    return p.walkable() ? p.cost : 1;
}

std::vector<std::string> slotJointNames(std::string_view entry) {
    std::vector<std::string> names;
    std::string name;
    // Some shipped lists have a space after the comma (" route_dartboard_watch_0_w").
    for (const char c : entry) {
        if (c == ',') {
            names.push_back(std::move(name));
            name.clear();
        } else if (c != ' ') {
            name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    names.push_back(std::move(name));
    return names;
}

std::optional<ModelBone> modelJoint(const World& world, const SimObject& object, const std::string& name) {
    constexpr std::size_t kLeadField = 0x13;
    constexpr float kPi = 3.14159265f;
    if (!world.modelBones()) {
        return std::nullopt;
    }
    const std::int16_t leadId = object.data[kLeadField];
    if (leadId == 0 || leadId == object.id) {
        return world.modelBones()(object.id, name);
    }
    const SimObject* lead = world.find(leadId);
    const auto joint = lead != nullptr ? world.modelBones()(lead->id, name) : std::nullopt;
    if (!joint) {
        return std::nullopt;
    }
    // The joint's offset from the lead, turned as routingSlotTarget turns it (on the world's axes,
    // the position's y and x), moved to the part, and turned back with the part's facing.
    const auto turned = [](float x, float y, int facing, float sign) {
        const float angle = sign * static_cast<float>(2 - facing) * kPi / 4;
        return std::pair<float, float>{x * std::cos(angle) - y * std::sin(angle), x * std::sin(angle) + y * std::cos(angle)};
    };
    auto [alongY, alongX] = turned(joint->x, joint->y, lead->data[1] & 7, 1);
    alongY += tiles(lead->y) - tiles(object.y);
    alongX += tiles(lead->x) - tiles(object.x);
    const auto [x, y] = turned(alongY, alongX, object.data[1] & 7, -1);
    return ModelBone{x, y, joint->yaw};
}

std::vector<std::optional<ModelBone>> slotJoints(const World& world, CodeSource& code, const SimObject& owner, const slot::Entry& entry,
                                                 std::uint32_t group) {
    constexpr std::uint16_t kModelBones = 0x90;
    std::vector<std::optional<ModelBone>> joints;
    if (isBoneSlot(entry) && entry.bone >= 0 && world.modelBones()) {
        if (const auto names = code.string(group, kModelBones, static_cast<std::size_t>(entry.bone))) {
            for (const auto& name : slotJointNames(*names)) {
                joints.push_back(modelJoint(world, owner, name));
            }
        }
    }
    return joints;
}

SimObject slotGoalBase(const World& world, CodeSource& code, const SimObject& owner) {
    constexpr std::size_t kContainerSlotField = 3;
    constexpr int kMostContainers = 8;  // broken data could make a loop of containers
    SimObject base = owner;
    const SimObject* held = &owner;
    for (int depth = 0; depth < kMostContainers && held->data[kContainerField] != 0; ++depth) {
        const SimObject* container = world.find(held->data[kContainerField]);
        const ObjectType* type = container != nullptr ? code.objectType(container->definition.guid()) : nullptr;
        const auto slot = static_cast<std::size_t>(held->data[kContainerSlotField]);
        if (type == nullptr || held->data[kContainerSlotField] < 0 || slot >= type->containerSlotEntries.size()) {
            break;
        }
        const slot::Entry& entry = type->containerSlotEntries[slot];
        const auto joints = slotJoints(world, code, *container, entry, type->group);
        const auto spot = routingSlotTarget(*container, entry, joints.empty() ? std::nullopt : joints.front());
        if (!spot) {
            break;
        }
        base.x = wrappedSum(base.x, spot->x, container->x);
        base.y = wrappedSum(base.y, spot->y, container->y);
        held = container;
    }
    return base;
}

std::vector<RouteGoal> boneSlotGoals(const SimObject& owner, const slot::Entry& slot, std::span<const std::optional<ModelBone>> joints,
                                     const SimObject& sim, const SlotGoalWorld& world, RouteRandom& random, GoalMisses* misses) {
    std::vector<RouteGoal> goals;
    const float bx = tiles(owner.x), by = tiles(owner.y);
    if (!(std::isfinite(bx) && std::isfinite(by)) || world.walls == nullptr) {
        return goals;
    }
    const int wallLevel = owner.level - 1;
    const int ownerTileX = static_cast<int>(std::floor(bx)), ownerTileY = static_cast<int>(std::floor(by));
    const std::uint8_t walled = (slot.routingFlags & kNoWallSides) ? 0 : walledDirections(*world.walls, wallLevel, ownerTileX, ownerTileY, 0);
    for (std::size_t i = 0; i < joints.size(); ++i) {
        const auto& joint = joints[i];
        if (!joint) {
            continue;
        }
        const auto spot = routingSlotTarget(owner, slot, joint);
        if (!spot) {
            continue;
        }
        const float gx = tiles(spot->x), gy = tiles(spot->y);
        const auto cost = straightTest(world, wallLevel, bx, by, gx, gy, owner.id, owner.id, true, GoalFit::Square, misses);
        if (!cost) {
            continue;
        }
        const int tileX = static_cast<int>(std::floor(gx)), tileY = static_cast<int>(std::floor(gy));
        float score = kBaseScore - *cost;
        if (slot.routingFlags & kRandomScore) {
            score += static_cast<float>(kRandomRange * random.next() - kRandomRange / 2);
        }
        // Toward which side of the owner's tile the spot lies, from the owner to the centre of the
        // spot's tile (none on the owner's own tile).
        const bool ownTile = tileX == ownerTileX && tileY == ownerTileY;
        const int dir = ownTile ? -1 : direction8(static_cast<float>(tileX) + 0.5f - bx, static_cast<float>(tileY) + 0.5f - by);
        if (dir < 0 || !(walled & (1u << dir))) {
            score += kOpenSide;
        }
        if (!(slot.routingFlags & kNoOwnSpot) && spot->x == sim.x && spot->y == sim.y) {
            score += kOwnSpot;
        }
        RouteGoal goal{gx, gy, score};
        goal.token = static_cast<std::int16_t>(i);
        goals.push_back(goal);
    }
    return goals;
}

bool isPointSlot(const slot::Entry& slot) {
    return (slot.routingFlags & 0xFF) == 0 && !isBoneSlot(slot);
}

std::vector<RouteGoal> pointSlotGoal(const SimObject& owner, const slot::Entry& slot, const SlotGoalWorld& world, GoalMisses* misses) {
    const auto spot = routingSlotTarget(owner, slot, std::nullopt);
    if (!spot || (world.walls == nullptr && !world.placing)) {
        return {};
    }
    const float bx = tiles(owner.x), by = tiles(owner.y), gx = tiles(spot->x), gy = tiles(spot->y);
    const auto cost = straightTest(world, owner.level - 1, bx, by, gx, gy, owner.id, -1, true, GoalFit::Square, misses);
    if (!cost) {
        return {};
    }
    return {RouteGoal{gx, gy, kBaseScore}};
}

std::optional<int> slotGoalFacing(const SimObject& owner, const slot::Entry& slot, float x, float y) {
    const int facing = owner.data[1] & 7;
    const float dx = x - tiles(owner.x), dy = y - tiles(owner.y);
    const int away = dx == 0 && dy == 0 ? facing : direction8(dx, dy);
    switch (slot.facing) {
    case -1: return away;
    case -2: return (away + 4) & 7;
    default:
        if (slot.facing >= 0 && slot.facing < 8) {
            return (((8 - slot.facing) & 7) + facing) & 7;
        }
        return std::nullopt;
    }
}

namespace {

struct Direction {
    int dir;
    bool unlisted;
};

// Where (x, y) lies in the frame of sweep direction `dir` from (cx, cy): how far along the direction
// and how far across it, as the sweep lays its spots out.
void inFrame(int dir, float cx, float cy, float x, float y, float& along, float& across) {
    const float angle = static_cast<float>(dir) * kPiQuarter;
    const float first = y - cy, second = x - cx;
    along = -first * std::cos(angle) + second * std::sin(angle);
    across = first * std::sin(angle) + second * std::cos(angle);
}

// The sweep's limits and progress, shared by the standing and seat groups.
struct Sweep {
    float minimum = 0, maximum = 0, optimal = 0, step = 0, weight = 0;
    std::vector<Direction> directions;
    int tried = 0, accepted = 0, mostTried = 0, mostGoals = 0;
    bool full() const { return tried >= mostTried || accepted >= mostGoals; }
};

// The game's sweep around (cx, cy): radii from the optimal outward by `step` (the near side first)
// within the minimum and maximum widened by `slack`, sideways within 22.5 degrees plus the slack, each
// direction at each spot. `visit(x, y, d, sideways, direction)` sees each spot until the sweep is full.
// Returns the last direction swept, if any spot was.
template <class Visit>
std::optional<Direction> sweepSpots(const Sweep& s, float cx, float cy, float step, float slack, Visit visit) {
    std::optional<Direction> last;
    const float low = s.minimum - slack, high = s.maximum + slack;
    const int rings = static_cast<int>(std::floor(std::max(s.optimal - low, high - s.optimal)));
    for (int ring = 0; ring <= rings && !s.full(); ++ring) {
        for (const int side : {-1, 1}) {
            if (ring == 0 && side == 1) {
                break;
            }
            const float d = s.optimal + static_cast<float>(side * ring) * step;
            if (d < low || d > high) {
                continue;
            }
            const float half = d * kTan22_5 + slack;
            for (int j = static_cast<int>(std::ceil(-half / step)); j <= static_cast<int>(std::floor(half / step)); ++j) {
                const float sideways = static_cast<float>(j) * step;
                for (const Direction& r : s.directions) {
                    if (s.full()) {
                        return last;
                    }
                    last = r;
                    const float angle = static_cast<float>(r.dir) * kPiQuarter;
                    // In 16.16 as the game adds them: the offset cut to whole units, then added.
                    const float first = shifted(cy, -d * std::cos(angle) + sideways * std::sin(angle));
                    const float second = shifted(cx, d * std::sin(angle) + sideways * std::cos(angle));
                    visit(second, first, d, sideways, r);
                }
            }
        }
    }
    return last;
}

bool inWedge(const Sweep& s, const Direction& r, float cx, float cy, float x, float y, float slack);

// The standing group: a goal at each swept spot where the straight test from the centre passes.
// First the spot the Sim stands on, when it lies in a direction's wedge and the Sim isn't sitting in
// something (unless the slot leaves out its own spot): tested and scored like a swept spot.
void standingGoals(const SimObject& owner, const slot::Entry& slot, const SimObject& sim, const SlotGoalWorld& world, RouteRandom& random,
                   Sweep& s, float cx, float cy, float standWeight, std::vector<RouteGoal>& goals) {
    const int wallLevel = owner.level - 1;
    struct Candidate {
        float x, y, cost, distance, sideways;
    };
    std::vector<Candidate> found;
    // A sweep wider than a quarter tile keeps its spots 0.3 tiles clear of objects.
    constexpr float kNarrowSweep = 0.25f;
    const GoalFit fit = !isBoneSlot(slot) && slot.maxDistance - slot.minDistance > kNarrowSweep ? GoalFit::Clearance : GoalFit::Square;
    if (!(slot.routingFlags & kNoOwnSpot) && standWeight > 0 && sim.x >= 0 && sim.y >= 0 && sim.data[kContainerField] == 0) {
        const float x = tiles(sim.x), y = tiles(sim.y);
        for (const Direction& r : s.directions) {
            if (!inWedge(s, r, cx, cy, x, y, 0)) {
                continue;
            }
            ++s.tried;
            if (const auto cost = straightTest(world, wallLevel, cx, cy, x, y, owner.id, -1, true, fit)) {
                float along = 0, across = 0;
                inFrame(r.dir, cx, cy, x, y, along, across);
                found.push_back({x, y, *cost, along, across});
                ++s.accepted;
            }
            break;
        }
    }
    const auto last = sweepSpots(s, cx, cy, s.step, 0, [&](float x, float y, float d, float sideways, const Direction&) {
        ++s.tried;
        if (const auto cost = straightTest(world, wallLevel, cx, cy, x, y, owner.id, -1, true, fit)) {
            found.push_back({x, y, *cost, d, sideways});
            ++s.accepted;
        }
    });
    if (!last) {
        return;
    }
    const int facing = (slot.routingFlags & kAbsoluteDirections) ? 0 : (owner.data[1] & 7);
    const std::uint8_t walled = (slot.routingFlags & kNoWallSides)
                                    ? 0
                                    : walledDirections(*world.walls, wallLevel, static_cast<int>(std::floor(tiles(owner.x))),
                                                       static_cast<int>(std::floor(tiles(owner.y))), facing);
    for (const Candidate& c : found) {
        float score = kBaseScore - std::fabs(std::hypot(c.distance, c.sideways) - s.optimal) * s.weight * kDistanceWeight - c.cost;
        if (world.floorCost) {
            const float floor = world.floorCost(wallLevel, c.x, c.y);
            if (floor > 1 && std::isfinite(floor)) {
                score -= (floor - 1) * kFloorCostWeight;
            }
        }
        if (s.weight != 0) {
            score -= std::fabs(c.sideways) * s.weight * kSidewaysWeight;
        }
        score += standWeight;
        if (slot.routingFlags & kRandomScore) {
            score += static_cast<float>(kRandomRange * random.next() - kRandomRange / 2);
        }
        // As the game reads it: the last direction swept, not this goal's.
        if (!(walled & (1u << last->dir))) {
            score += kOpenSide;
        }
        const auto gx = static_cast<std::int32_t>(c.x * kFixedPoint), gy = static_cast<std::int32_t>(c.y * kFixedPoint);
        if (!(slot.routingFlags & kNoOwnSpot) && gx == sim.x && gy == sim.y) {
            score += kOwnSpot;
        }
        if (last->unlisted) {
            score *= kUnlistedDirection;
        }
        goals.push_back({c.x, c.y, score});
    }
}

// Whether a seat joint at (x, y) lies in the wedge of sweep direction `r` from (cx, cy): far enough
// along it for the slot (with the seat sweep's slack) and within 22.5 degrees beyond the slack across.
bool inWedge(const Sweep& s, const Direction& r, float cx, float cy, float x, float y, float slack) {
    float along = 0, across = 0;
    inFrame(r.dir, cx, cy, x, y, along, across);
    if (!(along > 0) || along < s.minimum - slack - kWedgeTolerance || along > s.maximum + slack + kWedgeTolerance) {
        return false;
    }
    const float beyond = std::max(std::fabs(across) - slack, 0.0f);
    return 2 * std::atan2(beyond, along) <= kPiQuarter;
}

// The seat group: seats on the tiles of the spots swept around (cx, cy), a goal at each joint of a
// seat's bone slot. The wedge and the finish measure from the slot's spot.
void seatGoals(const SimObject& owner, const SimObject& sim, const SlotGoalWorld& world, RouteRandom& random, Sweep& s, float cx, float cy,
               float spotX, float spotY, float seatWeight, float standWeight, std::vector<RouteGoal>& goals, bool* seatTaken) {
    constexpr std::size_t kHeldSlot = 0;
    constexpr int kSeatFlagsField = 0x52, kSeatDisabledField = 0x57, kForSaleField = 0x6A;
    constexpr std::int16_t kSeatOff = 0x04;
    const int wallLevel = owner.level - 1;
    const std::size_t first = goals.size();
    std::vector<std::int16_t> seatOf;  // each seat goal's seat
    const auto boneGoals = [&](const Seat& seat, const SeatSlot& slot, const Direction& r) {
        const SimObject& o = *seat.object;
        const float sx = tiles(o.x), sy = tiles(o.y);
        for (std::size_t i = 0; i < slot.joints.size(); ++i) {
            const auto spot = slot.joints[i] ? routingSlotTarget(o, slot.entry, slot.joints[i]) : std::nullopt;
            if (!spot) {
                continue;
            }
            const float gx = tiles(spot->x), gy = tiles(spot->y);
            if (!inWedge(s, r, spotX, spotY, gx, gy, kSeatSlack)) {
                continue;
            }
            ++s.tried;
            const auto cost = straightTest(world, wallLevel, sx, sy, gx, gy, o.id, o.id);
            if (!cost) {
                continue;
            }
            float score = kBaseScore - *cost;
            if (slot.entry.routingFlags & kRandomScore) {
                score += static_cast<float>(kRandomRange * random.next() - kRandomRange / 2);
            }
            score += kOpenSide;
            if (!(slot.entry.routingFlags & kNoOwnSpot) && sim.data[kContainerField] == o.id) {
                score += kOwnSpot;
            }
            if (r.unlisted) {
                score *= kUnlistedDirection;
            }
            ++s.accepted;
            RouteGoal goal{gx, gy, score};
            goal.object = seatWeight > 0 && standWeight < seatWeight && seat.hasSit ? o.id : std::int16_t{0};
            goal.token = static_cast<std::int16_t>(i);
            goal.facing = spot->facing;
            goals.push_back(goal);
            seatOf.push_back(o.id);
        }
    };
    std::map<std::pair<int, int>, std::vector<Seat>> onTile;
    const auto last = sweepSpots(s, cx, cy, kSeatStep, kSeatSlack, [&](float x, float y, float, float, const Direction& r) {
        if (!world.seatsOnTile) {
            return;
        }
        const std::pair<int, int> tile{static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y))};
        auto seats = onTile.find(tile);
        if (seats == onTile.end()) {
            seats = onTile.emplace(tile, world.seatsOnTile(wallLevel, tile.first, tile.second)).first;
        }
        for (const Seat& seat : seats->second) {
            const SimObject* o = seat.object;
            if (o == nullptr || !seat.hasSit || (o->data[1] & 7) != (r.dir + 4) % 8 || o->data[kForSaleField] != 0 ||
                o->data[kSeatDisabledField] > 0 || (o->data[kSeatFlagsField] & kSeatOff)) {
                continue;
            }
            const std::int16_t holder = kHeldSlot < o->slots.size() ? o->slots[kHeldSlot] : std::int16_t{0};
            if (holder != 0 && holder != sim.id) {
                if (seatTaken) {
                    *seatTaken = true;
                }
                continue;
            }
            // The first bone slot naming two or more joints, gone through twice as the game does.
            const SeatSlot* bone = nullptr;
            for (const SeatSlot& slot : seat.slots) {
                if (isBoneSlot(slot.entry) && !(slot.entry.routingFlags & kKindSlot) && slot.joints.size() > 1) {
                    bone = &slot;
                    break;
                }
            }
            if (bone != nullptr && seat.hasStand) {
                boneGoals(seat, *bone, r);
                boneGoals(seat, *bone, r);
            }
        }
    });
    if (!last) {
        return;
    }
    // The seat goals' finish: the straight test from the slot's spot (the seat kept from blocking it,
    // and no Sim term), the seat weight and the distance terms, in the frame of the last direction swept.
    std::size_t kept = first;
    for (std::size_t i = first; i < goals.size(); ++i) {
        RouteGoal goal = goals[i];
        const auto cost = straightTest(world, wallLevel, spotX, spotY, goal.x, goal.y, seatOf[i - first], -1, false);
        if (!cost) {
            continue;
        }
        goal.score += seatWeight - *cost;
        if (s.weight > 0) {
            float along = 0, across = 0;
            inFrame(last->dir, spotX, spotY, goal.x, goal.y, along, across);
            goal.score -= std::fabs(std::hypot(along, across) - s.optimal) * s.weight * kDistanceWeight;
            goal.score -= std::fabs(across) * s.weight * kSeatSidewaysWeight;
        }
        goals[kept++] = goal;
    }
    goals.resize(kept);
}

} // namespace

std::vector<RouteGoal> sweptSlotGoals(const SimObject& owner, const slot::Entry& slot, const SimObject& sim, const SlotGoalWorld& world,
                                      RouteRandom& random, bool* seatTaken) {
    std::vector<RouteGoal> goals;
    const float bx = tiles(owner.x), by = tiles(owner.y);
    if (!(std::isfinite(bx) && std::isfinite(by)) || world.walls == nullptr) {
        return goals;
    }
    const float seatWeight = static_cast<float>(slot.routingInts[1]) * 10;
    float standWeight = static_cast<float>(std::max(slot.routingInts[0], slot.routingInts[2])) * 10;
    if (slot.routingInts[1] == 0 && slot.routingInts[0] == 0 && slot.routingInts[2] == 0) {
        standWeight = 10;
    }
    const bool standing = standWeight > 0, seats = seatWeight > 0;
    if (!standing && !seats) {
        return goals;
    }
    Sweep s;
    s.minimum = slot.minDistance;
    s.maximum = std::min(slot.maxDistance, kMaxDistance);
    s.optimal = slot.optimalDistance;
    if (!(s.minimum >= 0 && s.maximum >= 0 && s.maximum >= s.minimum && s.optimal >= s.minimum && s.optimal <= s.maximum)) {
        return goals;
    }
    s.step = std::max(slot.step, kMinStep);
    s.weight = slot.routingFloat;
    if (!std::isfinite(s.step) || !std::isfinite(s.weight)) {
        return goals;
    }
    // The directions to sweep: those the slot allows, turned with the owner's facing. With flag bit 20
    // the others too, marked.
    const int facing = (slot.routingFlags & kAbsoluteDirections) ? 0 : (owner.data[1] & 7);
    for (int d = 0; d < 8; ++d) {
        if (slot.allowsFacing(d) || (slot.routingFlags & kAnyDirection)) {
            s.directions.push_back({(facing + d) % 8, !slot.allowsFacing(d)});
        }
    }
    if (s.directions.empty()) {
        return goals;
    }
    // The first sweep is centred on the owner. It sets the slot's offset, which moves the centre of
    // what comes after it to the slot's spot.
    float cx = bx, cy = by;
    bool swept = false;
    const auto moveCentre = [&] {
        if (!swept) {
            return;
        }
        if (const auto spot = routingSlotTarget(owner, slot)) {
            cx = tiles(spot->x);
            cy = tiles(spot->y);
        }
    };
    s.mostTried = kMostTried;
    s.mostGoals = kMostGoals;
    const bool seatsFirst = seats && !(standWeight > seatWeight);
    for (int group = 0; group < 2; ++group) {
        const bool seatGroup = (group == 0) == seatsFirst;
        if (seatGroup ? !seats : !standing) {
            continue;
        }
        if (group == 1 && seats && standing) {
            s.mostTried += kMoreWithSeats;
            s.mostGoals += kMoreWithSeats;
        }
        moveCentre();
        const std::size_t before = goals.size();
        const int triedBefore = s.tried;
        if (seatGroup) {
            const auto spot = routingSlotTarget(owner, slot);
            const float spotX = spot ? tiles(spot->x) : bx, spotY = spot ? tiles(spot->y) : by;
            seatGoals(owner, sim, world, random, s, cx, cy, spotX, spotY, seatWeight, standWeight, goals, seatTaken);
        } else {
            standingGoals(owner, slot, sim, world, random, s, cx, cy, standWeight, goals);
        }
        swept = swept || s.tried > triedBefore || goals.size() > before;
    }
    // Over the limit the game keeps every seat goal and a random sample of the rest, not the best:
    // from a random place in the list, the next goal not yet taken (nothing when that runs off the end).
    if (static_cast<int>(goals.size()) > s.mostGoals) {
        const std::vector<RouteGoal> left = goals;
        std::vector<bool> taken(left.size());
        goals.clear();
        for (std::size_t i = 0; i < left.size(); ++i) {
            if (left[i].object != 0) {
                taken[i] = true;
                goals.push_back(left[i]);
            }
        }
        const int picks = s.mostGoals - static_cast<int>(goals.size());
        for (int i = 0; i < picks; ++i) {
            auto at = static_cast<std::size_t>(random.next() * static_cast<double>(left.size()));
            while (at < left.size() && taken[at]) {
                ++at;
            }
            if (at < left.size()) {
                taken[at] = true;
                goals.push_back(left[at]);
            }
        }
    }
    return goals;
}

} // namespace ts2::sim
