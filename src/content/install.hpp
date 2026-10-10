#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ts2::content {

struct Product {
    std::string code;
    std::string name;
    int releaseOrder = 0;
    std::vector<std::filesystem::path> objectPackages;
};

std::vector<Product> findProducts(const std::filesystem::path& installRoot);

// Treats every objects.package as a cumulative snapshot, so a larger one is newer. Snapshots load smallest-first, ties in release
// order, and stuff packs' own SPObjects.package files load last.
std::vector<std::filesystem::path> objectLoadOrder(const std::vector<Product>& products);

// The game's mask of installed expansion and stuff packs (what scripts test with
// nGameState.IsEPInstalled): one bit per pack in the order of the game's "EPsInstalled" registry
// list (EP1 = bit 0 ... Mansion & Garden = bit 16), plus bit 12, which the game always sets.
std::uint32_t installedPacksMask(const std::vector<Product>& products);

// The newest product's copy of a file under TSData (e.g. "Sys/NeighborhoodTerrain.ini"); empty when
// no product has one.
std::optional<std::filesystem::path> newestDataFile(const std::vector<Product>& products, const std::filesystem::path& relative);

} // namespace ts2::content
