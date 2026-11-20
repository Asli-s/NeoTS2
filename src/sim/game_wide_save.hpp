#pragma once

#include "paths/save_folders.hpp"
#include "sim/inventory.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ts2::sim {

// The game-wide inventory record (ngbh::kGameWideType) of a NeighborhoodManager.package file's
// bytes. Empty when the package has none, or one of another version. Throws std::runtime_error for
// a broken package.
std::optional<Inventory> gameWideFromPackage(std::span<const std::uint8_t> package);

// The package with its game-wide inventory record replaced by `inventory`, or added when it has
// none, every other resource kept. Throws std::runtime_error for a broken package.
std::vector<std::uint8_t> packageWithGameWide(std::span<const std::uint8_t> package, const Inventory& inventory);

// The game-wide inventory kept between launches, as the game keeps it in the user folder's
// Neighborhoods/NeighborhoodManager.package. The game reads it as it starts and writes it when it
// quits. Every lot played meanwhile shares the one inventory.
class GameWideStore {
public:
    // Reads it from the user folder. A missing package or record gives an empty inventory.
    explicit GameWideStore(const std::filesystem::path& userFolder);

    const std::shared_ptr<Inventory>& inventory() const { return inventory_; }

    // Writes it back when it changed since it was read. Nothing is written without the save
    // folder's lock, without the package (the game makes it, NeoTS2 does not), or when the record
    // there could not be read, so that it is never lost. Problems are logged, not thrown.
    void save(const paths::SaveFolderLock* lock) const;

private:
    std::filesystem::path package_;
    std::shared_ptr<Inventory> inventory_ = std::make_shared<Inventory>();
    std::vector<std::uint8_t> loaded_;  // the record as read, written anew to compare with
    bool writable_ = false;
};

} // namespace ts2::sim
