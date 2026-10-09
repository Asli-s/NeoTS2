#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ts2::bhav {

constexpr std::uint16_t kTargetError = 0xFFFC;
constexpr std::uint16_t kTargetTrue  = 0xFFFD;
constexpr std::uint16_t kTargetFalse = 0xFFFE;

struct Instruction {
    std::uint16_t opcode = 0;
    std::uint16_t trueTarget = kTargetError;
    std::uint16_t falseTarget = kTargetError;
    std::uint8_t nodeVersion = 0;
    std::array<std::uint8_t, 16> operands{};
};

struct Behavior {
    std::string name;
    std::uint16_t format = 0;
    std::uint8_t treeType = 0;
    std::uint8_t argCount = 0;
    std::uint8_t localCount = 0;
    std::uint8_t headerFlag = 0;
    std::uint32_t treeVersion = 0;
    std::uint8_t cacheFlags = 0;
    std::vector<Instruction> instructions;
    std::size_t trailingBytes = 0;
};

std::size_t headerSize(std::uint16_t format);
std::size_t instructionSize(std::uint16_t format);

Behavior parse(std::span<const std::uint8_t> data);

enum class OpcodeKind { Primitive, Global, Private, SemiGlobal };

OpcodeKind opcodeKind(std::uint16_t opcode);

// Brings an instruction's operands to the layout the game's primitives read, as the game does
// when it loads a tree: some primitives changed their operands over the game's versions, and an
// instruction saved with an older node version is rewritten. Run by parse().
void upgradeOperands(Instruction& in);

constexpr std::uint16_t kExpressionOpcode = 0x0002;

struct ExpressionOperands {
    std::uint16_t lhsData = 0;
    std::uint16_t rhsData = 0;
    bool isSigned = false;
    std::uint8_t op = 0;
    std::uint8_t lhsOwner = 0;
    std::uint8_t rhsOwner = 0;
};

ExpressionOperands decodeExpression(const Instruction& instruction);

} // namespace ts2::bhav
