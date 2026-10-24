#include "formats/scenegraph.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>

namespace sg = ts2::scenegraph;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> input(data, size);
    auto attempt = [](auto parse) {
        try {
            parse();
        } catch (const std::exception&) {
        }
    };
    attempt([&] { sg::parseMesh(input); });
    attempt([&] { sg::parseShape(input); });
    attempt([&] { sg::parseMaterial(input); });
    attempt([&] { sg::parseLevelInfo(input); });
    attempt([&] { sg::parseResourceNode(input); });
    attempt([&] { sg::parseDataLists(input); });
    attempt([&] { sg::parseSceneObject(input); });
    attempt([&] { sg::parseLight(input); });
    attempt([&] {
        const auto image = sg::parseImage(input);
        for (const auto& mip : image.mips) {
            if (!mip.data.empty() && image.width <= 1024 && image.height <= 1024) {
                // Decoded twice: different pixels mean memory read before it was written.
                if (sg::decodeRgba(mip.data, image.width, image.height, image.format) !=
                    sg::decodeRgba(mip.data, image.width, image.height, image.format)) {
                    std::abort();
                }
            }
        }
    });
    return 0;
}
