#include "audio/decode.hpp"

#include <speex/speex.h>

#include <algorithm>
#include <array>
#include <memory>

namespace ts2::audio {

namespace {

std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 | static_cast<std::uint32_t>(b[at + 2]) << 16 |
           static_cast<std::uint32_t>(b[at + 3]) << 24;
}

struct DecoderDeleter {
    void operator()(void* state) const { speex_decoder_destroy(state); }
};

struct Bits {
    SpeexBits bits{};
    Bits() { speex_bits_init(&bits); }
    ~Bits() { speex_bits_destroy(&bits); }
    Bits(const Bits&) = delete;
    Bits& operator=(const Bits&) = delete;
};

} // namespace

// The game's Speex clips: "SPX1", a version byte (1), a 32-bit count the game keeps (not needed to
// decode), the 32-bit Speex mode (0 narrowband 8 kHz, 1 wideband 16 kHz, 2 ultra-wideband 32 kHz) and
// the 16-bit frame size in samples, then packets of a length byte and that many bytes of one Speex
// frame, to the end. Mono.
std::optional<Pcm> decodeSpx(std::span<const std::uint8_t> bytes) {
    constexpr std::size_t kHeader = 15;
    if (bytes.size() < kHeader || bytes[4] != 1) {
        return std::nullopt;
    }
    const std::uint32_t mode = u32(bytes, 9);
    const SpeexMode* speexMode = nullptr;
    int rate = 0;
    switch (mode) {
    case 0: speexMode = &speex_nb_mode; rate = 8000; break;
    case 1: speexMode = &speex_wb_mode; rate = 16000; break;
    case 2: speexMode = &speex_uwb_mode; rate = 32000; break;
    default: return std::nullopt;
    }
    std::unique_ptr<void, DecoderDeleter> decoder(speex_decoder_init(speexMode));
    if (!decoder) {
        return std::nullopt;
    }
    int frameSize = 0;
    speex_decoder_ctl(decoder.get(), SPEEX_GET_FRAME_SIZE, &frameSize);
    int enhance = 1;
    speex_decoder_ctl(decoder.get(), SPEEX_SET_ENH, &enhance);
    if (frameSize <= 0 || frameSize > 2048) {
        return std::nullopt;
    }
    Pcm pcm;
    pcm.rate = rate;
    pcm.channels = 1;
    Bits bits;
    std::array<spx_int16_t, 2048> frame{};
    std::array<char, 256> packet{};
    std::size_t at = kHeader;
    while (at < bytes.size() && pcm.samples.size() + static_cast<std::size_t>(frameSize) <= kMaxFrames) {
        const std::size_t length = bytes[at++];
        if (length == 0 || length > bytes.size() - at) {
            break;
        }
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(at), length, reinterpret_cast<std::uint8_t*>(packet.data()));
        at += length;
        speex_bits_read_from(&bits.bits, packet.data(), static_cast<int>(length));
        if (speex_decode_int(decoder.get(), &bits.bits, frame.data()) != 0) {
            break;
        }
        pcm.samples.insert(pcm.samples.end(), frame.begin(), frame.begin() + frameSize);
    }
    if (pcm.samples.empty()) {
        return std::nullopt;
    }
    return pcm;
}

} // namespace ts2::audio
