#include "audio/xa.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace ts2::audio {

namespace {

constexpr std::size_t kHeaderSize = 24;
constexpr std::size_t kBlockSamples = 28;
constexpr std::size_t kBlockBytes = 15;
// The largest decoded sound accepted, so a bad header cannot ask for all the memory it likes.
constexpr std::size_t kMaxDecodedBytes = 64u << 20;

constexpr int kCoef[4][2] = {{0, 0}, {240, 0}, {460, -208}, {392, -220}};

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | p[1] << 8);
}

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

std::int16_t clip16(int value) {
    return static_cast<std::int16_t>(std::clamp(value, -32768, 32767));
}

// One channel's 15-byte block: the header byte, then 14 bytes of two nibbles, high first.
void decodeBlock(const std::uint8_t* header, const std::uint8_t* nibbles, std::size_t stride, int& current, int& previous,
                 std::int16_t* out, std::size_t outStride) {
    const int predictor = (*header >> 4) & 3;
    const int c1 = kCoef[predictor][0];
    const int c2 = kCoef[predictor][1];
    const int shift = (*header & 0x0F) + 8;
    for (std::size_t i = 0; i < 14; ++i) {
        const std::uint8_t byte = nibbles[i * stride];
        for (const int nibble : {byte >> 4, byte & 0x0F}) {
            const int residual = static_cast<std::int32_t>(static_cast<std::uint32_t>(nibble) << 28) >> shift;
            const int sample = clip16((residual + current * c1 + previous * c2 + 0x80) >> 8);
            previous = current;
            current = sample;
            *out = static_cast<std::int16_t>(sample);
            out += outStride;
        }
    }
}

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("XA: " + message);
}

} // namespace

Pcm decodeXa(std::span<const std::uint8_t> data) {
    if (data.size() < kHeaderSize) {
        fail("resource is smaller than its header");
    }
    if (data[0] != 'X' || data[1] != 'A') {
        fail("missing XA signature");
    }
    const std::uint32_t pcmBytes = le32(&data[4]);
    const std::uint16_t channels = le16(&data[10]);
    const std::uint32_t sampleRate = le32(&data[12]);
    const std::uint16_t bits = le16(&data[22]);
    if (le16(&data[8]) != 1) {
        fail("not PCM");
    }
    if (channels < 1 || channels > 2) {
        fail("unsupported channel count " + std::to_string(channels));
    }
    if (bits != 16) {
        fail("unsupported sample size " + std::to_string(bits));
    }
    if (sampleRate == 0 || sampleRate > 192000) {
        fail("unsupported sample rate " + std::to_string(sampleRate));
    }
    if (pcmBytes > kMaxDecodedBytes || (pcmBytes & 1) != 0) {
        fail("bad decoded size " + std::to_string(pcmBytes));
    }

    const std::size_t blockBytes = kBlockBytes * channels;
    const std::size_t body = data.size() - kHeaderSize;
    const std::size_t blocks = (body + blockBytes - 1) / blockBytes;
    const std::size_t wanted = pcmBytes / 2;  // samples over all channels

    Pcm pcm;
    pcm.sampleRate = sampleRate;
    pcm.channels = channels;
    pcm.samples.resize(blocks * kBlockSamples * channels);

    std::uint8_t padded[30] = {};
    int current[2] = {0, 0};
    int previous[2] = {0, 0};
    for (std::size_t b = 0; b < blocks; ++b) {
        const std::uint8_t* block = data.data() + kHeaderSize + b * blockBytes;
        if (const std::size_t left = body - b * blockBytes; left < blockBytes) {
            std::memset(padded, 0, sizeof padded);
            std::memcpy(padded, block, left);
            block = padded;
        }
        std::int16_t* out = pcm.samples.data() + b * kBlockSamples * channels;
        for (std::size_t c = 0; c < channels; ++c) {
            decodeBlock(block + c, block + channels + c, channels, current[c], previous[c], out + c, channels);
        }
    }

    // The header's size is what the game plays; the last block may carry more.
    if (wanted < pcm.samples.size()) {
        pcm.samples.resize(wanted - wanted % channels);
    }
    return pcm;
}

} // namespace ts2::audio
