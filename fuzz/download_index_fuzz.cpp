#include "content/download_index.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    try {
        // Whatever reads must write and read back the same.
        const auto packages = ts2::content::readDownloadIndex(bytes);
        const auto again = ts2::content::writeDownloadIndex(packages);
        if (ts2::content::readDownloadIndex(again).size() != packages.size()) {
            std::abort();
        }
    } catch (const std::exception&) {
    }
    return 0;
}
