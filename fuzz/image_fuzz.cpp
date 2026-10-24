#include "formats/image.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <optional>
#include <span>

namespace {

std::optional<ts2::tga::Pixels> decoded(std::span<const std::uint8_t> bytes) {
    try {
        return ts2::image::decode(bytes);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace

// Decoded twice: different pixels mean memory read before it was written (Windows has no
// sanitizer for that).
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    const auto first = decoded(bytes), second = decoded(bytes);
    if (first.has_value() != second.has_value() ||
        (first && (first->width != second->width || first->height != second->height || first->rgba != second->rgba))) {
        std::abort();
    }
    return 0;
}
