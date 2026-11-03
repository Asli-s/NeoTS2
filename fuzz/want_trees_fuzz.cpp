#include "formats/want_trees.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    try {
        ts2::want_trees::parse(std::string_view(reinterpret_cast<const char*>(data), size));
    } catch (const std::exception&) {
    }
    return 0;
}
