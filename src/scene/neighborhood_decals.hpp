#pragma once

#include "formats/effects.hpp"
#include "scene/builder.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace ts2::scene {

// A visual effect a neighborhood decoration's model plays: its "EffectsList" data list names a slot
// of the model and the effect played there. `offset` is the slot's place in the model.
struct EffectSlot {
    std::string effect;
    std::array<float, 3> offset{};
};
std::vector<EffectSlot> effectSlots(Assets& assets, const std::string& model);

// The game state bits effect components test, as the neighborhood has them without snow. Snow sets
// 0x40000, with 0x10000 for light and 0x20000 for heavy snow.
constexpr std::uint32_t kNoSnowState = 0;

// Adds the ground decals effect `effect` of `file` plays at `at` turned by `turn` (radians about z),
// as the game draws them: square textures laid on the terrain, each cut to its square. Components
// play only when `state` matches theirs, and those naming another effect (type 0) play it. Returns
// how many decals were added. Other kinds of blocks, and decals that change over their life, are
// left out with a warning.
std::size_t addGroundDecals(SceneBuilder& builder, const effects::File& file, const std::string& effect, const std::array<float, 3>& at,
                            float turn, std::uint32_t state, std::mt19937& random);

} // namespace ts2::scene
