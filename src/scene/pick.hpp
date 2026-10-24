#pragma once

#include "scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ts2::scene {

// Finds what the mouse is over, as the game does: the nearest triangle a ray from the camera hits
// among the instances shown. Each instance's box is worked out once, so a pick only tests the
// triangles of the instances whose box the ray crosses.
class Picker {
public:
    // Works out where the scene's instances are. Call again after the scene changes. Instances the
    // renderer poses (Sims and what they hold) are left out.
    void prepare(const Scene& scene);
    // The same for one instance that changed, moved or was added.
    void update(const Scene& scene, std::size_t instance);

    struct Hit {
        std::size_t instance = 0;
        float distance = 0;  // along the ray, in units of `direction`'s length
    };
    // The nearest instance hit by the ray from `origin` along `direction`, among those `shown`
    // flags (one per instance, as visibleInstances gives them, all when empty).
    [[nodiscard]] std::optional<Hit> pick(const Scene& scene, std::span<const std::uint8_t> shown, const std::array<float, 3>& origin,
                            const std::array<float, 3>& direction) const;

private:
    struct Box {
        std::array<float, 3> low{0, 0, 0}, high{0, 0, 0};
        bool pickable = false;
    };
    std::vector<Box> boxes_;
};

// What the game's "Hover" light adds to the ambient of the object under the mouse, `seconds` into
// the pulse: a grey of 0.1 whose strength swings between 0.8 and 1 once a second.
float hoverAmbient(double seconds);

} // namespace ts2::scene
