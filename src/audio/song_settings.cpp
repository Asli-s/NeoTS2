#include "audio/song_settings.hpp"

namespace ts2::audio {

namespace {

constexpr std::uint32_t kMagic = 0x2BEA4862, kBoolType = 0xCBA908E1;
constexpr std::uint16_t kVersion = 2;
constexpr std::size_t kHeader = 10, kEntry = 9;

std::uint32_t u32(std::span<const std::uint8_t> d, std::size_t at) {
    return d[at] | d[at + 1] << 8 | d[at + 2] << 16 | static_cast<std::uint32_t>(d[at + 3]) << 24;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>(v >> shift));
    }
}

} // namespace

std::optional<SongSettings> parseSongSettings(std::span<const std::uint8_t> data) {
    if (data.size() < kHeader || u32(data, 0) != kMagic || (data[4] | data[5] << 8) != kVersion) {
        return std::nullopt;
    }
    const std::uint32_t count = u32(data, 6);
    if (data.size() != kHeader + static_cast<std::size_t>(count) * kEntry) {
        return std::nullopt;
    }
    SongSettings out;
    for (std::size_t at = kHeader; at < data.size(); at += kEntry) {
        if (u32(data, at) != kBoolType) {
            return std::nullopt;
        }
        out.songs[u32(data, at + 4)] = data[at + 8] != 0;
    }
    return out;
}

std::vector<std::uint8_t> writeSongSettings(const SongSettings& settings) {
    std::vector<std::uint8_t> out;
    put32(out, kMagic);
    out.push_back(kVersion & 0xFF);
    out.push_back(kVersion >> 8);
    put32(out, static_cast<std::uint32_t>(settings.songs.size()));
    for (const auto& [id, on] : settings.songs) {
        put32(out, kBoolType);
        put32(out, id);
        out.push_back(on ? 1 : 0);
    }
    return out;
}

} // namespace ts2::audio
