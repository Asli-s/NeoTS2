#pragma once

#include "content/install.hpp"
#include "formats/tga.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ts2::content {

// An entry of the neighborhood object catalog: the model it shows ("<model>_cres") and its
// catalog section ("flora", "stone", "landmark", "effects", "misc").
struct NeighborhoodObjectType {
    std::string model;
    std::string sort;
    std::string name;            // in English, as the entry has it
    bool shownInCatalog = true;  // offered by the Decorations catalog
    // Its name in other languages: string table `nameTable` of the text package `nameGroup`.
    std::uint32_t nameGroup = 0, nameTable = 0;
};

// The neighborhood object catalog (TSData/Res/Catalog/CANHObjects of every product) by guid, later
// products overriding earlier ones.
std::map<std::uint32_t, NeighborhoodObjectType> neighborhoodObjectTypes(const std::vector<Product>& products);

// The catalog's pictures of its entries by guid (TSData/Res/UserData/Thumbnails/CANHObjectsThumbnails
// of every product, later products overriding earlier ones). Entries without one are left out.
std::map<std::uint32_t, tga::Pixels> neighborhoodObjectThumbnails(const std::vector<Product>& products);

} // namespace ts2::content
