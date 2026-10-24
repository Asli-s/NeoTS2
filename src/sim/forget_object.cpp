#include "sim/forget_object.hpp"

#include <algorithm>

namespace ts2::sim {

bool forgetObject(SimObject& sim, std::int16_t removed) {
    constexpr std::size_t kLockoutField = 0x19, kUseCountField = 0x3E;
    constexpr std::size_t kObjectRefFields[] = {0x5A, 0x5F};
    constexpr std::size_t kCurrentInteractionObject = 0x50, kSubQueueInteractionObject = 0x9D;
    for (const std::size_t field : kObjectRefFields) {
        if (sim.data[field] == removed) {
            sim.data[field] = 0;
        }
    }
    sim.data[kLockoutField] = 0;
    sim.data[kUseCountField] = 0;
    const bool runsOnIt = std::any_of(sim.thread.frames.begin(), sim.thread.frames.end(),
                                      [&](const Frame& f) { return f.runsInteraction && f.runningOnId == removed; });
    if (runsOnIt || sim.personData[kCurrentInteractionObject] == removed || sim.personData[kSubQueueInteractionObject] == removed) {
        return true;
    }
    std::erase_if(sim.queue, [&](const QueuedInteraction& q) {
        const std::int16_t icon = q.iconObject != 0 ? q.iconObject : q.partner != 0 ? q.partner : q.object;
        return q.object == removed || icon == removed;
    });
    return false;
}

} // namespace ts2::sim
