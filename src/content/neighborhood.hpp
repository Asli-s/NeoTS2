#pragma once

#include "formats/datatables.hpp"
#include "formats/ngbh.hpp"

#include "content/resource_index.hpp"
#include "formats/swaf.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ts2::content {

// The neighborhood folder a lot package belongs to (Neighborhoods/<hood>/Lots/<file> -> <hood>).
std::filesystem::path neighborhoodOfLot(const std::filesystem::path& lotPackage);

// Character files (one per Sim): Neighborhoods/<hood>/Characters/*.package.
std::vector<std::filesystem::path> characterFiles(const std::filesystem::path& neighborhood);

// The group given to a package's own resources (group 0xFFFFFFFF), derived from its file name.
std::uint32_t localGroupFor(const std::filesystem::path& package);

// Adds every character file of the neighborhood to the index, each under its own local group.
std::size_t addCharacters(ResourceIndex& index, const std::filesystem::path& neighborhood);

// The neighborhood's own package: Neighborhoods/<hood>/<hood>_Neighborhood.package.
std::filesystem::path neighborhoodPackage(const std::filesystem::path& neighborhood);

// A neighborhood as the main menu shows it: its folder, its name and description (the package's
// catalog string set) and its picture (<hood>_Neighborhood.png beside the package).
struct NeighborhoodEntry {
    std::filesystem::path folder;
    std::string name, description;
    std::filesystem::path thumbnail;
};

// The neighborhoods in a Neighborhoods folder, in the order of their folders' names (as the game's
// main menu lists them: not known yet), leaving out the tutorial.
std::vector<NeighborhoodEntry> listNeighborhoods(const std::filesystem::path& neighborhoods);

struct Neighbor {
    std::uint32_t guid = 0;                  // the Sim's object type
    std::vector<std::int16_t> personData;    // the neighbor's copy, indexed like a Sim's
};

struct Neighborhood {
    // Ids of the neighbors: the neighborhood's Sims, wherever they live. One per Sim description
    // (SDSC); the id equals the record's instance.
    std::set<std::int16_t> neighborIds;
    std::map<std::int16_t, Neighbor> neighbors;
    // Lots of the neighborhood: n for every Lots/<hood>_Lot<n>.package.
    std::set<std::int32_t> lotIds;
    // Relationship variables neighbor A holds towards neighbor B (SREL, instance (A << 16) | B).
    // Records naming Sims that are no longer neighbors are kept, as the game leaves them.
    std::map<std::pair<std::int16_t, std::int16_t>, std::vector<std::int32_t>> relationships;
    std::size_t relationshipsWithExtraBytes = 0;
    // Each neighbor's wants, fears and want history (instance = neighbor id).
    std::map<std::int16_t, swaf::WantsAndFears> wants;
    std::size_t wantsWithExtraBytes = 0;  // stale bytes after the history, which the game ignores
    // The neighborhood record (NGBH): the global, lot, family and neighbor inventories.
    std::optional<ngbh::Neighborhood> record;
    // Each neighbor's script data tables (instance = neighbor id).
    std::map<std::int16_t, datatables::DataTables> dataTables;
    std::vector<std::string> problems;
};

Neighborhood loadNeighborhood(const std::filesystem::path& neighborhoodPackage);

} // namespace ts2::content
