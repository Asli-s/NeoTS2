#pragma once

#include "formats/dbpf.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ts2::roundtrip {

// The records NeoTS2 writes for a family, each read and written again, so a writer can be checked
// against what the game wrote: families (FAMI), Sim descriptions (SDSC), string tables (STR#),
// relationships (SREL), family ties (FAMT), wants and fears (SWAF), the neighborhood record (NGBH)
// and Sim DNA (SDNA).

// What a record of `type` reads and writes back as, or nullopt when it isn't one of those types.
// Throws when the record can't be read.
std::optional<std::vector<std::uint8_t>> rewrite(std::uint32_t type, std::span<const std::uint8_t> data);

// Short names of the types rewrite handles ("FAMI", "SDSC"...), by type.
const std::map<std::uint32_t, std::string>& kinds();

// For each kind: how many records came back byte for byte, came back different, or couldn't be read.
struct Counts {
    std::size_t same = 0, different = 0, unreadable = 0;
};
struct Report {
    std::map<std::string, Counts> kinds;
    // The first few records that didn't come back the same: type, group, instance, and why.
    std::vector<std::string> problems;
    bool allSame() const;
};
Report check(dbpf::Package& package);

} // namespace ts2::roundtrip
