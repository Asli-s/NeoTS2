#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

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
