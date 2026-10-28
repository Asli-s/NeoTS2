#include "content/user_props.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    try {
        const auto props = ts2::content::parseUserProps(text);
        (void)props.flag("rotateCameraSims1Mode");
    } catch (const std::exception&) {
    }
    return 0;
}
