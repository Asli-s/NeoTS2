#include "audio/audio_config.hpp"

#include <cctype>

namespace ts2::audio {

namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// The path a segment line names below %DataDirectory%, empty when it names none.
std::string segmentPath(std::string_view line) {
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
        return {};
    }
    const std::string_view value = line.substr(equals + 1);
    const std::size_t semicolon = value.find(';');
    if (semicolon == std::string_view::npos) {
        return {};
    }
    std::string path = lower(trim(value.substr(semicolon + 1)));
    constexpr std::string_view kData = "%datadirectory%";
    if (!path.starts_with(kData) || path.find("%language%") != std::string::npos) {
        return {};
    }
    path.erase(0, kData.size());
    for (char& c : path) {
        if (c == '\\') {
            c = '/';
        }
    }
    // A path that climbs out of the data folder is not a sound package.
    if (path.empty() || path.front() == '/' || path.find("..") != std::string::npos) {
        return {};
    }
    return path;
}

} // namespace

std::vector<std::string> packedSegments(std::string_view ini) {
    std::vector<std::string> out;
    bool inSection = false;
    while (!ini.empty() && out.size() < kMaxSegments) {
        const std::size_t end = ini.find('\n');
        const std::string_view line = trim(ini.substr(0, end));
        ini.remove_prefix(end == std::string_view::npos ? ini.size() : end + 1);
        if (line.empty() || line.front() == ';') {
            continue;
        }
        if (line.front() == '[') {
            inSection = lower(line) == "[packeddbsegments]";
            continue;
        }
        if (inSection) {
            if (std::string path = segmentPath(line); !path.empty()) {
                out.push_back(std::move(path));
            }
        }
    }
    return out;
}

} // namespace ts2::audio
