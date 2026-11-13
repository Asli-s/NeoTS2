#include "formats/fencepostlayer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    try {
        // What the writer gives reads back to the same values, so it writes the same bytes again.
        const std::vector<std::uint8_t> written = ts2::fencepostlayer::write(ts2::fencepostlayer::parse(std::span<const std::uint8_t>(data, size)));
        if (ts2::fencepostlayer::write(ts2::fencepostlayer::parse(written)) != written) {
            std::abort();
        }
    } catch (const std::exception&) {
    }
    return 0;
}
