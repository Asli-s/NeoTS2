#pragma once

#include "content/cameras.hpp"
#include "scene/builder.hpp"
#include "scene/neighborhood_plumbobs.hpp"
#include "scene/neighborhood_scene.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace app {

// The lots households live on (the neighborhood's families with a lot and members, not the game's
// own families from id 0x7FE0 up).
std::vector<std::uint32_t> livedInLots(const std::filesystem::path& folder);

// How many neighborhood tiles the ground around a lot reaches in lot view: the newest product's
// lotSkirtSizeIncrease (Res/Config/globalProps.xml), or 9 when it isn't set.
int lotSkirtMargin(const std::filesystem::path& install);

// Adds a neighborhood (its folder in the user's Neighborhoods) to the scene, with the game's paint
// rules, lighting, road materials, lot imposter lift and object catalog from the install, and its
// decorations' ground decals. `variantSeed` fixes the ground's texture variants and the decals'
// random picks (see scene::addNeighborhood).
ts2::scene::NeighborhoodSceneStats buildNeighborhood(ts2::scene::SceneBuilder& builder, const std::filesystem::path& install,
                                                     const std::filesystem::path& folder, ts2::content::EffectsFiles& effects,
                                                     std::optional<std::uint32_t> variantSeed = std::nullopt);

// Adds the green plumbob the game shows over each lot a household lives on (the effect
// fxNeighborhoodPlumbob) to `plumbobs`. Lots without an imposter get none.
void addPlumbobs(ts2::scene::SceneBuilder& builder, ts2::content::EffectsFiles& effects, const std::filesystem::path& folder,
                 const ts2::scene::NeighborhoodSceneStats& stats, ts2::scene::NeighborhoodPlumbobs& plumbobs);

} // namespace app
