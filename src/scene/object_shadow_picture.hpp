#pragma once
// How the game frames a lot object's picture from the sun, and where the picture falls.

#include "scene/object_shadow.hpp"
#include "scene/scene.hpp"

#include <array>
#include <optional>

namespace ts2::scene::objectshadow {

// The picture of a model at a facing, looking along the sun line `toSun` (toward the sun). The
// picture holds the box around the model's vertices, every group counted, turned to the facing,
// with half a unit spare on either side across. None for a model under 0.15 in every direction.
std::optional<Picture> picture(const Mesh& mesh, int meshIndex, int facing, const std::array<float, 3>& toSun);

// The pictures and places of a scene's objects whose shadow is Sun. `toSun` is in the lot's own
// frame, and `place` puts the lot where the scene shows it (its objects, floor heights and rooms are
// the lot's). The places come back in the scene's frame.
Layout layout(const Scene& scene, const std::array<float, 3>& toSun, const Transform& place = {});

} // namespace ts2::scene::objectshadow
