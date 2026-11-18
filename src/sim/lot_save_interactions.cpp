#include "sim/lot_save_detail.hpp"

#include <climits>
#include <unordered_map>
#include <vector>

namespace ts2::sim {

using namespace lot_save_detail;

namespace {

// Interaction table flag 0x8000: the queue shows the interaction in a column under its first icon.
constexpr std::uint16_t kNestedLineFlag = 0x8000;

// An interaction a Sim holds, as the record keeps it. `was` is the interaction saved in its place,
// whose other fields are kept, since nothing in the running lot says what they should be.
xobj::SavedInteraction savedInteraction(const xobj::SavedInteraction& was, const QueuedInteraction& queued,
                                        const SimObject& sim) {
    xobj::SavedInteraction out = was;
    out.actor = sim.id;
    out.target = queued.object;
    out.index = queued.ownTree ? -1 : static_cast<std::int32_t>(queued.index);
    out.tree = queued.action;
    out.priority = queued.priority;
    for (std::size_t i = 0; i < queued.args.size(); ++i) {
        out.values[i] = static_cast<std::uint16_t>(queued.args[i]);
    }
    out.name = queued.name;
    out.iconObject = queued.noIconObject ? std::int16_t{-1} : queued.icon();
    out.iconGuid = static_cast<std::int32_t>(queued.iconGuid);
    out.iconIndex = static_cast<std::int32_t>(queued.iconIndex);
    // The nested flag is the only one of the three the engine works from, so the others stay.
    if ((was.lineFlag2 != 0) != queued.nested) {
        out.lineFlag2 = queued.nested ? kNestedLineFlag : 0;
    }
    out.id = static_cast<std::int16_t>(queued.linkId);
    out.linkId = static_cast<std::int16_t>(queued.linkedId);
    // Flags that follow what happened since the lot loaded: its last test's (with the value it
    // left), 0x100 once the player cancels it, 0x10 once its picture object is deleted, and 0x80
    // off once its name is shown, as the game does for a loaded one too.
    constexpr std::int32_t kTestFlags = 0x201C, kCancelled = 0x100, kNameNotShown = 0x80;
    if (queued.nameShown) {
        out.flags &= ~kNameNotShown;
    }
    if (queued.testFlags) {
        out.flags = (out.flags & ~kTestFlags) | *queued.testFlags;
        out.value6 = static_cast<std::uint16_t>(queued.testValue);
    }
    if (queued.cancelled) {
        out.flags |= kCancelled;
    }
    constexpr std::int32_t kLostIcon = 0x10;
    if (queued.lostIcon) {
        out.flags |= kLostIcon;
    }
    return out;
}

// Person data 0x20: the Sim is a visitor, which picks the visitor attenuations.
constexpr std::size_t kVisitorPersonData = 0x20;

// What the game holds for an interaction it has just made, the fields the engine works from aside:
// its flags, the object whose script pushed it (65535 for none) and its line's attenuation for the
// Sim (see attenuationOf). The player's pick has flags 0x0C (tested and passed) and autonomy's own
// pick 0x0C00. A script's push takes its test's flags (see QueuedInteraction::testFlags), with 0x400
// when markedAutonomous, 0x40 when the push marks it autonomous (Push Interaction byte 3 bit 3), 2
// for a continuation and 0x4000 for holdsContinuations. Autonomy's picks and pushes also have 0x80
// until their name is shown (see QueuedInteraction::nameShown). Flag 1 is the line's flag 0x100.
xobj::SavedInteraction madeInteraction(const QueuedInteraction& now, const SimObject& sim, const AutonomyTunings& tunings) {
    constexpr std::int32_t kPicked = 0x0C, kAutonomyPick = 0x0C00, kMarked = 0x400, kNameNotShown = 0x80;
    constexpr std::int32_t kPushedAutonomous = 0x40, kContinuation = 2, kHoldsContinuations = 0x4000;
    constexpr std::uint32_t kSavedLineFlag = 0x100;
    xobj::SavedInteraction out;
    switch (now.source) {
    case QueuedInteraction::Source::Player: out.flags = kPicked; break;
    case QueuedInteraction::Source::Autonomy: out.flags = kAutonomyPick; break;
    case QueuedInteraction::Source::Script:
        out.flags = (now.markedAutonomous ? kMarked : 0) | (now.autonomous ? kPushedAutonomous : 0) |
                    (now.continuation ? kContinuation : 0) | (now.holdsContinuations ? kHoldsContinuations : 0);
        break;
    }
    if (now.source != QueuedInteraction::Source::Player && !now.nameShown) {
        out.flags |= kNameNotShown;
    }
    if (now.lineFlags & kSavedLineFlag) {
        out.flags |= 1;
    }
    out.value2 = now.pushedBy >= 0 ? static_cast<std::uint16_t>(now.pushedBy) : std::uint16_t{0xFFFF};
    ttab::Interaction line;
    line.attenuationCode = now.attenuationCode;
    line.attenuation = now.lineAttenuation;
    out.value5 = attenuationOf(line, tunings, sim.personData[kVisitorPersonData] != 0);
    return out;
}

} // namespace

namespace lot_save_detail {

xobj::SavedInteraction blankInteraction(std::uint16_t id) {
    xobj::SavedInteraction blank;
    blank.iconObject = -1;
    blank.index = -1;
    constexpr std::int32_t kBlankFlags = 16;
    blank.flags = kBlankFlags;
    blank.id = static_cast<std::int16_t>(id);
    blank.linkId = static_cast<std::int16_t>(id);
    return blank;
}

void saveQueue(xobj::PersonState& state, const SimObject& sim, SaveCounters& counters, const AutonomyTunings& tunings) {
    const auto freshId = [&] {
        if (++counters.lastLinkId == 0) {
            ++counters.lastLinkId;
        }
        return counters.lastLinkId;
    };
    std::unordered_map<std::int16_t, xobj::SavedInteraction> byId;
    const auto note = [&](const xobj::SavedInteraction& i) {
        if (!xobj::isBlank(i) && i.id != 0) {
            byId.emplace(i.id, i);
        }
    };
    note(state.running);
    note(state.interrupted);
    for (const auto& i : state.queue) {
        note(i);
    }
    if (state.runningSub) {
        note(*state.runningSub);
    }
    for (const auto& i : state.subQueue) {
        note(i);
    }
    const auto written = [&](const QueuedInteraction& now) {
        // Only one the lot gave back is the saved one. A new one can draw an id an interaction that
        // has ended since held, once the counter wraps.
        const auto was = now.savedId && *now.savedId != 0 ? byId.find(static_cast<std::int16_t>(*now.savedId)) : byId.end();
        if (was != byId.end()) {
            return savedInteraction(was->second, now, sim);
        }
        xobj::SavedInteraction out = savedInteraction(madeInteraction(now, sim, tunings), now, sim);
        // A broken file can hold the highest number there is, and the count starts again at 1.
        counters.lastNumber = counters.lastNumber < INT32_MAX ? counters.lastNumber + 1 : 1;
        out.number = counters.lastNumber;
        return out;
    };
    const bool stillRunning = sim.running && !xobj::isBlank(state.running) && sim.running->linkId != 0 &&
                              static_cast<std::int16_t>(sim.running->linkId) == state.running.id;
    if (!stillRunning && !xobj::isBlank(state.interrupted)) {
        state.interrupted = blankInteraction(freshId());
    }
    // A Sim with nothing to do keeps the blank interaction it holds, with the id it was given when
    // it was made.
    if (sim.running) {
        state.running = written(*sim.running);
    } else if (sim.blankId != 0) {
        state.running = blankInteraction(sim.blankId);
    } else {
        // It has ended an interaction since, and the game's new blank has an id of its own.
        state.running = blankInteraction(freshId());
    }
    if (!sim.runningSub) {
        state.runningSub.reset();
    } else {
        state.runningSub = written(*sim.runningSub);
    }
    // Nested ones wait in a queue of their own, as the game keeps them.
    std::vector<xobj::SavedInteraction> queue, subQueue;
    for (const QueuedInteraction& now : sim.queue) {
        (now.nested ? subQueue : queue).push_back(written(now));
    }
    state.queue = std::move(queue);
    state.subQueue = std::move(subQueue);
    // The motive changes Set Motive Change put on the Sim: the motive, its change per tick and its limit.
    state.entries.clear();
    for (const MotiveChange& change : sim.motiveChanges) {
        state.entries.push_back({change.motive, change.perTick, change.limit});
    }
}

} // namespace lot_save_detail

} // namespace ts2::sim
