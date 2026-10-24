#include "sim/ray_trace.hpp"

namespace ts2::sim {

namespace {

constexpr double kStartSkip = 1e-5;    // hits this close to where a cast starts may be the start itself
constexpr double kEndReach = 0.99999;  // a hit this close to the end is the target
constexpr double kStepPast = 0.001;    // each new cast starts this far past the last hit

} // namespace

RayResult judgeRay(const std::vector<RayHit>& hits, const RayRules& rules) {
    auto container = [&](std::int16_t id) { return rules.containerOf ? rules.containerOf(id) : std::int16_t{0}; };
    const std::int16_t sourceHolder = container(rules.source);
    const std::int16_t targetHolder = rules.target != 0 ? container(rules.target) : 0;
    auto isTarget = [&](std::int16_t id) { return rules.target != 0 && (id == rules.target || (targetHolder != 0 && id == targetHolder)); };
    auto isSource = [&](std::int16_t id) { return id == rules.source || (sourceHolder != 0 && id == sourceHolder); };

    std::int16_t allowance = 1;  // walls the ray may still pass
    bool crossedWall = false;
    std::int16_t lastWindow = 0;
    double from = 0;  // where the current cast starts, along the whole ray
    std::size_t next = 0;
    while (true) {
        while (next < hits.size() && hits[next].t < from) {
            ++next;
        }
        if (next >= hits.size() || from >= 1) {
            break;
        }
        const RayHit& hit = hits[next];
        const double t = (hit.t - from) / (1 - from);  // along the current cast
        from += (t + kStepPast) * (1 - from);
        if (hit.kind == RayHit::Kind::Floor) {
            return {false, -1};
        }
        if (hit.kind == RayHit::Kind::Wall) {
            if (allowance <= 0) {
                return {false, -1};
            }
            --allowance;
            crossedWall = true;
            continue;
        }
        const std::int16_t id = hit.object;
        if (rules.sourceIsSlot) {
            const std::int16_t holder = container(id);
            if (isSource(id) || (targetHolder != 0 && id == targetHolder) ||
                (holder != 0 && (holder == rules.source || holder == rules.target))) {
                continue;
            }
        }
        if (t < kStartSkip && isSource(id)) {
            continue;
        }
        if (isTarget(id) && (!rules.targetIsSlot || t > kEndReach)) {
            return {true, id};
        }
        if (rules.windowsIgnored && rules.isWindow && rules.isWindow(id)) {
            if (id == lastWindow) {
                return {false, id};
            }
            lastWindow = id;
            allowance = static_cast<std::int16_t>(allowance + 2);
            continue;
        }
        return {false, id};
    }
    return crossedWall ? RayResult{false, -1} : RayResult{true, 0};
}

} // namespace ts2::sim
