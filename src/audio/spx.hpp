#pragma once

#include "audio/xa.hpp"

#include <cstdint>
#include <span>

namespace ts2::audio {

// The game's Speex voices (SPX1): a 15-byte header (signature, channels, decoded size, mode, samples per
// frame), then each frame's length and bytes. Throws std::runtime_error on bad data.
Pcm decodeSpx(std::span<const std::uint8_t> data);

} // namespace ts2::audio
