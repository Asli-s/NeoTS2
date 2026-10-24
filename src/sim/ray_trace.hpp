#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace ts2::sim {

// Where on an object a RayTrace starts or ends: 0 three quarters up the middle of its model, 1 a
// targeting slot, 2 a routing slot, 3 a containment slot (`slot` its number).
struct RayPoint {
    std::int16_t object = 0;
    std::uint8_t kind = 0;
    std::uint8_t slot = 0;
};

// A ray from one object to another, or (no `to`) 128 tiles straight ahead of the start.
struct RayRequest {
    RayPoint from;
    std::optional<RayPoint> to;
};

// What the ray met, at `t` along it (0 at the start, 1 at the end).
struct RayHit {
    enum class Kind : std::uint8_t { Object, Floor, Wall };
    Kind kind = Kind::Object;
    std::int16_t object = 0;  // for objects
    float t = 0;
};

// The scene's answer: every hit along the ray, nearest first, `slotMissing` when a point names a
// slot its object doesn't have, `objectMissing` when a point's object isn't drawn. No answer
// without a scene to trace in.
struct RayAnswer {
    std::vector<RayHit> hits;
    bool slotMissing = false;
    bool objectMissing = false;
};
using RayQuery = std::function<std::optional<RayAnswer>(const RayRequest&)>;

// What the game's rules need to know about the objects along a ray.
struct RayRules {
    std::int16_t source = 0;
    std::int16_t target = 0;  // 0 without one
    bool sourceIsSlot = false;
    bool targetIsSlot = false;
    bool windowsIgnored = false;
    std::function<std::int16_t(std::int16_t)> containerOf;  // 0 when not held
    std::function<bool(std::int16_t)> isWindow;
};

struct RayResult {
    bool clear = false;
    std::int16_t hit = 0;  // temp 0: the object that ended it, -1 for a wall or floor, 0 for nothing
};

// Walks the hits as the game's RayTrace does, casting again from just past each one it lets through.
RayResult judgeRay(const std::vector<RayHit>& hits, const RayRules& rules);

} // namespace ts2::sim
