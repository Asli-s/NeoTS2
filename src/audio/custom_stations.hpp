#pragma once

// The player's own radio stations, kept in Music/customstation.xml: a name and the songs it plays,
// each song by its two key halves (see Bank::songId).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ts2::audio {

struct CustomSong {
    std::uint32_t id1 = 0;
    std::uint32_t id2 = 0;
    bool operator==(const CustomSong&) const = default;
};

struct CustomStation {
    std::string name;
    std::vector<CustomSong> songs;
    bool operator==(const CustomStation&) const = default;
};

// <CustomStations><Station Name=".."><Songs><Song ID1="0x.." ID2="0x.."/>...
// Nothing for text that is not that document. A station may hold no songs, and a song with a missing
// or unreadable id is left out.
std::optional<std::vector<CustomStation>> parseCustomStations(std::string_view xml);
// The stations as the game writes them: a station without songs is not written.
std::string writeCustomStations(const std::vector<CustomStation>& stations);

} // namespace ts2::audio
