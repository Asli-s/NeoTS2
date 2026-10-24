#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

namespace ts2::paths {

enum class Edition { UltimateCollection, LegacyCollection, Other };

std::string_view editionName(Edition edition);

// What an install folder holds, from file and folder names only. No file is opened.
struct InstallCheck {
    bool hasBaseObjects = false;  // Base/TSData/Res/Objects/objects.package, the base game's objects
    int packFolders = 0;          // folders with a TSData folder, the base game's included
    bool downloading = false;     // a file still ends in _DiP_Staged, the EA app's mark while it downloads
    bool tooLarge = false;        // the walk stopped at its limit, so the folder isn't a game install

    bool ready() const { return hasBaseObjects && !downloading && !tooLarge; }
};

InstallCheck checkInstall(const std::filesystem::path& root);

// The install's packages whose header is broken: too short, without the DBPF mark, of another
// version, or with an index that reaches past the end of the file. A download cut short or never
// finished leaves such files. Only each package's first 96 bytes are read.
struct DamagedPackages {
    int count = 0;
    std::vector<std::filesystem::path> examples;  // the first few, for the player and the log
};

DamagedPackages findDamagedPackages(const std::filesystem::path& root);

struct GameInstall {
    Edition edition = Edition::Other;
    std::filesystem::path root;  // the folder NeoTS2 is started with
    // The save folders the game would use, in the order to offer them. For the Ultimate Collection
    // this is the game's own rule, so the folder may not exist yet. For the Legacy Collection it is
    // every existing folder the game might use, and the player confirms which.
    std::vector<std::filesystem::path> saveFolders;
    InstallCheck check;
};

// The installs this computer's game launchers recorded, checked. Empty off Windows.
std::vector<GameInstall> findInstalls();

// The names the original games give their save folders in "EA Games": the games' own defaults
// ("The Sims 2", and "The Sims 2 Legacy" for the Legacy Collection) and the names this computer's
// installs recorded. Off Windows, the defaults only.
std::vector<std::filesystem::path> originalSaveFolderNames();

// <My Documents, or Public Documents when there is none>\EA Games, where the games keep their save
// folders. Empty off Windows.
std::filesystem::path eaGamesFolder();

// The deepest folder that holds all of the given folders, or empty when they share none
// (for example, folders on two drives).
std::filesystem::path commonFolder(const std::vector<std::filesystem::path>& folders);

} // namespace ts2::paths
