#pragma once

#include "formats/effects.hpp"
#include "scene/particle_emitter.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace ts2::scene {

// One corner of a particle's sprite, in the world.
struct ParticleVertex {
    std::array<float, 3> position{};
    std::array<float, 2> uv{};
    std::array<std::uint8_t, 4> colour{};  // red, green, blue, alpha
};
static_assert(sizeof(ParticleVertex) == 24);

// Where sprites are seen from. The directions are unit length, in the world.
struct ParticleView {
    std::array<float, 3> right{1, 0, 0}, up{0, 0, 1}, forward{0, 1, 0};
    std::array<float, 16> clip{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // world to clip space, by rows
};

// The layer a block's sprites draw in, on the scale of scene::Material::layer: 17 for a block of layer 0, the
// layer of ordinary blended surfaces, and 8 higher for each layer above it.
int particleDrawLayer(const effects::ParticleBlock& block);

// Adds two triangles (six corners) for each living sprite of `emitter`, which plays `block`. `parent` is the
// playing effect's place, which carries the particles with it.
void appendParticleQuads(const effects::ParticleBlock& block, const ParticleEmitter& emitter, const EmitterPlace& parent, const ParticleView& view,
                         std::vector<ParticleVertex>& out);

} // namespace ts2::scene
