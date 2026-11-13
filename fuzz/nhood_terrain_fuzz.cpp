#include "formats/nhood_terrain.hpp"
#include "formats/saved_camera.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    try {
        ts2::nhood::parseTerrain(bytes);
    } catch (const std::exception&) {
    }
    try {
        (void)ts2::nhood::parseSavedCamera(bytes);
    } catch (const std::exception&) {
    }
    // A camera record the writer reads writes back to the very same bytes.
    if (const auto record = ts2::savedcamera::readRecord(bytes);
        record && ts2::savedcamera::writeRecord(*record) != std::vector<std::uint8_t>(bytes.begin(), bytes.end())) {
        std::abort();
    }
    // So does a terrain record.
    if (const auto terrain = ts2::nhood::readTerrainRecord(bytes);
        terrain && ts2::nhood::writeTerrainRecord(*terrain) != std::vector<std::uint8_t>(bytes.begin(), bytes.end())) {
        std::abort();
    }
    return 0;
}
