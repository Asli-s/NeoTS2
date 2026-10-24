#include "launcher/game_start.hpp"

#include "paths/names.hpp"

#include <SDL3/SDL.h>

#include <optional>
#include <system_error>
#include <vector>

namespace ts2::launcher {

namespace {

// A path exactly as UTF-8, for the game's command line. Nullopt for a name with no UTF-8 form,
// which the game couldn't be given.
std::optional<std::string> exactUtf8(const std::filesystem::path& path) {
    try {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace

SDL_Process* startGame(const std::filesystem::path& install, const std::filesystem::path& saveFolder, bool addNeighborhoods,
                       const std::string& renderer, std::string& error) {
    const char* base = SDL_GetBasePath();
    const auto baseFolder = base ? paths::pathFromUtf8(base) : std::nullopt;
    if (!baseFolder) {
        error = base ? "The launcher's own folder has a name the game can't be started from." : SDL_GetError();
        return nullptr;
    }
#ifdef _WIN32
    const std::filesystem::path game = *baseFolder / "neots2.exe";
#else
    const std::filesystem::path game = *baseFolder / "neots2";
#endif
    std::error_code missing;
    if (!std::filesystem::is_regular_file(game, missing)) {
        error = "neots2 isn't beside the launcher (" + paths::utf8Text(game) + ").";
        return nullptr;
    }
    const auto gameText = exactUtf8(game);
    const auto installText = exactUtf8(install);
    const auto saveText = exactUtf8(saveFolder);
    if (!gameText || !installText || !saveText) {
        error = "A folder has a name the game can't be given. Rename it, or choose another folder.";
        return nullptr;
    }
    // The same arguments the game takes when started by hand, so a launcher-made start can always
    // be repeated from a command line. The list ends at the first null.
    std::vector<const char*> args = {gameText->c_str(), installText->c_str(), "--menu", "--user", saveText->c_str()};
    if (addNeighborhoods) {
        args.push_back("--add-neighborhoods");
    }
    if (!renderer.empty()) {
        args.push_back("--renderer");
        args.push_back(renderer.c_str());
    }
    args.push_back(nullptr);
    SDL_Process* process = SDL_CreateProcess(args.data(), false);
    if (!process) {
        error = SDL_GetError();
    }
    return process;
}

} // namespace ts2::launcher
