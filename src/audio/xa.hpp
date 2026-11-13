#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ts2::audio {

// Decoded 16-bit samples, interleaved when there are two channels.
struct Pcm {
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    std::vector<std::int16_t> samples;

    double seconds() const {
        return sampleRate != 0 && channels != 0 ? static_cast<double>(samples.size()) / channels / sampleRate : 0;
    }
};

// The game's XA ADPCM, as the sound packages store it. Throws std::runtime_error on bad data.
Pcm decodeXa(std::span<const std::uint8_t> data);

} // namespace ts2::audio
