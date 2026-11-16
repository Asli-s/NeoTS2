#pragma once

// A music folder's `settings` file: for each song of the station, whether it plays.

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace ts2::audio {

struct SongSettings {
    // By song id: the song's key as the title tables hash it, 0xFF000000 | CRC24 of its title.
    std::map<std::uint32_t, bool> songs;

    // A song the file does not list plays.
    [[nodiscard]] bool enabled(std::uint32_t id) const {
        const auto it = songs.find(id);
        return it == songs.end() || it->second;
    }
};

// The file is the magic 0x2BEA4862, a u16 version (2), a u32 count, then per song the type
// 0xCBA908E1, the id (u32) and one byte, 1 for on. Nothing for another magic, version or type,
// bytes cut short or left over.
std::optional<SongSettings> parseSongSettings(std::span<const std::uint8_t> data);
// The songs by id, as the files are written.
std::vector<std::uint8_t> writeSongSettings(const SongSettings& settings);

} // namespace ts2::audio
