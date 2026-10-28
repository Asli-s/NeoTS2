#include "sim/pie_menu.hpp"

#include "sim/vm_execution.hpp"

#include <algorithm>

namespace ts2::sim {

namespace {

// Interaction flags.
constexpr std::uint32_t kVisitorsMay = 0x1;
constexpr std::uint32_t kDebugOnly = 0x80;
// The line names its own queue icon (an object type and picture).
constexpr std::uint32_t kOwnIcon = 0x00400000;  // a flag bit, leakcheck: ok
constexpr std::uint32_t kNested = 0x8000;
constexpr std::uint32_t kRunsAtOnce = 0x4;
constexpr std::uint32_t kHiddenByParentalControl = 0x20000;
// The flags naming who may use an interaction (see whoFlag).
constexpr std::uint32_t kWhoFlags = 0x3F013E51;

// Person data.
constexpr std::size_t kPersonType = 0x20;  // non-zero for visitors
constexpr std::size_t kAge = 0x3A;
constexpr std::size_t kYoungAdult = 0xAF;
constexpr std::size_t kSpecies = 0xBA;

// The Sim's object data the check tree answers in.
constexpr std::size_t kHideInteraction = 0x32;
constexpr std::size_t kStopBaseInteraction = 0x06;
constexpr std::size_t kMenuData65 = 0x65;
constexpr std::size_t kMenuOrder = 0x68;

// The object's object data that turn its menu off.
constexpr std::size_t kFlags = 0x08;
constexpr std::int16_t kBurning = 0x200;
constexpr std::size_t kPlacementFlags2 = 0x52;
constexpr std::int16_t kPlacementNoInteractions = 0x4;
constexpr std::size_t kDisableAllInteractions = 0x57;

constexpr std::size_t kParentalControlGlobal = 0x23;

// A Sim may queue a new interaction while fewer than this many wait, or one of them is less urgent
// than a menu choice.
constexpr std::size_t kMaxQueued = 7;
constexpr std::int16_t kMenuPriority = 50;

// The interaction flag for what `sim` is: its age and species.
std::uint32_t whoFlag(const SimObject& sim) {
    const auto& pd = sim.personData;
    const std::int16_t age = pd[kAge];
    switch (pd[kSpecies]) {
    case 0:
        if (age > 50) return 0x400;
        if (age > 18 || pd[kYoungAdult] != 0) return 0x40;
        if (age > 15) return 0x800;
        if (age > 2) return 0x10;
        if (age == 2) return 0x200;
        return 0x10000;
    case 1:
        return age > 50 ? 0x4000000 : age > 18 ? 0x1000 : 0x1000000;
    case 2:
        return age > 50 ? 0x20000000 : age > 18 ? 0x10000000 : 0x1000000;
    case 3:
        return age > 50 ? 0x8000000 : age > 18 ? 0x2000 : 0x2000000;
    default:
        return 0;
    }
}

bool objectOffersNothing(const SimObject& object) {
    return (object.data[kFlags] & kBurning) != 0 || object.data[kDisableAllInteractions] > 0 ||
           (object.data[kPlacementFlags2] & kPlacementNoInteractions) != 0;
}

bool queueHasRoom(const SimObject& sim) {
    return sim.queue.size() < kMaxQueued ||
           std::any_of(sim.queue.begin(), sim.queue.end(), [](const QueuedInteraction& q) { return q.priority < kMenuPriority; });
}

// The checks before the check tree runs.
bool mayOffer(const ttab::Interaction& line, const SimObject& sim, const Vm& vm, const PieMenuOptions& options) {
    if ((whoFlag(sim) & line.flags & kWhoFlags) == 0) {
        return false;
    }
    if (sim.personData[kPersonType] != 0 && (line.flags & kVisitorsMay) == 0) {
        return false;
    }
    if (vm.globals[kParentalControlGlobal] != 0 && (line.flags & kHiddenByParentalControl) != 0) {
        return false;
    }
    return ((line.flags & kDebugOnly) != 0) == options.debug;
}

// The text of an interaction table line: from the table's pie-menu strings, else the name of its
// action tree, else "?" and the tree's number.
std::string lineName(CodeSource& code, std::uint32_t group, std::uint16_t table, const ttab::Interaction& line, std::uint32_t codeGroup) {
    if (auto text = code.interactionString(group, table, line.stringIndex); text && !text->empty()) {
        return *text;
    }
    const std::uint32_t treeGroup = line.action < 0x1000 ? kGlobalGroup : line.action < 0x2000 ? codeGroup : code.semiGlobalGroup(codeGroup);
    if (const auto tree = code.behavior(treeGroup, line.action); tree && !tree->name.empty()) {
        return tree->name;
    }
    return "?" + std::to_string(line.action);
}

} // namespace

ObjectInteractions interactionsOf(CodeSource& code, const SimObject& object) {
    std::uint16_t tableId = object.definition.field(objd::field::kInteractionTableId);
    if (tableId == 0xFFFF) {
        const ObjectType* type = code.objectType(object.definition.guid());
        const ObjectType* master = type ? code.multiTileMaster(*type) : nullptr;
        tableId = master ? master->definition.field(objd::field::kInteractionTableId) : 0;
    }
    // Ids with the top bit set name no table (0xFFFF only redirects to the master once).
    if (tableId == 0 || tableId >= 0x8000) {
        return {};
    }
    if (auto table = code.interactionTable(object.group, tableId)) {
        return {std::move(table), object.group};
    }
    const std::uint32_t semiGlobal = code.semiGlobalGroup(object.group);
    return {code.interactionTable(semiGlobal, tableId), semiGlobal};
}

std::optional<QueuedInteraction> queuedChoice(CodeSource& code, const World& world, const SimObject& object,
                                              const PieChoice& choice) {
    const auto found = interactionsOf(code, object);
    const ttab::Interaction* line = found.table ? ttab::find(*found.table, choice.interaction) : nullptr;
    if (line == nullptr) {
        return std::nullopt;
    }
    const SimObject* part = world.find(world.interactionPart(object));
    if (part == nullptr) {
        part = &object;
    }
    QueuedInteraction queued;
    queued.object = part->id;
    queued.group = part->group;
    queued.index = choice.interaction;
    queued.action = line->action;
    queued.priority = kMenuPriority;
    queued.args = choice.params;
    queued.source = QueuedInteraction::Source::Player;
    queued.name = choice.name;
    // Line flag 0x8000 shows it nested in the queue.
    queued.nested = (line->flags & 0x8000) != 0;
    queued.iconObject = choice.icon.object;
    queued.iconGuid = choice.icon.guid;
    queued.iconIndex = choice.icon.index;
    return queued;
}

bool runsAtOnce(CodeSource& code, const SimObject& object, const PieChoice& choice) {
    const auto found = interactionsOf(code, object);
    const ttab::Interaction* line = found.table ? ttab::find(*found.table, choice.interaction) : nullptr;
    return line != nullptr && (line->flags & kRunsAtOnce) != 0;
}

RunResult runAtOnce(Vm& vm, CodeSource& code, World& world, SimObject& sim, SimObject& object, const PieChoice& choice) {
    const auto menu = pieMenu(vm, code, sim, object);
    const bool offered = std::any_of(menu.choices.begin(), menu.choices.end(), [&](const PieChoice& c) {
        return c.interaction == choice.interaction && c.params == choice.params && c.disabled == OfferedAction::Disabled::No;
    });
    const auto queued = offered ? queuedChoice(code, world, object, choice) : std::nullopt;
    SimObject* part = queued ? world.find(queued->object) : nullptr;
    if (part == nullptr) {
        return {Outcome::False};
    }
    const std::int16_t priority = sim.personData[vm_detail::kPriorityPersonData];
    const std::int16_t autonomous = sim.personData[vm_detail::kInteractionAutonomousPersonData];
    const std::optional<QueuedInteraction> running = sim.running;
    sim.personData[vm_detail::kPriorityPersonData] = queued->priority;
    sim.personData[vm_detail::kInteractionAutonomousPersonData] = 0;
    if (sim.running) {
        sim.running->autonomous = false;
    }
    const RunResult r = vm.runEvent(sim, queued->group, queued->action, part->id, queued->args, true);
    sim.personData[vm_detail::kPriorityPersonData] = priority;
    sim.personData[vm_detail::kInteractionAutonomousPersonData] = autonomous;
    // The tree can end the running interaction or start another, which keeps its own flag.
    if (running && sim.running && sim.running->object == running->object && sim.running->group == running->group &&
        sim.running->index == running->index && sim.running->action == running->action) {
        sim.running->autonomous = running->autonomous;
    }
    return r;
}

std::string interactionName(CodeSource& code, const SimObject& object, std::uint16_t index) {
    const auto [table, tableGroup] = interactionsOf(code, object);
    const ttab::Interaction* line = table ? ttab::find(*table, index) : nullptr;
    if (line == nullptr) {
        return "?" + std::to_string(index);
    }
    return ((line->flags & kDebugOnly) ? "*" : "") + lineName(code, tableGroup, object.definition.field(objd::field::kInteractionTableId), *line, object.group);
}

PieMenu pieMenu(Vm& vm, CodeSource& code, SimObject& sim, SimObject& object, const PieMenuOptions& options) {
    PieMenu menu;
    const auto [table, tableGroup] = interactionsOf(code, object);
    if (table == nullptr || objectOffersNothing(object)) {
        return menu;
    }
    const auto tableId = object.definition.field(objd::field::kInteractionTableId);

    // Each check tree starts with the temps the one before left. The Sim gets its own back at the end.
    const auto temps = sim.thread.temps;
    for (std::size_t i = 0; i < table->interactions.size(); ++i) {
        const ttab::Interaction& line = table->interactions[i];
        // The Sim's running interaction isn't known yet, so nested interactions are never offered.
        // Ids above 0x7FFF can't be named by scripts or the queue.
        if ((line.flags & kNested) != 0 || line.stringIndex > 0x7FFF || !queueHasRoom(sim)) {
            continue;
        }
        sim.data[kHideInteraction] = 0;
        sim.data[kStopBaseInteraction] = 0;
        sim.data[kMenuData65] = 0;
        sim.data[kMenuOrder] = 0;
        if (!mayOffer(line, sim, vm, options)) {
            continue;
        }
        std::vector<OfferedAction> offered;
        bool passed = true;
        // The line's queue icon: the object, or the object type it names with flag 0x400000, as
        // its check tree leaves it (a line the tree offers keeps it as it was then).
        InteractionIcon icon;
        if (line.flags & kOwnIcon) {
            icon.guid = line.iconGuid;
            icon.index = line.iconIndex;
        }
        if (line.test != 0) {
            const RunResult r = vm.runCheck(sim, object.group, line.test, object.id, std::array<std::int16_t, 4>{}, line, offered, &icon);
            if (r.outcome != Outcome::True && r.outcome != Outcome::False) {
                menu.problems.push_back("line " + std::to_string(i) + ": " + r.reason + " " + r.detail);
            }
            passed = r.outcome == Outcome::True;
        }
        if (options.allMenus) {
            passed = true;
            sim.data[kHideInteraction] = 0;
        }
        for (auto& o : offered) {
            menu.choices.push_back(PieChoice{std::move(o.name), static_cast<std::uint16_t>(line.stringIndex), o.params, o.disabled, 0, o.icon});
        }
        if (offered.empty() && passed && sim.data[kHideInteraction] == 0) {
            const std::string name = lineName(code, tableGroup, tableId, line, object.group);
            menu.choices.push_back(PieChoice{((line.flags & kDebugOnly) ? "*" : "") + name, static_cast<std::uint16_t>(line.stringIndex), {}, OfferedAction::Disabled::No,
                                             sim.data[kMenuOrder], icon});
        }
        if (options.firstOnly && !menu.choices.empty()) {
            break;
        }
    }
    sim.thread.temps = temps;
    std::stable_sort(menu.choices.begin(), menu.choices.end(), [](const PieChoice& a, const PieChoice& b) { return a.order > b.order; });
    return menu;
}

} // namespace ts2::sim
