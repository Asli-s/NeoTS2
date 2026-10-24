// Putting images on hardware whose textures are powers of two between a smallest and a largest side
// (the Dreamcast, the PlayStation 2, and the OpenGL libraries for them). Nothing here depends on a graphics API.
#pragma once

#include "scene/scene.hpp"

namespace ts2::render {

struct TextureLimits {
    unsigned maxSide = 1024;
    unsigned minSide = 8;
};

// Whether an image already has sides the hardware takes.
[[nodiscard]] bool fitsTexture(unsigned width, unsigned height, const TextureLimits& limits);

// The side an image is stretched to: the power of two nearest to `side` (by ratio), between the limits.
[[nodiscard]] unsigned fitSide(unsigned side, const TextureLimits& limits);

// How an image that is never repeated (clamped at its edges) goes into a texture. It is padded up to the next
// power of two when it fits the limits that way, so it needn't be resampled, and then it fills only the share
// `u` by `v` of the texture, which its texture coordinates are multiplied by. Otherwise (too large) it is
// stretched to the nearest powers of two, and fills the texture (u = v = 1).
struct ClampedFit {
    unsigned width = 0, height = 0;  // the texture's sides
    bool stretch = false;
    float u = 1, v = 1;
};
[[nodiscard]] ClampedFit fitClamped(unsigned width, unsigned height, const TextureLimits& limits);

// The image resampled to width x height: areas averaged where it shrinks, interpolated where it grows. It has
// none of the original's smaller levels.
[[nodiscard]] scene::Image stretched(const scene::Image& image, unsigned width, unsigned height);

// The image in the top left of a larger one, the edge texels repeated into the rest so that filtering at its
// edge picks up nothing else.
[[nodiscard]] scene::Image padded(const scene::Image& image, unsigned width, unsigned height);

// An image whose colours were multiplied by its alpha, with them divided by it again, so that it can be blended
// by its alpha (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) where the hardware can't blend premultiplied colours.
[[nodiscard]] scene::Image unpremultiplied(const scene::Image& image);

struct QuantizeOptions {
    unsigned iterations = 0;  // k-means passes (on a sample of the pixels) to improve the palette median cut makes
    bool dither = false;      // spread each pixel's error over the next ones (Floyd-Steinberg), in the colours, not the alpha
};

// The image with no more than `colours` different colours (alpha counted as a channel): a palette by median cut,
// where the pixels are split into boxes by their widest channel until there are enough and each box's average is
// a colour, then improved by `iterations` passes of k-means, and each pixel takes the nearest colour (spreading
// its error with `dither`). Without either, each pixel just takes its box's average. This is how much a palette
// keeps of a picture. It has none of the original's smaller levels.
[[nodiscard]] scene::Image quantized(const scene::Image& image, unsigned colours, const QuantizeOptions& options = {});

// How far apart two images of one size are: the root of the mean squared difference of their channels, in levels (0 to 255).
[[nodiscard]] double imageError(const scene::Image& a, const scene::Image& b);

} // namespace ts2::render
