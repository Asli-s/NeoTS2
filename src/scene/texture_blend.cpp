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

} // namespace ts2::scene::textureblend
