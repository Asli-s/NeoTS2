#pragma once

#include "formats/scenegraph.hpp"
#include "scene/age_data.hpp"
#include "scene/outfit_catalog.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace ts2::scene {

// The appearance editor's hair for each outfit category, by category bit number (0 everyday ... 12
// outerwear). A Sim's hair is the last of an outfit's hair list.
using HairSlots = std::array<std::optional<scenegraph::Link>, 13>;
HairSlots hairSlots(const AgeData& data);

// Whether a hair tone (a GUID) is one of the game's own five (black, brown, blond, red, grey).
bool isStockHairTone(const std::string& tone);

// The version of `hair` a Sim of this age data wears in outfit `category` (one category bit) with
// hair colour `tone`, as the game's Sim builder picks it. The hair itself when it fits that outfit,
// else a hair of the same family, else another hair of that colour made for that outfit. With
// `customTones` a hair of a colour the game doesn't ship counts as any colour. Nullopt when no hair
// fits the outfit at all.
std::optional<scenegraph::Link> outfitHair(const OutfitCatalog& catalog, const AgeData& data, const scenegraph::Link& hair,
                                           std::uint32_t category, const std::string& tone, bool customTones);

// The appearance editor's OK: the Sim takes the hair colour `tone`, then every outfit with a hair
// slot wears its version of that slot's hair. The bald scalp (0x80) and the PlantSim skin (0x800)
// stay as they are.
void wearHairs(AgeData& data, const OutfitCatalog& catalog, const HairSlots& slots, const std::string& tone);

} // namespace ts2::scene
