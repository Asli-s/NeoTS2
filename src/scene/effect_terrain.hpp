#pragma once

#include "scene/particle_emitter.hpp"
#include "scene/scene.hpp"

namespace ts2::scene {

// The neighborhood's terrain as effects meet it, as the game gives it to them: from 0 to its tiles
// times their size, its height and normal blended bilinearly between the four corners of a tile, and
// the water at the neighborhood's sea level. Places off the terrain take its nearest edge.
EffectTerrain effectTerrain(const PaintedTerrain& terrain);

} // namespace ts2::scene
