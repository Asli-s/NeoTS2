#pragma once

#include "formats/camera_config.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

namespace ts2::content {

// A camera from the game's camera files (TSData/Res/UserData/Cameras), the newest product's copy
// winning, e.g. "Neighborhood" for the neighborhood view.
std::optional<camera::Config> findCamera(const std::filesystem::path& installRoot, std::string_view name);

} // namespace ts2::content
