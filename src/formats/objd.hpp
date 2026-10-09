#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ts2::objd {

namespace field {
constexpr std::size_t kInteractionTableId = 0x07;
constexpr std::size_t kType = 0x09;
constexpr std::size_t kMultiTileMasterId = 0x0A;
constexpr std::size_t kGuid1 = 0x0E;
constexpr std::size_t kGuid2 = 0x0F;
constexpr std::size_t kPrice = 0x12;
constexpr std::size_t kSlotsId = 0x14;
constexpr std::size_t kNumAttributes = 0x3A;
constexpr std::size_t kObjectArrayCount = 0x3B;
} // namespace field

constexpr std::size_t kMaxFields = 108;

struct ObjectDefinition {
    std::string name;
    std::vector<std::uint16_t> fields;

    std::uint16_t field(std::size_t index) const {
        return index < fields.size() ? fields[index] : 0;
    }
    std::uint32_t guid() const {
        return static_cast<std::uint32_t>(field(field::kGuid1)) |
               static_cast<std::uint32_t>(field(field::kGuid2)) << 16;
    }
};

ObjectDefinition parse(std::span<const std::uint8_t> data);

} // namespace ts2::objd
