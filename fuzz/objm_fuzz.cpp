#include "formats/objm.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    try {
        ts2::objm::parse(bytes);
    } catch (const std::exception&) {
    }
    try {
        ts2::objm::parseGroups(bytes);
    } catch (const std::exception&) {
    }
    // A record the writer reads writes back to the very same bytes.
    const std::vector<std::uint8_t> input(bytes.begin(), bytes.end());
    if (const auto map = ts2::objm::readMapRecord(bytes); map && ts2::objm::writeMapRecord(*map) != input) {
        std::abort();
    }
    if (const auto groups = ts2::objm::readGroupsRecord(bytes); groups && ts2::objm::writeGroupsRecord(*groups) != input) {
        std::abort();
    }
    return 0;
}
