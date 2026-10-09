#include "formats/bhav.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace ts2::bhav {

namespace {

constexpr std::size_t kNameSize = 64;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("BHAV: " + message);
}

std::uint16_t normalizeTarget(std::uint16_t raw, bool wide) {
    if (wide) {
        return raw;
    }
    switch (raw) {
    case 0xFD: return kTargetError;
    case 0xFE: return kTargetTrue;
    case 0xFF: return kTargetFalse;
    default:   return raw;
    }
}

} // namespace

std::size_t headerSize(std::uint16_t format) {
    return kNameSize + 12 + (format > 0x8008 ? 1 : 0);
}

std::size_t instructionSize(std::uint16_t format) {
    const std::size_t targets = format < 0x8007 ? 2 : 4;
    if (format < 0x8003) {
        return 2 + targets + 8;
    }
    if (format < 0x8005) {
        return 2 + targets + 16;
    }
    return 2 + targets + 1 + 16;
}

Behavior parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    Behavior b;

    const auto nameBytes = r.bytes(kNameSize);
    const auto nameEnd = std::find(nameBytes.begin(), nameBytes.end(), std::uint8_t{0});
    b.name.assign(nameBytes.begin(), nameEnd);

    b.format = r.u16();
    if (b.format < 0x8000 || b.format > 0x8009) {
        char hex[8];
        std::snprintf(hex, sizeof(hex), "0x%04X", b.format);
        fail(std::string("unknown format ") + hex);
    }
    const std::uint16_t count = r.u16();
    b.treeType = r.u8();
    b.argCount = r.u8();
    b.localCount = r.u8();
    b.headerFlag = r.u8();
    b.treeVersion = r.u32();
    if (b.format > 0x8008) {
        b.cacheFlags = r.u8();
    }

    if (static_cast<std::size_t>(count) * instructionSize(b.format) > r.remaining()) {
        fail("data too short for " + std::to_string(count) + " instructions");
    }

    const bool wide = b.format >= 0x8007;
    b.instructions.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        Instruction in;
        in.opcode = r.u16();
        in.trueTarget = normalizeTarget(wide ? r.u16() : r.u8(), wide);
        in.falseTarget = normalizeTarget(wide ? r.u16() : r.u8(), wide);
        if (b.format >= 0x8005) {
            in.nodeVersion = r.u8();
        }
        const std::size_t operandCount = b.format < 0x8003 ? 8 : 16;
        const auto operands = r.bytes(operandCount);
        std::copy(operands.begin(), operands.end(), in.operands.begin());
        upgradeOperands(in);
        b.instructions.push_back(in);
    }

    b.trailingBytes = r.remaining();
    return b;
}

void upgradeOperands(Instruction& in) {
    auto& o = in.operands;
    const auto old = o;  // the operands as read, for the upgrades that rebuild them
    auto setWord = [&](std::size_t i, std::uint16_t v) {
        o[i] = static_cast<std::uint8_t>(v);
        o[i + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    auto clear = [&](std::size_t from, std::size_t count) { std::fill_n(o.begin() + from, count, std::uint8_t{0}); };
    std::uint8_t& v = in.nodeVersion;

    switch (in.opcode) {
    case 0x10:
        if (v == 0) { clear(12, 4); v = 1; }
        break;
    case 0x16:
        if (v == 0) { o[10] = old[8]; clear(6, 4); v = 1; }
        break;
    case 0x19:
        if (v == 0) { o[4] &= 0xF7; v = 1; }
        break;
    case 0x1A:
        if (v == 0) {
            const std::uint8_t flags = old[2];
            o[1] = static_cast<std::uint8_t>((o[1] & ~0x06) | (flags & 0x06));
            o[0] = old[0];
            const bool second = (o[1] & 0x02) != 0;
            // Owner and data pairs at bytes 2-4 and 5-7, chosen by the old byte 1.
            auto pair = [&](std::size_t at, std::uint8_t owner, std::uint16_t data) {
                o[at] = owner;
                setWord(at + 1, data);
            };
            switch (old[1]) {
            case 0:
                second ? pair(2, 0x12, 0x1F) : pair(2, 0x03, 0x0B);
                pair(5, 0x0A, 0);
                break;
            case 1:
                pair(2, 0x0A, 0);
                second ? pair(5, 0x12, 0x1F) : pair(5, 0x03, 0x0B);
                break;
            case 2:
                pair(2, 0x0A, 0);
                pair(5, 0x19, old[3]);
                break;
            case 3:
                pair(2, 0x19, old[3]);
                pair(5, 0x0A, 0);
                break;
            default:
                pair(2, 0x03, 0x0B);
                pair(5, 0x0A, 0);
                o[1] &= 0xFD;
                break;
            }
            o[1] = static_cast<std::uint8_t>((o[1] & ~0x01) | (flags & 0x01));
            o[8] = old[4];
            o[9] = old[6];
            o[10] = old[7];
            v = 1;
        }
        break;
    case 0x1F:
        if (v == 0) { o[8] &= 0xFC; clear(9, 4); v = 1; }
        break;
    case 0x24:
        if (v == 0) { setWord(13, old[2]); o[2] = 0; v = 1; }
        break;
    case 0x2D:
        if (v == 0) {
            o[9] = old[7];
            o[10] = old[8];
            o[11] = old[9];
            o[12] = old[10];
            o[13] = old[11];
            clear(5, 4);
            v = 1;
        }
        break;
    case 0x2E:
        if (v == 0) { o[10] = old[8]; o[11] = old[9]; clear(6, 4); v = 1; }
        break;
    case 0x32:
        if (v == 0) { o[3] &= 0xFE; v = 1; }
        if (v < 2) { setWord(14, o[4]); o[4] = 0; v = 2; }
        if (v < 3) { o[3] &= 0xEF; v = 3; }
        if (v < 4) { o[3] &= 0xDF; v = 4; }
        break;
    case 0x33:
        if (v == 0) {
            const std::uint8_t b = o[0];
            o[0] = static_cast<std::uint8_t>(((((b >> 3) & 7) | (b >> 7) << 3) << 1 | ((b >> 2) & 1)) << 3 | (b & 3));
            v = 1;
        }
        if (v < 2) {
            if ((o[0] & 0x08) == 0) {
                o[9] = 0x0C;
            }
            v = 2;
        }
        break;
    case 0x6B:
        if (v == 0) { o[10] = old[10]; o[11] = old[12]; o[12] = 0; o[13] = 0; v = 1; }
        break;
    case 0x6D:
        if (v == 0) { o[13] &= 0xFB; v = 1; }
        if (v < 2) { o[13] &= 0xF7; v = 2; }
        if (v < 3) { o[13] &= 0xEF; v = 3; }
        break;
    case 0x6E:
        if (v == 0) {
            const auto half = static_cast<std::uint8_t>(static_cast<std::int16_t>(old[4] | old[5] << 8) / 2);
            o[4] = half;
            o[5] = half;
            if (o[0] & 0x08) {
                o[15] |= 0x02;
            }
            v = 1;
        }
        break;
    case 0x71:
        if (v == 0) { o[11] = old[9]; clear(7, 4); v = 1; }
        break;
    default:
        break;
    }
}

OpcodeKind opcodeKind(std::uint16_t opcode) {
    if (opcode < 0x0100) return OpcodeKind::Primitive;
    if (opcode < 0x1000) return OpcodeKind::Global;
    if (opcode < 0x2000) return OpcodeKind::Private;
    return OpcodeKind::SemiGlobal;
}

ExpressionOperands decodeExpression(const Instruction& instruction) {
    const auto& o = instruction.operands;
    ExpressionOperands e;
    e.lhsData = static_cast<std::uint16_t>(o[0] | o[1] << 8);
    e.rhsData = static_cast<std::uint16_t>(o[2] | o[3] << 8);
    e.isSigned = o[4] != 0;
    e.op = o[5];
    e.lhsOwner = o[6];
    e.rhsOwner = o[7];
    return e;
}

} // namespace ts2::bhav
