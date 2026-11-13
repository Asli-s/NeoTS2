#include "formats/array3d.hpp"
#include "formats/lot_grid.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    try {
        ts2::array3d::parse(bytes);
    } catch (const std::exception&) {
    }
    // A grid the writer reads writes back to the very same bytes.
    if (const auto grid = ts2::lotgrid::readGrid(bytes);
        grid && ts2::lotgrid::writeGrid(*grid) != std::vector<std::uint8_t>(bytes.begin(), bytes.end())) {
        std::abort();
    }
    return 0;
}
