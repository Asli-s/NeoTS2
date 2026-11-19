#pragma once

#include "formats/dbpf_writer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace ts2::sim {

class World;

// Whether a Sim's description in the neighborhood takes person data `index` from the Sim on the lot
// when the lot is saved. The fields it does not take are the ones only the lot uses, such as the
// running interaction, routing, look-at and selection state, and the description keeps its own
// value of those.
bool descriptionTakesPersonData(std::size_t index);

// The person data a Sim's description holds after a save: the Sim's own where the description
// takes it, else `description`'s. Missing values count as 0.
std::array<std::int16_t, 231> savedDescriptionPersonData(std::span<const std::int16_t> description,
                                                         std::span<const std::int16_t> sim);

// Whether a save writes the neighbors made since the neighborhood loaded. It writes them only with
// their character files (see newCharacterFiles), as the game would load a description with no
// character file behind it. Left out, every relationship, member and tie naming them goes too.
enum class NewNeighbors { LeftOut, Written };

struct HoodSaveCounts {
    int descriptions = 0;     // Sim descriptions rewritten
    bool lotDescription = false;  // the lot's description rewritten
    bool family = false;          // the lot's family rewritten
    // The lot's family and the funds it saved, which become its last saved funds.
    std::optional<std::pair<std::uint32_t, std::int32_t>> savedFunds;
    int relationships = 0;        // relationship records rewritten or added
    int newNeighbors = 0;         // descriptions added for Sims made this run
    int newDna = 0;               // DNA records added for them
    int familyMembers = 0;        // families whose member list changed
    bool familyTies = false;      // the family ties record rewritten
    bool neighborCounter = false; // the counter new neighbor ids start from rewritten
    int inventories = 0;          // inventories written into the neighborhood record
    int wants = 0;                // wants records written
};

// Writes into `hood`, the neighborhood package, what the game's save of a lot changes there: the
// description of each Sim on the lot (see savedDescriptionPersonData, starting from the neighbor's
// working copy), and lot `lotId`'s description, whose lot info block becomes the one `lot`, the
// lot's saved package, now holds. The family living on the lot gets the funds the Budget globals in
// `globals` hold, as both its funds and its last saved funds. Each relationship record takes the
// values the neighbor's working copy holds now, and a relationship made since that holds values
// gets a new record. The description of a neighbor whose relationships changed is saved again too,
// with its list in the game's table order (see relationship_table.hpp). A Sim made this run gets a
// description and DNA, joins its family's member list and family ties, and the counter new neighbor
// ids start from is saved. Records of another version stay as they are.
HoodSaveCounts fillNeighborhoodPackage(dbpf::PackageImage& hood, const dbpf::PackageImage& lot, std::uint32_t lotId,
                                       const World& world, std::span<const std::int16_t> globals,
                                       NewNeighbors newNeighbors = NewNeighbors::LeftOut);

} // namespace ts2::sim
