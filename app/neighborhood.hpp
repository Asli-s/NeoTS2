#pragma once

#include "scene/builder.hpp"
#include "scene/neighborhood_scene.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace app {

// Adds a neighborhood (its folder in the user's Neighborhoods) to the scene, with the game's paint
// rules, lighting, road materials, lot imposter lift and object catalog from the install.
// The lots households live on (the neighborhood's families with a lot and members, not the game's
// own families from id 0x7FE0 up).
std::vector<std::uint32_t> livedInLots(const std::filesystem::path& folder);

// How many neighborhood tiles the ground around a lot reaches in lot view: the newest product's
// lotSkirtSizeIncrease (Res/Config/globalProps.xml); 9 when it isn't set.
int lotSkirtMargin(const std::filesystem::path& install);

ts2::scene::NeighborhoodSceneStats buildNeighborhood(ts2::scene::SceneBuilder& builder, const std::filesystem::path& install,
                                                     const std::filesystem::path& folder);

} // namespace app
