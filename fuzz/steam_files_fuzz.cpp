#include "paths/steam_files.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    ts2::paths::steamLibraryFolders(text);
    ts2::paths::steamInstallFolder(text);
    return 0;
}
