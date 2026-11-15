#pragma once

#include "audio/xa.hpp"

#include <cstdint>
#include <span>

namespace ts2::audio {

// The game's MP3 music, as the sound packages store it (MPEG layer III, no ID3 tag). Throws
// std::runtime_error on bad data.
Pcm decodeMp3(std::span<const std::uint8_t> data);

} // namespace ts2::audio
