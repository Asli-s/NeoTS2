#include "formats/terrain.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    try {
        ts2::terrain::parseLotTexture(bytes);
    } catch (const std::exception&) {
    }
    try {
        ts2::terrain::parseArray2D(bytes);
    } catch (const std::exception&) {
    }
    return 0;
}
