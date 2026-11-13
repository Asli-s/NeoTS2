#include "formats/str.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    try {
        ts2::str::parse(bytes);
    } catch (const std::exception&) {
    }
    // A table the writer reads writes back to the very same bytes.
    if (const auto table = ts2::str::readRecord(bytes);
        table && ts2::str::writeRecord(*table) != std::vector<std::uint8_t>(bytes.begin(), bytes.end())) {
        std::abort();
    }
    return 0;
}
