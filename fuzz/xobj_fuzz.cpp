#include "formats/xobj.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2) {
        return 0;
    }
    const auto kind = static_cast<ts2::xobj::RecordKind>(data[1] % 3);
    try {
        ts2::xobj::parse(std::span<const std::uint8_t>(data + 2, size - 2), data[0], kind);
    } catch (const std::exception&) {
    }
    return 0;
}
