#include "formats/terrain.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    try {
        // What the writer gives reads back to the same values, so it writes the same bytes again.
        const std::vector<std::uint8_t> written = ts2::terrain::writeLotTexture(ts2::terrain::parseLotTexture(bytes));
        if (ts2::terrain::writeLotTexture(ts2::terrain::parseLotTexture(written)) != written) {
            std::abort();
        }
    } catch (const std::exception&) {
    }
    try {
        ts2::terrain::parseArray2D(bytes);
    } catch (const std::exception&) {
    }
    return 0;
}
