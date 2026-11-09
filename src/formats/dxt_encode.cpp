#include "formats/dxt_encode.hpp"

#include <algorithm>
#include <cstddef>

#define STB_DXT_IMPLEMENTATION
#define STB_DXT_STATIC
#include "stb_dxt.h"

namespace ts2::dxt {

std::vector<std::uint8_t> compressDxt5(std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height) {
    constexpr std::size_t kBlockBytes = 16;
    if (width == 0 || height == 0 || rgba.size() < std::size_t{width} * height * 4) {
        return {};
    }
    const std::uint32_t blocksX = (width + 3) / 4, blocksY = (height + 3) / 4;
    std::vector<std::uint8_t> out(std::size_t{blocksX} * blocksY * kBlockBytes);
    unsigned char block[16 * 4];
    for (std::uint32_t by = 0; by < blocksY; ++by) {
        for (std::uint32_t bx = 0; bx < blocksX; ++bx) {
            for (std::uint32_t y = 0; y < 4; ++y) {
                const std::uint32_t sy = std::min(by * 4 + y, height - 1);
                for (std::uint32_t x = 0; x < 4; ++x) {
                    const std::uint32_t sx = std::min(bx * 4 + x, width - 1);
                    std::copy_n(rgba.data() + (std::size_t{sy} * width + sx) * 4, 4, block + (y * 4 + x) * 4);
                }
            }
            stb_compress_dxt_block(out.data() + (std::size_t{by} * blocksX + bx) * kBlockBytes, block, 1, STB_DXT_HIGHQUAL);
        }
    }
    return out;
}

} // namespace ts2::dxt
