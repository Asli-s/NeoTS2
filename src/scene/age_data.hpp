#pragma once

#include "formats/scenegraph.hpp"
#include "scene/outfit_catalog.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ts2::scene {

// A Sim's age data (its AGED resource): its properties, and the part lists of its outfits, each
// keyed (category << 16) | part and holding part property set keys.
struct AgeData {
    std::map<std::string, std::string> properties;
    std::map<std::uint32_t, std::vector<scenegraph::Link>> lists;
    // The skeleton the file's resource list names, none for the age's own.
    std::optional<scenegraph::Link> skeleton;
};

// The age data in a character file: its property set and its resource list, which the lists point
// into. The standard lists are read ("listcntStd", or the older "listcnt").
AgeData readAgeData(const std::map<std::string, std::string>& file, const std::vector<scenegraph::Link>& references);

// The part property sets in a character file (the Sim's own faces and parts), in the file's order.
std::vector<CatalogPart> characterParts(const std::filesystem::path& characterFile);

// The game's generator for an aging Sim's new clothes. It is seeded once a run, from the time
// since the run started, so its picks can't be repeated from a save.
class OutfitRandom {
public:
    explicit OutfitRandom(std::uint32_t seed) : state_(seed) {}
    // A draw from 0 to n - 1, nothing drawn for 0.
    std::uint32_t below(std::uint32_t n);

private:
    std::uint32_t state_;
};

// The age data a Sim gets on reaching `age` (an age flag: 1 toddler, 2 child, 4 teen, 8 adult,
// 0x10 elder, 0x40 young adult), as the game makes it: the old data copied, faces of exactly the
// new age from the Sim's own parts (`ownParts`, its character file's part property sets), default
// skin outfits from the catalog, each hair turned into the new age's hair of its family (elders go
// grey), new clothes drawn at random, and the PlantSim parts kept or swapped for the new age's.
// Nullopt for what isn't known yet: a baby, a pet, and a teen becoming a young adult with no
// young adult face of its own.
std::optional<AgeData> ageUp(const AgeData& old, std::uint32_t age, const std::vector<CatalogPart>& ownParts,
                             const OutfitCatalog& catalog, OutfitRandom& random);

// A Sim Make New Character makes from a template, before its look: its age flag, gender (1 female,
// 2 male) and skin (1 to 4 a tone of its own, anything else one drawn from the catalog).
struct NewCharacter {
    std::uint32_t age = 0, gender = 0;
    int skin = 0;
};

// A made Sim's look as the game draws it: its age data, the family of its face template (none for
// the plain face) and its face overlays (eyebrows, eyes...) in the order drawn.
struct MadeLook {
    AgeData data;
    std::string faceFamily;
    std::vector<scenegraph::Link> faceOverlays;
    std::vector<std::string> problems;
};

// The look Make New Character gives a human made from a template. The catalog is drawn from twice:
// clothes of flag 4 first, then everything but the skin among parts without flags 1 or 8 (hair
// colour, hair, eyebrows, eyes, facial hair 74 times in 100 for males past childhood, glasses 9 times
// in 100, a face template and clothes). Then the skin, unless the Sim has a tone of its own. Then
// `outfits` (the outfit object's age data) of exactly the Sim's age, gender and species put on what
// their "validparts" names: a uniform replaces the outfits it has a list for.
MadeLook makeLook(const NewCharacter& who, const std::vector<AgeData>& outfits, const OutfitCatalog& catalog, OutfitRandom& random);

// The one of an outfit object's age data (`outfits`, in the order listed) a Sim of `age` and
// `gender` flags wears, as Change Outfit picks it: among those whose age and gender are 0 or share
// a bit with the Sim's, one drawn by a generator seeded with the Sim's GUID when there are several.
// Null when none fits.
const AgeData* pickOutfit(const std::vector<AgeData>& outfits, std::uint32_t age, std::uint32_t gender, std::uint32_t simGuid);

// Puts on `data` what `outfit`'s "validparts" names, as the game copies an outfit into a Sim's age
// data: its tops, bottoms and bodies replace the Sim's in each outfit it has them for. Gives what
// isn't put on yet.
std::vector<std::string> putOn(AgeData& data, const AgeData& outfit);

} // namespace ts2::scene
