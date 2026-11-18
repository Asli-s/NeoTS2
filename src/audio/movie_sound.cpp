#include "audio/movie_sound.hpp"

#include <algorithm>

namespace ts2::audio {

namespace {

constexpr std::size_t kFrameBytes = 15;
constexpr std::size_t kFrameSamples = 28;
// The largest decoded sound accepted, so a bad file cannot ask for all the memory it likes. The
// longest game movie decodes to about 6 MB.
constexpr std::size_t kMaxDecodedSamples = 32u << 20;

// The game's table has four predictor pairs. A frame naming another is broken.
constexpr int kCoef[4][2] = {{0, 0}, {240, 0}, {460, -208}, {392, -220}};

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 |
           static_cast<std::uint32_t>(p[3]) << 24;
}

// One channel's frame: the header byte (predictor in the high nibble, shift in the low), then 14
// bytes of two samples, high nibble first. No rounding before the shift, as the game decodes.
bool decodeFrame(const std::uint8_t* frame, int history[2], std::int16_t* out, std::size_t stride) {
    const int predictor = frame[0] >> 4;
    if (predictor > 3) {
        return false;
    }
    const int c1 = kCoef[predictor][0], c2 = kCoef[predictor][1];
    const int shift = (frame[0] & 0x0F) + 8;
    for (std::size_t i = 1; i <= 14; ++i) {
        for (const int nibble : {frame[i] >> 4, frame[i] & 0x0F}) {
            const int residual = static_cast<std::int32_t>(static_cast<std::uint32_t>(nibble) << 28) >> shift;
            const int sample = std::clamp((residual + history[0] * c1 + history[1] * c2) >> 8, -32768, 32767);
            history[1] = history[0];
            history[0] = sample;
            *out = static_cast<std::int16_t>(sample);
            out += stride;
        }
    }
    return true;
}

} // namespace

Pcm decodeMovieSound(std::span<const std::uint8_t> file, const movie::Sound& sound) {
    Pcm pcm;
    if (sound.channels < 1 || sound.channels > 2) {
        return pcm;
    }
    pcm.sampleRate = sound.sampleRate;
    pcm.channels = sound.channels;
    const std::size_t channels = sound.channels;
    int history[2][2] = {};
    for (const movie::Chunk& chunk : sound.blocks) {
        if (chunk.offset > file.size() || chunk.size > file.size() - chunk.offset) {
            break;
        }
        const std::uint8_t* block = file.data() + chunk.offset;
        const std::size_t head = 4 + 4 * channels;
        if (chunk.size < head) {
            break;
        }
        // Each channel takes the bytes between its offset and the next one's (or the block's end).
        // The game reads them one after another from the first.
        const std::size_t first = le32(block + 4);
        const std::size_t next = channels == 2 ? le32(block + 8) : chunk.size - head;
        if (next <= first || next - first < 4) {
            break;
        }
        const std::size_t bytes = next - first - 4;
        const std::size_t frames = bytes / kFrameBytes;
        if (channels * (4 + bytes) > chunk.size - head) {
            break;
        }
        if (pcm.samples.size() + frames * kFrameSamples * channels > kMaxDecodedSamples) {
            break;
        }
        const std::size_t at = pcm.samples.size();
        pcm.samples.resize(at + frames * kFrameSamples * channels);
        bool broken = false;
        for (std::size_t c = 0; c < channels && !broken; ++c) {
            const std::uint8_t* data = block + head + c * (4 + bytes) + 4;
            for (std::size_t f = 0; f < frames && !broken; ++f) {
                broken = !decodeFrame(data + f * kFrameBytes, history[c], pcm.samples.data() + at + f * kFrameSamples * channels + c, channels);
            }
        }
        if (broken) {
            pcm.samples.resize(at);
            break;
        }
    }
    return pcm;
}

} // namespace ts2::audio
