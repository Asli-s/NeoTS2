#include "sim/vm_execution.hpp"
#include "sim/linked_interactions.hpp"

namespace ts2::sim::vm_detail {

// In a candidate's check tree the text becomes a line of its own, with the stack object as its
// param 0, temp 0 as param 1, temp 4 as param 2 (flag 0x400) and temp 5 as param 3 (flag 0x800).
// Flags 0x100 and 0x200 show it disabled.
void Execution::offerAction(const std::string& name, std::uint16_t flags, const Frame& f) {
    OfferedAction line;
    line.name = name;
    line.params[0] = f.stackObjectId;
    line.params[1] = thread_.temps[0];
    line.params[2] = (flags & 0x400) ? thread_.temps[4] : std::int16_t{0};
    line.params[3] = (flags & 0x800) ? thread_.temps[5] : std::int16_t{0};
    line.disabled = (flags & 0x100)   ? OfferedAction::Disabled::Propagating
                    : (flags & 0x200) ? OfferedAction::Disabled::NonPropagating
                                      : OfferedAction::Disabled::No;
    if (candidateIcon != nullptr) {
        line.icon = *candidateIcon;
    }
    if (testedLine != nullptr) {
        line.line = *testedLine;
    }
    offered->push_back(std::move(line));
}

// Add/Change the Action String: the name and icon the queue shows for my running interaction,
// or for the candidate a check tree checks (see Vm::runCheck). Always true.
// Byte 9 zero sets the text: entry [bytes 14-15] (byte 2 bit 4: [temp 0]), counted from 1, of string table
// 0x12E, from the code owner's group, then its semi-global group, then the global one (byte 2
// bit 2: only the global one, bit 3: only the semi-global one). A missing text is an error.
// Otherwise it sets the icon: of the object (owner byte 11, data bytes 12-13), or with byte 2
// bit 5 of the object type with GUID bytes 5-8 (bit 6: temps 2 and 3), and its index (byte 10,
// bit 7: temp 1). Byte 3 bit 4 (text) and bit 5 (icon) set them on the running sub-interaction
// when there is one. The text's tokens are not used.
bool Execution::actionString(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    SimObject& self = me();
    const auto flags = static_cast<std::uint16_t>(o[2] | o[3] << 8);
    if (o[9] == 0) {
        const auto index = static_cast<std::uint16_t>((flags & 0x10) ? thread_.temps[0] : (o[14] | o[15] << 8));
        std::vector<std::uint32_t> groups;
        if (flags & 0x04) {
            groups = {kGlobalGroup};
        } else if (flags & 0x08) {
            groups = {code_.semiGlobalGroup(f.codeOwner)};
        } else {
            groups = {f.codeOwner, code_.semiGlobalGroup(f.codeOwner), kGlobalGroup};
        }
        for (const auto group : groups) {
            const auto text = index > 0 ? code_.string(group, kActionStringTable, index - 1u) : std::nullopt;
            if (text && !text->empty()) {
                if (offered != nullptr) {
                    offerAction(*text, flags, f);
                } else if (self.isPerson() && (flags & 0x1000) && self.runningSub) {
                    self.runningSub->name = *text;
                } else if (self.isPerson() && self.running) {
                    self.actionString = *text;
                }
                return true;
            }
        }
        throw Stop{Outcome::Error, "missing action string in table", std::to_string(index)};
    }
    const std::uint32_t iconIndex = (flags & 0x80) ? static_cast<std::uint16_t>(thread_.temps[1]) : o[10];
    if (flags & 0x20) {
        const std::uint32_t guid =
            (flags & 0x40) ? (static_cast<std::uint32_t>(static_cast<std::uint16_t>(thread_.temps[2])) |
                              static_cast<std::uint32_t>(static_cast<std::uint16_t>(thread_.temps[3])) << 16)
                           : static_cast<std::uint32_t>(o[5] | o[6] << 8 | o[7] << 16) |
                                 static_cast<std::uint32_t>(o[8]) << 24;
        if (guid == 0) {
            throw Stop{Outcome::Error, "you sent me a crappy GUID, please fix it", "0"};
        }
        setActionIcon(self, flags, 0, guid, iconIndex);
        return true;
    }
    const std::int16_t id = read(o[11], static_cast<std::uint16_t>(o[12] | o[13] << 8), f);
    if (world().find(id) == nullptr) {
        throw Stop{Outcome::Error, "could not find object reference", std::to_string(id)};
    }
    setActionIcon(self, flags, id, 0, iconIndex);
    return true;
}

// Sets the icon Add/Change the Action String names (an object, or an object type and picture):
// on the candidate a check tree is checking, with picture 0 counted as 1, else on my running
// interaction, or its sub-interaction with flag 0x2000, when I am a Sim running one.
void Execution::setActionIcon(SimObject& self, std::uint16_t flags, std::int16_t object, std::uint32_t guid, std::uint32_t index) {
    if (candidateIcon != nullptr) {
        *candidateIcon = InteractionIcon{object, guid, index != 0 ? index : 1};
        return;
    }
    if (!self.isPerson()) {
        return;
    }
    if ((flags & 0x2000) && self.runningSub) {
        self.runningSub->iconObject = object;
        self.runningSub->iconGuid = guid;
        self.runningSub->iconIndex = index;
    } else if (self.running) {
        self.actionIconObject = object;
        self.actionIconGuid = guid;
        self.actionIconIndex = index;
    }
}

// Run Functional Tree: calls the stack object's tree for an object function (bytes 0-1, see
// objectFunctionEntry) from its OBJf: the action tree, or with byte 2 bit 2 the check tree, given
// my params with bit 1. The call runs on my stack with the object as stack object. It is false when the
// object has no such tree. With bit 0 the object becomes the queue icon (picture 0) of a Sim's running
// sub-interaction, else of its running interaction.
std::optional<Execution::NamedCall> Execution::functionalTree(const bhav::Instruction& in, const Frame& f) {
    const auto& o = in.operands;
    SimObject& object = stack();
    const auto entry = objectFunctionEntry(static_cast<std::uint16_t>(o[0] | o[1] << 8));
    const ObjectType* type = code_.objectType(object.definition.guid());
    const auto* functions = type ? code_.objectFunctions(*type) : nullptr;
    if (!entry || functions == nullptr || *entry >= functions->size()) {
        return std::nullopt;
    }
    const objf::Function& function = (*functions)[*entry];
    const std::uint16_t tree = (o[2] & 0x04) ? function.check : function.action;
    if (tree == 0) {
        return std::nullopt;
    }
    NamedCall call;
    call.group = groupFor(tree, object.group);
    call.instance = tree;
    call.code = code_.behavior(call.group, tree);
    if (!call.code) {
        throw Stop{Outcome::MissingCode, "BHAV not found", hex(call.group, 8) + " " + hex(tree, 4)};
    }
    call.codeOwner = object.group;
    call.stackObject = object.id;
    if (o[2] & 0x02) {
        call.args = f.params;
    }
    if ((o[2] & 0x01) && me().isPerson()) {
        SimObject& self = me();
        if (self.runningSub) {
            self.runningSub->iconObject = object.id;
            self.runningSub->iconGuid = 0;
            self.runningSub->iconIndex = 0;
        } else if (self.running) {
            self.actionIconObject = object.id;
            self.actionIconGuid = 0;
            self.actionIconIndex = 0;
        }
    }
    return call;
}

// Gosub Found Action (0x1E, Sims, operand bytes 0-1 must be 0): runs what Find Best Interaction
// picked, on my stack: the action tree of the stack object's interaction line whose id is in my
// object data 0x14, running on the stack object, with it as stack object and the pick's params
// (an offered line's, else 0). The tree's result is the instruction's. False when the stack
// object is gone or has no interaction table.
std::optional<Execution::NamedCall> Execution::foundAction(const bhav::Instruction& in) {
    const auto& o = in.operands;
    const auto mode = static_cast<std::uint16_t>(o[0] | o[1] << 8);
    if (!me().isPerson()) {
        throw Stop{Outcome::Unsupported, "Gosub Found Action on an object that is not a person", ""};
    }
    if (stackObject() == 0) {
        throw Stop{Outcome::Error, "Gosub Found Action without a stack object", ""};
    }
    if (mode != 0) {
        throw Stop{mode <= 3 ? Outcome::Unsupported : Outcome::Error, "Gosub Found Action mode", std::to_string(mode)};
    }
    SimObject* object = world().find(stackObject());
    const auto table = object != nullptr ? interactionsOf(code_, *object).table : nullptr;
    if (table == nullptr) {
        return std::nullopt;
    }
    const std::int16_t id = me().data[kTreeTableEntryField];
    if (id < 0) {
        throw Stop{Outcome::Error, "Gosub Found Action without a found action", ""};
    }
    const ttab::Interaction* line = ttab::find(*table, static_cast<std::uint32_t>(id));
    if (line == nullptr) {
        throw Stop{Outcome::Error, "reference to tree table entry that does not exist", std::to_string(id)};
    }
    NamedCall call;
    call.group = groupFor(line->action, object->group);
    call.instance = line->action;
    call.code = code_.behavior(call.group, line->action);
    if (!call.code) {
        throw Stop{Outcome::MissingCode, "BHAV not found", hex(call.group, 8) + " " + hex(line->action, 4)};
    }
    call.codeOwner = object->group;
    call.stackObject = object->id;
    call.runningOn = object->id;
    call.args.assign(me().foundParams.begin(), me().foundParams.end());
    // Autonomy's pick, with the queue icon autonomy gave it (see FoundInteraction::icon).
    QueuedInteraction found;
    found.object = object->id;
    found.group = object->group;
    found.index = static_cast<std::uint16_t>(id);
    found.action = line->action;
    found.priority = 20;
    found.autonomous = true;
    found.source = QueuedInteraction::Source::Autonomy;
    found.nested = (line->flags & kNestedInteractionFlag) != 0;
    found.iconObject = me().foundIcon.object;
    found.iconGuid = me().foundIcon.guid;
    found.iconIndex = me().foundIcon.index;
    found.name = me().foundName;
    found.args = me().foundParams;
    call.interaction = found;
    return call;
}

// Find Best Object for Function (see FunctionSearch): the best object becomes the stack object.
// False when there is none.
bool Execution::findBestObject(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    SimObject* origin = &me();
    if (o[2] & 0x08) {
        const std::int16_t id = read(o[3], static_cast<std::uint16_t>(o[4] | o[5] << 8), f);
        origin = world().find(id);
        if (origin == nullptr) {
            throw Stop{Outcome::Error, "could not find object reference", std::to_string(id)};
        }
    }
    if ((o[6] & 0x01) && world().find(thread_.temps[0]) == nullptr) {
        throw Stop{Outcome::Error, "could not find object reference", std::to_string(thread_.temps[0])};
    }
    const FunctionSearch search{world(), code_, me(), in.operands, thread_.temps, *origin,
                                [&](SimObject& candidate, std::uint16_t tree) {
                                    const RunResult r = runNow(me(), candidate.group, tree, candidate.id, {});
                                    if (r.outcome != Outcome::True && r.outcome != Outcome::False) {
                                        throw Stop{r.outcome, "function check tree: " + r.reason, r.detail};
                                    }
                                    return r.outcome == Outcome::True;
                                }};
    const auto best = findBestObjectForFunction(search);
    if (!best) {
        return false;
    }
    stackObject() = *best;
    return true;
}

// Find Best Interaction (see InteractionSearch): the pick becomes the stack object, and my object
// data 0x14 the id of its interaction. False when nothing qualifies.
bool Execution::findBestInteraction(const bhav::Instruction& in) {
    // What a check tree reports through the Sim's object data. It starts cleared.
    constexpr std::size_t kCheckDataFields[] = {0x06, 0x32, 0x65, 0x68};
    constexpr std::size_t kRejectedField = 0x06;
    // A safety limit of ours, not the game's: steps all of one search's check trees may take.
    constexpr std::size_t kMaxCheckSteps = 2'000'000;
    SimObject& sim = me();
    if (!sim.isPerson()) {
        throw Stop{Outcome::Unsupported, "Find Best Interaction on an object that is not a person", ""};
    }
    // Each check tree starts with the temps the one before left. The Sim gets its own back at the
    // end, on this thread and on its main one, which the check trees use.
    const auto temps = thread_.temps;
    struct RestoreTemps {
        std::array<std::int16_t, kTempCount>& temps;
        std::array<std::int16_t, kTempCount> saved;
        ~RestoreTemps() { temps = saved; }
    } restoreMain{sim.thread.temps, sim.thread.temps}, restoreThis{thread_.temps, thread_.temps};
    std::size_t checkSteps = 0;
    const InteractionSearch search{
        world(), code_, sim, in.operands, temps, *autonomyTunings, rng_,
        [&](SimObject& object, const ttab::Interaction& line, ttab::Interaction& changed, InteractionIcon& icon,
            std::vector<AutonomyOffer>& offers) {
            if (checkSteps >= kMaxCheckSteps) {
                diag::warnOnce("autonomy: check trees past the limit of 2000000 steps leave the remaining interactions unchecked");
                return false;
            }
            for (const auto field : kCheckDataFields) {
                sim.data[field] = 0;
            }
            const std::array<std::int16_t, 4> args{1, 0, 0, 0};
            std::vector<OfferedAction> offered;
            const RunResult r = runCheck(sim, object.group, line.test, object.id, args, line, changed, &icon, &offered);
            checkSteps += r.steps;
            for (auto& o : offered) {
                offers.push_back({std::move(o.name), o.params, o.icon, o.line.value_or(line)});
            }
            return r.outcome == Outcome::True && sim.data[kRejectedField] == 0;
        },
        [&](const SimObject& object, const ttab::Interaction& line) {
            return line.test != 0 && checkTreeKinds->changesLine(object.group, line.test);
        },
        nullptr};
    const auto found = ts2::sim::findBestInteraction(search);
    if (!found) {
        return false;
    }
    stackObject() = found->object;
    return true;
}

namespace {

// Whether the Sim's running interaction lets sub-interactions wait: its line has flag 0x4000.
bool takesSubInteractions(CodeSource& code, const World& world, const SimObject& sim) {
    constexpr std::uint32_t kTakesSubInteractionsFlag = 0x4000;
    const SimObject* object = sim.running ? world.find(sim.running->object) : nullptr;
    const auto table = object != nullptr ? interactionsOf(code, *object).table : nullptr;
    const ttab::Interaction* line = table ? ttab::find(*table, sim.running->index) : nullptr;
    return line != nullptr && (line->flags & kTakesSubInteractionsFlag) != 0;
}

// Forgets the sub-queue interaction: the running one and person data 0x4B-0x4E and 0x9D, which name
// it and the sub-queue's next one.
void dropSubQueue(SimObject& sim) {
    sim.runningSub.reset();
    for (const std::size_t field : {kSubQueueMasterObjectPersonData, kSubQueueMasterIndexPersonData, kNextSubIndexPersonData,
                                    kNextSubObjectPersonData}) {
        sim.personData[field] = 0;
    }
    sim.personData[kSubQueueObjectPersonData] = 0;
}

} // namespace

// The test the game gives a waiting interaction just before it starts: the checks of mayStartLine,
// then the line's check tree, as me with the object as stack object. The tree's params are 1 or 0
// for whether the Sim queued it as autonomy's pick, then 0, 0, 0 (the game's second one is a join
// flag, not kept yet). My object data 0x32, 0x06, 0x65 and 0x68 start at 0 and keep what the tree
// leaves. My temps are put back afterwards. A line no longer in its table isn't tested.
bool Execution::passesStartCheck(SimObject& sim, const SimObject& object, const QueuedInteraction& waiting) {
    const auto table = interactionsOf(code_, object).table;
    const ttab::Interaction* line = table ? ttab::find(*table, waiting.index) : nullptr;
    if (line == nullptr) {
        return true;
    }
    for (const std::size_t field : {0x32, 0x06, 0x65, 0x68}) {
        sim.data[field] = 0;
    }
    const bool autonomy = waiting.source == QueuedInteraction::Source::Autonomy;
    if (!mayStartLine(*line, sim, object, globals_[kParentalControlGlobal] != 0, autonomy)) {
        return false;
    }
    if (line->test == 0) {
        return true;
    }
    const auto temps = sim.thread.temps;
    const std::array<std::int16_t, 4> args{static_cast<std::int16_t>(autonomy ? 1 : 0), 0, 0, 0};
    ttab::Interaction changed;
    const RunResult check = runCheck(sim, object.group, line->test, object.id, args, *line, changed, nullptr, nullptr);
    sim.thread.temps = temps;
    if (check.outcome == Outcome::Sleeping) {
        throw Stop{Outcome::Unsupported, "interaction test tree waited", hex(line->test, 4)};
    }
    if (check.outcome != Outcome::True && check.outcome != Outcome::False) {
        throw Stop{check.outcome, "interaction test tree: " + check.reason, check.detail};
    }
    return check.outcome == Outcome::True;
}

// A waiting interaction that can't start runs its object's Queue Skipped function (OBJf entry 4) at
// once, as that object, with me as stack object and no params. A missing tree is skipped. A tree that
// fails resets the object on its next turn.
void Execution::queueSkipped(const SimObject& sim, SimObject& object) {
    constexpr std::size_t kQueueSkippedEntry = 4;
    const ObjectType* type = code_.objectType(object.definition.guid());
    const auto* functions = type ? code_.objectFunctions(*type) : nullptr;
    if (functions == nullptr || kQueueSkippedEntry >= functions->size()) {
        return;
    }
    const std::uint16_t tree = (*functions)[kQueueSkippedEntry].action;
    const std::uint32_t treeGroup = tree < 0x1000 ? kGlobalGroup : tree < 0x2000 ? type->group : code_.semiGlobalGroup(type->group);
    if (tree == 0 || code_.behavior(treeGroup, tree) == nullptr) {
        return;
    }
    const RunResult r = runOnObject(object, type->group, tree, sim.id, {});
    switch (r.outcome) {
    case Outcome::True:
    case Outcome::False:
        return;
    case Outcome::Unsupported:
    case Outcome::MissingCode:
        throw Stop{r.outcome, "queue skipped: " + r.reason, r.detail};
    default:
        object.failedRun = (r.outcome == Outcome::Sleeping ? "a queue skipped tree waited" : r.reason + " " + r.detail) + " in " +
                           hex(r.group, 8) + " " + hex(r.instance, 4) + " at " + std::to_string(r.pc);
        return;
    }
}

void Execution::dropLinked(QueuedInteraction& ended, const SimObject* skip) {
    dropLinkedInteractions(world(), ended, skip, [this](SimObject& sim, const QueuedInteraction& removed) {
        if (SimObject* object = world().find(removed.object)) {
            queueSkipped(sim, *object);
        }
    });
}

// Idle for Input (Sims only). Operands:
//   bytes 0-1  the parameter that counts down the ticks left to wait
//   bytes 2-3  non-zero: a waiting interaction may start
//   byte 4     bit 0: don't wait, just start the first waiting sub-interaction if there is one
// The game keeps sub-interactions (interaction table flag 0x8000) in a queue of their own. Here they
// wait in the one queue, marked nested, and each branch takes the first of its own kind.
// Waiting: with nothing pending (see interruptPending) the count goes down by one each tick and
// the primitive is true at 0. With something pending it sets bit 0x40 of my object data 8 and
// starts the first waiting interaction, or is true when none may start or none waits: that is
// how a cancelled interaction's loop learns to finish. A started interaction runs its action
// tree on my stack with its object as stack object, and the count becomes 0. When it ends the
// primitive runs again, so a continuation pushed meanwhile starts at once and anything else waits
// for the next call.
// Each interaction leaves the queue when its turn comes and is tested again (see passesStartCheck).
// One whose object is gone or which fails the test is dropped. Failing the test also drops the
// interactions linked with it and runs its object's Queue Skipped function. Then a sub-interaction
// is true, and an interaction waits a tick.
Execution::IdleResult Execution::idleForInput(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    SimObject& sim = me();
    if (!sim.isPerson()) {
        throw Stop{Outcome::Unsupported, "Idle for Input on an object that is not a person", ""};
    }
    const auto firstOf = [&sim](bool nested) {
        return std::find_if(sim.queue.begin(), sim.queue.end(), [nested](const QueuedInteraction& q) { return q.nested == nested; });
    };
    // Takes the first waiting interaction of its kind out of the queue and tests it.
    const auto take = [&](bool nested) -> std::optional<QueuedInteraction> {
        const auto it = firstOf(nested);
        QueuedInteraction head = *it;
        sim.queue.erase(it);
        noteNextQueued(sim);
        if (!nested) {
            dropSubQueue(sim);
        }
        SimObject* object = world().find(head.object);
        if (object == nullptr) {
            if (nested) {
                dropSubQueue(sim);
            }
            return std::nullopt;
        }
        if (!passesStartCheck(sim, *object, head)) {
            if (nested) {
                dropSubQueue(sim);
            }
            dropLinked(head, &sim);
            queueSkipped(sim, *object);
            return std::nullopt;
        }
        return head;
    };
    if (o[4] & 0x01) {
        if (firstOf(true) == sim.queue.end()) {
            return IdleResult{};
        }
        auto started = take(true);
        if (!started) {
            return IdleResult{};
        }
        IdleResult r{IdleResult::Kind::Start, std::move(*started)};
        r.sub = true;
        return r;
    }
    const auto counter = static_cast<std::int16_t>(o[0] | o[1] << 8);
    if (counter < 0 || static_cast<std::size_t>(counter) >= f.params.size()) {
        throw Stop{Outcome::Error, "stack number out of range", std::to_string(counter)};
    }
    if (!interruptPending(sim)) {
        std::int16_t& ticks = f.params[static_cast<std::size_t>(counter)];
        if (ticks == 0) {
            sim.data[kIdleFlagsField] &= static_cast<std::int16_t>(~kInteractionsWaitingBit);
            return IdleResult{};
        }
        --ticks;
        return IdleResult{IdleResult::Kind::Wait, {}};
    }
    sim.data[kIdleFlagsField] |= kInteractionsWaitingBit;
    const bool mayStart = (o[2] | o[3]) != 0;
    const auto first = firstOf(false);
    if (!mayStart || first == sim.queue.end()) {
        return IdleResult{};
    }
    // Run again after an interaction it started, it starts only a continuation, and only while
    // the Sim carries nothing. The game also checks a flag that a few social pushes set (Push
    // Interaction byte 14 bit 4) on an interaction the Sim keeps, which one is not known yet.
    if (f.idleStarted && (!first->continuation || sim.semiAttributes[kCarryFlagsSemiAttribute] != 0)) {
        return IdleResult{};
    }
    auto started = take(false);
    if (!started) {
        return IdleResult{IdleResult::Kind::Wait, {}};
    }
    f.params[static_cast<std::size_t>(counter)] = 0;
    return IdleResult{IdleResult::Kind::Start, std::move(*started)};
}

// Alter Budget. Operands:
//   byte 0       the amount's owner: 0 literal, 1 param, 2 local, else byte 1
//   bytes 2-3    the amount's data
//   byte 4       flags: 0x01 test only, 0x02 the amount is income, 0x04 times temp 2 instead of
//                bytes 7-8, 0x08 the amount is temp 2 x 10000 + temp 3
//   byte 6       the kind of transaction, 0-10 (not kept)
//   bytes 7-8    the multiplier (0 counts as 1)
// False when the family can't pay. Otherwise the funds change (unless only testing) and it is true.
bool Execution::alterBudget(const bhav::Instruction& in, Frame& f) {
    constexpr std::uint8_t kTestOnly = 0x01, kIncome = 0x02, kTimesTemp2 = 0x04, kFromTemps = 0x08;
    constexpr std::uint8_t kLastTransaction = 10;
    const auto& o = in.operands;
    const std::uint8_t owner = o[0] == 0 ? owner::kLiteral : o[0] == 1 ? owner::kParam : o[0] == 2 ? owner::kLocal : o[1];
    if (o[6] > kLastTransaction) {
        throw Stop{Outcome::Error, "bad transaction type", std::to_string(o[6])};
    }
    std::int64_t amount = 0;
    if (o[4] & kFromTemps) {
        amount = std::int64_t{thread_.temps[2]} * 10000 + thread_.temps[3];
    } else {
        std::int16_t times = (o[4] & kTimesTemp2) ? thread_.temps[2] : static_cast<std::int16_t>(o[7] | o[8] << 8);
        if (times == 0) {
            times = 1;
        }
        amount = std::int64_t{read(owner, static_cast<std::uint16_t>(o[2] | o[3] << 8), f)} * times;
    }
    if (o[4] & kIncome) {
        amount = -amount;
    }
    const std::int32_t funds = sim::funds(globals_);
    if (amount >= 0 && funds < amount) {
        return false;
    }
    if ((o[4] & kTestOnly) == 0) {
        setFunds(globals_, funds - amount);
    }
    return true;
}

// While it runs the Sim's Priority and "Interaction Autonomous" person data, and its running
// interaction's autonomous flag, are the pushed interaction's. They and the Sim's temps 0-7 are
// put back after. It runs on the Sim's own stack, so a tree that waits or fails leaves the Sim to
// be reset (game error 0x451 for waiting), not me.
void Execution::runPushedAtOnce(SimObject& sim, const SimObject& object, const QueuedInteraction& pushed) {
    constexpr std::size_t kKeptTemps = 8;
    // When I push onto myself the tree runs on the thread running now, which may be a nested one.
    const bool onMe = &sim == &me();
    ThreadState& simThread = onMe ? thread_ : sim.thread;
    struct Restore {
        SimObject& sim;
        ThreadState& thread;
        World& world;
        std::int16_t priority, autonomous;
        std::optional<std::pair<std::uint32_t, bool>> running;  // the running interaction's id and flag
        std::array<std::int16_t, kKeptTemps> temps;
        ~Restore() {
            sim.personData[kPriorityPersonData] = priority;
            sim.personData[kInteractionAutonomousPersonData] = autonomous;
            // The tree can end the running interaction or start another, which keeps its own flag.
            if (running && sim.running && sim.running->id == running->first) {
                sim.running->autonomous = running->second;
            }
            std::copy(temps.begin(), temps.end(), thread.temps.begin());
            world.setRunningAtOnce(false);
        }
    } restore{sim, simThread, world(), sim.personData[kPriorityPersonData], sim.personData[kInteractionAutonomousPersonData],
              sim.running ? std::optional(std::pair{sim.running->id, sim.running->autonomous}) : std::nullopt, {}};
    std::copy_n(simThread.temps.begin(), kKeptTemps, restore.temps.begin());
    world().setRunningAtOnce(true);
    sim.personData[kPriorityPersonData] = pushed.priority;
    sim.personData[kInteractionAutonomousPersonData] = pushed.autonomous ? 1 : 0;
    if (sim.running) {
        sim.running->autonomous = pushed.autonomous;
    }
    const RunResult r = onMe ? runOnMe(pushed.group, pushed.action, object.id, pushed.args)
                             : runOnObject(sim, pushed.group, pushed.action, object.id, pushed.args);
    switch (r.outcome) {
    case Outcome::True:
    case Outcome::False:
        return;
    case Outcome::Unsupported:
    case Outcome::MissingCode:
        throw Stop{r.outcome, "interaction run at once: " + r.reason, r.detail};
    default:
        sim.failedRun = (r.outcome == Outcome::Sleeping ? "an interaction run at once waited" : r.reason + " " + r.detail) + " in " +
                        hex(r.group, 8) + " " + hex(r.instance, 4) + " at " + std::to_string(r.pc);
        return;
    }
}

// Push Interaction: adds interaction N of an object's interaction table to the stack object's
// queue. Operands:
//   byte 0       interaction number (byte 3 bit 4: from owner byte 5, data bytes 6-7)
//   byte 1       the param (byte 3 bit 1: the local) holding the object
//   byte 2       priority: 0 my own ("Priority" person data), 1 high (100), 2 low (20), 3 medium (50)
//   byte 3       bit 0: the Sim it is with (its queue icon) is the object in local [byte 4]. With
//                no such object and no icon GUID (byte 14 bit 2) the queue leaves it out. Bit 3: it is
//                autonomous (person data 0xA6 while it runs). Bit 2 makes it a continuation,
//                which isn't held back by the queue limit. Bit 6 links it with the interaction
//                whose link number is the value of owner byte 8, data bytes 9-10. Bit 7 writes its
//                own link number to owner byte 11, data bytes 12-13 (see linked_interactions.hpp)
//   byte 14     bit 0: pass my parameters as its arguments. Bit 1: take the object and the number
//                from autonomy's choice (my person data 0x35, my object data 0x14). Bit 2: its
//                queue icon is the object type with GUID temp 5 << 16 | temp 4. Bit 3: that
//                icon's index is temp 6 instead of byte 15
// On failure my object data 0x34 says why: 1 no priority, 2 the test tree said no, 3 the queue
// is full, 4 the sub-queue is full, 6 a sub-interaction the Sim can't take now or a line with flag
// 0x4 that couldn't run at once. Byte 3 bit 5 and byte 14 bit 4 are not implemented.
bool Execution::pushInteraction(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    SimObject& self = me();
    self.data[kPrimitiveResultField] = 0;
    SimObject& sim = stack();
    if (!sim.isPerson()) {
        throw Stop{Outcome::Error, "object referred to must be a person", std::to_string(sim.id)};
    }
    const bool fromAutonomy = (o[14] & 0x02) != 0;
    std::int16_t objectId = 0;
    if (fromAutonomy) {
        objectId = personData(self, kBestActionObjectPersonData);
    } else if (o[3] & 0x02) {
        if (o[1] >= f.locals.size()) {
            throw Stop{Outcome::Error, "local out of range", std::to_string(o[1])};
        }
        objectId = f.locals[o[1]];
    } else {
        objectId = slotAt(f.params, o[1], "param");
    }
    const SimObject* object = world().find(objectId);
    if (object == nullptr) {
        throw Stop{Outcome::Error, "could not find object reference", std::to_string(objectId)};
    }
    object = world().find(world().interactionPart(*object));
    const std::int16_t index = (o[3] & 0x10) ? read(o[5], static_cast<std::uint16_t>(o[6] | o[7] << 8), f)
                               : fromAutonomy ? self.data[kTreeTableEntryField]
                                              : static_cast<std::int16_t>(o[0]);
    const auto table = interactionsOf(code_, *object).table;
    if (table == nullptr) {
        throw Stop{Outcome::Error, "tree table not found", hex(object->group, 8)};
    }
    const ttab::Interaction* found = index >= 0 ? ttab::find(*table, static_cast<std::uint32_t>(index)) : nullptr;
    if (found == nullptr) {
        throw Stop{Outcome::Error, "reference to tree table entry that does not exist", std::to_string(index)};
    }
    const ttab::Interaction& entry = *found;

    std::int16_t priority = 0;
    switch (o[2]) {
    case 0:
        priority = self.isPerson() ? self.personData[kPriorityPersonData] : 0;
        if (self.isPerson() && priority == 0) {
            self.data[kPrimitiveResultField] = 1;
            return false;
        }
        break;
    case 1: priority = 100; break;
    case 2: priority = 20; break;
    case 3: priority = 50; break;
    default:
        throw Stop{Outcome::Error, "unknown interaction priority", std::to_string(o[2])};
    }

    QueuedInteraction queued;
    queued.object = object->id;
    queued.group = object->group;
    queued.index = static_cast<std::uint16_t>(index);
    queued.action = entry.action;
    queued.priority = priority;
    queued.source = fromAutonomy ? QueuedInteraction::Source::Autonomy : QueuedInteraction::Source::Script;
    queued.autonomous = (o[3] & 0x08) != 0;
    queued.continuation = (o[3] & 0x04) != 0;
    if (o[3] & 0x01) {
        queued.partner = slotAt(f.locals, o[4], "local");
        queued.noIconObject = world().find(queued.partner) == nullptr;
    }
    if (o[14] & 0x01) {
        std::copy_n(f.params.begin(), std::min<std::size_t>(4, f.params.size()), queued.args.begin());
    }
    queued.nested = (entry.flags & kNestedInteractionFlag) != 0;
    if (o[14] & 0x04) {
        queued.iconGuid = static_cast<std::uint32_t>(static_cast<std::uint16_t>(thread_.temps[5])) << 16 |
                          static_cast<std::uint16_t>(thread_.temps[4]);
    }
    queued.iconIndex = (o[14] & 0x08) ? static_cast<std::uint16_t>(thread_.temps[6]) : o[15];

    // Tested the way autonomy tests it: on a copy of the menu line.
    const auto passesTest = [&] {
        if (entry.test == 0) {
            return true;
        }
        ttab::Interaction changed;
        const RunResult check = runCheck(sim, object->group, entry.test, object->id, queued.args, entry, changed, nullptr, nullptr);
        if (check.outcome == Outcome::Sleeping) {
            throw Stop{Outcome::Unsupported, "interaction test tree waited", hex(entry.test, 4)};
        }
        if (check.outcome != Outcome::True && check.outcome != Outcome::False) {
            throw Stop{check.outcome, "interaction test tree: " + check.reason, check.detail};
        }
        return check.outcome == Outcome::True;
    };
    if (!passesTest()) {
        self.data[kPrimitiveResultField] = 2;
        return false;
    }
    // Each queue has its own limit, which continuations and lines that run at once skip.
    const bool runsAtOnce = (entry.flags & kRunAtOnceInteractionFlag) != 0;
    if (!queued.continuation && !runsAtOnce) {
        const auto waiting = static_cast<std::size_t>(
            std::count_if(sim.queue.begin(), sim.queue.end(), [&](const QueuedInteraction& q) { return q.nested == queued.nested; }));
        if (waiting >= (queued.nested ? kMaxQueuedSubInteractions : kMaxQueuedInteractions)) {
            self.data[kPrimitiveResultField] = queued.nested ? 4 : 3;
            return false;
        }
    }
    if (!runsAtOnce && sim.queue.size() >= kMaxQueuedWithContinuations) {
        self.data[kPrimitiveResultField] = 3;
        return false;
    }
    // The links are made before the queue takes it, so a refused push still leaves them written.
    if (o[3] & 0x40) {
        linkInteraction(world(), queued, read(o[8], static_cast<std::uint16_t>(o[9] | o[10] << 8), f));
    }
    if (o[3] & 0x80) {
        write(o[11], static_cast<std::uint16_t>(o[12] | o[13] << 8), f, static_cast<std::int16_t>(ownLinkId(world(), queued)));
    }
    // A sub-interaction is taken only while the Sim runs an interaction whose line has flag 0x4000.
    if (queued.nested && !takesSubInteractions(code_, world(), sim)) {
        self.data[kPrimitiveResultField] = 6;
        return false;
    }
    if (runsAtOnce) {
        // It is tested again and runs to its end now, never queued, so it neither shows in the
        // queue nor interrupts the Sim. Only one runs at once at a time.
        if (world().runningAtOnce() || !passesTest()) {
            self.data[kPrimitiveResultField] = 6;
            return false;
        }
        runPushedAtOnce(sim, *object, queued);
        return true;
    }
    sim.queue.push_back(queued);
    sim.queue.back().id = ++sim.nextInteractionId;
    if (queued.nested) {
        noteSubQueue(sim);
    } else {
        noteNextQueued(sim);
    }
    return true;
}

} // namespace ts2::sim::vm_detail
