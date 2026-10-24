#pragma once

#include "formats/effects.hpp"
#include "scene/builder.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ts2::scene {

// The plumbobs the game shows over lots in the neighborhood view: one visual effect per lot (the
// green plumbob over a lived-in house), whose metaparticle plays a bundle of particle systems (the
// spinning shape, its glows and shines). Only what those bundles use is played: systems that keep
// one particle alive or emit one at a time, with a fixed life and spawn point. Others are left out.

// The name the plumbobs' instances are placed under.
inline constexpr const char* kNeighborhoodPlumbobName = "neighborhood plumbob";

class NeighborhoodPlumbobs {
public:
    // Adds effect `effect` of `file` at each of `anchors` (where the game starts the effect). Returns
    // how many particle systems each one plays, 0 when the effect is missing or plays none.
    std::size_t add(SceneBuilder& builder, const effects::File& file, const std::string& effect,
                    const std::vector<std::array<float, 3>>& anchors);

    // Where each part is drawn `seconds` after the neighborhood opened, for a camera whose right
    // and up directions (unit, in the world) are given: sprites face the camera.
    void place(double seconds, const std::array<float, 3>& right, const std::array<float, 3>& up,
               std::vector<std::pair<std::size_t, Transform>>& out) const;

    bool empty() const { return parts_.empty(); }

private:
    struct Part {
        std::size_t instance = 0;
        std::size_t block = 0;      // in blocks_
        std::array<float, 3> at{};  // where its particle is
        float scale = 1;            // the metaparticle's size
    };
    std::vector<effects::ParticleBlock> blocks_;
    std::vector<Part> parts_;
};

} // namespace ts2::scene
