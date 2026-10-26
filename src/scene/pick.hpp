#pragma once

#include "scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace ts2::scene {

struct ScenePose;

// Finds what the mouse is over, as the game does: the nearest triangle a ray from the camera hits
// among the instances shown. Each instance's box is worked out once, so a pick only tests the
// triangles of the instances whose box the ray crosses.
class Picker {
public:
    // Works out where the scene's instances are. Call again after the scene changes. Instances the
    // renderer poses (Sims and what they hold) are left out here (see pick).
    void prepare(const Scene& scene);
    // The same for one instance that changed, moved or was added.
    void update(const Scene& scene, std::size_t instance);

    struct Hit {
        std::size_t instance = 0;
        float distance = 0;  // along the ray, in units of `direction`'s length
        std::uint32_t object = 0;  // the game object it draws (a Sim's for its body), 0 for none
    };
    // The nearest instance hit by the ray from `origin` along `direction`, among those `shown`
    // flags (one per instance, as visibleInstances gives them, all when empty). With `pose`, Sims
    // are hit by their triangles where it poses them, as in the game. What they hold is not hit.
    [[nodiscard]] std::optional<Hit> pick(const Scene& scene, std::span<const std::uint8_t> shown, const std::array<float, 3>& origin,
                            const std::array<float, 3>& direction, const ScenePose* pose = nullptr) const;

private:
    struct Box {
        std::array<float, 3> low{0, 0, 0}, high{0, 0, 0};
        bool pickable = false;
    };
    // What a skinned primitive's vertices are made from, so a box round it however it is posed can
    // be found from its joints alone: its rest box and the joints its vertices use. Not `bounded`
    // when a vertex's weights add up to more than 1 (as in a broken file), as such a vertex can
    // land outside that box.
    struct SkinBound {
        std::size_t vertices = 0;
        std::array<float, 3> low{0, 0, 0}, high{0, 0, 0};
        std::vector<std::uint8_t> joints;
        bool bounded = false;
    };
    const SkinBound& skinBound(std::size_t mesh, std::size_t primitive, const Primitive& p) const;
    std::optional<float> posedHit(const Scene& scene, const ScenePose& pose, std::size_t instance, const std::array<float, 3>& origin,
                                  const std::array<float, 3>& direction, float nearest) const;

    std::vector<Box> boxes_;
    // By mesh and primitive, worked out on a primitive's first pick and kept until prepare.
    mutable std::map<std::pair<std::size_t, std::size_t>, SkinBound> skinBounds_;
};

// What the game's "Hover" light adds to the ambient of the object under the mouse, `seconds` into
// the pulse: a grey of 0.1 whose strength swings between 0.8 and 1 once a second.
float hoverAmbient(double seconds);

} // namespace ts2::scene
