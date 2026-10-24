#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace ts2::paths {

// The ends of the names NeoTS2 gives what it makes beside a save folder while it works: the copy
// made for a new save folder, the copy of neighborhoods being added, the lock file, and the mark
// file that says a copy folder is NeoTS2's own.
inline constexpr std::u8string_view kStagingSuffix = u8".neots2-setup";
inline constexpr std::u8string_view kNeighborhoodsStagingSuffix = u8".neots2-neighborhoods";
inline constexpr std::u8string_view kLockSuffix = u8".neots2-lock";
inline constexpr std::u8string_view kMarkSuffix = u8".neots2-mark";

// True when a name ends like one of those. No save folder may be named so, or NeoTS2 could take a
// player's folder for its own leftovers.
bool isWorkName(const std::filesystem::path& name);

// A save folder as NeoTS2 names it: absolute, with no "." or ".." parts and no trailing separator.
// Empty when the path can't name a save folder (a drive root, for one).
std::filesystem::path saveFolderPath(const std::filesystem::path& folder);

enum class SaveFolderState {
    New,         // missing, or a real folder with nothing in it
    InUse,       // a real folder with anything in it
    Linked,      // a link or junction to a folder, used as it is and never added to
    Unreadable,  // can't be read, or isn't a folder (a file, a link to nothing)
};

SaveFolderState inspectSaveFolder(const std::filesystem::path& folder);

// True when a save folder holds files but no neighborhoods: its Neighborhoods folder is missing or
// a real, empty folder. NeoTS2 then asks before adding the ones the install ships.
bool lacksNeighborhoods(const std::filesystem::path& folder);

// True when the folder is one of the original games' own save folders: directly inside an
// "EA Games" folder, with a name from originalSaveFolderNames or any name starting "The Sims"
// except NeoTS2's own, as spelled or where its path really leads (links, junctions, short names).
// Other folders players make in "EA Games" are theirs. A path that can't be resolved counts as one,
// so NeoTS2 never fills it.
bool isOriginalGamesFolder(const std::filesystem::path& folder);

// True when `inner` is `outer` or lies inside it, as spelled or where the paths really lead (links,
// junctions, short names). Letter case doesn't matter.
bool isWithin(const std::filesystem::path& inner, const std::filesystem::path& outer);

// True for the system's own folders and anything inside them: Windows, Program Files, Program Files
// (x86) and ProgramData on Windows. Saves never go there. The installs do, so this is for save
// folders only.
bool isSystemFolder(const std::filesystem::path& folder);

// Why NeoTS2 never uses a folder as a save folder with this install, for the player: a system
// folder, one that overlaps the install, or one named like NeoTS2's own work folders. Null when it
// can be used. The launcher and the game both ask this, so they always agree.
const char* saveFolderRefusal(const std::filesystem::path& folder, const std::filesystem::path& install);

// True when another NeoTS2 holds the save folder's lock. Only looks, so it never makes a file.
bool saveFolderBusy(const std::filesystem::path& folder);

// Keeps a save folder to one NeoTS2 at a time, from its first-start fill until the game ends. It is
// a hidden file beside the folder, named after where the folder really leads, so every spelling of
// one folder shares it. Windows deletes the file when NeoTS2 ends, even in a crash. The folder that
// holds the save folder must exist. A system folder is never locked, so no game uses one.
class SaveFolderLock {
public:
    explicit SaveFolderLock(const std::filesystem::path& folder);
    ~SaveFolderLock();
    SaveFolderLock(const SaveFolderLock&) = delete;
    SaveFolderLock& operator=(const SaveFolderLock&) = delete;

    bool held() const { return held_; }
    // True when another NeoTS2 holds it. Otherwise a lock not held means the folder can't be used.
    bool busy() const { return busy_; }
    // Why the lock couldn't be taken, for the player. Empty when it is held or busy.
    const std::string& problem() const { return problem_; }
    // The save folder, as saveFolderPath names it.
    const std::filesystem::path& folder() const { return folder_; }

private:
    std::filesystem::path folder_;
    std::string problem_;
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int file_ = -1;
#endif
    bool held_ = false;
    bool busy_ = false;
};

} // namespace ts2::paths
