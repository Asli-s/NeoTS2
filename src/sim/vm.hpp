#pragma once

#include "formats/bhav.hpp"
#include "formats/ttab.hpp"
#include "sim/code_source.hpp"
#include "sim/find_best_interaction.hpp"
#include "sim/world.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace ts2::sim {

constexpr std::size_t kGlobalCount = 256;

// Where an instruction's operand reads or writes its value (its data owner). "My" is the object
// running the tree. The stack object is the object the tree works on: each frame keeps its own, and a
// called tree starts with its caller's. Temps are the thread's own values, shared by all its trees.
// Params are the values the calling tree passed. Globals are the simulator's. A literal is the number.
namespace owner {
constexpr std::uint8_t kMyAttribute = 0x00;
constexpr std::uint8_t kStackObjectAttribute = 0x01;
constexpr std::uint8_t kMySemiAttribute = 0x02;
constexpr std::uint8_t kMyData = 0x03;
constexpr std::uint8_t kStackObjectData = 0x04;
constexpr std::uint8_t kStackObjectSemiAttribute = 0x05;
constexpr std::uint8_t kGlobal = 0x06;
constexpr std::uint8_t kLiteral = 0x07;
constexpr std::uint8_t kTemp = 0x08;
constexpr std::uint8_t kParam = 0x09;
constexpr std::uint8_t kStackObjectId = 0x0A;
constexpr std::uint8_t kTempByTemp = 0x0B;
constexpr std::uint8_t kMyMotives = 0x0E;
constexpr std::uint8_t kRoomByTemp = 0x17;
constexpr std::uint8_t kStackObjectSlot = 0x10;
constexpr std::uint8_t kMySlot = 0x14;
constexpr std::uint8_t kNeighborInStackObject = 0x18;
constexpr std::uint8_t kNeighborPersonData = 0x20;
constexpr std::uint8_t kNeighborObjectDefinition = 0x26;
constexpr std::uint8_t kTemporaryToken = 0x27;
constexpr std::uint8_t kMyArrayIterator = 0x29;
constexpr std::uint8_t kStackObjectArrayIterator = 0x2A;
constexpr std::uint8_t kMyArrayAtIterator = 0x2B;
constexpr std::uint8_t kStackObjectArrayAtIterator = 0x2C;
// My or the stack object's array (data = which) at the index in temp 0.
constexpr std::uint8_t kMyArrayAtTemp = 0x2D;
constexpr std::uint8_t kStackObjectArrayAtTemp = 0x2E;
constexpr std::uint8_t kStackObjectMotives = 0x0F;
constexpr std::uint8_t kStackObjectMotiveByTemp = 0x11;
constexpr std::uint8_t kMyPersonData = 0x12;
constexpr std::uint8_t kStackObjectPersonData = 0x13;
constexpr std::uint8_t kStackObjectDefinition = 0x15;
// The stack object's attribute whose index is in a param of the frame (data = the param).
constexpr std::uint8_t kStackObjectAttributeByParam = 0x16;
constexpr std::uint8_t kLocal = 0x19;
constexpr std::uint8_t kMyPersonDataByTemp = 0x1E;
// Table from data >> 3, index from temp [data & 7].
constexpr std::uint8_t kConstantByTemp = 0x2F;
// The advertisement of motive [data] for my age group in the interaction line a check tree tests:
// its minimum, its range and its personality type. Sims only.
constexpr std::uint8_t kCheckTreeAdRange = 0x0C;
constexpr std::uint8_t kCheckTreeAdPersonality = 0x1C;
constexpr std::uint8_t kCheckTreeAdMin = 0x1D;
// 1 when the stack object's function [data] (OBJf) has an action tree, else 0.
constexpr std::uint8_t kStackObjectHasFunction = 0x23;
// My or the stack object's container slot [temp [data]].
constexpr std::uint8_t kMySlotByTemp = 0x30;
constexpr std::uint8_t kStackObjectSlotByTemp = 0x31;
constexpr std::uint8_t kStackObjectPersonDataByTemp = 0x1F;
constexpr std::uint8_t kConstant = 0x1A;
} // namespace owner

namespace op {
constexpr std::uint8_t kGreater = 0x00;
constexpr std::uint8_t kLess = 0x01;
constexpr std::uint8_t kEquals = 0x02;
constexpr std::uint8_t kAdd = 0x03;
constexpr std::uint8_t kSubtract = 0x04;
constexpr std::uint8_t kAssign = 0x05;
constexpr std::uint8_t kMultiply = 0x06;
constexpr std::uint8_t kDivide = 0x07;
constexpr std::uint8_t kIsFlagSet = 0x08;
constexpr std::uint8_t kSetFlag = 0x09;
constexpr std::uint8_t kClearFlag = 0x0A;
constexpr std::uint8_t kIncrementAndLess = 0x0B;
constexpr std::uint8_t kModulo = 0x0C;
constexpr std::uint8_t kAnd = 0x0D;
constexpr std::uint8_t kGreaterOrEqual = 0x0E;
constexpr std::uint8_t kLessOrEqual = 0x0F;
constexpr std::uint8_t kNotEqual = 0x10;
constexpr std::uint8_t kDecrementAndGreater = 0x11;
constexpr std::uint8_t kOr = 0x12;
constexpr std::uint8_t kXor = 0x13;
constexpr std::uint8_t kAbs = 0x14;
} // namespace op

enum class Outcome { True, False, Error, Unsupported, StepLimit, DepthLimit, MissingCode, Sleeping,
                     MeRemoved };

const char* outcomeName(Outcome outcome);

struct RunResult {
    Outcome outcome = Outcome::Error;
    std::string reason;
    std::string detail;
    std::size_t steps = 0;
    std::uint32_t group = 0;
    std::uint16_t instance = 0;
    std::uint16_t pc = 0;
};

struct Limits {
    std::size_t maxSteps = 100000;
    std::size_t maxDepth = 64;
};

struct StepInfo {
    std::size_t depth;
    std::uint32_t group;
    std::uint16_t instance;
    std::uint16_t pc;
    const bhav::Instruction& instruction;
    const std::string& treeName;
    std::int16_t object;
};

// A line a candidate's check tree offers with Add/Change the Action String: for the pie menu in
// place of its interaction's own line, or for autonomy to weigh beside it. It is a copy of the
// candidate as it was then: its queue icon and the line as the tree had changed it so far.
struct OfferedAction {
    std::string name;
    std::array<std::int16_t, 4> params{};
    enum class Disabled { No, Propagating, NonPropagating } disabled = Disabled::No;
    InteractionIcon icon;
    std::optional<ttab::Interaction> line;
};

// Puts an interaction at the end of a Sim's queue (a pie menu choice), as Push Interaction does.
void queueInteraction(SimObject& sim, const QueuedInteraction& interaction);
// The player's click on a queue icon: a waiting interaction leaves the queue at once. The running
// one is marked cancelled and the Sim's Priority (person data 0x21) drops to 0, which tells its
// scripts to finish. False when the Sim has no such interaction or it is cancelled already.
bool cancelInteraction(SimObject& sim, std::uint32_t id);

class Vm {
public:
    explicit Vm(CodeSource& code, Limits limits = {}, World* world = nullptr);

    RunResult run(ThreadState& thread, std::uint32_t objectGroup, std::uint16_t entry,
                  std::span<const std::int16_t> args = {});
    RunResult run(SimObject& me, std::uint16_t entry, std::span<const std::int16_t> args = {});

    // Runs code owned by another object's group as "me", like an interaction: a Sim using
    // the fridge runs the fridge's private and semi-global BHAVs and constants in its own thread.
    RunResult run(SimObject& me, std::uint32_t codeGroup, std::uint16_t entry,
                  std::span<const std::int16_t> args = {});

    RunResult resume(ThreadState& thread, std::uint32_t objectGroup);
    RunResult resume(SimObject& me);

    // Runs tree `tree` of `codeGroup` for `me` on a thread of its own until it ends, leaving `me`'s
    // own thread alone: how the game runs an object's Load and Init functions while a lot loads.
    RunResult runOnce(SimObject& me, std::uint32_t codeGroup, std::uint16_t tree, std::span<const std::int16_t> args = {});
    // Whether tree `tree` exists for code of `codeGroup` (global, the group's own or its semi-global).
    bool hasTree(std::uint32_t codeGroup, std::uint16_t tree);

    // Makes a new object of type `guid` out of the world and starts it as Create New Object Instance
    // does (its Init runs and its Main runs until it first waits). Null when the type is unknown.
    SimObject* create(std::uint32_t guid);

    // Runs an event tree for `me` as the game does, on top of whatever `me` is running: until it
    // ends, with `stackObject` as its stack object and sharing `me`'s temps. With `keepTemps` my
    // temps are put back as they were before it.
    RunResult runEvent(SimObject& me, std::uint32_t codeOwner, std::uint16_t tree, std::int16_t stackObject,
                       std::span<const std::int16_t> args, bool keepTemps = false);

    // Runs an interaction's check tree as the pie menu does: as `me`, with `stackObject` as its stack
    // object, on a thread of its own sharing `me`'s temps. Each Add/Change the Action String it runs
    // adds a line to `offered` instead of naming my action.
    // The check tree works on a copy of the interaction `line` it tests (see owner::kCheckTreeAdMin).
    // With `icon` the tree checks a candidate (a pie menu line, a line autonomy weighs): Add/Change
    // the Action String then offers lines into `offered` and sets the candidate's queue icon in
    // `icon`. Without it, it changes the Sim's running interaction as anywhere else.
    RunResult runCheck(SimObject& me, std::uint32_t codeOwner, std::uint16_t tree, std::int16_t stackObject,
                       std::span<const std::int16_t> args, const ttab::Interaction& line, std::vector<OfferedAction>& offered,
                       InteractionIcon* icon = nullptr);
    // That copy as the last check tree left it.
    const std::optional<ttab::Interaction>& testedLine() const { return testedLine_; }

    std::array<std::int16_t, kGlobalCount> globals{};
    std::function<void(const StepInfo&)> onStep;
    // Called when a Sim takes an interaction from its queue and starts it, or starts autonomy's pick.
    std::function<void(const SimObject& sim, const QueuedInteraction& interaction)> onInteractionStart;
    std::mt19937 rng{0x5132};

private:
    // entryGroup: where the entry tree is, when it isn't where its number says.
    RunResult execute(ThreadState& thread, std::uint32_t objectGroup, SimObject* me,
                      const std::uint16_t* entry, std::span<const std::int16_t> args,
                      std::optional<std::uint32_t> entryGroup = std::nullopt);
    // Starts an object made by Create New Object Instance, before the primitive returns.
    void startCreated(SimObject& created, const ObjectType& type);
    // A check tree run by a primitive, as Vm::runCheck runs it, with the step limit to itself. The line
    // it tests is put back afterwards. `offered` (if set) receives the lines it offers.
    RunResult checkFromTree(SimObject& who, std::uint32_t codeOwner, std::uint16_t tree, std::int16_t stackObject,
                            std::span<const std::int16_t> args, const ttab::Interaction& line, ttab::Interaction& changed,
                            InteractionIcon* icon, std::vector<OfferedAction>* offered);
    // A tree run to its end at once on top of `thread`'s own stack, as `me`, on its temps (a message handler).
    RunResult runOnTop(ThreadState& thread, SimObject* me, std::uint32_t codeGroup, std::uint16_t tree,
                       std::int16_t stackObject, std::span<const std::int16_t> args);
    // A tree run to its end at once on a scratch thread, as `who`, counting its own steps.
    RunResult runNow(SimObject& who, std::uint32_t codeGroup, std::uint16_t tree, std::int16_t stackObject,
                     std::span<const std::int16_t> args);

    class RunState;
    // Runs started by primitives (check trees and the like) nest at most this deep.
    static constexpr int kMaxNestedRuns = 8;

    CodeSource& code_;
    Limits limits_;
    World* world_;
    int nestedRuns_ = 0;  // synchronous runs started by primitives (check trees), in progress
    // Runs started inside a run share its step limit, so nested runs can't multiply it.
    int activeRuns_ = 0;
    std::size_t stepsUsed_ = 0;
    std::vector<OfferedAction>* offered_ = nullptr;  // while a candidate's check tree runs
    InteractionIcon* candidateIcon_ = nullptr;       // that candidate's queue icon
    // A copy of the interaction line last tested. Like the game, it is kept after the check tree ends.
    std::optional<ttab::Interaction> testedLine_;
    std::optional<AutonomyTunings> autonomyTunings_;  // read from the game's tunings on first use
    CheckTreeKinds checkTreeKinds_{code_};

    // Emptied frame lists and frame values, kept for the next tree call so that check trees, which
    // make and drop many frames each tick, don't allocate them again.
    class Spares {
    public:
        std::vector<std::int16_t> take() { return pop(values_); }
        void keep(std::vector<std::int16_t>&& v) { push(values_, std::move(v)); }
        std::vector<Frame> takeFrames() { return pop(frames_); }
        void keepFrames(std::vector<Frame>&& v) { push(frames_, std::move(v)); }

    private:
        static constexpr std::size_t kMax = 64;
        template <typename T>
        static T pop(std::vector<T>& from) {
            if (from.empty()) {
                return {};
            }
            T v = std::move(from.back());
            from.pop_back();
            return v;
        }
        template <typename T>
        static void push(std::vector<T>& to, T&& v) {
            if (to.size() < kMax && v.capacity() > 0) {
                v.clear();
                to.push_back(std::move(v));
            }
        }
        std::vector<std::vector<std::int16_t>> values_;
        std::vector<std::vector<Frame>> frames_;
    };
    Spares spares_;
};

} // namespace ts2::sim
