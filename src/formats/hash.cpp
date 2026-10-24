#include "formats/hash.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace ts2::hash {

std::uint32_t crc24(std::string_view text) {
    std::uint32_t crc = 0xB704CE;
    for (const char c : text) {
        crc ^= static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16;
        for (int bit = 0; bit < 8; ++bit) {
            crc <<= 1;
            if (crc & 0x1000000) {
                crc ^= 0x864CFB;
            }
        }
    }
    return crc & 0xFFFFFF;
}

namespace {

std::string lowered(std::string_view name) {
    std::string out(name);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace

std::uint32_t groupId(std::string_view name) {
    return 0x7F000000 | crc24(lowered(name));
}

std::uint32_t crc32(std::string_view text) {
    std::uint32_t crc = 0xFFFFFFFF;
    for (const char c : text) {
        crc ^= static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
        }
    }
    return crc;
}

// Resources the game ships filed without their instance's high byte. It looks these up so.
constexpr std::array<std::string_view, 14> kWithoutHighByte = {
    "paintinghorizontallandscape_canvasback_txmt",
    "aftophalter-dots0_lifo",
    "aftophalter-dots1_lifo",
    "afhairmeg_black_txmt",
    "ambodysuperchef_japanblack_reflected_txmt",
    "a-teach-see_anim",
    "o2a-counter-sandwich2tray-door-counter-animation_anim",
    "o-accessorytoolswrench-maintain-stop-lhand_anim",
    "afhairhatfirefighter_kinky_red_txmt",
    "wall-wallpaperwcoffer-bump_txtr",
    "afhairmeg_blond_txmt",
    "amhairpgskater_black_alpha3_txmt",
    "wall-wallpaperwcoffer-bump0_lifo",
    "wall-wallpaperwcoffer-bump1_lifo",
};

ScenegraphKey scenegraphKey(std::string_view name) {
    const std::string low = lowered(name);
    const bool withoutHighByte = std::find(kWithoutHighByte.begin(), kWithoutHighByte.end(), low) != kWithoutHighByte.end();
    return {(withoutHighByte ? 0 : 0xFF000000) | crc24(low), crc32(low)};
}

} // namespace ts2::hash
