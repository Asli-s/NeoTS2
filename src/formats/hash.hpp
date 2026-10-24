#pragma once

#include <cstdint>
#include <string_view>

namespace ts2::hash {

std::uint32_t crc24(std::string_view text);

std::uint32_t groupId(std::string_view name);

// CRC-32/MPEG-2 (polynomial 0x04C11DB7, start 0xFFFFFFFF, no reflection, no final xor).
std::uint32_t crc32(std::string_view text);

// The key a scenegraph resource is filed under: group 0x1C0532FA (unless the name says
// otherwise), instance 0xFF000000 | crc24 and high instance crc32 of the lowercase name (just
// crc24 for a few resources the game ships that way).
struct ScenegraphKey {
    std::uint32_t instance = 0;
    std::uint32_t instanceHigh = 0;
};
ScenegraphKey scenegraphKey(std::string_view name);

} // namespace ts2::hash
