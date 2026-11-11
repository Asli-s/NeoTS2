#pragma once

#include <filesystem>
#include <string>

struct SDL_Process;

namespace ts2::launcher {

// Starts NeoTS2 (neots2 beside the launcher) on an install and a save folder, first adding the
// install's neighborhoods with `addNeighborhoods`. Null when it couldn't start, with the reason in
// `error`. `renderer` is "sdlgpu", "gl2" or empty to let it choose. `smoothFollow` and
// `smoothLotSkirts` and `noGameTips` are the LauncherSettings options of the same names.
SDL_Process* startGame(const std::filesystem::path& install, const std::filesystem::path& saveFolder, bool addNeighborhoods,
                       const std::string& renderer, bool smoothFollow, bool smoothLotSkirts, bool noGameTips,
                       std::string& error);

} // namespace ts2::launcher
