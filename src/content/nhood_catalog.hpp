#pragma once

#include "content/install.hpp"

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
};

// The neighborhood object catalog (TSData/Res/Catalog/CANHObjects of every product) by guid, later
// products overriding earlier ones.
std::map<std::uint32_t, NeighborhoodObjectType> neighborhoodObjectTypes(const std::vector<Product>& products);

} // namespace ts2::content
