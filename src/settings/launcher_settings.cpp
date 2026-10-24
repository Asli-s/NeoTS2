#include "settings/launcher_settings.hpp"

#include "paths/names.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace ts2::settings {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kVersionLine = "NeoTS2 launcher settings 1";
// The file holds a few paths. The limits stop a broken or huge file from taking memory or time.
constexpr std::uintmax_t kMaxFileBytes = 64 * 1024;
constexpr std::size_t kMaxLineBytes = 4096;
constexpr std::size_t kMaxAdded = 64;

// A path from UTF-8 text, when it is a usable absolute path.
std::optional<fs::path> pathFrom(std::string_view text) {
    if (text.empty() || text.size() > kMaxLineBytes) {
        return std::nullopt;
    }
    auto path = paths::pathFromUtf8(text);
    if (!path || !path->is_absolute()) {
        return std::nullopt;
    }
    return path;
}

void addOnce(std::vector<fs::path>& list, const fs::path& path) {
    if (list.size() < kMaxAdded && std::find(list.begin(), list.end(), path) == list.end()) {
        list.push_back(path);
    }
}

} // namespace

std::optional<LauncherSettings> parseLauncherSettings(std::string_view text) {
    if (text.size() > kMaxFileBytes) {
        return std::nullopt;
    }
    LauncherSettings settings;
    bool first = true;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (first) {
            if (line != kVersionLine) {
                return std::nullopt;
            }
            first = false;
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const std::string_view key = line.substr(0, equals);
        if (key == "backup-reminder") {
            settings.backupReminderSeen = line.substr(equals + 1) == "seen";
            continue;
        }
        if (key == "renderer") {
            const std::string_view value = line.substr(equals + 1);
            if (value == "sdlgpu" || value == "gl2") {
                settings.renderer = value;
            }
            continue;
        }
        const auto path = pathFrom(line.substr(equals + 1));
        if (!path) {
            continue;
        }
        if (key == "install") {
            settings.install = *path;
        } else if (key == "save-folder") {
            settings.saveFolder = *path;
        } else if (key == "added-install") {
            addOnce(settings.addedInstalls, *path);
        } else if (key == "added-save-folder") {
            addOnce(settings.addedSaveFolders, *path);
        }
    }
    if (first) {
        return std::nullopt;
    }
    return settings;
}

std::string formatLauncherSettings(const LauncherSettings& settings) {
    std::string out(kVersionLine);
    out += '\n';
    // Only what parseLauncherSettings reads back is written, so a long list never makes a file
    // the launcher then refuses. Paths past the limits are left out.
    const auto line = [&out](std::string_view key, const fs::path& path) {
        std::u8string text;
        try {
            text = path.u8string();
        } catch (const std::exception&) {
            return;  // a name with no UTF-8 form
        }
        const std::size_t size = key.size() + 2 + text.size();
        if (path.empty() || text.size() > kMaxLineBytes || out.size() + size > kMaxFileBytes) {
            return;
        }
        out.append(key).append("=").append(text.begin(), text.end()).append("\n");
    };
    line("install", settings.install);
    line("save-folder", settings.saveFolder);
    if (settings.backupReminderSeen) {
        out += "backup-reminder=seen\n";
    }
    if (settings.renderer == "sdlgpu" || settings.renderer == "gl2") {
        out.append("renderer=").append(settings.renderer).append("\n");
    }
    for (std::size_t i = 0; i < settings.addedInstalls.size() && i < kMaxAdded; ++i) {
        line("added-install", settings.addedInstalls[i]);
    }
    for (std::size_t i = 0; i < settings.addedSaveFolders.size() && i < kMaxAdded; ++i) {
        line("added-save-folder", settings.addedSaveFolders[i]);
    }
    return out;
}

LoadedSettings loadLauncherSettings(const fs::path& file) {
    LoadedSettings loaded;
    std::error_code error;
    const fs::file_type type = fs::symlink_status(file, error).type();
    if (type == fs::file_type::not_found) {
        return loaded;
    }
    const std::uintmax_t size = type == fs::file_type::regular ? fs::file_size(file, error) : 0;
    std::string text;
    if (type == fs::file_type::regular && !error && size <= kMaxFileBytes) {
        text.resize(static_cast<std::size_t>(size));
        std::ifstream in(file, std::ios::binary);
        if (!in.read(text.data(), static_cast<std::streamsize>(text.size()))) {
            text.clear();
        }
    }
    if (const auto settings = parseLauncherSettings(text)) {
        loaded.settings = *settings;
    } else {
        loaded.problem = "The launcher's settings couldn't be read, so it started with its defaults.";
    }
    return loaded;
}

bool saveLauncherSettings(const fs::path& file, const LauncherSettings& settings) {
    try {
        std::error_code error;
        fs::create_directories(file.parent_path(), error);
        fs::path temporary = file;
        temporary += ".new";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            const std::string text = formatLauncherSettings(settings);
            if (!out.write(text.data(), static_cast<std::streamsize>(text.size())) || !out.flush()) {
                return false;
            }
        }
        fs::rename(temporary, file, error);
        if (error) {
            fs::remove(temporary, error);
            return false;
        }
        return true;
    } catch (const std::exception&) {
        return false;  // a path with no UTF-8 form, for one
    }
}

} // namespace ts2::settings
