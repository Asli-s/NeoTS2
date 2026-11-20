#include "sim/game_wide_save.hpp"

#include "content/save_writer.hpp"
#include "diag/log.hpp"
#include "formats/dbpf_writer.hpp"
#include "formats/ngbh.hpp"
#include "formats/qfs.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

namespace ts2::sim {

namespace {

constexpr dbpf::ResourceKey kGameWideKey{ngbh::kGameWideType, ngbh::kGameWideGroup, ngbh::kGameWideInstance, 0};

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

const dbpf::StoredResource* findRecord(const dbpf::PackageImage& image) {
    const auto it = std::ranges::find_if(image.resources, [](const auto& r) { return r.key == kGameWideKey; });
    return it != image.resources.end() ? &*it : nullptr;
}

std::vector<std::uint8_t> bodyOf(const dbpf::StoredResource& resource) {
    return resource.compressed ? qfs::decompress(resource.bytes) : resource.bytes;
}

} // namespace

std::optional<Inventory> gameWideFromPackage(std::span<const std::uint8_t> package) {
    const dbpf::PackageImage image = dbpf::readImage(package);
    const dbpf::StoredResource* record = findRecord(image);
    if (record == nullptr) {
        return std::nullopt;
    }
    const auto saved = ngbh::readGameWideRecord(bodyOf(*record));
    if (!saved) {
        return std::nullopt;
    }
    return toInventory(*saved);
}

std::vector<std::uint8_t> packageWithGameWide(std::span<const std::uint8_t> package, const Inventory& inventory) {
    dbpf::PackageImage image = dbpf::readImage(package);
    auto stored = dbpf::storeResource(kGameWideKey, ngbh::writeGameWideRecord(toSavedInventory(inventory)));
    const auto same = std::ranges::find_if(image.resources, [](const auto& r) { return r.key == kGameWideKey; });
    if (same != image.resources.end()) {
        *same = std::move(stored);
    } else {
        image.dataOrder.push_back(image.resources.size());
        image.resources.push_back(std::move(stored));
    }
    return dbpf::writeImage(image);
}

GameWideStore::GameWideStore(const std::filesystem::path& userFolder)
    : package_(userFolder / "Neighborhoods" / "NeighborhoodManager.package") {
    std::error_code error;
    if (userFolder.empty() || !std::filesystem::is_regular_file(package_, error)) {
        return;
    }
    try {
        const std::vector<std::uint8_t> file = readFile(package_);
        const dbpf::PackageImage image = dbpf::readImage(file);
        const dbpf::StoredResource* record = findRecord(image);
        if (record == nullptr) {
            writable_ = true;
        } else if (const auto saved = ngbh::readGameWideRecord(bodyOf(*record))) {
            *inventory_ = toInventory(*saved);
            writable_ = true;
        } else {
            diag::warning("the game-wide inventory in NeighborhoodManager.package can't be read, so it starts empty and is not saved");
            return;
        }
        loaded_ = ngbh::writeGameWideRecord(toSavedInventory(*inventory_));
    } catch (const std::runtime_error& e) {
        diag::warning(std::string("NeighborhoodManager.package can't be read, so the game-wide inventory starts empty and is not saved: ") + e.what());
        writable_ = false;
    }
}

void GameWideStore::save(const paths::SaveFolderLock* lock) const {
    if (lock == nullptr || !lock->held() || !writable_) {
        return;
    }
    try {
        if (ngbh::writeGameWideRecord(toSavedInventory(*inventory_)) == loaded_) {
            return;
        }
        // Read anew, so whatever else the package holds is kept as it is now.
        const std::vector<std::uint8_t> file = readFile(package_);
        content::SaveWriter writer(*lock);
        writer.add(package_, packageWithGameWide(file, *inventory_), content::SaveTarget::Replace);
        writer.commit();
        diag::info("saved the game-wide inventory");
    } catch (const std::runtime_error& e) {
        diag::warning(std::string("the game-wide inventory can't be saved: ") + e.what());
    }
}

} // namespace ts2::sim
