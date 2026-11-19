#pragma once

#include "formats/famt.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ts2::sim {

// A household member as the family picture reads it, from its person data.
struct FamilyPictureSim {
    std::int16_t id = 0;       // neighbor id
    std::uint32_t guid = 0;    // its GUID, which picks some poses
    std::int16_t species = 0;  // person data 0xBA: 0 human, 1 dog, 2 large dog, 3 cat
    std::int16_t age = 0;      // person data 0x3A
    std::int16_t gender = 0;   // person data 0x41: 1 female
};

// What the picture asks of the neighborhood.
struct FamilyPictureFacts {
    // The neighbors `id` has a family tie of `type` with, in the order the record lists them.
    std::function<std::vector<std::int16_t>(std::int16_t id, famt::TieType type)> ties;
    // Relationship variables `from` holds towards `to`, nullopt when there is no record.
    std::function<std::optional<std::vector<std::int32_t>>(std::int16_t from, std::int16_t to)> relationship;
};

// Where a Sim stands in the picture: its slot (the simMark_<slot> node of casNeighborhoodPose_cres)
// and the animation it is posed with. A held baby or toddler shares its holder's slot.
struct FamilyPicturePlace {
    std::int16_t id = 0;
    int slot = 0;
    std::string animation;  // with "_anim"
};

struct FamilyPicture {
    std::string camera;  // the camera_thumbnail_casNeighborhoodPose_<letter> node, by its letter
    std::vector<FamilyPicturePlace> sims;  // in the household's member order, without Sims left out
    // Set when the household needs a rule that is not known yet. The picture is not drawn then.
    std::string unknown;
};

// Arranges a household's family picture as the game does. `members` are in the family's member
// order (FAMI). No random numbers are used: what varies comes from each Sim's GUID.
FamilyPicture arrangeFamilyPicture(const std::vector<FamilyPictureSim>& members, const FamilyPictureFacts& facts);

} // namespace ts2::sim
