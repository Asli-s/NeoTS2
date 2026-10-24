#pragma once

#include "formats/tga.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace ts2::reia {

// A neighborhood's moving picture (<hood>_Neighborhood.reia): a RIFF file of type "Reia" with a
// "head" chunk (version 1, width, height, then frames per second as a fraction, frame count) and
// one "frme" chunk per frame. A frame is the picture's 32 x 32 blocks row by row, each a flag byte
// (0: unchanged from the last frame) followed, when set, by its 1024 pixels run-length coded: a byte
// c below 0x80 starts c + 1 pixels written out, a byte c from 0x80 one pixel repeated 257 - c times.
// A pixel is three bytes, blue, green, red, each added (modulo 256) to the picture so far, which
// starts black.
struct Video {
    std::uint32_t width = 0, height = 0;
    std::uint32_t rateNumerator = 0, rateDenominator = 1;  // frames per second
    std::vector<std::span<const std::uint8_t>> frames;     // into the file's bytes
};

// The frames of a video file, with no frames when it is not one.
Video parse(std::span<const std::uint8_t> file);

// Adds a frame to the picture so far (RGBA rows from the top, black when it is not yet sized by the
// video). False when the frame is broken. Frames build on each other, so they go in order.
[[nodiscard]] bool decodeFrame(const Video& video, std::size_t frame, tga::Pixels& picture);

} // namespace ts2::reia
