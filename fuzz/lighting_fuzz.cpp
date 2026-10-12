#include "content/lighting.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

// The light settings reader on any text: macros that create themselves or each other, open
// defines and ifs, odd brackets and values.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);
    const auto lighting = ts2::content::readLightingText(text);
    for (const auto& [name, light] : lighting.lights) {
        (void)lighting.light(name);
    }
    return 0;
}
