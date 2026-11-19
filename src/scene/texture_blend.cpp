#include "scene/texture_blend.hpp"

namespace ts2::scene::textureblend {

int weight(std::uint8_t alpha) {
    return alpha + (alpha >> 7);
}

std::uint8_t blend(std::uint8_t under, std::uint8_t over, int weight) {
    // under + (over - under) * weight / 256, rounded half up.
    const int d = under, s = over;
    return static_cast<std::uint8_t>(d + (((s - d) * weight + 0x80) >> 8));
}

void drawOver(Image& under, const Image& layer) {
    if (under.width != layer.width || under.height != layer.height || under.rgba.size() != layer.rgba.size()) {
        return;
    }
    for (std::size_t i = 0; i + 3 < under.rgba.size(); i += 4) {
        const int w = weight(layer.rgba[i + 3]);
        for (std::size_t c = 0; c < 3; ++c) {
            under.rgba[i + c] = blend(under.rgba[i + c], layer.rgba[i + c], w);
        }
    }
}

int maskWeight(std::uint8_t maskGreen, std::uint8_t amount) {
    if (amount == 255) {
        return weight(maskGreen);
    }
    const std::uint32_t t = (static_cast<std::uint32_t>(maskGreen) << 8) * amount;
    return static_cast<int>((t >> 23) + (t >> 16));
}

void drawThroughMask(Image& under, const Image& layer, const Image& mask, std::uint8_t amount) {
    if (under.width != layer.width || under.height != layer.height || under.rgba.size() != layer.rgba.size() ||
        mask.rgba.size() != under.rgba.size()) {
        return;
    }
    for (std::size_t i = 0; i + 3 < under.rgba.size(); i += 4) {
        if ((layer.rgba[i] | layer.rgba[i + 1] | layer.rgba[i + 2] | layer.rgba[i + 3]) == 0) {
            continue;
        }
        const int w = maskWeight(mask.rgba[i + 1], amount);
        for (std::size_t c = 0; c < 3; ++c) {
            under.rgba[i + c] = blend(under.rgba[i + c], layer.rgba[i + c], w);
        }
    }
}

} // namespace ts2::scene::textureblend
