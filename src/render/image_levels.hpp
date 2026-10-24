// Mip levels of an image, for the renderers that upload them.
#pragma once

#include "scene/scene.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ts2::render {

// An image's own smaller levels are uploaded when it has them all the way down. Otherwise the
// GPU makes them.
inline bool ownLevels(const scene::Image& image) {
    std::uint32_t w = image.width, h = image.height;
    for (const auto& level : image.smallerLevels) {
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
        if (level.size() != static_cast<std::size_t>(w) * h * 4) {
            return false;
        }
    }
    return !image.smallerLevels.empty() && w == 1 && h == 1;
}

// Whether all of an image's pixels are there (a broken one is drawn plain white instead).
inline bool complete(const scene::Image& image) {
    return image.width > 0 && image.height > 0 && image.rgba.size() == static_cast<std::size_t>(image.width) * image.height * 4;
}

} // namespace ts2::render
