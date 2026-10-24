#pragma once

#include "formats/datatables.hpp"
#include "formats/fami.hpp"
#include "formats/famt.hpp"
#include "formats/ngbh.hpp"

#include "content/resource_index.hpp"
#include "formats/swaf.hpp"
#include "formats/neighborhood_id.hpp"
#include "formats/weather.hpp"

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

// A lot package's number n (Lots/<hood>_Lot<n>.package), if its name has one.
[[nodiscard]] std::optional<std::int32_t> lotNumber(const std::filesystem::path& lotPackage);

// Character files (one per Sim): Neighborhoods/<hood>/Characters/*.package.
std::vector<std::filesystem::path> characterFiles(const std::filesystem::path& neighborhood);

// The image (instance) in a Sim's character file that pictures the Sim at a life stage (person
// data 0x3A): a file keeps one per life stage the Sim has had. None for an unknown stage.
[[nodiscard]] std::optional<std::uint32_t> portraitImage(std::int16_t lifeStage);

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
    // (SDSC). The id equals the record's instance.
    std::set<std::int16_t> neighborIds;
    std::map<std::int16_t, Neighbor> neighbors;
    // Lots of the neighborhood: n for every Lots/<hood>_Lot<n>.package.
    std::set<std::int32_t> lotIds;
    // Each placed lot's type from its description (LTXT, instance = lot id): 0 a house, 1 a
    // community lot, 8 an apartment building, 9 one of its apartments.
    std::map<std::int32_t, std::uint8_t> lotTypes;
    // Each placed lot's owner (a neighbor id, 0 for none).
    std::map<std::int32_t, std::int32_t> lotOwners;
    // Each placed lot's size in tiles and, on a beach lot (description flag 0x80), its beach's side.
    struct LotShape {
        std::uint32_t width = 0, depth = 0;
        std::optional<std::uint8_t> beachSide;
    };
    std::map<std::int32_t, LotShape> lotShapes;
    // The households (FAMI), by instance.
    std::map<std::uint32_t, fami::Family> families;
    // Relationship variables neighbor A holds towards neighbor B (SREL, instance (A << 16) | B).
    // Records naming Sims that are no longer neighbors are kept, as the game leaves them.
    std::map<std::pair<std::int16_t, std::int16_t>, std::vector<std::int32_t>> relationships;
    std::size_t relationshipsWithExtraBytes = 0;
    // Each neighbor's family ties (FAMT), in the order the record lists them.
    std::map<std::int16_t, std::vector<famt::Tie>> familyTies;
    // Each neighbor's wants, fears and want history (instance = neighbor id).
    std::map<std::int16_t, swaf::WantsAndFears> wants;
    std::size_t wantsWithExtraBytes = 0;  // stale bytes after the history, which the game ignores
    // The neighborhood record (NGBH): the global, lot, family and neighbor inventories.
    std::optional<ngbh::Neighborhood> record;
    // Each neighbor's script data tables (instance = neighbor id).
    std::map<std::int16_t, datatables::DataTables> dataTables;
    // Its identity: id, type and the season of each quadrant.
    std::optional<nhoodid::NeighborhoodId> identity;
    // Its weather and season (Seasons), nullopt when not saved.
    std::optional<weather::Weather> weather;
    std::vector<std::string> problems;
};

Neighborhood loadNeighborhood(const std::filesystem::path& neighborhoodPackage);

// The identity of every neighborhood and sub-neighborhood in a Neighborhoods folder (the
// packages directly in each neighborhood's folder), by id. The first of two with one id is kept.
std::map<std::int32_t, nhoodid::NeighborhoodId> neighborhoodIds(const std::filesystem::path& neighborhoods);

} // namespace ts2::content
