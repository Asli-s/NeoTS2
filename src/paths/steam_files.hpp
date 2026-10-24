#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ts2::paths {

// Steam's library list (steamapps/libraryfolders.vdf): the folder of every Steam library on this
// computer. Empty when the text isn't a library list.
std::vector<std::string> steamLibraryFolders(std::string_view vdfText);

// The folder name under steamapps/common that a game's manifest (steamapps/appmanifest_<id>.acf)
// names. Empty when the manifest has none or the name isn't a plain folder name.
std::optional<std::string> steamInstallFolder(std::string_view manifestText);

} // namespace ts2::paths
