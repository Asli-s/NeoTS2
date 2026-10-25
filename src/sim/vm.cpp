#include "sim/vm.hpp"
#include "sim/vm_execution.hpp"
#include "sim/forget_object.hpp"

namespace ts2::sim {

namespace vm_detail {

// Run Tree by Name. The tree name is an entry of string table 0x12F, counted from 1 (byte 4, plus
// 0x100 with flag 0x40). Modes (byte 5):
//   0 run in my stack, 2 push onto my stack as an action: one of the stack object's trees
//   3: one of the trees of the object type whose GUID is in temps 0 (low) and 1 (high), run
//      with the stack object (or me, without one) as its stack object
// The name comes from the global table (flag 0x01), the code owner's semi-global one (0x02) or its
// own. The tree is looked up in the object's group, then its semi-global group (unless flag 0x08),
// then the global one (unless flag 0x04).
// Mode 1 runs the tree at once on the stack object's own stack (see runOnStack).
std::optional<Execution::NamedCall> Execution::namedTree(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    const std::uint8_t mode = o[5];
    if (mode > kRunTypeTreeFromTemps) {
        throw Stop{Outcome::Unsupported, "Run Tree by Name mode " + hex(mode, 2) + " not implemented", ""};
    }
    const std::uint8_t flags = o[2];
    const std::size_t nameNumber = o[4] | ((flags & 0x40) ? 0x100 : 0);
    if (nameNumber == 0) {
        return std::nullopt;
    }
    const std::size_t nameIndex = nameNumber - 1;
    std::uint32_t treeGroup = 0;
    std::optional<std::int16_t> frameStackObject;
    if (mode == kRunTypeTreeFromTemps) {
        const std::uint32_t guid = static_cast<std::uint32_t>(static_cast<std::uint16_t>(thread_.temps[0])) |
                                   static_cast<std::uint32_t>(static_cast<std::uint16_t>(thread_.temps[1])) << 16;
        const ObjectType* type = code_.objectType(guid);
        if (type == nullptr) {
            throw Stop{Outcome::Error, "you sent me a crappy GUID, please fix it", hex(guid, 8)};
        }
        treeGroup = type->group;
        frameStackObject = world().find(stackObject()) != nullptr ? stackObject() : me().id;
    } else {
        treeGroup = stack().group;
    }
    // The names: the global table, the code owner's semi-global one (flag 0x02) or the code owner's own.
    const std::uint32_t tableGroup = (flags & 0x01) ? kGlobalGroup
                                   : (flags & 0x02) ? code_.semiGlobalGroup(f.codeOwner)
                                                    : f.codeOwner;
    auto name = code_.string(tableGroup, kTreeNameTable, nameIndex);
    if (!name || name->empty()) {
        return std::nullopt;
    }

    NamedCall call;
    call.stackObject = frameStackObject;
    const std::array<std::uint32_t, 3> groups{treeGroup, code_.semiGlobalGroup(treeGroup), kGlobalGroup};
    const std::array<bool, 3> searched{true, (flags & 0x08) == 0, (flags & 0x04) == 0};
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const std::uint32_t group = groups[i];
        if (!searched[i]) {
            continue;
        }
        if (const auto instance = code_.behaviorByName(group, *name)) {
            call.group = group;
            call.instance = *instance;
            call.code = code_.behavior(group, *instance);
            break;
        }
    }
    if (!call.code) {
        return std::nullopt;
    }
    call.codeOwner = treeGroup;

    // The new frame takes as many of these as the tree has params (the frame push copies them).
    if (flags & 0x20) {
        call.args.assign(f.params.begin(), f.params.end());
    } else if (flags & 0x10) {
        for (std::size_t i = 0; i < 3; ++i) {
            const auto data = static_cast<std::uint16_t>(o[7 + 3 * i] | o[8 + 3 * i] << 8);
            call.args[i] = read(o[6 + 3 * i], data, f);
        }
    } else {
        // Without either flag the called tree's params are the temps of the object it runs on.
        const auto& temps = mode == kRunInStackObjectsStack ? stack().thread.temps : thread_.temps;
        call.args.assign(temps.begin(), temps.end());
    }
    if (mode == kRunInStackObjectsStack) {
        call.onStackObject = true;
        call.stackObject = stackObject();
    }
    return call;
}

bool Execution::primitive(const bhav::Instruction& in, Frame& f) {
    switch (in.opcode) {
    case bhav::kExpressionOpcode: return expression(in, f);
    case kSetToNextOpcode:        return setToNext(in, f);
    case kSetMotiveChangeOpcode:  return setMotiveChange(in, f);
    case kWantSatisfyOpcode:      return wantSatisfy(in, f);
    case kOnTimerOpcode:          return onTimer(in, f);
    case kMessageOpcode:          return message(in, f);
    case kGetDirectionToOpcode:   return getDirectionTo(in, f);
    case kGetDistanceToOpcode:    return getDistanceTo(in, f);
    case kPushInteractionOpcode:  return pushInteraction(in, f);
    case kAlterBudgetOpcode:      return alterBudget(in, f);
    case kActionStringOpcode:     return actionString(in, f);
    case kManageInventoryOpcode:  return manageInventory(in, f);
    case kLuaScriptOpcode:        return luaScript(in, f);
    case kTestObjectTypeOpcode:   return testObjectType(in, f);
    case kNotifyOutOfIdleOpcode:  return notifyOutOfIdle();
    case kRandomNumberOpcode:     return randomNumber(in, f);
    case kGenericSimsCallOpcode:  return genericSimsCall(in);
    case kCreateObjectOpcode:     return createObject(in, f);
    case kRelationshipOpcode:     return relationship(in, f);
    case kRemoveObjectOpcode:     return removeObject(in);
    case kMakeNewCharacterOpcode: return makeNewCharacter(in, f);
    case kBreakPointOpcode:       return breakPoint(in, f);
    case kFindLocationForOpcode:  return findLocationFor(in, f);
    case kSnapOpcode:             return snap(in, f);
    case kSnapIntoOpcode:         return snapInto(in, f);
    case kArrayOperationOpcode:   return arrayOperation(in, f);
    case kRefreshOpcode:          return refresh(in);
    case kFindWorstMotivesOpcode: return findWorstMotives(in);
    case kTestSimInteractingWithOpcode: return testSimInteractingWith();
    case kFollowSimOpcode:        return followSim();
    case kChangeMaterialOpcode:   return changeMaterial(in, f);
    case kSoundEventOpcode:       return soundEvent(in, f);
    case kRayTraceOpcode:         return rayTrace(in, f);
    case kChangeOutfitOpcode:     return changeOutfit(in, f);
    case kFindBestObjectOpcode:   return findBestObject(in, f);
    case kFindBestInteractionOpcode: return findBestInteraction(in);
    case kChangeLightOpcode:      return changeLight(in, f);
    case kAnimateOverlayOpcode:   return animateOverlay(in, f);
    case kAnimateStopOpcode:      return animateStop(in, f);
    case kAnimateObjectOpcode:    return animateObject(in, f);
    // These start a head turn (Look At) or an effect, stop all of an object's sounds or
    // pick a Sim's walk animations (Assign Locomotion Animations), and carry on at once. What they
    // show or play isn't kept yet.
    case kLookAtOpcode:
    case kStopAllSoundsOpcode:
    case kAssignLocomotionOpcode:
    case kEffectOpcode:
        return true;
    // Debug only does something with the game's debug (cheat) setting on: console commands.
    case kDebugOpcode:
        return true;
    default:
        throw Stop{Outcome::Unsupported, "primitive not implemented", hex(in.opcode, 4)};
    }
}

// The arguments of a tree call. Byte 12 picks the form in trees of format 0x8003 and later (0xFF
// picks neither): bit 0 four (owner, 16-bit data) pairs, else bit 1 the caller's own params. A call
// saved before node version 1 only has bit 0 left (see bhav::upgradeOperands).
// Otherwise they are bytes 0-15 as eight numbers. Bytes 0-7 all 0xFF mean no arguments (zeros)
// in the first two forms and the temps in the last. Only the callee's argument count is passed.
std::array<std::int16_t, 8> Execution::callArgs(const bhav::Instruction& in, Frame& f,
                                     std::size_t calleeArgCount) {
    const auto& o = in.operands;
    const std::size_t count = std::min<std::size_t>(calleeArgCount, 8);
    const bool noArgs = std::all_of(o.begin(), o.begin() + 8, [](std::uint8_t v) { return v == 0xFF; });
    const bool flagged = f.code->format >= 0x8003 && o[12] != 0xFF;
    std::array<std::int16_t, 8> args{};
    if (flagged && (o[12] & 0x01) != 0) {
        for (std::size_t i = 0; !noArgs && i < std::min<std::size_t>(count, 4); ++i) {
            const auto data = static_cast<std::uint16_t>(o[i * 3 + 1] | o[i * 3 + 2] << 8);
            args[i] = read(o[i * 3], data, f);
        }
    } else if (flagged && (o[12] & 0x02) != 0) {
        for (std::size_t i = 0; !noArgs && i < std::min(count, f.params.size()); ++i) {
            args[i] = f.params[i];
        }
    } else if (noArgs) {
        std::copy_n(thread_.temps.begin(), std::min(count, thread_.temps.size()), args.begin());
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            args[i] = static_cast<std::int16_t>(o[i * 2] | o[i * 2 + 1] << 8);
        }
    }
    return args;
}

} // namespace vm_detail

using namespace vm_detail;

namespace {

// Counts a run started by a primitive as nested while it lasts, however it ends.
class NestedRun {
public:
    explicit NestedRun(int& runs) : runs_(runs) { ++runs_; }
    ~NestedRun() { --runs_; }
    NestedRun(const NestedRun&) = delete;
    NestedRun& operator=(const NestedRun&) = delete;

private:
    int& runs_;
};

RunResult tooManyNestedChecks() {
    RunResult r;
    r.outcome = Outcome::DepthLimit;
    r.reason = "too many nested check trees";
    return r;
}

} // namespace

const char* outcomeName(Outcome outcome) {
    switch (outcome) {
    case Outcome::True:        return "returned true";
    case Outcome::False:       return "returned false";
    case Outcome::Error:       return "error";
    case Outcome::Unsupported: return "unsupported";
    case Outcome::StepLimit:   return "step limit";
    case Outcome::DepthLimit:  return "depth limit";
    case Outcome::MissingCode: return "missing code";
    case Outcome::Sleeping:    return "sleeping";
    case Outcome::MeRemoved:   return "me was removed";
    }
    return "?";
}

Vm::Vm(CodeSource& code, Limits limits, World* world)
    : code_(code), limits_(limits), world_(world) {}

RunResult Vm::run(ThreadState& thread, std::uint32_t objectGroup, std::uint16_t entry,
                  std::span<const std::int16_t> args) {
    return execute(thread, objectGroup, nullptr, &entry, args);
}

RunResult Vm::run(SimObject& me, std::uint16_t entry, std::span<const std::int16_t> args) {
    return execute(me.thread, me.group, &me, &entry, args);
}

RunResult Vm::run(SimObject& me, std::uint32_t codeGroup, std::uint16_t entry,
                  std::span<const std::int16_t> args) {
    return execute(me.thread, codeGroup, &me, &entry, args);
}

bool Vm::hasTree(std::uint32_t codeGroup, std::uint16_t tree) {
    const std::uint32_t group = tree < 0x1000 ? kGlobalGroup : tree < 0x2000 ? codeGroup : code_.semiGlobalGroup(codeGroup);
    return code_.behavior(group, tree) != nullptr;
}

RunResult Vm::runOnce(SimObject& me, std::uint32_t codeGroup, std::uint16_t tree, std::span<const std::int16_t> args) {
    ThreadState scratch;
    scratch.codeGroup = codeGroup;
    return execute(scratch, codeGroup, &me, &tree, args);
}

RunResult Vm::runEvent(SimObject& me, std::uint32_t codeOwner, std::uint16_t tree, std::int16_t stackObject,
                       std::span<const std::int16_t> args, bool keepTemps) {
    ThreadState event;
    event.temps = me.thread.temps;
    event.stackObjectId = stackObject;
    event.codeGroup = codeOwner;
    RunResult r = execute(event, codeOwner, &me, &tree, args);
    if (!keepTemps) {
        me.thread.temps = event.temps;
    }
    return r;
}

RunResult Vm::runCheck(SimObject& me, std::uint32_t codeOwner, std::uint16_t tree, std::int16_t stackObject,
                       std::span<const std::int16_t> args, const ttab::Interaction& line, std::vector<OfferedAction>& offered,
                       InteractionIcon* icon) {
    testedLine_ = line;
    ThreadState check;
    check.temps = me.thread.temps;
    check.stackObjectId = stackObject;
    check.codeGroup = codeOwner;
    struct Restore {
        std::vector<OfferedAction>*& sink;
        std::vector<OfferedAction>* before;
        InteractionIcon*& iconSink;
        InteractionIcon* iconBefore;
        ~Restore() {
            sink = before;
            iconSink = iconBefore;
        }
    } restore{offered_, offered_, candidateIcon_, candidateIcon_};
    offered_ = icon != nullptr ? &offered : nullptr;
    candidateIcon_ = icon;
    RunResult r = execute(check, codeOwner, &me, &tree, args);
    me.thread.temps = check.temps;
    return r;
}

RunResult Vm::resume(SimObject& me) {
    return execute(me.thread, me.thread.codeGroup, &me, nullptr, {});
}

RunResult Vm::resume(ThreadState& thread, std::uint32_t objectGroup) {
    return execute(thread, objectGroup, nullptr, nullptr, {});
}

// The game gives a new object the reset an object gets when it loads with a changed type: data from
// its OBJD, Main at the top of its stack, then (unless it is a multi-tile part) Init and the two
// placement fields. Its Main then runs until it first waits. Failures are reported as the object's.
void Vm::startCreated(SimObject& created, const ObjectType& type) {
    constexpr std::uint16_t kInit = 0;  // OBJf entry
    if (nestedRuns_ >= kMaxNestedRuns) {
        throw Stop{Outcome::DepthLimit, "too many nested runs", "creating an object"};
    }
    const NestedRun counted(nestedRuns_);
    auto note = [&](const RunResult& r) {
        if (r.outcome != Outcome::True && r.outcome != Outcome::False && r.outcome != Outcome::Sleeping &&
            !created.failedRun) {
            created.failedRun = r.detail.empty() ? r.reason : r.reason + " " + r.detail;
        }
    };

    // A new Sim is the neighbor of its GUID: it gets that neighbor's person data and id (person data
    // 0x1F) before Init runs, which reads them.
    if (created.isPerson() && world_ != nullptr) {
        constexpr std::size_t kNeighborIdPersonData = 0x1F;
        if (const std::int16_t neighborId = world_->neighborIdWithGuid(type.definition.guid()); neighborId != 0) {
            if (const auto* data = world_->livePersonData(neighborId)) {
                std::copy_n(data->begin(), std::min(data->size(), created.personData.size()), created.personData.begin());
            }
            created.personData[kNeighborIdPersonData] = neighborId;
        }
    }
    resetObject(created, type, code_);
    if (!isMultiTilePart(type)) {
        const auto* functions = code_.objectFunctions(type);
        if (functions != nullptr && kInit < functions->size() && (*functions)[kInit].action != 0) {
            note(runOnce(created, type.group, (*functions)[kInit].action));
        }
        finishSingleTileReset(created, type);
    }
    if (created.thread.isRunning()) {
        note(resume(created));
    }
}

void Vm::tellSimsObjectGoes(std::int16_t object) {
    if (world_ == nullptr) {
        return;
    }
    // Sims queued for delete still hear it, as the game keeps them in its list of Sims until then.
    const auto sims = world_->idsIncludingRemoved([&](const SimObject& o) { return o.isPerson() && o.id != object; });
    for (const std::int16_t id : sims) {
        SimObject* sim = world_->findIncludingRemoved(id);
        if (sim == nullptr || !forgetObject(*sim, object)) {
            continue;
        }
        const bool busy = std::ranges::any_of(running_, [&](const auto& run) { return run.first == &sim->thread || run.second == sim; });
        if (busy) {
            throw Stop{Outcome::Unsupported, "a Sim reset while its own code runs", std::to_string(id)};
        }
        if (!resetSim) {
            throw Stop{Outcome::Unsupported, "a Sim reset needs the lot simulation", std::to_string(id)};
        }
        resetSim(*sim);
    }
}

SimObject* Vm::create(std::uint32_t guid) {
    const ObjectType* type = code_.objectType(guid);
    if (type == nullptr || world_ == nullptr) {
        return nullptr;
    }
    SimObject& created = world_->create(type->group, type->definition, type->containerSlots);
    startCreated(created, *type);
    return &created;
}

RunResult Vm::checkFromTree(SimObject& who, std::uint32_t codeOwner, std::uint16_t tree, std::int16_t stackObject,
                            std::span<const std::int16_t> callArgs, const ttab::Interaction& line, ttab::Interaction& changed,
                            InteractionIcon* icon, std::vector<OfferedAction>* offeredOut) {
    // A check tree that pushes its own interaction would run its check tree again without end.
    if (nestedRuns_ >= kMaxNestedRuns) {
        return tooManyNestedChecks();
    }
    const NestedRun counted(nestedRuns_);
    // Autonomy runs hundreds of check trees in one step. Each gets the step limit to itself. The
    // line a running check tree tests is put back afterwards.
    struct Restore {
        std::size_t& steps;
        std::size_t outerSteps;
        std::optional<ttab::Interaction>& tested;
        std::optional<ttab::Interaction> outerTested;
        ~Restore() {
            steps = outerSteps;
            tested = std::move(outerTested);
        }
    } restore{stepsUsed_, std::exchange(stepsUsed_, 0), testedLine_, std::move(testedLine_)};
    std::vector<OfferedAction> offered;
    RunResult r = runCheck(who, codeOwner, tree, stackObject, callArgs, line, offered, icon);
    if (offeredOut != nullptr) {
        std::move(offered.begin(), offered.end(), std::back_inserter(*offeredOut));
    }
    if (testedLine_) {
        changed = std::move(*testedLine_);  // the outer line is put back next
    }
    r.steps = stepsUsed_;  // with the trees it ran
    return r;
}

RunResult Vm::runOnTop(ThreadState& thread, SimObject* me, std::uint32_t codeGroup, std::uint16_t tree,
                       std::int16_t stackObject, std::span<const std::int16_t> callArgs) {
    if (nestedRuns_ >= kMaxNestedRuns) {
        throw Stop{Outcome::DepthLimit, "too many nested runs", ""};
    }
    ThreadState nested;
    nested.temps = thread.temps;
    nested.stackObjectId = stackObject;
    nested.codeGroup = codeGroup;
    const NestedRun counted(nestedRuns_);
    RunResult r = execute(nested, codeGroup, me, &tree, callArgs);
    thread.temps = nested.temps;
    return r;
}

RunResult Vm::runNow(SimObject& who, std::uint32_t codeGroup, std::uint16_t tree, std::int16_t stackObject,
                     std::span<const std::int16_t> callArgs) {
    if (nestedRuns_ >= kMaxNestedRuns) {
        return tooManyNestedChecks();
    }
    ThreadState scratch;
    scratch.stackObjectId = stackObject;
    scratch.codeGroup = codeGroup;
    const NestedRun counted(nestedRuns_);
    // Each such run counts its own steps, as the game counts nested runs apart (PR-6).
    struct Restore {
        std::size_t& steps;
        std::size_t outerSteps;
        ~Restore() { steps = outerSteps; }
    } restore{stepsUsed_, std::exchange(stepsUsed_, 0)};
    return execute(scratch, codeGroup, &who, &tree, callArgs);
}

// One call of Vm::execute: its thread's frames as trees are called and return, and the run's result.
class Vm::RunState {
public:
    // What follows an instruction: the next one, the end of the run, or a wait until the thread resumes.
    enum class Next { Continue, Ended, Sleep };

    RunState(Vm& vm, ThreadState& thread, SimObject* me, Execution& ex, RunResult& result)
        : vm_(vm), thread_(thread), frames_(thread.frames), me_(me), ex_(ex), result_(result) {}

    // Pushes the entry tree, or for a resumed thread follows the result it was waiting for.
    Next start(const std::uint16_t* entry, std::span<const std::int16_t> args, std::uint32_t objectGroup,
               std::optional<std::uint32_t> entryGroup);
    // Runs the current instruction.
    Next step();

private:
    void checkDepth();
    std::pair<std::uint32_t, std::shared_ptr<const bhav::Behavior>> resolve(std::uint16_t instance, std::uint32_t codeOwner);
    void push(std::uint32_t group, std::shared_ptr<const bhav::Behavior> code, std::uint16_t instance,
              std::span<const std::int16_t> callArgs, std::uint32_t codeOwner);
    void pushCall(Execution::NamedCall& call);
    bool followBranch(bool branch);
    bool runOnStack(const Execution::NamedCall& call);

    Next branch(bool result) { return followBranch(result) ? Next::Ended : Next::Continue; }
    // Waits a tick: the same instruction runs again when the thread resumes.
    Next waitTick() {
        thread_.sleepTicks = 1;
        result_.outcome = Outcome::Sleeping;
        return Next::Sleep;
    }
    // A primitive that isn't done yet (a dialog waiting for its answer, a camera move, an animation
    // still playing) has no result and runs again next tick.
    Next branchOrWait(std::optional<bool> done) { return done ? branch(*done) : waitTick(); }

    Next runTreeByName(const bhav::Instruction& in, Frame& f);
    Next gosubFoundAction(const bhav::Instruction& in);
    Next runFunctionalTree(const bhav::Instruction& in, Frame& f);
    Next idleForInput(const bhav::Instruction& in, Frame& f);
    Next reachPut(const bhav::Instruction& in, Frame& f);
    Next walk(const bhav::Instruction& in, Frame& f);
    Next turn(const bhav::Instruction& in, Frame& f);
    Next sleep(const bhav::Instruction& in, Frame& f);

    Vm& vm_;
    ThreadState& thread_;
    std::vector<Frame>& frames_;
    SimObject* me_;
    Execution& ex_;
    RunResult& result_;
};

void Vm::RunState::checkDepth() {
    if (frames_.size() >= vm_.limits_.maxDepth) {
        throw Stop{Outcome::DepthLimit, "call depth limit reached",
                   std::to_string(vm_.limits_.maxDepth)};
    }
}

std::pair<std::uint32_t, std::shared_ptr<const bhav::Behavior>> Vm::RunState::resolve(std::uint16_t instance,
                                                                                       std::uint32_t codeOwner) {
    checkDepth();
    const std::uint32_t group = ex_.groupFor(instance, codeOwner);
    auto code = vm_.code_.behavior(group, instance);
    if (!code) {
        throw Stop{Outcome::MissingCode, "BHAV not found", hex(group, 8) + " " + hex(instance, 4)};
    }
    return std::pair{group, std::move(code)};
}

void Vm::RunState::push(std::uint32_t group, std::shared_ptr<const bhav::Behavior> code, std::uint16_t instance,
                        std::span<const std::int16_t> callArgs, std::uint32_t codeOwner) {
    Frame frame;
    frame.group = group;
    frame.codeOwner = codeOwner;
    frame.instance = instance;
    frame.params = vm_.spares_.take();
    frame.locals = vm_.spares_.take();
    frame.params.assign(std::max<std::size_t>(4, code->argCount), 0);
    std::copy_n(callArgs.begin(), std::min(callArgs.size(), frame.params.size()),
                frame.params.begin());
    frame.locals.assign(code->localCount, 0);
    frame.stackObjectId = frames_.empty() ? thread_.stackObjectId : frames_.back().stackObjectId;
    if (!frames_.empty()) {
        frame.runningOnId = frames_.back().runningOnId;
    } else if (me_ != nullptr) {
        frame.runningOnId = codeOwner != me_->group && thread_.stackObjectId != 0 ? thread_.stackObjectId : me_->id;
    }
    frame.code = std::move(code);
    frames_.push_back(std::move(frame));
}

void Vm::RunState::pushCall(Execution::NamedCall& call) {
    checkDepth();
    push(call.group, std::move(call.code), call.instance, call.args, call.codeOwner);
}

// Follows the exit of the current instruction, unwinding returns. True when the run ended.
bool Vm::RunState::followBranch(bool branch) {
    const bhav::Instruction& done = frames_.back().code->instructions[frames_.back().pc];
    std::uint16_t target = branch ? done.trueTarget : done.falseTarget;
    while (true) {
        if (target == bhav::kTargetError) {
            throw Stop{Outcome::Error, "reached an error exit", ""};
        }
        if (target != bhav::kTargetTrue && target != bhav::kTargetFalse) {
            frames_.back().pc = target;
            return false;
        }
        const bool returned = target == bhav::kTargetTrue || frames_.back().callerContinuesTrue;
        const bool callerReruns = frames_.back().callerReruns;
        const bool callerIdlesAgain = frames_.back().callerIdlesAgain;
        if (frames_.back().runsInteraction && me_ != nullptr) {
            if (vm_.world_ != nullptr) {
                vm_.world_->releaseUse(frames_.back());
            }
            noteCurrentInteraction(*me_, 0, 0);
            const std::uint32_t ended = frames_.back().interactionId;
            if (me_->runningSub && me_->runningSub->id == ended) {
                me_->runningSub.reset();
            } else if (me_->running && me_->running->id == ended) {
                me_->running.reset();
                notePriority(*me_, nullptr);
            }
        }
        if (frames_.size() == 1) {
            thread_.stackObjectId = frames_.back().stackObjectId;
        }
        vm_.spares_.keep(std::move(frames_.back().params));
        vm_.spares_.keep(std::move(frames_.back().locals));
        frames_.pop_back();
        if (frames_.empty()) {
            result_.outcome = returned ? Outcome::True : Outcome::False;
            return true;
        }
        if (callerIdlesAgain) {
            return false;
        }
        if (callerReruns) {
            if (me_ != nullptr && me_->turning) {
                me_->turning->preRoute = returned;
            }
            if (me_ != nullptr && me_->walking && me_->walking->checking && !me_->walking->goalCheck) {
                me_->walking->goalCheck = returned;
            } else if (me_ != nullptr && me_->walking && !me_->walking->started && !me_->walking->preRoute) {
                me_->walking->preRoute = returned;
            }
            if (me_ != nullptr && me_->walking && me_->walking->climbing && !me_->walking->climbed) {
                me_->walking->climbed = returned;
            }
            if (me_ != nullptr && me_->walking && me_->walking->block.step != RouteBlock::Step::None && !me_->walking->block.returned &&
                me_->walking->frame == frames_.size() - 1) {
                me_->walking->block.returned = returned;
            }
            if (me_ != nullptr && me_->postRoute && !me_->postRoute->result && me_->postRoute->frame == frames_.size() - 1) {
                me_->postRoute->result = returned;
            }
            return false;
        }
        const Frame& caller = frames_.back();
        const bhav::Instruction& callSite = caller.code->instructions[caller.pc];
        target = returned ? callSite.trueTarget : callSite.falseTarget;
    }
}

// Run Tree by Name mode 1: the tree runs to its end at once as the stack object, on its temps, with
// itself as stack object, and its result is the instruction's. A tree that waits, or fails, leaves
// the stack object to be reset (game error 0x451 for waiting) and is false here. What the engine
// can't run yet stops the caller as before.
bool Vm::RunState::runOnStack(const Execution::NamedCall& call) {
    SimObject* target = vm_.world_ ? vm_.world_->find(*call.stackObject) : nullptr;
    if (target == nullptr) {
        throw Stop{Outcome::Error, "stack object does not exist", std::to_string(*call.stackObject)};
    }
    if (vm_.nestedRuns_ >= kMaxNestedRuns) {
        throw Stop{Outcome::DepthLimit, "too many nested runs", ""};
    }
    ThreadState nested;
    nested.temps = target->thread.temps;
    nested.stackObjectId = target->id;
    nested.codeGroup = call.codeOwner;
    RunResult r;
    {
        const NestedRun counted(vm_.nestedRuns_);
        const std::uint16_t tree = call.instance;
        r = vm_.execute(nested, call.codeOwner, target, &tree, call.args, call.group);
    }
    target->thread.temps = nested.temps;
    switch (r.outcome) {
    case Outcome::True:
        return true;
    case Outcome::False:
        return false;
    case Outcome::Unsupported:
    case Outcome::MissingCode:
        throw Stop{r.outcome, r.reason, r.detail};
    default:
        target->failedRun = r.outcome == Outcome::Sleeping ? "a tree run on it waited" : r.reason;
        return false;
    }
}

Vm::RunState::Next Vm::RunState::start(const std::uint16_t* entry, std::span<const std::int16_t> args,
                                       std::uint32_t objectGroup, std::optional<std::uint32_t> entryGroup) {
    if (entry != nullptr) {
        if (thread_.isRunning()) {
            throw Stop{Outcome::Error, "thread is already running a behavior", ""};
        }
        thread_.codeGroup = objectGroup;
        if (entryGroup) {
            auto code = vm_.code_.behavior(*entryGroup, *entry);
            if (!code) {
                throw Stop{Outcome::MissingCode, "BHAV not found", hex(*entryGroup, 8) + " " + hex(*entry, 4)};
            }
            push(*entryGroup, std::move(code), *entry, args, objectGroup);
        } else {
            auto [group, code] = resolve(*entry, objectGroup);
            push(group, std::move(code), *entry, args, objectGroup);
        }
    } else if (!thread_.isRunning()) {
        throw Stop{Outcome::Error, "nothing to resume", ""};
    } else if (thread_.pendingResult) {
        const bool pending = *thread_.pendingResult;
        thread_.pendingResult.reset();
        thread_.sleepTicks = 0;
        return branch(pending);
    }
    return Next::Continue;
}

Vm::RunState::Next Vm::RunState::step() {
    if (vm_.stepsUsed_ >= vm_.limits_.maxSteps) {
        throw Stop{Outcome::StepLimit, "step limit reached", std::to_string(vm_.limits_.maxSteps)};
    }
    Frame& f = frames_.back();
    if (f.code->instructions.empty()) {
        throw Stop{Outcome::Error, "BHAV has no instructions", ""};
    }
    if (f.pc >= f.code->instructions.size()) {
        throw Stop{Outcome::Error, "jump to a missing instruction", std::to_string(f.pc)};
    }
    const bhav::Instruction& in = f.code->instructions[f.pc];
    ++result_.steps;
    ++vm_.stepsUsed_;
    if (vm_.onStep) {
        vm_.onStep({frames_.size() - 1, f.group, f.instance, f.pc, in, f.code->name, me_ != nullptr ? me_->id : std::int16_t{0}});
    }

    if (in.opcode >= 0x0100) {
        const std::uint32_t codeOwner = f.codeOwner;
        auto [group, code] = resolve(in.opcode, codeOwner);
        const auto callArgs = ex_.callArgs(in, f, code->argCount);
        push(group, std::move(code), in.opcode, callArgs, codeOwner);
        return Next::Continue;
    }
    switch (in.opcode) {
    case kRunTreeByNameOpcode:
        return runTreeByName(in, f);
    case kGosubFoundActionOpcode:
        return gosubFoundAction(in);
    case kRunFunctionalTreeOpcode:
        return runFunctionalTree(in, f);
    case kIdleForInputOpcode:
        return idleForInput(in, f);
    case kReachPutOpcode:
        return reachPut(in, f);
    case kGoToRoutingSlotOpcode:
    case kGoToRelativePositionOpcode:
        return walk(in, f);
    case kTurnBodyTowardsOpcode:
        return turn(in, f);
    case kDialogOpcode:
        return branchOrWait(ex_.dialog(in, f));
    case kCameraControlOpcode:
        return branchOrWait(ex_.cameraControl(in, f));
    case kAnimateSimOpcode:
        return branchOrWait(ex_.animateSim(in, f));
    case kSleepOpcode:
        return sleep(in, f);
    default:
        return branch(ex_.primitive(in, f));
    }
}

Vm::RunState::Next Vm::RunState::runTreeByName(const bhav::Instruction& in, Frame& f) {
    auto call = ex_.namedTree(in, f);
    if (!call) {
        return branch(false);
    }
    if (call->onStackObject) {
        return branch(runOnStack(*call));
    }
    pushCall(*call);
    if (call->stackObject) {
        frames_.back().stackObjectId = *call->stackObject;
    }
    return Next::Continue;
}

Vm::RunState::Next Vm::RunState::gosubFoundAction(const bhav::Instruction& in) {
    auto call = ex_.foundAction(in);
    if (!call) {
        return branch(false);
    }
    pushCall(*call);
    frames_.back().stackObjectId = *call->stackObject;
    frames_.back().runningOnId = *call->runningOn;
    frames_.back().runsInteraction = true;
    vm_.world_->countUse(frames_.back());
    noteCurrentInteraction(*me_, me_->data[kTreeTableEntryField], *call->stackObject);
    notePriority(*me_, &*call->interaction);
    call->interaction->id = ++me_->nextInteractionId;
    if (vm_.onInteractionStart) {
        vm_.onInteractionStart(*me_, *call->interaction);
    }
    startRunning(*me_, *call->interaction);
    frames_.back().interactionId = me_->running->id;
    return Next::Continue;
}

Vm::RunState::Next Vm::RunState::runFunctionalTree(const bhav::Instruction& in, Frame& f) {
    auto call = ex_.functionalTree(in, f);
    if (!call) {
        return branch(false);
    }
    pushCall(*call);
    frames_.back().stackObjectId = *call->stackObject;
    return Next::Continue;
}

Vm::RunState::Next Vm::RunState::idleForInput(const bhav::Instruction& in, Frame& f) {
    const auto idle = ex_.idleForInput(in, f);
    if (idle.kind == Execution::IdleResult::Kind::Wait) {
        return waitTick();
    }
    if (idle.kind == Execution::IdleResult::Kind::True) {
        f.idleStarted = false;
        return branch(true);
    }
    const QueuedInteraction& started = idle.interaction;
    if (vm_.onInteractionStart && me_ != nullptr) {
        vm_.onInteractionStart(*me_, started);
    }
    auto [group, code] = resolve(started.action, started.group);
    push(group, std::move(code), started.action, started.args, started.group);
    frames_.back().stackObjectId = started.object;
    frames_.back().runningOnId = started.object;
    if (me_ != nullptr) {
        me_->interactionPartner = started.partner != 0 ? started.partner : started.object;
        noteCurrentInteraction(*me_, static_cast<std::int16_t>(started.index), started.object);
        if (idle.sub) {
            me_->runningSub = started;
        } else {
            notePriority(*me_, &started);
            startRunning(*me_, started);
        }
        frames_.back().interactionId = started.id;
        frames_.back().runsInteraction = true;
        vm_.world_->countUse(frames_.back());
    }
    if (idle.sub) {
        frames_.back().callerContinuesTrue = true;
    } else {
        frames_[frames_.size() - 2].idleStarted = true;
        frames_.back().callerIdlesAgain = true;
    }
    return Next::Continue;
}

Vm::RunState::Next Vm::RunState::reachPut(const bhav::Instruction& in, Frame& f) {
    const auto reach = ex_.reachPut(in, f);
    if (reach.kind == Execution::ReachStep::Kind::Wait) {
        return waitTick();
    }
    return branch(reach.result);
}

// Go To Routing Slot and Go To Relative Position. A walk's pre-route, post-route, stair, goal-check
// and blocked route trees run on top of it, and the instruction runs again after them.
Vm::RunState::Next Vm::RunState::walk(const bhav::Instruction& in, Frame& f) {
    auto walk = in.opcode == kGoToRoutingSlotOpcode ? ex_.goToRoutingSlot(in, f) : ex_.goToRelativePosition(in, f);
    if (walk.kind == Execution::WalkStep::Kind::Wait) {
        return waitTick();
    }
    if (walk.kind == Execution::WalkStep::Kind::PreRoute || walk.kind == Execution::WalkStep::Kind::PostRoute) {
        checkDepth();
        push(walk.group, std::move(walk.code), walk.instance, walk.args, me_->group);
        frames_.back().stackObjectId = me_->id;
        frames_.back().callerReruns = true;
        return Next::Continue;
    }
    if (walk.kind == Execution::WalkStep::Kind::Stair || walk.kind == Execution::WalkStep::Kind::GoalCheck ||
        walk.kind == Execution::WalkStep::Kind::RouteTree) {
        checkDepth();
        push(walk.group, std::move(walk.code), walk.instance, walk.args, walk.codeOwner);
        frames_.back().stackObjectId = walk.stackObject;
        frames_.back().callerReruns = true;
        return Next::Continue;
    }
    if (me_ != nullptr) {
        ex_.routeEnded(*me_);
    }
    return branch(walk.result);
}

Vm::RunState::Next Vm::RunState::turn(const bhav::Instruction& in, Frame& f) {
    auto step = ex_.turnBodyTowards(in, f);
    if (step.kind == Execution::TurnStep::Kind::Wait) {
        return waitTick();
    }
    if (step.kind == Execution::TurnStep::Kind::PreRoute) {
        // What the game passes a turn's pre-route tree. The instruction runs again after it.
        constexpr std::array<std::int16_t, 7> kPreRouteArgs{0, -1, 0, 0, -1, 0, 0};
        checkDepth();
        push(step.group, std::move(step.code), step.instance, kPreRouteArgs, me_->group);
        frames_.back().stackObjectId = me_->id;
        frames_.back().callerReruns = true;
        return Next::Continue;
    }
    return branch(step.result);
}

// A Sleep of 0 ticks is true at once, without yielding. Otherwise the thread waits and Sleep is true
// when it resumes.
Vm::RunState::Next Vm::RunState::sleep(const bhav::Instruction& in, Frame& f) {
    const std::int32_t ticks = ex_.sleepTicks(in, f);
    if (ticks == 0) {
        return branch(true);
    }
    thread_.sleepTicks = ticks;
    thread_.pendingResult = true;
    result_.outcome = Outcome::Sleeping;
    return Next::Sleep;
}

RunResult Vm::execute(ThreadState& thread, std::uint32_t objectGroup, SimObject* me,
                      const std::uint16_t* entry, std::span<const std::int16_t> args,
                      std::optional<std::uint32_t> entryGroup) {
    if (activeRuns_++ == 0) {
        stepsUsed_ = 0;
    }
    running_.emplace_back(&thread, me);
    struct EndRun {
        int& runs;
        std::vector<std::pair<const ThreadState*, const SimObject*>>& running;
        ~EndRun() {
            --runs;
            running.pop_back();
        }
    } endRun{activeRuns_, running_};
    Execution ex(code_, globals, thread, world_, me, rng);
    ex.offered = offered_;
    ex.candidateIcon = candidateIcon_;
    if (!autonomyTunings_) {
        autonomyTunings_ = AutonomyTunings::from(code_.tunings());
    }
    ex.autonomyTunings = &*autonomyTunings_;
    ex.checkTreeKinds = &checkTreeKinds_;
    ex.testedLine = testedLine_ ? &*testedLine_ : nullptr;
    ex.runCheck = std::bind_front(&Vm::checkFromTree, this);
    ex.startCreated = std::bind_front(&Vm::startCreated, this);
    ex.tellSimsObjectGoes = std::bind_front(&Vm::tellSimsObjectGoes, this);
    ex.runOnMe = std::bind_front(&Vm::runOnTop, this, std::ref(thread), me);
    ex.runNow = std::bind_front(&Vm::runNow, this);
    ex.runOnObject = [this](SimObject& who, std::uint32_t codeGroup, std::uint16_t tree, std::int16_t stackObject,
                            std::span<const std::int16_t> args) { return runOnTop(who.thread, &who, codeGroup, tree, stackObject, args); };

    std::vector<Frame>& frames = thread.frames;
    if (frames.capacity() == 0) {
        frames = spares_.takeFrames();
    }
    // A finished run's frame list goes back for the next run, however the run ends.
    struct KeepFrames {
        Spares& spares;
        std::vector<Frame>& frames;
        ~KeepFrames() {
            if (frames.empty()) {
                spares.keepFrames(std::move(frames));
            }
        }
    } keepFrames{spares_, frames};

    RunResult result;
    RunState run(*this, thread, me, ex, result);
    try {
        RunState::Next next = run.start(entry, args, objectGroup, entryGroup);
        while (next == RunState::Next::Continue) {
            next = run.step();
        }
        if (next == RunState::Next::Ended) {
            return result;
        }
    } catch (const Stop& stop) {
        result.outcome = stop.outcome;
        result.reason = stop.reason;
        result.detail = stop.detail;
    } catch (const std::exception& ex2) {
        result.outcome = Outcome::Error;
        result.reason = "exception";
        result.detail = ex2.what();
    }

    if (!frames.empty()) {
        result.group = frames.back().group;
        result.instance = frames.back().instance;
        result.pc = frames.back().pc;
    }
    if (result.outcome != Outcome::Sleeping) {
        if (world_ != nullptr) {
            world_->releaseUses(frames);
        }
        frames.clear();
        thread.pendingResult.reset();
        thread.sleepTicks = 0;
    }
    return result;
}

void queueInteraction(SimObject& sim, const QueuedInteraction& interaction) {
    sim.queue.push_back(interaction);
    sim.queue.back().id = ++sim.nextInteractionId;
    noteNextQueued(sim);
}

bool cancelInteraction(SimObject& sim, std::uint32_t id) {
    if (sim.running && sim.running->id == id) {
        if (sim.running->cancelled) {
            return false;
        }
        sim.running->cancelled = true;
        if (sim.runningSub) {
            sim.runningSub->cancelled = true;
        }
        sim.personData[kPriorityPersonData] = 0;
        return true;
    }
    // What cancelling the sub-interaction alone does besides marking it is not known yet.
    if (sim.runningSub && sim.runningSub->id == id) {
        if (sim.runningSub->cancelled) {
            return false;
        }
        sim.runningSub->cancelled = true;
        return true;
    }
    const auto it = std::find_if(sim.queue.begin(), sim.queue.end(), [id](const QueuedInteraction& q) { return q.id == id; });
    if (it == sim.queue.end()) {
        return false;
    }
    sim.queue.erase(it);
    noteNextQueued(sim);
    return true;
}

} // namespace ts2::sim