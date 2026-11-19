#pragma once

#include "formats/dbpf_writer.hpp"
#include "formats/propset.hpp"
#include "sim/genetics.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <vector>

namespace ts2::sim {

class World;

namespace hoodsave {

std::vector<std::uint8_t> bodyOf(const dbpf::StoredResource& resource);

// A neighbor's DNA (0xEBFEE33F) as the game writes a new one: genes 6, 2, 1, 3 and 5, each
// dominant then recessive. Empty for a genome holding another gene, whose place in that order is
// not known yet.
std::optional<std::vector<propset::Property>> dnaProperties(const Genome& genome);

// Adds a description (0xAACE2EFB) for each neighbor made since the neighborhood loaded that the
// package has none of: its working copy of the person data, no relationships yet and both closing
// values 0, as the game's new ones have. The save then fills it like any other. Returns how many.
int addNewDescriptions(dbpf::PackageImage& hood, const World& world);

// Adds the DNA of each neighbor made since the neighborhood loaded that has genes and no DNA
// record, and lists it in the DNA index (0x2C310F46). Returns how many.
int addNewDna(dbpf::PackageImage& hood, const World& world);

// Rewrites the family ties record (FAMT) with the ties the neighbors hold now: each listed Sim's
// ties, and a new entry, with the value 1 every game entry has, for a Sim that gained its first.
// The Sims in `leftOut` and every tie to them are left out. False when nothing changed or the
// record is of another version.
bool saveFamilyTies(dbpf::StoredResource& resource, const World& world, const std::set<std::int16_t>& leftOut);

// Rewrites the counter new neighbor ids start from (0xCC2A6A34). False when it did not change.
bool saveNeighborCounter(dbpf::StoredResource& resource, const World& world);

// Rewrites the neighborhood record (NGBH) with every inventory scripts used since it loaded, as the
// world holds it now (a birth takes the pregnancy token away). Each keyed list stays in ascending id
// order as the game keeps it, a new inventory going in by its id. The inventories of the neighbors
// in `leftOut` are left out. Returns how many inventories were written, 0 for a record of another
// version.
int saveInventories(dbpf::StoredResource& resource, const World& world, const std::set<std::int16_t>& leftOut);

// Writes the wants record (SWAF) of every Sim whose want history is in use, but those in
// `leftOut`: its wants and fears without the empty places, its history in use, and the extra want
// and hold count it was loaded with, which NeoTS2 doesn't change yet. The game writes a record over
// the one it had without cutting it short, so a shorter record keeps the old bytes after it. A record
// of another version is left as it is. Returns how many were written.
int saveWants(dbpf::PackageImage& hood, const World& world, const std::set<std::int16_t>& leftOut);

} // namespace hoodsave
} // namespace ts2::sim
