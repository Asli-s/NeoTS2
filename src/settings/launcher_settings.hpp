#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ts2::settings {

// The game window's sizes the launcher offers until the game has its own display settings.
struct WindowSize {
    std::string_view key;  // as the settings file keeps it
    int width, height;
    // The window fills the screen's free area (all but the taskbar or dock) instead of taking a size.
    bool fitScreen = false;
};
inline constexpr WindowSize kWindowSizes[] = {
    {"1280x720", 1280, 720}, {"1920x1080", 1920, 1080}, {"2560x1440", 2560, 1440}, {"fit-screen", 0, 0, true}};

// The offered size with this key, null for any other text.
inline const WindowSize* findWindowSize(std::string_view key) {
    for (const auto& size : kWindowSizes) {
        if (size.key == key) {
            return &size;
        }
    }
    return nullptr;
}

// The sizes of the game's UI the launcher offers, in percent. The game draws its UI at 100% only.
inline constexpr int kUiScales[] = {100, 125, 150, 175, 200};

// True for a size in kUiScales.
inline bool isUiScale(int percent) {
    for (const int scale : kUiScales) {
        if (scale == percent) {
            return true;
        }
    }
    return false;
}

// What the launcher remembers between starts. Every path is absolute.
struct LauncherSettings {
    std::filesystem::path install;     // the chosen install, empty for the first one found
    std::filesystem::path saveFolder;  // the chosen save folder, empty for the install's first
    std::vector<std::filesystem::path> addedInstalls;     // installs the player picked by hand
    std::vector<std::filesystem::path> addedSaveFolders;  // save folders the player picked by hand
    bool backupReminderSeen = false;  // the player was reminded to back up their saves and went on
    // What draws the game: "sdlgpu" (Direct3D 12 or Vulkan), "gl2" (OpenGL 2.1), or empty for the game's own choice.
    std::string renderer;
    // The camera glides after a followed Sim instead of moving in the game's steps. Not in the
    // original game, so off unless the player turns it on.
    bool smoothFollow = false;
    // The game window's size: a key of kWindowSizes, or empty for the game's own default.
    std::string windowSize;
    // The game's UI drawn larger, in percent: one of kUiScales. Not in the original game, so 100
    // unless the player chooses another.
    int uiScale = 100;

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
