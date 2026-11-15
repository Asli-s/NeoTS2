#include "audio/mp3.hpp"

#define MINIMP3_IMPLEMENTATION
#include <minimp3_ex.h>

#include <cstdlib>
#include <memory>
#include <stdexcept>

namespace ts2::audio {

namespace {
// Cap on the decoded size so a bad file can't ask for all the memory.
constexpr std::size_t kMaxDecodedBytes = 256u << 20;
}

Pcm decodeMp3(std::span<const std::uint8_t> data) {
    mp3dec_t decoder;
    mp3dec_file_info_t info{};
    if (mp3dec_load_buf(&decoder, data.data(), data.size(), &info, nullptr, nullptr) != 0 || info.buffer == nullptr) {
        std::free(info.buffer);
        throw std::runtime_error("MP3: not a decodable stream");
    }
    const std::unique_ptr<mp3d_sample_t, decltype(&std::free)> buffer(info.buffer, &std::free);
    if (info.samples * sizeof(mp3d_sample_t) > kMaxDecodedBytes || info.channels <= 0 || info.hz <= 0) {
        throw std::runtime_error("MP3: unusable stream");
    }
    Pcm pcm;
    pcm.sampleRate = static_cast<std::uint32_t>(info.hz);
    pcm.channels = static_cast<std::uint16_t>(info.channels);
    pcm.samples.assign(buffer.get(), buffer.get() + info.samples);
    return pcm;
}

} // namespace ts2::audio
