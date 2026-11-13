#include "audio/decode.hpp"

#include "dr_mp3.h"

#include <memory>

namespace ts2::audio {

// The game's music and some sound clips: MP3 frames, sometimes after an ID3 tag. dr_mp3 skips the tag.
std::optional<Pcm> decodeMp3(std::span<const std::uint8_t> bytes) {
    drmp3_config config{};
    drmp3_uint64 frames = 0;
    drmp3_int16* decoded = drmp3_open_memory_and_read_pcm_frames_s16(bytes.data(), bytes.size(), &config, &frames, nullptr);
    if (decoded == nullptr) {
        return std::nullopt;
    }
    const std::unique_ptr<drmp3_int16, void (*)(drmp3_int16*)> owned(decoded, [](drmp3_int16* p) { drmp3_free(p, nullptr); });
    if (config.channels < 1 || config.channels > 2 || config.sampleRate < 4000 || config.sampleRate > 192000 || frames == 0) {
        return std::nullopt;
    }
    if (frames > kMaxFrames) {
        frames = kMaxFrames;
    }
    Pcm pcm;
    pcm.rate = static_cast<int>(config.sampleRate);
    pcm.channels = static_cast<int>(config.channels);
    pcm.samples.assign(decoded, decoded + static_cast<std::size_t>(frames) * config.channels);
    return pcm;
}

} // namespace ts2::audio
