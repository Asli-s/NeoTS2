#include "formats/nhood_paint_rules.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    try {
        ts2::nhood::parsePaintRules(text);
    } catch (const std::exception&) {
    }
    return 0;
}
