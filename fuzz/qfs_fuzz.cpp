#include "formats/qfs.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>
#include <vector>

// Any bytes compress and unpack to themselves, and any compressed stream that unpacks does so again
// after it is compressed anew.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > ts2::qfs::kMaxInput) {
        return 0;
    }
    const std::span<const std::uint8_t> input(data, size);
    if (ts2::qfs::decompress(ts2::qfs::compress(input)) != std::vector<std::uint8_t>(input.begin(), input.end())) {
        std::abort();
    }
    if (!ts2::qfs::isCompressed(input)) {
        return 0;
    }
    std::vector<std::uint8_t> unpacked;
    try {
        unpacked = ts2::qfs::decompress(input);
    } catch (const std::exception&) {
        return 0;
    }
    if (ts2::qfs::decompress(ts2::qfs::compress(unpacked)) != unpacked) {
        std::abort();
    }
    return 0;
}
