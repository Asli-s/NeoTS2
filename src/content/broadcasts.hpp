#pragma once

#include "content/install.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// TV stations and console games ("broadcasts"): the movies a TV screen plays for a station name such
// as tvstation_sbn or vgs_ssx_attract, which TV and game scripts hand to Change Material.
namespace ts2::content {

// One section of a broadcast INI file.
struct BroadcastEntry {
    std::string name;              // lower case
    std::string folder;            // Folder=, under Movies
    std::string file;              // File=, for a single movie
    std::string commercialFolder;  // CommercialFolder=
    bool isGames = false;          // IsGames=1: plays File= once
    bool dontRestart = false;      // DontRestart=1: stops after its movie
};

// The stations an INI file names in its [Broadcasts] list, in list order. Sections the list doesn't
// name are left out. Files over 1 MB or with more than 4,096 stations are cut there.
[[nodiscard]] std::vector<BroadcastEntry> parseBroadcastIni(std::string_view text);

struct Station {
    BroadcastEntry entry;
    std::vector<std::filesystem::path> shows;        // in the order they play
    std::vector<std::filesystem::path> commercials;  // one plays, picked at random, between shows
};

class Broadcasts {
public:
    // Reads the save folder's Config/Broadcast.ini (or, when it has none, the newest pack's copy
    // the game puts there) and every pack's Movies/Games/INI files. The first INI to name a station
    // wins. A station's movies are the .movie files in every pack's Movies folders whose own folder
    // has the name of the station's folder's last part, so packs add to each other's stations.
    static Broadcasts load(const std::vector<Product>& products, const std::filesystem::path& saveFolder);

    // The station of that name (any case), or null.
    [[nodiscard]] const Station* find(std::string_view name) const;
    [[nodiscard]] std::size_t size() const { return stations_.size(); }

    // For tests: stations from entries and a folder-name-to-movies table (keys in lower case).
    static Broadcasts make(const std::vector<BroadcastEntry>& entries, const std::map<std::string, std::vector<std::filesystem::path>>& movies);

private:
    std::map<std::string, Station, std::less<>> stations_;
};

} // namespace ts2::content
