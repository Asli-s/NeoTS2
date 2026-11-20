#pragma once

#include "paths/game_folders.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace ts2::paths {

// Values as a 32-bit program reads them from HKEY_LOCAL_MACHINE: plain strings and DWORDs only.
using RegistryText = std::function<std::optional<std::string>(const std::string& key, const char* name)>;
using RegistryNumber = std::function<std::optional<std::uint32_t>(const std::string& key, const char* name)>;
// A registry path as a folder on this computer, or empty.
using RegistryPath = std::function<std::filesystem::path(const std::string& text)>;

// The game's rule: the base game from App Paths\Sims2.exe, then each EPsInstalled entry whose
// App Paths entry is marked installed.
std::vector<RegisteredPack> readPackRecords(const RegistryText& text, const RegistryNumber& number, const RegistryPath& toPath);

} // namespace ts2::paths
