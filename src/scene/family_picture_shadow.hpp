#pragma once
// The soft shadow under the household in its family picture (the material FamilyThumbnailShadow):
// the Sims drawn as seen from its light into a small picture, blurred, and laid on the stage's floor
// behind them.

#include "scene/scene.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace ts2::scene::familyshadow {

// The light's way, toward the light, in the stage's frame (the material's lightDirection).
constexpr std::array<float, 3> kToLight{0.2f, 1, 2};
// How dark the shadow lays itself over the picture where the Sims cover all of its picture: the
// material's shadowStrength 0.5, as the graphics card is handed it (128 / 255).
constexpr float kStrength = 128.0f / 255;
// The Sims' picture, texels square, and how far its blur reaches each way.
constexpr int kResolution = 128;
constexpr int kBlur = 2;
// The floor the shadow lies on: a square this many units across round the stage's origin, at height 0.
constexpr float kFloorSize = 8;

// A box in the stage's frame. Empty until grown.
struct Box {
    std::array<float, 3> low{1e30f, 1e30f, 1e30f}, high{-1e30f, -1e30f, -1e30f};
    [[nodiscard]] bool empty() const { return !(low[0] <= high[0] && low[1] <= high[1] && low[2] <= high[2]); }
};

// Grows `box` by a posed Sim mesh placed at `place`, as the game bounds it: per joint, the box
// round the rest vertices whose first joint it is, in that joint's own frame, moved where the joint
// is posed. `joints` are the mesh's skin matrices (see posedPositions). The mesh's box is made in its
// own frame and then turned by `place`, as a box round that box.
void growByJoints(Box& box, const Mesh& mesh, const std::vector<Transform>& joints, const Transform& place);

// The light's camera for the Sims' box (see objectshadow::boxCamera). None when the box is too
// small or empty.
std::optional<std::array<float, 16>> camera(const Box& box);

// The Sims' picture: where the triangles (x, y, z in the stage's frame, three corners each) cover
// it through `camera`, 1 covered and 0 not, rows from the top. The graphics card the game drew for
// samples each texel at its corner, not its centre.
std::vector<float> coverage(const std::vector<float>& triangles, const std::array<float, 16>& camera);

// The picture blurred as the game blurs it, the edge texels standing in past its edges.
std::vector<float> blurred(const std::vector<float>& picture);

// Lays the blurred picture on the floor of a family picture drawn from `eye` toward `target` with
// `up`, `fovY` radians from top to bottom (square, rows from the top), its colour premultiplied by
// its alpha. Only where the Sims don't cover the picture in full is it laid, and there it darkens
// what is drawn and adds to its alpha.
void lay(Image& picture, const std::array<float, 3>& eye, const std::array<float, 3>& target, const std::array<float, 3>& up,
         float fovY, const std::array<float, 16>& camera, const std::vector<float>& blurredPicture);

} // namespace ts2::scene::familyshadow
