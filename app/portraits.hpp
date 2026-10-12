#pragma once

#include "formats/tga.hpp"
#include "render/renderer.hpp"
#include "scene/animation_library.hpp"
#include "scene/scene.hpp"

#include <cstdint>
#include <optional>
#include <random>

namespace app {

// Draws a Sim on the lot as the game draws its portrait: alone, in one of its age's UI poses picked
// at random, from in front of its head, over a transparent background (256 pixels square). Nullopt
// when the Sim has no actor on the lot or can't be drawn. The renderer's shown instances and the
// Sim's joints are changed; the next frame sets them again.
std::optional<ts2::tga::Pixels> drawPortrait(const ts2::scene::Scene& lot, ts2::render::Renderer& renderer,
                                             ts2::scene::AnimationLibrary& animations, std::uint32_t sim, std::mt19937& random);

} // namespace app
