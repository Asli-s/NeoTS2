#pragma once

#include "scene/assets.hpp"
#include "scene/scene.hpp"

#include <filesystem>

namespace app {

// Works out a lot's light the way the game does (the lighting state "day"; lamps off until the
// lot's own scripts switch them) and fills the scene's floor and wall light maps with it.
// Returns false when the lot gives nothing to light.
bool lightLot(ts2::scene::Assets& assets, ts2::scene::Scene& scene, const std::filesystem::path& lotPackage);

} // namespace app
