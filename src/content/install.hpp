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

// The products as the game numbers them: the base game, then the packs in the order of its
// "EPsInstalled" list. Folders that aren't known packs are left out.
std::vector<Product> inGameOrder(const std::vector<Product>& products);

// The game's number for a product: 0 for the base game, then each pack's place in its
// "EPsInstalled" list, from 1. -1 for a folder that isn't a known pack.
int gameNumber(const Product& product);

// The newest product's copy of a file under TSData (e.g. "Sys/NeighborhoodTerrain.ini"). Empty when
// no product has one.
[[nodiscard]] std::optional<std::filesystem::path> newestDataFile(const std::vector<Product>& products, const std::filesystem::path& relative);

} // namespace ts2::content
