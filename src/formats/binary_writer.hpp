#pragma once

#include <bit>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ts2 {

// Little-endian values appended to a byte buffer: the writing side of BinaryReader, for the records
// NeoTS2 saves.
class BinaryWriter {
public:
    void u8(std::uint8_t v) { out_.push_back(v); }
    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v));
        u8(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v) {
        u16(static_cast<std::uint16_t>(v));
        u16(static_cast<std::uint16_t>(v >> 16));
    }
    void i16(std::int16_t v) { u16(static_cast<std::uint16_t>(v)); }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
    void bytes(std::span<const std::uint8_t> b) { out_.insert(out_.end(), b.begin(), b.end()); }
    void text(std::string_view s) { out_.insert(out_.end(), s.begin(), s.end()); }
    // A text ended by a zero byte.
    void cString(std::string_view s) {
        text(s);
        u8(0);
    }
    // A text after its length in 7-bit groups, as BinaryReader::varString reads it.
    void varString(std::string_view s) {
        std::size_t n = s.size();
        do {
            const auto low = static_cast<std::uint8_t>(n & 0x7F);
            n >>= 7;
            u8(n != 0 ? static_cast<std::uint8_t>(low | 0x80) : low);
        } while (n != 0);
        text(s);
    }

    std::size_t size() const { return out_.size(); }
    const std::vector<std::uint8_t>& data() const { return out_; }
    std::vector<std::uint8_t> take() { return std::move(out_); }

private:
    std::vector<std::uint8_t> out_;
};

} // namespace ts2
