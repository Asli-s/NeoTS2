#pragma once

#include <filesystem>
#include <string>

struct SDL_Process;

namespace ts2::launcher {

// Starts NeoTS2 (neots2 beside the launcher) on an install and a save folder, first adding the
// install's neighborhoods with `addNeighborhoods`. Null when it couldn't start, with the reason in
// `error`. `renderer` is "sdlgpu", "gl2" or empty to let it choose. `smoothFollow`, the pool-style
// and skirt bools are the LauncherSettings options of the same names. `width` and `height` of 0 give the default
// window, and `fitScreen` asks for one filling the screen's free area instead.
SDL_Process* startGame(const std::filesystem::path& install, const std::filesystem::path& saveFolder, bool addNeighborhoods,
                       const std::string& renderer, bool smoothFollow, bool poolStylePonds, bool poolStyleSea, bool smoothLotSkirts, int width, int height, bool fitScreen,
                       std::string& error);

} // namespace ts2::launcher
