// What every renderer takes from the game: where to look, and what to draw over the scene.
#pragma once

#include "render/math.hpp"

namespace ts2::render {

// Where the camera is and what it looks at, in game coordinates (z up).
struct Camera {
    Vec3 eye{10, -10, 10};
    Vec3 target{0, 0, 0};
    float fovY = 0.7f;  // radians
    float fovX = 0;  // radians across the screen, used instead of fovY when set
    float farPlane = 500;  // nothing farther is drawn
    float nearPlane = 0;  // 0: a ten-thousandth of farPlane
};

// A textured rectangle drawn over the scene, in screen pixels from the top left. Texture -1 draws
// the plain color.
struct Sprite {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    int texture = -1;
    float color[4] = {1, 1, 1, 1};
};

} // namespace ts2::render
