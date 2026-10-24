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

// The products' effects files (TSData/Res/Effects/effects.package), each read once when first
// needed. Each product's file holds only some effects (the neighborhood plumbob only an early one),
// so an effect is looked for newest product first, as the game finds it. Files that can't be read
// are passed over.
class EffectsFiles {
public:
    explicit EffectsFiles(const std::filesystem::path& installRoot);

    // The newest file that has a visual effect named `name`, or null.
    [[nodiscard]] const effects::File* with(std::string_view name);
    // The newest product's file, or null when it is missing or can't be read.
    [[nodiscard]] const effects::File* newest();

private:
    const effects::File* file(std::size_t index);

    std::vector<std::filesystem::path> paths_;  // newest first
    std::vector<std::optional<effects::File>> files_;
    std::vector<bool> read_;
};

} // namespace ts2::content
