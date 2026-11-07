// What every renderer takes from the game: where to look, and what to draw over the scene.
#pragma once

#include "render/math.hpp"
#include "scene/object_shadow.hpp"
#include "scene/particle_quads.hpp"

#include <array>
#include <cstddef>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace ts2::render {

// Where the camera is and what it looks at, in game coordinates (z up).
struct Camera {
    Vec3 eye{10, -10, 10};
    Vec3 target{0, 0, 0};
    float fovY = 0.7f;  // radians
    float fovX = 0;  // radians across the screen, used instead of fovY when set
    float farPlane = 500;  // nothing farther is drawn
    float nearPlane = 0;  // 0: a ten-thousandth of farPlane
    Vec3 up{0, 0, 1};
    // With a width, the view is square on instead of in perspective: left, right, bottom and top around
    // the line from the eye to the target, in world units.
    std::array<float, 4> orthographic{};
};

// A textured rectangle drawn over the scene, in screen pixels from the top left. Texture -1 draws
// the plain color.
struct Sprite {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    int texture = -1;
    float color[4] = {1, 1, 1, 1};
};

// A run of particle corners drawn with one texture and one way of drawing.
struct ParticleBatch {
    int image = -1;               // the scene's image
    std::uint8_t draw = 0;        // see effects::ParticleBlock::Draw
    int layer = 0;                // on the scale of scene::Material::layer
    std::uint32_t firstVertex = 0, vertices = 0;
};

// A Sim casting the game's projected shadow onto the ground under it (see scene/sim_shadows.hpp).
struct ShadowCaster {
    std::vector<std::size_t> instances;  // its skinned instances
    Vec3 root{};                         // its root joint, in world coordinates
    int level = 0;                       // the storey it stands on, as scene::Instance::level counts them
};

// The projected shadows drawn from the next frame on.
struct Shadows {
    std::vector<ShadowCaster> casters;
    Vec3 toSun{0, 0, 1};                  // the lighting state's sun line, toward the sun
    std::array<float, 3> shade{1, 1, 1};  // what a shadow multiplies the ground by (see simshadows::shade)
    int groundLevel = 0;                  // the level of the lot's terrain
    // The lot objects' shadows from the sun. Their pictures are drawn again only when this changes.
    std::shared_ptr<const scene::objectshadow::Layout> objects;
};

} // namespace ts2::render
