#pragma once

#include "formats/bhav.hpp"
#include "sim/code_source.hpp"
#include "sim/world.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <random>
#include <span>
#include <string>

namespace ts2::sim {

constexpr std::size_t kGlobalCount = 256;

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
constexpr std::uint8_t kMyArrayIterator = 0x29;
constexpr std::uint8_t kStackObjectArrayIterator = 0x2A;
constexpr std::uint8_t kMyArrayAtIterator = 0x2B;
constexpr std::uint8_t kStackObjectArrayAtIterator = 0x2C;
constexpr std::uint8_t kStackObjectMotives = 0x0F;
constexpr std::uint8_t kStackObjectMotiveByTemp = 0x11;
constexpr std::uint8_t kMyPersonData = 0x12;
constexpr std::uint8_t kStackObjectPersonData = 0x13;
constexpr std::uint8_t kStackObjectDefinition = 0x15;
// The stack object's attribute whose index is in a param of the frame (data = the param).
constexpr std::uint8_t kStackObjectAttributeByParam = 0x16;
constexpr std::uint8_t kLocal = 0x19;
constexpr std::uint8_t kMyPersonDataByTemp = 0x1E;
// Table from data >> 3; index from temp 0.
constexpr std::uint8_t kConstantByTemp = 0x2F;
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
};

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

    std::array<std::int16_t, kGlobalCount> globals{};
    std::function<void(const StepInfo&)> onStep;
    std::mt19937 rng{0x5132};

private:
    RunResult execute(ThreadState& thread, std::uint32_t objectGroup, SimObject* me,
                      const std::uint16_t* entry, std::span<const std::int16_t> args);

    CodeSource& code_;
    Limits limits_;
    World* world_;
    int nestedRuns_ = 0;  // synchronous runs started by primitives (check trees), in progress
};

} // namespace ts2::sim
