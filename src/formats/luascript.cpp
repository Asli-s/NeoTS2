#include "formats/luascript.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <stdexcept>

namespace ts2::luascript {

namespace {

constexpr std::uint8_t kSignature[] = {0x1B, 'L', 'u', 'a'};
constexpr std::uint8_t kVersion = 0x50;
constexpr std::size_t kHeaderSize = 4 + 1 + 1 + 8 + 8;  // signature, version, endianness, sizes, test number
constexpr std::size_t kSizeTField = 7;                  // where the header records sizeof(size_t)
constexpr int kMaxNesting = 200;
constexpr std::uint32_t kMaxCount = 0x1000000;

// Copies one function of a chunk, rewriting every string length from the chunk's size_t to the
// native one. The layout follows Lua 5.0's lundump.c.
class Converter {
public:
    Converter(BinaryReader& r, std::size_t sizeT, std::vector<std::uint8_t>& out) : r_(r), sizeT_(sizeT), out_(out) {}

    void function(int depth) {
        if (depth > kMaxNesting) {
            throw std::runtime_error("luascript: functions nested too deeply");
        }
        string();               // source
        copy(4 + 4);            // line defined, then upvalue count, parameter count, vararg, max stack
        copyCounted(4);         // line info
        for (std::uint32_t n = count(); n > 0; --n) {  // locals: name, start pc, end pc
            string();
            copy(8);
        }
        for (std::uint32_t n = count(); n > 0; --n) {  // upvalue names
            string();
        }
        for (std::uint32_t n = count(); n > 0; --n) {  // constants
            const std::uint8_t type = r_.u8();
            out_.push_back(type);
            switch (type) {
            case 0: break;           // nil
            case 3: copy(8); break;  // number
            case 4: string(); break;
            default: throw std::runtime_error("luascript: unknown constant type");
            }
        }
        for (std::uint32_t n = count(); n > 0; --n) {  // nested functions
            function(depth + 1);
        }
        copyCounted(4);  // code
    }

private:
    std::uint32_t count() {
        const std::uint32_t n = r_.u32();
        if (n > kMaxCount) {
            throw std::runtime_error("luascript: count too large");
        }
        put(n, 4);
        return n;
    }

    void copyCounted(std::size_t elementSize) {
        const std::uint32_t n = count();
        copy(static_cast<std::size_t>(n) * elementSize);
    }

    void copy(std::size_t bytes) {
        const auto data = r_.bytes(bytes);
        out_.insert(out_.end(), data.begin(), data.end());
    }

    void string() {
        std::uint64_t length = r_.u32();
        if (sizeT_ == 8) {
            length |= static_cast<std::uint64_t>(r_.u32()) << 32;
        }
        if (length > r_.remaining()) {
            throw std::runtime_error("luascript: string longer than the chunk");
        }
        put(length, sizeof(std::size_t));
        copy(static_cast<std::size_t>(length));
    }

    void put(std::uint64_t value, std::size_t bytes) {
        for (std::size_t i = 0; i < bytes; ++i) {
            out_.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
        }
    }

    BinaryReader& r_;
    std::size_t sizeT_;
    std::vector<std::uint8_t>& out_;
};

} // namespace

Script parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    Script s;
    s.value = r.u32();
    const auto name = r.bytes(r.u32());
    s.name.assign(name.begin(), name.end());
    const auto chunk = r.bytes(r.remaining());
    s.chunk.assign(chunk.begin(), chunk.end());
    if (s.chunk.size() < sizeof(kSignature) || !std::equal(std::begin(kSignature), std::end(kSignature), s.chunk.begin())) {
        throw std::runtime_error("luascript: not a compiled Lua chunk");
    }
    return s;
}

std::vector<std::uint8_t> nativeChunk(std::span<const std::uint8_t> chunk) {
    if (chunk.size() < kHeaderSize || !std::equal(std::begin(kSignature), std::end(kSignature), chunk.begin())) {
        throw std::runtime_error("luascript: not a compiled Lua chunk");
    }
    // Version 5.0, little-endian, 4-byte int and instruction, standard opcode fields, 8-byte numbers.
    if (chunk[4] != kVersion || chunk[5] != 1 || chunk[6] != 4 || chunk[8] != 4 || chunk[9] != 6 ||
        chunk[10] != 8 || chunk[11] != 9 || chunk[12] != 9 || chunk[13] != 8) {
        throw std::runtime_error("luascript: unsupported chunk layout");
    }
    const std::size_t sizeT = chunk[kSizeTField];
    if (sizeT != 4 && sizeT != 8) {
        throw std::runtime_error("luascript: unsupported size_t");
    }
    std::vector<std::uint8_t> out(chunk.begin(), chunk.begin() + kHeaderSize);
    out[kSizeTField] = static_cast<std::uint8_t>(sizeof(std::size_t));
    BinaryReader r(chunk.subspan(kHeaderSize));
    Converter(r, sizeT, out).function(0);
    if (r.remaining() != 0) {
        throw std::runtime_error("luascript: bytes after the chunk");
    }
    return out;
}

} // namespace ts2::luascript
