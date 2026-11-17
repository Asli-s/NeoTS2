#include "formats/rcol.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    try {
        // What reads must write back to the bytes read.
        const auto resource = ts2::rcol::read(std::span<const std::uint8_t>(data, size));
        const auto written = ts2::rcol::write(resource);
        if (written.size() != size || !std::equal(written.begin(), written.end(), data)) {
            std::abort();
        }
    } catch (const std::exception&) {
    }
    return 0;
}
