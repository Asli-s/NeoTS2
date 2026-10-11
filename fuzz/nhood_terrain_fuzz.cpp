#include "formats/nhood_terrain.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    try {
        ts2::nhood::parseTerrain(bytes);
    } catch (const std::exception&) {
    }
    try {
        ts2::nhood::parseSavedCamera(bytes);
    } catch (const std::exception&) {
    }
    return 0;
}
