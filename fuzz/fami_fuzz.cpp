#include "formats/fami.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    try {
        // What the writer gives reads back to the same values, so it writes the same bytes again.
        const std::vector<std::uint8_t> written = ts2::fami::write(ts2::fami::parse(std::span<const std::uint8_t>(data, size)));
        if (ts2::fami::write(ts2::fami::parse(written)) != written) {
            std::abort();
        }
    } catch (const std::exception&) {
    }
    // A history the writer reads writes back to the very same bytes.
    if (const auto history = ts2::fami::readHistory(std::span<const std::uint8_t>(data, size));
        history && ts2::fami::writeHistory(*history) != std::vector<std::uint8_t>(data, data + size)) {
        std::abort();
    }
    return 0;
}
