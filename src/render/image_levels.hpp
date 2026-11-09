// Mip levels of an image, for the renderers that upload them.
#pragma once

#include "formats/scenegraph.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>

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

// An image with its pixels, for uploading it uncompressed: itself, or its largest DXT level decoded
// into `decoded` when it brings only blocks (High lot imposter pictures). A broken one comes back as
// it is.
inline const scene::Image& withPixels(const scene::Image& image, scene::Image& decoded) {
    if (complete(image) || image.blockLevels.empty()) {
        return image;
    }
    try {
        decoded = scene::Image{image.width, image.height, scenegraph::decodeRgba(image.blockLevels.front(), image.width, image.height, image.blockFormat), {}, 0, {}};
    } catch (const std::exception&) {
        return image;
    }
    return decoded;
}

} // namespace ts2::render
