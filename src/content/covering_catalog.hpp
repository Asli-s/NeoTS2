#pragma once

#include "content/covering_colors.hpp"
#include "content/install.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ts2::content {

// A wall or floor covering as Build mode's catalog lists it, from its pattern property set (XOBJ
// resource 0xCCA8E925 in each product's Catalog/Patterns packages).
struct Covering {
    CoveringKind kind = CoveringKind::Floor;
    std::uint32_t guid = 0;
    std::string subsort;  // the catalog button's name: "brick", "lino", "paint" and so on
    std::string name, description;
    std::string filename;  // the catalog lists coverings by this, ignoring case
    int cost = 0;
};

// The coverings the catalog shows, in its order: by file name, ignoring case. A later product's
// covering takes the place of an earlier one with the same GUID. Those with showincatalog 0 (pool
// siding and the like) or deprecated set are left out, and so is anything that can't be read.
std::vector<Covering> loadCoverings(const std::vector<Product>& products);

// The name a covering's cached catalog picture is filed under: its GUID as 8 lower-case hex digits.
std::string coveringPictureName(std::uint32_t guid);

// A half wall as the 1/2 Walls catalog lists it: a fence property set (XFNC resource 0x2CB230B8 in
// each product's Catalog/Patterns packages) with ishalfwall set. Its name, description and price
// are strings 0, 1 and 2 of the string table the set names (stringsetgroupid, stringsetid).
struct HalfWall {
    std::uint32_t guid = 0;
    std::string name, description;
    int price = 0;
};

// The half walls the catalog shows, in its order: the newest product's first, and within a
// package in the order their property sets' data lies in the file. Anything that can't be read is
// left out.
std::vector<HalfWall> loadHalfWalls(const std::vector<Product>& products);

} // namespace ts2::content
