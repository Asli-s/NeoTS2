#include "formats/cursor.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    try {
        (void)ts2::cursor::decode(std::span<const std::uint8_t>(data, size));
        (void)ts2::cursor::decodeAnimated(std::span<const std::uint8_t>(data, size));
    } catch (const std::exception&) {
    }
    return 0;
}
