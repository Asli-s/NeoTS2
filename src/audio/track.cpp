#include "audio/track.hpp"

#include "parse_float.hpp"

#include <charconv>
#include <cmath>
#include <string_view>

namespace ts2::audio {

namespace {

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

// A setting key written as hex ("0xff53dbf3"), none when it is not one.
std::optional<std::uint32_t> hexKey(std::string_view text) {
    if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    const char* end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data() + 2, end, value, 16);
    return error == std::errc() && stop == end ? std::optional(value) : std::nullopt;
}

// A finite number, none for anything else.
std::optional<double> number(std::string_view text) {
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    const std::optional<double> value = parseLeadingDouble(text);
    if (!value || !std::isfinite(*value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> find(const std::map<std::string, std::string>& properties, std::string_view key) {
    const auto it = properties.find(std::string(key));
    return it == properties.end() ? std::nullopt : number(it->second);
}

// A comma list of numbers, such as "10,16,28". Stops at the first word that is not a number.
std::vector<double> list(const std::map<std::string, std::string>& properties, std::string_view key) {
    std::vector<double> out;
    const auto it = properties.find(std::string(key));
    if (it == properties.end()) {
        return out;
    }
    std::string_view text = it->second;
    // The longest list the tuning has is the per group limits, 27 numbers.
    constexpr std::size_t kMaxNumbers = 64;
    while (!text.empty() && out.size() < kMaxNumbers) {
        const std::size_t comma = text.find(',');
        const auto value = number(text.substr(0, comma));
        if (!value) {
            break;
        }
        out.push_back(*value);
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    return out;
}

// A distance or scale kept to a range a file cannot break the mixer with.
float clampedFloat(double value, double low, double high) {
    return static_cast<float>(value < low ? low : value > high ? high : value);
}

std::uint32_t clampedWord(double value) {
    return value <= 0 ? 0 : value >= 4294967295.0 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(value);
}

} // namespace

TrackSettings trackSettings(const std::map<std::string, std::string>& properties) {
    TrackSettings out;
    double volume = 1024, volume2 = 1024;
    for (const auto& [name, text] : properties) {
        const auto key = hexKey(name);
        const auto value = number(text);
        if (!key || !value) {
            continue;
        }
        switch (*key) {
        case track_key::kLooping: out.looping = *value != 0; break;
        case track_key::kPositional: out.positional = *value != 0; break;
        case track_key::kMinDistance: out.minDistance = clampedFloat(*value, 0, 1e6); break;
        case track_key::kMaxDistance: out.maxDistance = clampedFloat(*value, 0, 1e6); break;
        case track_key::kPriority: out.priority = clampedFloat(*value, 0, 1e9); break;
        case track_key::kVolume: volume = *value; break;
        case track_key::kVolume2: volume2 = *value; break;
        case track_key::kFadeOut: out.fadeOutMs = clampedWord(*value); break;
        case track_key::kPlayChance: out.playChance = clampedWord(*value); break;
        case track_key::kInstanceMode: out.instanceMode = clampedWord(*value); break;
        case track_key::kRolloffScale: out.rolloffScale = clampedFloat(*value, 0, 1e6); break;
        case track_key::kKeepPlaying: out.keepPlaying = *value != 0; break;
        default: break;
        }
    }
    if (out.rolloffScale == 0) {
        out.rolloffScale = 1;
    }
    // A volume above 16 times full is a broken file, not a loud track.
    out.volume = clampedFloat(volume / 1024 * (volume2 / 1024), 0, 16);
    return out;
}

std::vector<SampleKey> hitListSamples(std::span<const std::uint8_t> data) {
    std::vector<SampleKey> out;
    if (data.size() < 8) {
        return out;
    }
    const std::size_t count = le32(&data[4]);
    // Each entry is two words. A count past the resource is a broken file, not a long list.
    if (count > (data.size() - 8) / 8) {
        return out;
    }
    const std::size_t kept = count < kMaxHitListSamples ? count : kMaxHitListSamples;
    out.reserve(kept);
    for (std::size_t i = 0; i < kept; ++i) {
        const std::uint8_t* entry = &data[8 + i * 8];
        out.push_back({le32(entry), le32(entry + 4)});
    }
    return out;
}

void readManagerTuning(const std::map<std::string, std::string>& properties, Tuning& tuning) {
    const auto levels = list(properties, "MaxPlayingSoundsPerPerformanceLevel");
    for (std::size_t i = 0; i < levels.size() && i < tuning.maxSounds.size(); ++i) {
        tuning.maxSounds[i] = static_cast<int>(clampedFloat(levels[i], -1, 4096));
    }
    const auto byGroup = list(properties, "MaxPlayingSoundsPerPerformanceLevelByGroup");
    for (std::size_t i = 0; i < byGroup.size() && i < tuning.maxSoundsByGroup.size() * 3; ++i) {
        tuning.maxSoundsByGroup[i / 3][i % 3] = static_cast<int>(clampedFloat(byGroup[i], -1, 4096));
    }
    const auto pitch = list(properties, "SimSpeedPitchMultipliers");
    for (std::size_t i = 0; i < pitch.size() && i < tuning.speedPitch.size(); ++i) {
        tuning.speedPitch[i] = clampedFloat(pitch[i], 0, 8);
    }
    const auto volume = list(properties, "SimSpeedVolumeMultipliers");
    for (std::size_t i = 0; i < volume.size() && i < tuning.speedVolume.size(); ++i) {
        tuning.speedVolume[i] = clampedFloat(volume[i], 0, 8);
    }
    if (const auto v = find(properties, "SecondaryListenerInfluence")) {
        tuning.secondaryListenerInfluence = clampedFloat(*v, 0, 1);
    }
    if (const auto v = find(properties, "RolloffAspectRatio")) {
        tuning.rolloffAspectRatio = clampedFloat(*v, 0, 1e6);
    }
    if (const auto v = find(properties, "ListenerAttenuationPositionsScalar")) {
        tuning.attenuationPositionsScalar = clampedFloat(*v, -1e3, 1e3);
    }
}

void readListenerTuning(const std::map<std::string, std::string>& properties, Tuning& tuning) {
    if (const auto v = find(properties, "rolloff")) {
        tuning.rolloff = clampedFloat(*v, 0, 1e6);
    }
    if (const auto v = find(properties, "listenerminheight")) {
        tuning.listenerMinHeight = clampedFloat(*v, -1e3, 1e3);
    }
    if (const auto v = find(properties, "listenertocameraoffset")) {
        tuning.listenerToCameraOffset = clampedFloat(*v, -1e3, 1e3);
    }
}

} // namespace ts2::audio
