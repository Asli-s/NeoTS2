#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ts2::settings {

// What the launcher remembers between starts. Every path is absolute.
struct LauncherSettings {
    std::filesystem::path install;     // the chosen install, empty for the first one found
    std::filesystem::path saveFolder;  // the chosen save folder, empty for the install's first
    std::vector<std::filesystem::path> addedInstalls;     // installs the player picked by hand
    std::vector<std::filesystem::path> addedSaveFolders;  // save folders the player picked by hand
    bool backupReminderSeen = false;  // the player was reminded to back up their saves and went on

    bool operator==(const LauncherSettings&) const = default;
};

// Reads the settings file's text: a version line, then one "key=value" line each, values in UTF-8.
// Unknown keys and unusable values are skipped. Nullopt when the text isn't a settings file this
// version can read.
std::optional<LauncherSettings> parseLauncherSettings(std::string_view text);

std::string formatLauncherSettings(const LauncherSettings& settings);

struct LoadedSettings {
    LauncherSettings settings;
    std::string problem;  // why the file was set aside, for the player. Empty when it was read or missing.
};

// Reads the settings file. A missing file gives the defaults. A file that can't be read gives the
// defaults with a problem, and the next save replaces it.
LoadedSettings loadLauncherSettings(const std::filesystem::path& file);

// Writes the settings file in one step: a temporary file beside it is written, then put in its
// place, so a stop part way never leaves half a file. False when it couldn't be written.
bool saveLauncherSettings(const std::filesystem::path& file, const LauncherSettings& settings);

} // namespace ts2::settings
