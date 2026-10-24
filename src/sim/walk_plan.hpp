#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ts2::sim {

// How the game plans a Sim's walk along a path, and where the Sim is along it at a moment.
//
// A walk is a start clip, a walk loop and a stop clip laid along the path by distance. Each part
// mixes a short and a long clip so that its travel fits the distance it covers, and each clip's
// time follows the distance walked, not the clock, so the feet never slide. The Sim moves along
// the path by a speed curve: the start clip's own speed, a ramp up to the walk's speed, a hold, a
// ramp down, and the stop clip's own speed. Distances are in tiles, times in milliseconds.

// What the planner reads from a locomotion clip's header.
struct LocoClip {
    std::string name;
    float lengthMs = 0;
    float travel = 0;     // how far the clip carries the Sim over its length
    float loopPhase = 0;  // a start clip's: the walk loop's phase (0-1) as the clip ends
    float match = 0;      // the value the planner matches start, stop and loop clips by (meaning not known yet)
    float speed = 0;      // tiles per millisecond the clip is made for
    float rootTurn = 0;   // radians
};

// A walk part's two clips: the short one and the long one it is mixed with (none: the short one alone).
struct LocoPair {
    LocoClip clip;
    std::optional<LocoClip> alt;
    bool mirrored = false;
};

// One style of the walk loop (the plain walk, a second plain walk, a happy walk...) and its share.
struct LoopStyle {
    LocoPair clips;
    float share = 1;
};

struct WalkPlanInput {
    float length = 0;  // along the path
    // The start, and the stop for the foot the start leaves forward (see turnClipsForFoot) and for
    // the other: a walk loop of an odd number of half cycles ends on the other foot.
    std::optional<LocoPair> start, stop, stopOtherFoot;
    // The walk loop's styles. Their clips are mixed by share into one loop, whose travels and match
    // values are the styles' added up by share. Shares are scaled to add up to 1. When none has a
    // share, the first style plays alone.
    std::vector<LoopStyle> loops;
    float speed = 0;  // the walk's speed, tiles per millisecond (see walkSpeed)
    // The plain walk loop's fastest clip's speed, and the fastest start or stop clip's that doesn't turn.
    float topLoopSpeed = 0;
    float topStraightSpeed = 0;
    // The Sim's size against its skeleton's own (teens 0.94). The walk loop's travels are shortened
    // by it, as the game plans. The start's and stop's travels, the match values and speeds are not.
    float scale = 1;
};

// One part of the walk, played while the distance walked is in [start, end], fading in and out
// over `blend` around those ends.
struct WalkSegment {
    enum class Kind { Start, Loop, Stop, Step };
    Kind kind = Kind::Start;
    LocoPair clips;
    float start = 0, end = 0, blend = 0;
    float weight = 1;  // its share against the parts showing with it (a step's two clips)
    // The distance the mix of its clips must travel per play: the part's distance for a start or
    // stop, the loop's stride (from `strideStart` to `strideEnd` over its last stretch).
    float strideStart = 0, strideEnd = 0;
    // Loop only: where its cycles count from at its start and at its end, and where the stride
    // starts changing (none when it doesn't).
    float originStart = 0, originEnd = 0;
    std::optional<float> changeAt;
    float changeLength = 0;  // the stretch over which the stride changes (half a cycle)
    float changeSpeedFrom = 0, changeSpeedTo = 0, changeMs = 0;
    // A loop of several styles (one part each, `weight` its share): their clips' travels and match
    // values added up by share, which every style's mix is fitted by.
    std::optional<LocoPair> styles;
};

// A stretch of the speed curve: the speed goes linearly from `from` to `to` over `ms`.
struct SpeedPart {
    float from = 0, to = 0, ms = 0;
};

struct WalkPlan {
    float length = 0;
    bool keepsFacing = false;  // a step: the Sim keeps its facing as it moves
    std::vector<WalkSegment> segments;
    std::vector<SpeedPart> speed;
};

// The walk set's speeds when its clips don't say: its walk loop's, and the share of that its
// straight starts and stops take.
constexpr float kFallbackLoopSpeed = 0.00175f;
constexpr float kFallbackStraightShare = 0.65f;

// A turning start's or stop's clips for a turn of `angle` radians (in 45-degree steps, positive
// towards positive headings) beginning on foot `foot` (0 or 0.5, see LocoClip::loopPhase), as the
// game's picker chooses among the walk set's `clips`: those turning that way that begin on that
// foot, or those turning the other way that begin on the other foot, played mirrored. The shorter
// is the short clip, the next the long one. None when no clip fits.
std::optional<LocoPair> turnClipsForFoot(std::vector<LocoClip> clips, float angle, float foot);

// The walk's speed before planning, from the walk loop's own speed: slower on paths under a tile.
float walkSpeed(float loopSpeed, float pathLength);

WalkPlan planWalk(const WalkPlanInput& in);

// A short straight move that keeps the Sim's facing is a step instead of a walk: a path shorter
// than a tile whose ends lie within 0.02 tiles of its length apart, with the facing at its end
// within 0.01 radians of the one at its start.
bool takesStep(float length, float straightLength, float turn);

// A step clip for a direction off the facing (see stepClips): its short clip with the full one,
// mirrored or not, and the direction the played clip steps in.
struct StepClip {
    LocoPair clips;
    float angle = 0;
};
using StepClipAt = std::function<std::optional<StepClip>(float angle)>;

// Plans a step of `length` tiles in the direction `angle` off the facing, as the game does: the
// step clip for that direction, mixed with the next one 45 degrees on when the direction lies
// between them, each clip's short and full lengths mixed to cover the move (with the step on the
// spot, `inPlace`, for moves shorter than the short one). `scale` is the Sim's size against its
// skeleton's own (teens 0.94), which shortens how far the clips carry it. The Sim moves at a
// constant speed. None when no step clip reaches that far (the move is walked instead).
std::optional<WalkPlan> planStep(float length, float angle, float scale, const StepClipAt& clipAt, const std::optional<LocoClip>& inPlace);

// How long the walk lasts, how far along the path it is after `ms` (up to its length), and how
// fast it moves then (tiles per millisecond).
float walkMs(const WalkPlan& plan);
float walkedAt(const WalkPlan& plan, float ms);
float speedAt(const WalkPlan& plan, float ms);

// Stops a walk soon, `ms` into it, as the game does when the walk's route is interrupted. The part
// playing goes on to the next half of the walk cycle (a start plays to its end). From there the walk
// set's straight stop clips (`straightStop`, unmirrored) play on the foot that comes down next,
// mirrored when they start on the other foot. Their short and long clips are mixed to match the walk
// loop's mix. Their travel is shortened by the Sim's `scale`, unlike a planned walk's stop.
// The Sim keeps its speed up to the stop, then slows to the stop's own speed, which is scaled
// against `topLoopSpeed` as the walk's is. Without a stop clip for that foot, the planned stop is
// dropped and the walk loop plays on to its end.
enum class StopSoon {
    Soon,       // the walk now ends as above
    AsPlanned,  // it goes on as planned: already in its stop, or a step
    Now,        // it isn't moving: it stops where it is
};
StopSoon stopSoon(WalkPlan& plan, float ms, const std::optional<LocoPair>& straightStop, float scale, float topLoopSpeed);

// A clip showing at a distance along the walk: how much, at which point of it (0-1), mirrored or not.
struct WalkPose {
    const LocoClip* clip = nullptr;
    float weight = 0;
    float phase = 0;
    bool mirrored = false;
    // A step clip: radians it plays turned off the Sim's facing, its own direction (none for the
    // step on the spot), so the body keeps facing forward.
    float turn = 0;
};
// The clips showing at `distance`, their weights summing to 1 (none before the walk starts).
void walkPoses(const WalkPlan& plan, float distance, std::vector<WalkPose>& out);

} // namespace ts2::sim
