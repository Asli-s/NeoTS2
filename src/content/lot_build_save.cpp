#include "content/lot_build_save.hpp"

#include "diag/log.hpp"
#include "formats/fencepostlayer.hpp"
#include "formats/lot_grid.hpp"
#include "formats/qfs.hpp"
#include "formats/resource_types.hpp"
#include "formats/stringmap.hpp"
#include "formats/walllayer.hpp"
#include "formats/wallgraph.hpp"

#include <algorithm>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ts2::content {

namespace {

std::vector<std::uint8_t> bodyOf(const dbpf::StoredResource& resource) {
    return resource.compressed ? qfs::decompress(resource.bytes) : resource.bytes;
}

// The grid record with its cells replaced by `elements`, or none when the record isn't a grid the
// writer knows or its cells are another size.
std::optional<std::vector<std::uint8_t>> gridWith(const dbpf::StoredResource& resource, const std::vector<std::uint8_t>& elements) {
    auto grid = lotgrid::readGrid(bodyOf(resource));
    if (!grid || grid->cells.size() != elements.size()) {
        return std::nullopt;
    }
    grid->cells = elements;
    return lotgrid::writeGrid(*grid);
}

// The record the lot holds for `resource` now, none for one this save leaves as it is.
std::optional<std::vector<std::uint8_t>> buildRecord(const dbpf::StoredResource& resource, const Lot& lot) {
    const auto& key = resource.key;
    if (key.type == types::kWallGraph) {
        const auto graph = lot.wallLayers.find(key.instance);
        if (graph != lot.wallLayers.end()) {
            return wallgraph::write(graph->second);
        }
        // One the wall manager keeps but the lot couldn't read: the others would no longer match it.
        if (std::ranges::find(kWallLayerInstances, key.instance) != std::end(kWallLayerInstances)) {
            throw std::runtime_error("wall graph " + std::to_string(key.instance) + " wasn't read");
        }
        return std::nullopt;
    }
    if (key.type == types::kWallLayer && key.instance == kWallCoveringLayerInstance) {
        return walllayer::write(lot.savedWallCoverings);
    }
    if (key.type == types::kPatternMap && key.instance == kWallPatternMapInstance) {
        return stringmap::write(lot.wallPatterns);
    }
    if (key.type == types::kPatternMap && key.instance == kFloorPatternMapInstance) {
        return stringmap::write(lot.floorPatterns);
    }
    if (key.type == types::kFencePostLayer) {
        return fencepostlayer::write(lot.fencePosts);
    }
    if (key.type == types::kArray3D) {
        const auto grid = lot.grids.find(key.instance);
        return grid != lot.grids.end() ? gridWith(resource, grid->second.elements) : std::nullopt;
    }
    if (key.type == types::kArray2D && key.instance != kByteListGridInstance) {
        const auto grid = lot.flatGrids.find(key.instance);
        return grid != lot.flatGrids.end() ? gridWith(resource, grid->second.elements) : std::nullopt;
    }
    return std::nullopt;
}

} // namespace

int putBuildRecords(dbpf::PackageImage& package, const Lot& lot) {
    std::vector<std::pair<std::size_t, dbpf::StoredResource>> written;
    try {
        for (std::size_t i = 0; i < package.resources.size(); ++i) {
            if (const auto record = buildRecord(package.resources[i], lot)) {
                written.emplace_back(i, dbpf::storeResource(package.resources[i].key, *record));
            }
        }
    } catch (const std::exception& ex) {
        diag::warning(std::string("save: the lot's build data is kept as it was loaded: ") + ex.what());
        return 0;
    }
    for (auto& [i, stored] : written) {
        package.resources[i] = std::move(stored);
    }
    return static_cast<int>(written.size());
}

} // namespace ts2::content
