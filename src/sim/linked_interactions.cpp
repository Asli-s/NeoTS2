#include "sim/linked_interactions.hpp"
#include "sim/vm_execution.hpp"

#include <algorithm>

namespace ts2::sim {

using namespace vm_detail;

namespace {

// Where an interaction with link number `id` is: its Sim and the interaction itself. The running
// one is looked at first, then the sub-interaction, the queue and the sub-queue.
struct Found {
    SimObject* sim = nullptr;
    QueuedInteraction* interaction = nullptr;
    bool running = false;
};

Found findOn(SimObject& sim, std::uint16_t id) {
    const auto has = [id](const std::optional<QueuedInteraction>& q) { return q && q->linkId == id; };
    if (has(sim.running)) {
        return {&sim, &*sim.running, true};
    }
    if (has(sim.runningSub)) {
        return {&sim, &*sim.runningSub, true};
    }
    for (const bool nested : {false, true}) {
        for (auto& q : sim.queue) {
            if (q.nested == nested && q.linkId == id) {
                return {&sim, &q, false};
            }
        }
    }
    return {};
}

Found find(World& world, std::uint16_t id, const SimObject* skip) {
    const auto isPerson = [](const SimObject& o) { return o.isPerson(); };
    for (SimObject* sim = world.nextAfter(0, isPerson); sim != nullptr; sim = world.nextAfter(sim->id, isPerson)) {
        if (sim == skip) {
            continue;
        }
        if (const Found found = findOn(*sim, id); found.interaction != nullptr) {
            return found;
        }
    }
    return {};
}

} // namespace

std::uint16_t ownLinkId(World& world, QueuedInteraction& pushed) {
    if (pushed.linkId == 0) {
        pushed.linkId = world.newLinkId();
        pushed.linkedId = pushed.linkId;
    }
    return pushed.linkId;
}

void linkInteraction(World& world, QueuedInteraction& pushed, std::int16_t with) {
    const auto id = static_cast<std::uint16_t>(with);
    if (id == 0) {
        return;
    }
    const Found other = find(world, id, nullptr);
    if (other.interaction == nullptr) {
        return;
    }
    pushed.linkedId = other.interaction->linkedId;
    pushed.linkId = world.newLinkId();
    other.interaction->linkedId = pushed.linkId;
}

void dropLinkedInteractions(World& world, QueuedInteraction& ended, const SimObject* skip,
                            const std::function<void(SimObject& sim, const QueuedInteraction& removed)>& skipped) {
    std::uint16_t next = ended.linkedId;
    ended.linkedId = 0;
    if (next == ended.linkId) {
        return;
    }
    // Each step clears the link it follows, so the chain ends.
    while (next != 0) {
        const Found found = find(world, next, skip);
        if (found.interaction == nullptr) {
            return;
        }
        next = found.interaction->linkedId;
        found.interaction->linkedId = 0;
        if (found.running) {
            found.sim->personData[kLinkedDeletedPersonData] = 1;
            continue;
        }
        const QueuedInteraction removed = *found.interaction;
        if (skipped) {
            skipped(*found.sim, removed);
        }
        auto& queue = found.sim->queue;
        const auto it = std::find_if(queue.begin(), queue.end(), [&](const QueuedInteraction& q) { return q.id == removed.id; });
        if (it != queue.end()) {
            queue.erase(it);
        }
        if (removed.nested) {
            noteSubQueue(*found.sim);
        } else {
            noteNextQueued(*found.sim);
        }
    }
}

} // namespace ts2::sim
