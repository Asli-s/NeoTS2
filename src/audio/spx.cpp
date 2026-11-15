#include "audio/spx.hpp"

#include <speex/speex.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace ts2::audio {

namespace {

constexpr std::size_t kHeaderSize = 15;
// Cap on the decoded size so a bad header can't ask for all the memory.
constexpr std::size_t kMaxDecodedBytes = 64u << 20;

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | p[1] << 8);
}

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("SPX: " + message);
}

} // namespace

Pcm decodeSpx(std::span<const std::uint8_t> data) {
    if (data.size() < kHeaderSize) {
        fail("resource is smaller than its header");
    }
    if (data[0] != 'S' || data[1] != 'P' || data[2] != 'X' || data[3] != '1') {
        fail("missing SPX1 signature");
    }
    const std::uint8_t channels = data[4];
    const std::uint32_t pcmBytes = le32(&data[5]);
    const std::uint32_t mode = le32(&data[9]);
    const std::uint16_t frameSamples = le16(&data[13]);
    if (channels != 1) {
        fail(std::to_string(channels) + " channels");
    }
    if (pcmBytes > kMaxDecodedBytes) {
        fail("decoded size is too large");
    }
    if (mode > SPEEX_MODEID_UWB) {
        fail("unknown Speex mode " + std::to_string(mode));
    }

    const SpeexMode* speexMode = speex_lib_get_mode(static_cast<int>(mode));
    std::unique_ptr<void, void (*)(void*)> decoder(speex_decoder_init(speexMode), speex_decoder_destroy);
    if (!decoder) {
        fail("the decoder could not be made");
    }
    int frameSize = 0, rate = 0;
    speex_decoder_ctl(decoder.get(), SPEEX_GET_FRAME_SIZE, &frameSize);
    speex_decoder_ctl(decoder.get(), SPEEX_GET_SAMPLING_RATE, &rate);
    // The game's decoder runs without the perceptual enhancer, which Speex turns on by default.
    int enhance = 0;
    speex_decoder_ctl(decoder.get(), SPEEX_SET_ENH, &enhance);
    if (frameSize <= 0 || (frameSamples != 0 && frameSamples != frameSize)) {
        fail("frame size " + std::to_string(frameSamples) + " is not the mode's " + std::to_string(frameSize));
    }

    SpeexBits bits;
    speex_bits_init(&bits);
    std::unique_ptr<SpeexBits, void (*)(SpeexBits*)> bitsGuard(&bits, speex_bits_destroy);

    Pcm pcm;
    pcm.sampleRate = static_cast<std::uint32_t>(std::max(rate, 0));
    pcm.channels = channels;
    pcm.samples.reserve(pcmBytes / sizeof(std::int16_t));
    std::vector<spx_int16_t> frame(static_cast<std::size_t>(frameSize));
    std::size_t at = kHeaderSize;
    while (at < data.size() && pcm.samples.size() * sizeof(std::int16_t) < pcmBytes) {
        const std::size_t length = data[at++];
        if (length == 0 || at + length > data.size()) {
            fail("truncated frame at " + std::to_string(at - 1));
        }
        speex_bits_read_from(&bits, reinterpret_cast<const char*>(data.data() + at), static_cast<int>(length));
        at += length;
        // A frame that doesn't decode still gives its samples, as in the game, which plays on.
        (void)speex_decode_int(decoder.get(), &bits, frame.data());
        for (const spx_int16_t sample : frame) {
            pcm.samples.push_back(static_cast<std::int16_t>(sample));
        }
    }
    // The header's size is the real length: a long file is cut, a short one keeps what it has.
    const std::size_t wanted = pcmBytes / sizeof(std::int16_t);
    if (wanted < pcm.samples.size()) {
        pcm.samples.resize(wanted);
    }
    return pcm;
}

} // namespace ts2::audio
