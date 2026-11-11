#pragma once

#include "content/cameras.hpp"
#include "scene/builder.hpp"
#include "scene/neighborhood_lighting.hpp"
#include "scene/neighborhood_plumbobs.hpp"
#include "scene/neighborhood_scene.hpp"
#include "scene/scene.hpp"

#include <array>
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

// How the neighborhood looks by day or by night, from the lighting state the game picks for each:
// "neighbourhood" by day, "neighborhoodnight" by night.
struct NeighborhoodTime {
    ts2::scene::NeighborhoodLight terrain;  // what the terrain's light map is made with
    std::optional<ts2::scene::ObjectLighting> objects;
    std::array<float, 3> imposters{1, 1, 1};  // the state's environment colour, which lot imposters take
    std::vector<ts2::scene::Image> sky;      // the sky cube's six faces, empty to keep the one built
};
struct NeighborhoodTimes {
    NeighborhoodTime day;
    std::optional<NeighborhoodTime> night;  // none when the install's lighting has no night state
    bool atNight = false;                   // the neighborhood's saved Day/Night Toggle
    const NeighborhoodTime& now() const { return atNight && night ? *night : day; }
};

// The effect state bits the neighborhood plays its effects with: bit 0 by day, and the bits a lot
// always sets. Snow and weather bits stay clear.
std::uint32_t neighborhoodEffectState(bool night);

// Adds a neighborhood (its folder in the user's Neighborhoods) to the scene with the install's paint
// rules, lighting, road materials, imposter lift and object catalog, and its decorations' ground decals.
// `variantSeed` fixes the ground's texture variants and decal picks. It is lit by day or night as saved,
// and `times` gets both looks. Imposters take the High pictures kept in `highImposters` when given.
ts2::scene::NeighborhoodSceneStats buildNeighborhood(ts2::scene::SceneBuilder& builder, const std::filesystem::path& install,
                                                     const std::filesystem::path& folder, ts2::content::EffectsFiles& effects,
                                                     std::optional<std::uint32_t> variantSeed = std::nullopt,
                                                     NeighborhoodTimes* times = nullptr, const std::filesystem::path& highImposters = {});

// Not in the original game: how bright the pool-style sea is in the look shown now, by day 1 and less
// at night, from how the light map lights flat ground (see scene::PaintedTerrain::seaLight).
float seaLight(const NeighborhoodTimes& times);

// Adds the green plumbob the game shows over each lot a household lives on (the effect
// fxNeighborhoodPlumbob) to `plumbobs`. Lots without an imposter get none.
void addPlumbobs(ts2::scene::SceneBuilder& builder, ts2::content::EffectsFiles& effects, const std::filesystem::path& folder,
                 const ts2::scene::NeighborhoodSceneStats& stats, ts2::scene::NeighborhoodPlumbobs& plumbobs);

} // namespace app
