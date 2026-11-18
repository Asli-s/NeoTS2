#pragma once

#include "audio/xa.hpp"
#include "formats/movie.hpp"

#include <cstdint>
#include <span>

namespace ts2::audio {

// Decodes a movie's sound (its SCDl blocks) to 16-bit samples, interleaved when there are two
// channels. Each block holds a sample count, one offset per channel, then each channel's data in
// turn: two starting samples the game skips, and 15-byte frames of 28 samples. The decoder's
// history runs on from block to block. A broken block ends the sound there. No samples when the
// movie has no sound.
[[nodiscard]] Pcm decodeMovieSound(std::span<const std::uint8_t> file, const movie::Sound& sound);

} // namespace ts2::audio
