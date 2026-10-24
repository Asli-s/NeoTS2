#include "paths/save_folders.hpp"

#include "paths/game_folders.hpp"
#include "paths/names.hpp"
#include "paths/new_save_folder.hpp"

#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace ts2::paths {

namespace {

namespace fs = std::filesystem;

std::vector<fs::path> systemFolders() {
    std::vector<fs::path> out;
#ifdef _WIN32
    for (const KNOWNFOLDERID& id : {FOLDERID_Windows, FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86, FOLDERID_ProgramData}) {
        PWSTR text = nullptr;
        if (SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &text) == S_OK && text && *text) {
            out.emplace_back(text);
        }
        CoTaskMemFree(text);
    }
#else
    for (const char* folder : {"/bin", "/boot", "/dev", "/etc", "/lib", "/lib64", "/proc", "/sbin", "/sys", "/usr", "/System",
                               "/Library"}) {
        out.emplace_back(folder);
    }
#endif
    return out;
}

// The lock file for a save folder, beside where the folder really is, so every spelling of one
// folder shares it.
fs::path lockFileFor(const fs::path& folder) {
    std::error_code error;
    fs::path real = fs::weakly_canonical(folder, error);
    if (error || !real.has_filename()) {
        real = folder;
    }
    return real.parent_path() / (real.filename().native() + fs::path(std::u8string(kLockSuffix)).native());
}

} // namespace

bool isWorkName(const fs::path& name) {
    const std::string folded = foldedName(name);
    for (const std::u8string_view suffix : {kStagingSuffix, kNeighborhoodsStagingSuffix, kLockSuffix, kMarkSuffix}) {
        if (folded.ends_with(std::string_view(reinterpret_cast<const char*>(suffix.data()), suffix.size()))) {
            return true;
        }
    }
    return false;
}

fs::path saveFolderPath(const fs::path& folder) {
    std::error_code error;
    fs::path out = fs::absolute(folder, error).lexically_normal();
    if (!error && !out.has_filename()) {
        out = out.parent_path();  // given with a trailing separator
    }
    if (error || !out.has_filename() || !out.has_parent_path() || out.parent_path() == out) {
        return {};
    }
    return out;
}

SaveFolderState inspectSaveFolder(const fs::path& folder) {
    std::error_code error;
    const fs::file_type type = fs::symlink_status(folder, error).type();
    if (type == fs::file_type::not_found) {
        return SaveFolderState::New;
    }
    if (error) {
        return SaveFolderState::Unreadable;
    }
    if (type != fs::file_type::directory) {
        // A link or junction (players move their saves to another drive this way) is read where
        // it leads, but NeoTS2 never puts anything through it.
        if (!fs::is_directory(folder, error) || error) {
            return SaveFolderState::Unreadable;
        }
        const fs::directory_iterator readable(folder, error);
        return error ? SaveFolderState::Unreadable : SaveFolderState::Linked;
    }
    const bool empty = fs::is_empty(folder, error);
    if (error) {
        return SaveFolderState::Unreadable;
    }
    return empty ? SaveFolderState::New : SaveFolderState::InUse;
}

bool lacksNeighborhoods(const fs::path& folder) {
    if (inspectSaveFolder(folder) != SaveFolderState::InUse) {
        return false;
    }
    std::error_code error;
    const fs::path neighborhoods = folder / "Neighborhoods";
    const fs::file_type type = fs::symlink_status(neighborhoods, error).type();
    if (type == fs::file_type::not_found) {
        return true;
    }
    return !error && type == fs::file_type::directory && fs::is_empty(neighborhoods, error) && !error;
}

bool isOriginalGamesFolder(const fs::path& folder) {
    const fs::path named = saveFolderPath(folder);
    std::error_code error;
    const fs::path real = named.empty() ? fs::path() : fs::weakly_canonical(named, error);
    if (named.empty() || error) {
        return true;
    }
    const std::vector<fs::path> names = originalSaveFolderNames();
    const std::string ours = foldedName(fs::path(std::u8string(kNeoSaveFolderPrefix)));
    for (const fs::path& path : {named, real}) {
        if (foldedName(path.parent_path().filename()) != "ea games") {
            continue;
        }
        const std::string name = foldedName(path.filename());
        // Any "The Sims ..." folder is left to the original games too, for editions this computer
        // doesn't record.
        if (name.starts_with("the sims") && !name.starts_with(ours)) {
            return true;
        }
        for (const fs::path& original : names) {
            if (name == foldedName(original)) {
                return true;
            }
        }
    }
    return false;
}

bool isWithin(const fs::path& inner, const fs::path& outer) {
    const fs::path a = saveFolderPath(inner);
    const fs::path b = saveFolderPath(outer);
    if (a.empty() || b.empty()) {
        return false;
    }
    if (spelledWithin(a, b)) {
        return true;
    }
    std::error_code errorA;
    std::error_code errorB;
    const fs::path realA = fs::weakly_canonical(a, errorA);
    const fs::path realB = fs::weakly_canonical(b, errorB);
    return !errorA && !errorB && spelledWithin(realA, realB);
}

bool isSystemFolder(const fs::path& folder) {
    for (const fs::path& system : systemFolders()) {
        if (isWithin(folder, system)) {
            return true;
        }
    }
    return false;
}

const char* saveFolderRefusal(const fs::path& folder, const fs::path& install) {
    // Judged by the folder's own name, so "X\" and "X\." are judged as X.
    const fs::path named = saveFolderPath(folder);
    if (named.empty()) {
        return "The path doesn't name a folder.";
    }
    if (isSystemFolder(named)) {
        return "It is a system folder, and NeoTS2 never keeps saves in one.";
    }
    if (isWithin(named, install) || isWithin(install, named)) {
        return "It is inside the game's install, or holds it. Saves are kept apart from the install.";
    }
    if (isWorkName(named.filename())) {
        return "Its name ends like the folders NeoTS2 makes for itself while it works.";
    }
    return nullptr;
}

bool saveFolderBusy(const fs::path& folder) {
    const fs::path named = saveFolderPath(folder);
    if (named.empty()) {
        return false;
    }
    const fs::path file = lockFileFor(named);
#ifdef _WIN32
    // Only an open lock file can refuse to be opened, and a missing one is never made here.
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_SHARING_VIOLATION;
    }
    CloseHandle(handle);
    return false;
#else
    const int handle = ::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (handle < 0) {
        return false;
    }
    const bool busy = ::flock(handle, LOCK_SH | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    ::close(handle);
    return busy;
#endif
}

SaveFolderLock::SaveFolderLock(const fs::path& folder) : folder_(saveFolderPath(folder)) {
    if (folder_.empty()) {
        problem_ = "The path doesn't name a folder.";
        return;
    }
    if (isSystemFolder(folder_)) {
        problem_ = "It is a system folder, and NeoTS2 never keeps saves in one.";
        return;
    }
    const fs::path file = lockFileFor(folder_);
    // Only a plain file is ever used as the lock, so a link put there can't lead the lock elsewhere.
    std::error_code error;
    const fs::file_type type = fs::symlink_status(file, error).type();
    if (error && type != fs::file_type::not_found) {
        problem_ = "Its lock file can't be checked: " + error.message();
        return;
    }
    if (type != fs::file_type::not_found && type != fs::file_type::regular) {
        problem_ = "Something other than a file is in the way of its lock file.";
        return;
    }
#ifdef _WIN32
    // Windows deletes the lock file when it closes, so an existing file is only reused when it is
    // an empty lock left by a power cut. Any other file with that name is a player's and is kept.
    constexpr DWORD kFlags = FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT;
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, kFlags, nullptr);
    if (handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS) {
        std::error_code sizeError;
        if (fs::file_size(file, sizeError) != 0 || sizeError) {
            problem_ = "A file of yours has the name NeoTS2 uses for its lock file, so it was left alone.";
            return;
        }
        handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, kFlags, nullptr);
    }
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        busy_ = code == ERROR_SHARING_VIOLATION;
        if (!busy_) {
            problem_ = "Its lock file, which keeps two games from saving to it at once, can't be made: " +
                       std::system_category().message(static_cast<int>(code));
        }
        return;
    }
    handle_ = handle;
    held_ = true;
#else
    file_ = ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (file_ < 0) {
        problem_ = "Its lock file, which keeps two games from saving to it at once, can't be made: " +
                   std::generic_category().message(errno);
        return;
    }
    if (::flock(file_, LOCK_EX | LOCK_NB) != 0) {
        busy_ = errno == EWOULDBLOCK;
        if (!busy_) {
            problem_ = "Its lock file can't be locked: " + std::generic_category().message(errno);
        }
        ::close(file_);
        file_ = -1;
        return;
    }
    held_ = true;
#endif
}

SaveFolderLock::~SaveFolderLock() {
#ifdef _WIN32
    if (handle_) {
        CloseHandle(static_cast<HANDLE>(handle_));
    }
#else
    if (file_ >= 0) {
        ::close(file_);
    }
#endif
}

} // namespace ts2::paths
