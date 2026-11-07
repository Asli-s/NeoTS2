#pragma once

#include "scene.hpp"

#include <array>
#include <optional>

// Lot view's fading of the neighborhood's props and the neighbors' lot imposters near the camera
// (Game Options' Fade Distance, userProps.xml's neighborhoodPropFadeDistance). One between the
// camera and the lot turns see-through so the lot shows past it. Everything here is in the lot's
// own coordinates, where a tile is 1 and z is up, except the neighborhood's tile lines.
namespace ts2::scene::propfade {

// A box, low corner then high.
struct Box {
    std::array<float, 3> low{}, high{};
};

// The box around a mesh placed by `place`: its model's box turned and moved, and the box around
// that, as the game's scene nodes give their bounds. Nothing for a mesh without vertices.
std::optional<Box> placedBox(const Mesh& mesh, const Transform& place);

// The neighborhood tile lines (10 world units apart) around a plant's or decoration's ground, as its
// record stores it (x0, y0, x1, y1 in world units): the line at or before each low side and at or
// after each high side, as x0, y0, x1, y1.
std::array<int, 4> groundTiles(const std::array<float, 4>& ground);

// Whether lot view fades a neighborhood plant or decoration at all. One whose top stands more than
// 15 above the lot's level does, and a lower one only when its ground reaches within 3 tiles of the
// lot. `ground` is from groundTiles and `lotTiles` the lot's own tile lines, x0, y0, x1, y1.
bool fadesInLotView(const Box& placed, const std::array<int, 4>& ground, const std::array<int, 4>& lotTiles);

// How near a prop must be to fade: the squared distance along the ground from the camera to the
// lot, measured to the lot's middle on each axis the camera is beyond its edges. Nothing while the
// camera is over the lot (`lotSize` tiles from (0, 0), edges included), when no prop fades.
std::optional<float> reach(const std::array<float, 3>& eye, const std::array<float, 2>& lotSize);

// A prop's alpha for the camera at `eye`: 1 for drawn as it is, less to fade it. `fadeSquared` is
// the Fade Distance squared, as the game keeps it. Nothing for a prop wholly below the lot's level,
// which the game leaves as it was.
std::optional<float> alpha(const Box& prop, const std::array<float, 3>& eye, float reach, float fadeSquared);

// A neighbor's lot imposter's alpha, by the same rule with the curve squared instead of to the
// fourth. Its distance also counts the camera's height above the lot's level, and the camera's
// height against the imposter's top counts however tall the imposter is.
float imposterAlpha(const Box& imposter, const std::array<float, 3>& eye, float reach, float fadeSquared);

} // namespace ts2::scene::propfade
