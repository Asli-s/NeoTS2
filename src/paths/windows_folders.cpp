// Where the game launchers on Windows record the installs, and the save folder rules the game
// itself follows.
#include "paths/game_folders.hpp"
#include "paths/names.hpp"
#include "paths/steam_files.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <fstream>
#include <optional>
#include <string>

namespace ts2::paths {

namespace {

namespace fs = std::filesystem;

constexpr const char* kAppPaths = "Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\";
// The Legacy Collection's own key, and its Steam app id.
constexpr const char* kLegacyKey = "Software\\Electronic Arts\\The Sims 2 Ultimate Collection 25";
constexpr const char* kLegacyManifest = "appmanifest_3314070.acf";
// The game reads at most 260 bytes of a name or path, and splits the pack list into at most 32.
constexpr DWORD kMaxValue = MAX_PATH;
constexpr std::size_t kMaxPacks = 32;
constexpr std::uintmax_t kMaxSteamFile = 1 << 20;

// A text value read the way the game reads it: as ANSI text, which Windows converts with the
// system code page. The ™ in a save folder's name comes back right only this way.
std::optional<std::string> ansiValue(HKEY root, const std::string& key, const char* name) {
    HKEY handle = nullptr;
    if (RegOpenKeyExA(root, key.c_str(), 0, KEY_READ | KEY_WOW64_32KEY, &handle) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    char buffer[kMaxValue + 1] = {};
    DWORD type = 0;
    DWORD size = kMaxValue;
    const LONG result = RegQueryValueExA(handle, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size);
    RegCloseKey(handle);
    if (result != ERROR_SUCCESS || type != REG_SZ) {
        return std::nullopt;
    }
    buffer[std::min<DWORD>(size, kMaxValue)] = '\0';
    return std::string(buffer);
}

std::optional<DWORD> numberValue(HKEY root, const std::string& key, const char* name) {
    HKEY handle = nullptr;
    if (RegOpenKeyExA(root, key.c_str(), 0, KEY_READ | KEY_WOW64_32KEY, &handle) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    DWORD value = 0;
    DWORD type = 0;
    DWORD size = sizeof(value);
    const LONG result = RegQueryValueExA(handle, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(handle);
    if (result != ERROR_SUCCESS || type != REG_DWORD) {
        return std::nullopt;
    }
    return value;
}

// ANSI text back to a path, through the same system code page.
fs::path fromAnsi(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return fs::path(wide);
}

// My Documents, or Public Documents when there is none, as the game picks.
fs::path documentsFolder() {
    wchar_t buffer[MAX_PATH] = {};
    for (const int folder : {CSIDL_PERSONAL, CSIDL_COMMON_DOCUMENTS}) {
        if (SUCCEEDED(SHGetFolderPathW(nullptr, folder, nullptr, 0, buffer))) {
            return fs::path(buffer);
        }
    }
    return {};
}

bool isFolder(const fs::path& path) {
    std::error_code error;
    return !path.empty() && fs::is_directory(path, error);
}

std::optional<std::string> readSmallFile(const fs::path& path) {
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error || size > kMaxSteamFile) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!file.read(text.data(), static_cast<std::streamsize>(size))) {
        return std::nullopt;
    }
    return text;
}

// A pack's folder from App Paths\<exe>, kept only when the pack is marked installed.
std::optional<fs::path> packFolder(const std::string& exe) {
    if (exe.empty() || exe.find_first_of("\\/") != std::string::npos) {
        return std::nullopt;
    }
    const std::string key = kAppPaths + exe;
    const auto installed = numberValue(HKEY_LOCAL_MACHINE, key, "Installed");
    const auto path = ansiValue(HKEY_LOCAL_MACHINE, key, "Path");
    if (!installed || *installed == 0 || !path) {
        return std::nullopt;
    }
    return fromAnsi(*path);
}

// The Ultimate Collection, as the game finds it: the base game from App Paths\Sims2.exe, each pack
// from the game's EPsInstalled list, and the save folder from the game key's DisplayName.
std::optional<GameInstall> ultimateCollection(const fs::path& documents) {
    const std::string baseKey = std::string(kAppPaths) + "Sims2.exe";
    const auto basePath = ansiValue(HKEY_LOCAL_MACHINE, baseKey, "Path");
    const auto gameKey = ansiValue(HKEY_LOCAL_MACHINE, baseKey, "Game Registry");
    if (!basePath || !gameKey) {
        return std::nullopt;
    }
    std::vector<fs::path> folders{fromAnsi(*basePath)};
    if (const auto packs = ansiValue(HKEY_LOCAL_MACHINE, *gameKey, "EPsInstalled")) {
        std::size_t start = 0;
        for (std::size_t n = 0; n < kMaxPacks && start <= packs->size(); ++n) {
            const std::size_t comma = std::min(packs->find(',', start), packs->size());
            if (const auto folder = packFolder(packs->substr(start, comma - start))) {
                folders.push_back(*folder);
            }
            start = comma + 1;
        }
    }
    GameInstall install;
    install.edition = Edition::UltimateCollection;
    install.root = commonFolder(folders);
    if (!isFolder(install.root)) {
        return std::nullopt;
    }
    const auto name = ansiValue(HKEY_LOCAL_MACHINE, *gameKey, "DisplayName");
    const fs::path folderName = name ? fromAnsi(*name) : fs::path(L"The Sims 2");
    if (!documents.empty() && !folderName.empty()) {
        install.saveFolders.push_back(documents / L"EA Games" / folderName);
    }
    install.check = checkInstall(install.root);
    return install;
}

// The Legacy Collection, from Steam's libraries. Its save folder rule isn't known, so every
// existing folder it might use is offered.
std::vector<GameInstall> legacyCollections(const fs::path& documents) {
    std::vector<GameInstall> installs;
    HKEY handle = nullptr;
    wchar_t steam[MAX_PATH] = {};
    DWORD size = sizeof(steam) - sizeof(wchar_t);
    DWORD type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &handle) == ERROR_SUCCESS) {
        if (RegQueryValueExW(handle, L"SteamPath", nullptr, &type, reinterpret_cast<BYTE*>(steam), &size) != ERROR_SUCCESS ||
            type != REG_SZ) {
            steam[0] = L'\0';
        }
        RegCloseKey(handle);
    }
    if (steam[0] == L'\0') {
        return installs;
    }
    std::vector<fs::path> libraries{fs::path(steam)};
    if (const auto list = readSmallFile(fs::path(steam) / L"steamapps" / L"libraryfolders.vdf")) {
        for (const std::string& folder : steamLibraryFolders(*list)) {
            if (const auto path = pathFromUtf8(folder)) {
                libraries.push_back(*path);
            }
        }
    }

    std::vector<fs::path> saveFolders;
    if (!documents.empty()) {
        const auto name = ansiValue(HKEY_CURRENT_USER, kLegacyKey, "DisplayName");
        for (const fs::path& candidate : {name ? fromAnsi(*name) : fs::path(), fs::path(L"The Sims 2 Legacy")}) {
            const fs::path folder = documents / L"EA Games" / candidate;
            if (!candidate.empty() && isFolder(folder) &&
                std::find(saveFolders.begin(), saveFolders.end(), folder) == saveFolders.end()) {
                saveFolders.push_back(folder);
            }
        }
    }

    std::vector<fs::path> seen;
    for (const fs::path& library : libraries) {
        const fs::path apps = library / L"steamapps";
        const auto manifest = readSmallFile(apps / kLegacyManifest);
        const auto folder = manifest ? steamInstallFolder(*manifest) : std::nullopt;
        const auto name = folder ? pathFromUtf8(*folder) : std::nullopt;
        if (!name) {
            continue;
        }
        const fs::path root = (apps / L"common" / *name).lexically_normal();
        // Steam may list one library twice, written differently (the Steam folder itself, for one).
        const auto sameFolder = [&root](const fs::path& other) {
            std::error_code error;
            return fs::equivalent(root, other, error);
        };
        if (!isFolder(root) || std::any_of(seen.begin(), seen.end(), sameFolder)) {
            continue;
        }
        seen.push_back(root);
        GameInstall install;
        install.edition = Edition::LegacyCollection;
        // Steam records its own folder in lowercase. The folder's real spelling reads better.
        std::error_code error;
        const fs::path spelled = fs::canonical(root, error);
        install.root = error ? root : spelled;
        install.saveFolders = saveFolders;
        install.check = checkInstall(root);
        installs.push_back(std::move(install));
    }
    return installs;
}

} // namespace

std::vector<fs::path> originalSaveFolderNames() {
    std::vector<fs::path> names{fs::path(L"The Sims 2"), fs::path(L"The Sims 2 Legacy")};
    const std::string baseKey = std::string(kAppPaths) + "Sims2.exe";
    if (const auto gameKey = ansiValue(HKEY_LOCAL_MACHINE, baseKey, "Game Registry")) {
        if (const auto name = ansiValue(HKEY_LOCAL_MACHINE, *gameKey, "DisplayName")) {
            names.push_back(fromAnsi(*name));
        }
    }
    if (const auto name = ansiValue(HKEY_CURRENT_USER, kLegacyKey, "DisplayName")) {
        names.push_back(fromAnsi(*name));
    }
    return names;
}

fs::path eaGamesFolder() {
    const fs::path documents = documentsFolder();
    return documents.empty() ? fs::path() : documents / L"EA Games";
}

std::vector<GameInstall> findInstalls() {
    const fs::path documents = documentsFolder();
    std::vector<GameInstall> installs;
    if (auto install = ultimateCollection(documents)) {
        installs.push_back(std::move(*install));
    }
    for (GameInstall& install : legacyCollections(documents)) {
        installs.push_back(std::move(install));
    }
    return installs;
}

} // namespace ts2::paths
