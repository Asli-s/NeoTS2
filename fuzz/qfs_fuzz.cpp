#include "formats/qfs.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> input(data, size);
    if (!ts2::qfs::isCompressed(input)) {
        return 0;
    }
    try {
        ts2::qfs::decompress(input);
    } catch (const std::exception&) {
    }
    return 0;
}
