#pragma once

#include "launcher/launcher_style.hpp"
#include "paths/game_folders.hpp"
#include "settings/launcher_settings.hpp"

#include <array>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct SDL_Process;
struct SDL_Window;

namespace ts2::launcher {

enum class Picking { Nothing, Install, SaveFolder };

// The folder the player picked in a folder dialog. SDL may answer from another thread, so the
// answer waits here until the next frame takes it.
struct PickedFolder {
    std::mutex lock;
    Picking what = Picking::Nothing;
    std::optional<std::string> folder;  // as SDL gives it, in UTF-8
};

struct LauncherState {
    std::vector<paths::GameInstall> installs;
    int install = -1;     // index into installs
    int saveFolder = -1;  // index into the chosen install's saveFolders
    std::string message;  // the last thing that went wrong, shown until the player picks or starts something
    SDL_Process* game = nullptr;
    bool askNeighborhoods = false;  // open the question about a save folder without neighborhoods
    bool askBackup = false;         // open the reminder to back up saves
    bool backupReminderSeen = false;  // the reminder was shown and the player went on, so it isn't shown again
    std::string renderer;             // what the game is asked to draw with: "sdlgpu", "gl2", or empty for its own choice
    bool smoothFollow = false;        // see LauncherSettings::smoothFollow
    std::string windowSize;           // see LauncherSettings::windowSize
    std::array<char, 128> newFolderName{};  // what the player types for a new save folder (UTF-8)
    std::string newFolderProblem;           // why the last one couldn't be made
    PickedFolder picked;
    // What each save folder will get on the next start, worked out again after a game ends or another
    // install is chosen.
    std::map<std::filesystem::path, std::string> folderNotes;
    // Installs and save folders the player picked by hand, remembered between starts.
    std::vector<std::filesystem::path> addedInstalls;
    std::vector<std::filesystem::path> addedSaveFolders;
    std::filesystem::path settingsFile;    // where the choices are saved, empty when nowhere
    settings::LauncherSettings remembered;  // the choices as last saved
    std::filesystem::path eaGames;          // where new save folders are made, found once at start
    // The widths the window needs, in pixels, measured while drawing: everything above the Play row
    // (its left padding included), and the Play row itself.
    float contentWidth = 0;
    float playRowWidth = 0;
    // The heights measured in the last frame: the sections above the Play row, the Play row with its
    // message, and both together with the space between them.
    float sectionsHeight = 0;
    float footerHeight = 0;
    float contentHeight = 0;
    // The widths of the two columns' contents in the last frame, to know whether they fit side by side.
    float leftWidth = 0;
    float rightWidth = 0;
    float maxWidth = 0;  // the widest the window's contents can be on its screen, in pixels
    float columnRight = 0;  // the game column's right edge on the screen, while it is drawn
};

// Finds the installs, adds the ones the player picked before and chooses the remembered install
// and save folder, or the first ready install.
void findInstalls(LauncherState& state, const settings::LauncherSettings& remembered);

// Draws the launcher's window contents for one frame and acts on its buttons.
void drawLauncher(LauncherState& state, SDL_Window* window, const LauncherFonts& fonts);

// Notices when a started game has ended, and says so if it ended with an error.
void watchGame(LauncherState& state);

} // namespace ts2::launcher
