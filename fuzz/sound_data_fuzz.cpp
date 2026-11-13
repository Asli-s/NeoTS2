#include "audio/audio_config.hpp"
#include "audio/names.hpp"
#include "audio/track.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <span>
#include <string>
#include <string_view>

// Reads any bytes as each of the small sound files: a TSAudio configuration, a hit list, a
// modifier list and the tuning and track settings' values. None may keep more than its limit.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const auto segments = ts2::audio::packedSegments(text);
    if (segments.size() > ts2::audio::kMaxSegments) {
        std::abort();
    }
    for (const auto& path : segments) {
        if (path.empty() || path.front() == '/' || path.find("..") != std::string::npos) {
            std::abort();
        }
    }
    if (ts2::audio::hitListSamples(std::span<const std::uint8_t>(data, size)).size() > ts2::audio::kMaxHitListSamples) {
        std::abort();
    }
    (void)ts2::audio::modifierMask(text);
    const std::size_t half = size / 2;
    const std::map<std::string, std::string> values{
        {"MaxPlayingSoundsPerPerformanceLevel", std::string(text.substr(0, half))},
        {"MaxPlayingSoundsPerPerformanceLevelByGroup", std::string(text.substr(half))},
        {"SimSpeedPitchMultipliers", std::string(text)},
        {"rolloff", std::string(text.substr(half))},
        {std::string(text.substr(0, half)), std::string(text.substr(half))},
        {"0xffaab623", std::string(text)},
        {"0xff53dbf3", std::string(text.substr(0, half))},
    };
    ts2::audio::Tuning tuning;
    ts2::audio::readManagerTuning(values, tuning);
    ts2::audio::readListenerTuning(values, tuning);
    const ts2::audio::TrackSettings track = ts2::audio::trackSettings(values);
    if (!(track.volume >= 0 && track.volume <= 16) || !(track.minDistance >= 0) || !(track.maxDistance >= 0)) {
        std::abort();
    }
    return 0;
}
