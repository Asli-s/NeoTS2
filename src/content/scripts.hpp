#pragma once

#include "formats/luascript.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ts2::content {

struct ObjectScripts {
    std::vector<luascript::Script> modules;  // run once when the scripts are set up
    std::vector<luascript::Script> scripts;  // what objects run, by name
    std::vector<std::string> problems;
};

// The newest ObjectScripts.package of the install, if any. Each product ships a cumulative copy,
// and the largest is the newest.
[[nodiscard]] std::optional<std::filesystem::path> objectScriptsPackage(const std::filesystem::path& installRoot);

// The package's modules and scripts, in the package's order.
ObjectScripts loadObjectScripts(const std::filesystem::path& package);

} // namespace ts2::content
