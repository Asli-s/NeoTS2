#include "formats/stringmap.hpp"

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
        const std::vector<std::uint8_t> written = ts2::stringmap::write(ts2::stringmap::parse(bytes));
        if (ts2::stringmap::write(ts2::stringmap::parse(written)) != written) {
            std::abort();
        }
    } catch (const std::exception&) {
    }
    return 0;
}
