#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

namespace ts2 {

class BinaryReader {
public:
    explicit BinaryReader(std::span<const std::uint8_t> data) : data_(data) {}

    std::uint8_t u8() {
        require(1);
        return data_[pos_++];
    }

    std::uint16_t u16() {
        require(2);
        const auto value = static_cast<std::uint16_t>(data_[pos_] | data_[pos_ + 1] << 8);
        pos_ += 2;
        return value;
    }

    std::span<const std::uint8_t> bytes(std::size_t count) {
        require(count);
        const auto view = data_.subspan(pos_, count);
        pos_ += count;
        return view;
    }

    std::uint32_t u32() {
        require(4);
        std::uint32_t value = static_cast<std::uint32_t>(data_[pos_])
                            | static_cast<std::uint32_t>(data_[pos_ + 1]) << 8
                            | static_cast<std::uint32_t>(data_[pos_ + 2]) << 16
                            | static_cast<std::uint32_t>(data_[pos_ + 3]) << 24;
        pos_ += 4;
        return value;
    }

    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    float f32() { return std::bit_cast<float>(u32()); }

    void skip(std::size_t count) {
        require(count);
        pos_ += count;
    }

    // A count of elements that each take at least `minBytes`, so a count larger than the data left
    // can hold cannot be real.
    std::size_t checkedCount(std::int64_t count, std::size_t minBytes) const {
        if (count < 0 || static_cast<std::uint64_t>(count) > remaining() / std::max<std::size_t>(minBytes, 1)) {
            throw std::runtime_error("BinaryReader: bad count " + std::to_string(count));
        }
        return static_cast<std::size_t>(count);
    }

    std::size_t i32Count(std::size_t minBytes) { return checkedCount(i32(), minBytes); }
    std::size_t u32Count(std::size_t minBytes) { return checkedCount(u32(), minBytes); }

    std::string text(std::size_t length) {
        const auto view = bytes(length);
        return {view.begin(), view.end()};
    }

    // Length as 7-bit groups, low group first. The top bit of each byte says another byte follows.
    std::string varString() {
        std::size_t length = 0;
        for (int shift = 0;; shift += 7) {
            if (shift > 28) {
                throw std::runtime_error("BinaryReader: bad string length");
            }
            const std::uint8_t byte = u8();
            length |= static_cast<std::size_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) {
                break;
            }
        }
        return text(length);
    }

    std::string cString() {
        std::string out;
        for (std::uint8_t c = u8(); c != 0; c = u8()) {
            out.push_back(static_cast<char>(c));
        }
        return out;
    }

    std::size_t position() const { return pos_; }
    std::size_t remaining() const { return data_.size() - pos_; }

private:
    void require(std::size_t count) const {
        if (count > remaining()) {
            throw std::runtime_error("BinaryReader: unexpected end of data");
        }
    }

    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
};

} // namespace ts2
