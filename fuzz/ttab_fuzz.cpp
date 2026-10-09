#include "formats/ttab.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    try {
        ts2::ttab::parse(std::span<const std::uint8_t>(data, size));
    } catch (const std::exception&) {
    }
    return 0;
}
