#include "formats/face_shape.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    try {
        // What reads must write and read again.
        const auto shape = ts2::faceshape::parse(std::span<const std::uint8_t>(data, size));
        ts2::faceshape::parse(ts2::faceshape::write(shape));
    } catch (const std::exception&) {
    }
    return 0;
}
