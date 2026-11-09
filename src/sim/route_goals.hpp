#pragma once

#include "formats/slot.hpp"
#include "sim/code_source.hpp"
#include "sim/idle_spot.hpp"
#include "sim/route_search.hpp"
#include "sim/world.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Where Go To Routing Slot sends a Sim: the goals the path search walks toward, each with the game's
// score (the highest-scoring goal reached wins). See docs/open-questions.md (Routing, RT-6).
namespace ts2::sim {

// The joint names in a bone slot's entry of the type's "Model - Bones" string table: comma
// separated, in lower case, as models name their joints.
std::vector<std::string> slotJointNames(std::string_view entry);

// A joint of `object`'s model. A part of a multi-tile object other than its lead has no model of its
// own, and the game then puts every joint at the part's own place, unturned.
std::optional<ModelBone> modelJoint(const World& world, const SimObject& object, const std::string& name);

// A bone slot's joints on `owner`'s model (named in STR# 0x90 of `group`), none for other slots.
std::vector<std::optional<ModelBone>> slotJoints(const World& world, CodeSource& code, const SimObject& owner, const slot::Entry& entry,
                                                 std::uint32_t group);

// An object the planner may offer a Sim as a seat, with what its seat goals read.
struct SeatSlot {
    slot::Entry entry;
    std::vector<std::optional<ModelBone>> joints;  // a bone slot's joints on the seat's model
};
struct Seat {
    const SimObject* object = nullptr;
    bool hasSit = false;    // its OBJf entry 0x1A (Sit) has a tree
    bool hasStand = false;  // its OBJf entry 0x1B (Stand) has a tree
    std::vector<SeatSlot> slots;  // its routing slots, in order
};

// How a goal spot is kept clear of objects:
// - Square: the Sim's 0.2-tile square, snapped as footprints are, overlaps no footprint (seat and
//   bone-slot goals, and sweeps no wider than a quarter tile).
// - Clearance: over 0.3 tiles from every footprint (wider sweeps), or Square with data 0x28 bit 0x4000.
// - Point: no padded footprint covers the spot (placing objects).
enum class GoalFit { Square, Clearance, Point };

// What the goal score reads from around the slot's owner.
struct SlotGoalWorld {
    const WallTiles* walls = nullptr;
    // Whether a door stands on tile (x, y) of `level` (object coordinates, 0 = the ground storey).
    std::function<bool(int level, int x, int y)> doorOnTile;
    // The route grid's floor cost under (x, y) on `level` (object coordinates): 1 indoors, more on the
    // lot's bare ground. Swept goals lose 10 for each 1 above 1.
    std::function<float(int level, float x, float y)> floorCost;
    // Whether a Sim can stand at (x, y) on `level` (object coordinates): floor there and nothing but
    // `owner` (the slot's owner or the seat, which the game keeps from blocking its own goals, but not a
    // multi-tile object's other parts) in the way, as `fit` tests it. A goal where the Sim can't stand
    // is dropped.
    std::function<bool(int level, float x, float y, std::int16_t owner, GoalFit fit)> canStand;
    // Whether there is floor at (x, y) on `level`: a goal where a Sim can't stand counts as one with
    // something in the way only where there is.
    std::function<bool(int level, float x, float y)> hasFloor;
    // Whether another Sim (not `ownerSim`, a bone slot's owner) stands within 0.283 tiles of (x, y) on
    // `level` (object coordinates). The game keeps such a goal, 30 lower, so free spots win.
    std::function<bool(int level, float x, float y, std::int16_t ownerSim)> simNear;
    // The objects on tile (x, y) of `level` that may be seats (those without object data 8 flag 0x04 or
    // 0x10), for slots that prefer seats.
    std::function<std::vector<Seat>(int level, int x, int y)> seatsOnTile;
    // Set when the goals are spots for placing an object (Find Location For): the game then skips the
    // wall, near-wall, door and height tests, leaving only canStand.
    bool placing = false;
};

// Why a slot's goals were left out, for the route's failure reason when none is left: something in
// the way of a Sim there (7, destination occupied), or a wall across the way or within 0.3 tiles of
// it (9, wall in the way). The game also counts a goal in another room or at another height. Those
// tests are not ported.
struct GoalMisses {
    bool object = false;
    bool wall = false;
};
// The route result (person data 0x3E) for a goal builder that found no goal.
std::int16_t noGoalReason(const GoalMisses& misses);

// What a route grid says of a spot for SlotGoalWorld. canStandAt: whether a Sim can stand at (x, y),
// with floor there, no route item of type 2 or more there other than an object's, and no object's
// footprint in the way as `fit` tests it. `owner`'s items (its lead's id, as items are filed) and
// dynamic ones `intersects` lets through don't count. floorCostAt: the floor's cost there (1 where
// there is none to read).
bool canStandAt(const RouteGrid& grid, float x, float y, std::uint32_t owner, const RouteIntersects& intersects = {},
                GoalFit fit = GoalFit::Point);
float floorCostAt(const RouteGrid& grid, float x, float y);
bool floorAt(const RouteGrid& grid, float x, float y);
// The game's eighth (0-7) of the way (dx, dy) in object coordinates: 0 toward lower y, 2 higher x,
// 4 higher y, 6 lower x, the diagonals between.
int eighthToward(float dx, float dy);
// The objects' part of canStandAt for `fit` Square or Clearance: the first object (its lead's id, as
// items are filed) whose footprint is in the way of a Sim at (x, y), none when the way is clear.
// Square is the game's test for a Sim's place: its 0.2-tile square, snapped, against the objects'
// snapped footprints, overlapping strictly.
std::optional<std::uint32_t> objectInTheWayOfSim(const RouteGrid& grid, float x, float y, std::uint32_t owner,
                                                 const RouteIntersects& intersects, GoalFit fit);

// The game's rule for a walking Sim's step from (hereX, hereY) to (x, y), given `test`, which
// returns the first thing in the way of a place (none when it passes). The step is blocked by what
// is in its way only when the Sim's current place passes: a Sim already in a bad place takes every
// step, even into something else. 0 when the step is taken.
template <class Test>
std::int16_t blockedStep(const Test& test, float x, float y, float hereX, float hereY) {
    const std::optional<std::int16_t> blocker = test(x, y);
    return blocker && !test(hereX, hereY) ? *blocker : std::int16_t{0};
}

// Where an object is: its place, or for an object in a container's slot (a Sim on a sofa, a glass on
// a tray on a bar) the spot that slot holds it at, as Snap places it, through containers in
// containers. The height counts up from its floor, a bone slot's from its joint in the container's
// model and another slot's from its entry.
struct HeldPlace {
    std::int32_t x = 0, y = 0;  // 16.16 tiles
    float height = 0;           // tiles
};
HeldPlace heldPlace(const World& world, CodeSource& code, const SimObject& object);

// Where something put in slot `slot` of `container` goes (see heldPlace).
HeldPlace slotPlace(const World& world, CodeSource& code, const SimObject& container, std::int16_t slot);

// Where the game measures goals around `owner` from (see heldPlace). The rest of `owner` is kept.
SimObject slotGoalBase(const World& world, CodeSource& code, const SimObject& owner);

// The goals of bone slot `slot` of `owner`, in route coordinates: one per joint the model has
// (`joints` in slotJointNames order), where Snap would put the Sim. Score: 100, + 25 unless toward a
// walled side of the owner's tile, + 20 at the Sim's spot, - 10 near a wall, - 5 with a door on the
// tile, - 30 with another Sim there, and a random -10 to 10 when the slot asks. Goals behind or right
// against a wall, or where the Sim can't stand, are left out and noted in `misses`.
std::vector<RouteGoal> boneSlotGoals(const SimObject& owner, const slot::Entry& slot, std::span<const std::optional<ModelBone>> joints,
                                     const SimObject& sim, const SlotGoalWorld& world, RouteRandom& random,
                                     GoalMisses* misses = nullptr);

// Whether the planner takes `slot` as one point: no facing allowed and not a bone slot.
bool isPointSlot(const slot::Entry& slot);

// A point slot's goal: the slot's spot by `owner` (where Snap would put it), scored 100, when the
// straight test passes there. Otherwise none, noted in `misses`.
std::vector<RouteGoal> pointSlotGoal(const SimObject& owner, const slot::Entry& slot, const SlotGoalWorld& world,
                                     GoalMisses* misses = nullptr);

// The facing the planner gives a goal at (x, y) of a slot that isn't a bone slot, from the slot's
// facing: -1 away from `owner` (the owner's own facing on its spot), -2 toward it, 0-7 turned with
// the owner, -3 none (any facing).
std::optional<int> slotGoalFacing(const SimObject& owner, const slot::Entry& slot, float x, float y);

// The goals of a routing slot that isn't a bone slot (an object's own, or a global slot used with
// `owner`), in route coordinates, in two groups: standing spots (weight 10 x the larger of the slot's
// first and third numbers, 10 when all three are 0) and seats (weight 10 x its second number). The
// heavier group goes first (seats on a tie), trying at most 100 spots and keeping 48, and the other
// then shares 16 more. Over the limit every seat goal is kept and a random sample of the rest.
// Standing: a sweep in each direction the slot allows (turned with the owner unless absolute), from
// the optimal distance outward by the step within min and max, and sideways within 22.5 degrees.
// Score: 100 - 16 x weight x distance from optimal - 0.001 x weight x sideways + standing weight,
// + 25 unless the last direction swept is a walled side of the owner's tile, + 20 at the Sim's spot,
// a random -10 to 10 when the slot asks, and boneSlotGoals' wall, door and Sim terms.
// Seats: the same sweep in whole tiles with a tile of slack offers the objects on each tile. A seat
// needs a Sit tree, faces back along the sweep, has object data 0x6A (for sale) at 0, is not disabled
// (data 0x57 above 0 or data 0x52 flag 0x04), and holds nobody else in container slot 0 (else
// `seatTaken` is set). With a Stand tree, its first bone slot of two or more joints gives a goal per
// joint in the direction's 22.5 degree wedge, listed twice as the game does. Each scores as a bone slot
// goal from the seat (always + 25, + 20 when the Sim sits there) + seat weight - the straight test's
// costs from the owner - 16 x weight x distance from optimal - 0.01 x weight x sideways, and carries
// the seat when seats outweigh standing.
std::vector<RouteGoal> sweptSlotGoals(const SimObject& owner, const slot::Entry& slot, const SimObject& sim, const SlotGoalWorld& world,
                                      RouteRandom& random, bool* seatTaken = nullptr);

} // namespace ts2::sim
