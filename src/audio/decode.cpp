#include "audio/decode.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace ts2::audio {

namespace {

std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 | static_cast<std::uint32_t>(b[at + 2]) << 16 |
           static_cast<std::uint32_t>(b[at + 3]) << 24;
}

std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8);
}

bool startsWith(std::span<const std::uint8_t> b, std::string_view tag) {
    return b.size() >= tag.size() && std::equal(tag.begin(), tag.end(), b.begin(), [](char c, std::uint8_t x) { return static_cast<std::uint8_t>(c) == x; });
}

bool sensible(int rate, int channels) { return rate >= 4000 && rate <= 192000 && channels >= 1 && channels <= 2; }

} // namespace

std::optional<Pcm> decodeClip(std::span<const std::uint8_t> bytes) {
    if (startsWith(bytes, "XA")) {
        return decodeXa(bytes);
    }
    if (startsWith(bytes, "SPX1")) {
        return decodeSpx(bytes);
    }
    if (startsWith(bytes, "RIFF")) {
        return decodeWav(bytes);
    }
    if (startsWith(bytes, "ID3") || (bytes.size() >= 2 && bytes[0] == 0xFF && (bytes[1] & 0xE0) == 0xE0)) {
        return decodeMp3(bytes);
    }
    return std::nullopt;
}

// Maxis XA: a 24-byte header ("XA", two bytes that vary, the decoded size in bytes, then a WAVE
// format: tag 1, channels, rate, byte rate, block align, 16 bits), then blocks of 15 bytes per
// channel: first each channel's head byte, which picks its filter (high nibble) and shift (low
// nibble plus 8), then 14 bytes of each channel taken in turn (left, right, left, ...), each
// holding two 4-bit samples, high nibble first: 28 samples a channel.
std::optional<Pcm> decodeXa(std::span<const std::uint8_t> bytes) {
    constexpr std::size_t kHeader = 24, kBlock = 15, kSamplesPerBlock = 28;
    if (bytes.size() < kHeader || !startsWith(bytes, "XA")) {
        return std::nullopt;
    }
    Pcm pcm;
    pcm.channels = u16(bytes, 10);
    pcm.rate = static_cast<int>(u32(bytes, 12));
    if (!sensible(pcm.rate, pcm.channels) || u16(bytes, 22) != 16) {
        return std::nullopt;
    }
    const auto channels = static_cast<std::size_t>(pcm.channels);
    const std::size_t blocks = (bytes.size() - kHeader) / (kBlock * channels);
    const std::size_t declared = u32(bytes, 4) / (2 * channels);
    const std::size_t frames = std::min({blocks * kSamplesPerBlock, declared > 0 ? declared : blocks * kSamplesPerBlock, kMaxFrames});
    pcm.samples.resize(frames * channels);
    static constexpr std::array<int, 4> kC0{0, 240, 460, 392}, kC1{0, 0, -208, -220};
    std::array<int, 2> previous1{}, previous2{};
    std::size_t at = kHeader;
    for (std::size_t block = 0; block < blocks; ++block, at += kBlock * channels) {
        for (std::size_t c = 0; c < channels; ++c) {
            const std::uint8_t head = bytes[at + c];
            const int filter = (head >> 4) & 3;  // the table has four filters; a broken clip's higher index wraps
            const int shift = (head & 0x0F) + 8;
            for (std::size_t i = 0; i < kSamplesPerBlock; ++i) {
                const std::uint8_t pair = bytes[at + channels + (i / 2) * channels + c];
                const int nibble = (i % 2 == 0) ? pair >> 4 : pair & 0x0F;
                const auto widened = static_cast<std::int32_t>(static_cast<std::uint32_t>(nibble) << 28);
                // The scaled nibble and the prediction are summed, then the whole sum is scaled down.
                const std::int64_t sum = static_cast<std::int64_t>(widened >> shift) +
                                         static_cast<std::int64_t>(previous1[c]) * kC0[static_cast<std::size_t>(filter)] +
                                         static_cast<std::int64_t>(previous2[c]) * kC1[static_cast<std::size_t>(filter)] + 128;
                const int sample = static_cast<int>(std::clamp<std::int64_t>(sum >> 8, -32768, 32767));
                previous2[c] = previous1[c];
                previous1[c] = sample;
                const std::size_t frame = block * kSamplesPerBlock + i;
                if (frame < frames) {
                    pcm.samples[frame * channels + c] = static_cast<std::int16_t>(sample);
                }
            }
        }
    }
    return pcm;
}

// A RIFF WAVE holding 16-bit PCM (format 1). Its chunks are walked to find "fmt " and "data".
std::optional<Pcm> decodeWav(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12 || !startsWith(bytes, "RIFF") || !std::equal(bytes.begin() + 8, bytes.begin() + 12, "WAVE")) {
        return std::nullopt;
    }
    Pcm pcm;
    bool format = false;
    std::size_t at = 12;
    while (at + 8 <= bytes.size()) {
        const std::uint32_t size = u32(bytes, at + 4);
        const std::size_t body = at + 8;
        if (size > bytes.size() - body) {
            break;
        }
        if (std::equal(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(at) + 4, "fmt ") && size >= 16) {
            if (u16(bytes, body) != 1 || u16(bytes, body + 14) != 16) {
                return std::nullopt;
            }
            pcm.channels = u16(bytes, body + 2);
            pcm.rate = static_cast<int>(u32(bytes, body + 4));
            format = sensible(pcm.rate, pcm.channels);
        } else if (std::equal(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(at) + 4, "data") && format) {
            const std::size_t frames = std::min<std::size_t>(size / (2 * static_cast<std::size_t>(pcm.channels)), kMaxFrames);
            pcm.samples.resize(frames * static_cast<std::size_t>(pcm.channels));
            for (std::size_t i = 0; i < pcm.samples.size(); ++i) {
                pcm.samples[i] = static_cast<std::int16_t>(u16(bytes, body + 2 * i));
            }
            return pcm;
        }
        at = body + size + (size & 1);
    }
    return std::nullopt;
}

} // namespace ts2::audio
