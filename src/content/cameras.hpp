#pragma once

#include "formats/camera_config.hpp"
#include "formats/effects.hpp"

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace ts2::content {

// A camera from the game's camera files (TSData/Res/UserData/Cameras), the newest product's copy
// winning, e.g. "Neighborhood" for the neighborhood view.
[[nodiscard]] std::optional<camera::Config> findCamera(const std::filesystem::path& installRoot, std::string_view name);

// The products' effects files (TSData/Res/Effects/effects.package) joined as the game joins them, by
// resource instance from lowest to highest. They are read once, when first needed. Files that can't
// be read are passed over.
class EffectsFiles {
public:
    explicit EffectsFiles(const std::filesystem::path& installRoot);

    // All the products' effects, or null when none could be read.
    [[nodiscard]] const effects::File* all();
    // The joined effects when they have a visual effect named `name`, or null.
    [[nodiscard]] const effects::File* with(std::string_view name);

private:
    std::vector<std::filesystem::path> paths_;  // newest product first
    std::optional<effects::File> joined_;
    bool read_ = false;
};

} // namespace ts2::content
