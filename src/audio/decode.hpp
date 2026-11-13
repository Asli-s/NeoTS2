#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ts2::audio {

// Sound decoded to 16-bit samples, channels interleaved.
struct Pcm {
    int rate = 0;
    int channels = 0;
    std::vector<std::int16_t> samples;

    [[nodiscard]] std::size_t frames() const { return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0; }
};

// A clip (resource type 0x2026960B) of the game's sound packages, by what its bytes start with:
// "XA" Maxis XA ADPCM, "SPX1" Speex, MP3 frames (with or without an ID3 tag) or a RIFF WAVE of
// 16-bit PCM. Nothing for anything else or a clip that doesn't decode. Clips are untrusted:
// every size and count is checked, and a decoded clip is at most kMaxFrames frames long.
std::optional<Pcm> decodeClip(std::span<const std::uint8_t> bytes);

// Each kind on its own.
std::optional<Pcm> decodeXa(std::span<const std::uint8_t> bytes);
std::optional<Pcm> decodeSpx(std::span<const std::uint8_t> bytes);
std::optional<Pcm> decodeMp3(std::span<const std::uint8_t> bytes);
std::optional<Pcm> decodeWav(std::span<const std::uint8_t> bytes);

// Ten minutes at 48 kHz: longer than any of the game's clips or songs.
constexpr std::size_t kMaxFrames = std::size_t{48000} * 600;

} // namespace ts2::audio
