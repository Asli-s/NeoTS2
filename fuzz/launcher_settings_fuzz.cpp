#include "settings/launcher_settings.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    if (const auto settings = ts2::settings::parseLauncherSettings(text)) {
        ts2::settings::formatLauncherSettings(*settings);
    }
    return 0;
}
