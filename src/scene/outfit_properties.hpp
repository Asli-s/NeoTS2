#pragma once

#include "scene/assets.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ts2::scene {

// What the game works out about a Sim's looks for scripts (CachedNeighbor:getOutfitProperty) from its
// character file: whether its face wears kinds of makeup or facial hair (properties 3-6, 9, 11) and
// whether its hair of an outfit category is flagged (7). Each answer is kept per Sim once asked, as
// the game keeps it, so a later question can get an answer an earlier one already settled.
class OutfitProperties {
public:
    explicit OutfitProperties(Assets& assets) : assets_(assets) {}

    // Property `property` (3-7, 9-11) for outfit category `category`, 0 for the others. The Sim's looks
    // are in its character file, or for a Sim without one in its object's group.
    std::int16_t get(const std::filesystem::path& characterFile, std::uint16_t category, std::uint16_t property);
    std::int16_t get(std::uint32_t objectGroup, std::uint16_t category, std::uint16_t property);

    // A catalog skin tone or makeup (XTOL or XMOL) that a face material's overlay names by family.
    struct Overlay {
        std::uint32_t subtype = 0;
        std::uint32_t bin = 0;
        std::uint32_t category = 0;
    };

private:
    Assets& assets_;
    std::map<std::string, std::vector<Overlay>> overlaysByFamily_;
    bool catalogRead_ = false;
    std::map<std::string, std::map<std::uint32_t, std::int16_t>> answers_;  // by Sim, then (category << 16) | property

    const std::vector<Overlay>* overlays(const std::string& family);
    std::int16_t answerFor(const std::string& sim, std::uint16_t category, std::uint16_t property);
};

} // namespace ts2::scene
