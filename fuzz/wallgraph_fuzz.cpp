#include "formats/wallgraph.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    try {
        // What the writer gives reads back to the same values, so it writes the same bytes again.
        const std::vector<std::uint8_t> written = ts2::wallgraph::write(ts2::wallgraph::parse(std::span<const std::uint8_t>(data, size)));
        if (ts2::wallgraph::write(ts2::wallgraph::parse(written)) != written) {
            std::abort();
        }
    } catch (const std::exception&) {
    }
    return 0;
}
