#pragma once

#include <bit>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace ts2 {

// Little-endian output for the records the game saves, the counterpart of BinaryReader.
class BinaryWriter {
public:
    void u8(std::uint8_t v) { out_.push_back(v); }
    void u16(std::uint16_t v) {
        out_.push_back(static_cast<std::uint8_t>(v));
        out_.push_back(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v) {
        for (int shift = 0; shift < 32; shift += 8) {
            out_.push_back(static_cast<std::uint8_t>(v >> shift));
        }
    }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
    void zeros(std::size_t n) { out_.insert(out_.end(), n, 0); }
    void bytes(const std::vector<std::uint8_t>& b) { out_.insert(out_.end(), b.begin(), b.end()); }

    // A count the game writes as 32 bits.
    void count(std::size_t n) {
        if (n > 0x7FFFFFFF) {
            throw std::runtime_error("a list is too long to write");
        }
        u32(static_cast<std::uint32_t>(n));
    }

    // A class name as the game writes one: its length in a byte, then the text.
    void className(std::string_view name) {
        if (name.size() >= 0x80) {
            throw std::runtime_error("a class name is too long to write");
        }
        u8(static_cast<std::uint8_t>(name.size()));
        out_.insert(out_.end(), name.begin(), name.end());
    }

    // Text with its length first, in as few 7-bit groups as it needs, as the game writes text.
    void text(std::string_view t) {
        std::size_t length = t.size();
        do {
            const auto low = static_cast<std::uint8_t>(length & 0x7F);
            length >>= 7;
            out_.push_back(static_cast<std::uint8_t>(low | (length != 0 ? 0x80 : 0)));
        } while (length != 0);
        out_.insert(out_.end(), t.begin(), t.end());
    }

    // Text with a 32-bit length first, as the neighborhood package writes text.
    void fullText(std::string_view t) {
        if (t.size() > 0xFFFFFFFFu) {
            throw std::runtime_error("a text is too long to write");
        }
        u32(static_cast<std::uint32_t>(t.size()));
        out_.insert(out_.end(), t.begin(), t.end());
    }

    // The 64 zero bytes, type, version and class name that start the lot's records.
    void recordHeader(std::uint32_t type, std::uint32_t version, std::string_view name) {
        zeros(64);
        u32(type);
        u32(version);
        className(name);
    }

    std::vector<std::uint8_t> take() { return std::move(out_); }

private:
    std::vector<std::uint8_t> out_;
};

} // namespace ts2
