#include "content/video_capture.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>
#include <string>

namespace ts2::content {

namespace {

std::optional<unsigned long> number(const UserProps& saved, const UserProps& defaults, const std::string& key) {
    for (const UserProps* props : {&saved, &defaults}) {
        const auto it = props->values.find(key);
        if (it == props->values.end()) {
            continue;
        }
        unsigned long v = 0;
        const auto& text = it->second;
        if (std::from_chars(text.data(), text.data() + text.size(), v).ec == std::errc{}) {
            return v;
        }
    }
    return std::nullopt;
}

// The longest recording kept to, so a broken setting can't ask for a file without end.
constexpr unsigned long kMostSeconds = 3600;

} // namespace

VideoCaptureSettings videoCaptureSettings(const UserProps& saved, const UserProps& defaults) {
    VideoCaptureSettings out;
    switch (number(saved, defaults, "videoCaptureSize").value_or(1)) {
    case 0: out.width = out.height = 128; break;
    case 2: out.width = 640, out.height = 480; break;
    default: out.width = 320, out.height = 240; break;
    }
    switch (number(saved, defaults, "videoCaptureQuality").value_or(1)) {
    case 0: out.fps = 10, out.jpegQuality = 50; break;
    case 2: out.fps = 30, out.jpegQuality = 85; break;
    case 3: out.fps = 30, out.jpegQuality = 95; break;
    default: out.fps = 15, out.jpegQuality = 75; break;
    }
    out.maxSeconds = static_cast<std::uint32_t>(std::clamp(number(saved, defaults, "videoCaptureMaxRecordingSeconds").value_or(60), 1ul, kMostSeconds));
    out.sound = saved.flag("videoCaptureAudioOn").value_or(defaults.flag("videoCaptureAudioOn").value_or(true));
    return out;
}

std::filesystem::path nextMovieFile(const std::filesystem::path& folder) {
    std::error_code ec;
    for (int n = 1; n < 100000; ++n) {
        auto path = folder / std::format("movie{:04}.avi", n);
        if (!std::filesystem::exists(path, ec)) {
            return path;
        }
    }
    return folder / "movie.avi";
}

} // namespace ts2::content
