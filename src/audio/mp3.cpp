#include "audio/mp3.hpp"

#define MINIMP3_IMPLEMENTATION
#include <minimp3_ex.h>

#include <stdexcept>

namespace ts2::audio {

namespace {
// Cap on the decoded size so a bad file can't ask for all the memory. Checked before decoding.
constexpr std::uint64_t kMaxDecodedBytes = 256u << 20;

struct Decoder {
    mp3dec_ex_t dec{};
    ~Decoder() { mp3dec_ex_close(&dec); }
};
} // namespace

Pcm decodeMp3(std::span<const std::uint8_t> data) {
    Decoder decoder;
    if (mp3dec_ex_open_buf(&decoder.dec, data.data(), data.size(), MP3D_SEEK_TO_SAMPLE) != 0 || decoder.dec.samples == 0) {
        throw std::runtime_error("MP3: not a decodable stream");
    }
    const mp3dec_ex_t& dec = decoder.dec;
    if (dec.samples * sizeof(mp3d_sample_t) > kMaxDecodedBytes || dec.info.channels <= 0 || dec.info.hz <= 0) {
        throw std::runtime_error("MP3: unusable stream");
    }
    Pcm pcm;
    pcm.sampleRate = static_cast<std::uint32_t>(dec.info.hz);
    pcm.channels = static_cast<std::uint16_t>(dec.info.channels);
    pcm.samples.resize(static_cast<std::size_t>(dec.samples));
    pcm.samples.resize(mp3dec_ex_read(&decoder.dec, pcm.samples.data(), pcm.samples.size()));
    if (pcm.samples.empty()) {
        throw std::runtime_error("MP3: not a decodable stream");
    }
    return pcm;
}

} // namespace ts2::audio
