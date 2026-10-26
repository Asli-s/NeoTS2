#pragma once

#include <filesystem>
#include <string>

struct SDL_Process;

namespace ts2::launcher {

// Starts NeoTS2 (neots2 beside the launcher) on an install and a save folder, asking it to add the
// install's neighborhoods first when the player chose that. Null when it couldn't start, with the
// reason in `error`. `renderer` is what the game is asked to draw with ("sdlgpu" or "gl2"), empty to let it choose.
// `smoothFollow` asks for the gliding follow camera (see LauncherSettings::smoothFollow).
SDL_Process* startGame(const std::filesystem::path& install, const std::filesystem::path& saveFolder, bool addNeighborhoods,
                       const std::string& renderer, bool smoothFollow, std::string& error);

} // namespace ts2::launcher
