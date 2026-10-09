#include "formats/dbpf.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    try {
        ts2::dbpf::Package package(std::span<const std::uint8_t>(data, size));
        for (const auto& entry : package.entries()) {
            try {
                package.read(entry);
            } catch (const std::exception&) {
            }
        }
    } catch (const std::exception&) {
    }
    return 0;
}
