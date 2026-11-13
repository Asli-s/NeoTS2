#include "formats/dbpf.hpp"
#include "formats/dbpf_writer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> input(data, size);
    try {
        ts2::dbpf::Package package(input);
        for (const auto& entry : package.entries()) {
            try {
                package.read(entry);
            } catch (const std::exception&) {
            }
        }
    } catch (const std::exception&) {
    }

    // A package the save writer reads must write back to one that passes its check and writes back
    // to the very same bytes again.
    ts2::dbpf::PackageImage image;
    try {
        image = ts2::dbpf::readImage(input);
    } catch (const std::exception&) {
        return 0;
    }
    try {
        const std::vector<std::uint8_t> written = ts2::dbpf::writeImage(image);
        ts2::dbpf::verifyImage(written, image);
        if (ts2::dbpf::writeImage(ts2::dbpf::readImage(written)) != written) {
            std::abort();
        }
    } catch (const std::exception&) {
        std::abort();
    }
    return 0;
}
