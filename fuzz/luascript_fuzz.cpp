#include "formats/luascript.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    try {
        const auto script = ts2::luascript::parse(std::span<const std::uint8_t>(data, size));
        ts2::luascript::nativeChunk(script.chunk);
    } catch (const std::exception&) {
    }
    return 0;
}
