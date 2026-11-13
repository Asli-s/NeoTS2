#include "formats/nhood_objects.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    try {
        ts2::nhood::parseObjects(bytes);
    } catch (const std::exception&) {
    }
    // A record the writer reads writes back to the very same bytes.
    if (const auto record = ts2::nhood::readObjectsRecord(bytes);
        record && ts2::nhood::writeObjectsRecord(*record) != std::vector<std::uint8_t>(bytes.begin(), bytes.end())) {
        std::abort();
    }
    return 0;
}
