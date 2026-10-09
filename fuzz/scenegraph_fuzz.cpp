#include "formats/scenegraph.hpp"

#include <cstddef>
#include <cstdint>
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
    attempt([&] { sg::parseSceneObject(input); });
    attempt([&] {
        const auto image = sg::parseImage(input);
        for (const auto& mip : image.mips) {
            if (!mip.data.empty() && image.width <= 1024 && image.height <= 1024) {
                sg::decodeRgba(mip.data, image.width, image.height, image.format);
            }
        }
    });
    return 0;
}
