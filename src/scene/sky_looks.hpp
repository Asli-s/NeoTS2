#pragma once

#include "scene/scene.hpp"

#include <string_view>
#include <vector>

namespace ts2::scene {

// Not in the original game: the Realistic sky, a recolouring of the game's own sky cube maps, so no
// picture of them is shipped. It changes the neighborhood's sky and a lot's day and night skies.
// Recolours `faces` of the texture `name` when it is one of them. True when it was.
bool recolourRealisticSky(std::string_view name, std::vector<Image>& faces);

} // namespace ts2::scene
