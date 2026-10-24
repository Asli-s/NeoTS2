#pragma once

#include "scene/rig.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ts2::scene {

// Rays through a lot's scene as it is drawn, for the RayTrace primitive: what they meet among the
// shown objects, Sims (as posed), walls (the version shown: cut walls are low) and floors.
class SceneRays {
public:
    explicit SceneRays(const Scene& scene) : scene_(scene) {}

    // A point on a lot object or Sim (by game object id): `kind` 0 three quarters up the middle of
    // its model's bounds (which always hold the model's origin), 3 its containment slot `slot`.
    struct Point {
        std::uint32_t object = 0;
        int kind = 0;
        std::size_t slot = 0;
    };
    struct Hit {
        enum class Kind : std::uint8_t { Object, Floor, Wall };
        Kind kind = Kind::Object;
        std::uint32_t object = 0;
        float t = 0;  // along the ray, 0 to 1
    };
    struct Result {
        std::vector<Hit> hits;  // nearest first
        bool slotMissing = false;
        bool objectMissing = false;  // a point's object isn't drawn
    };
    // From one point to another, or (no `to`) 128 tiles ahead of `from` (its facing's +y).
    // `visible` says which instances are shown (all when empty). `pose` places the Sims.
    Result trace(const Point& from, const std::optional<Point>& to, const ScenePose& pose,
                 const std::vector<std::uint8_t>& visible);

private:
    struct Box {
        std::array<float, 3> low{}, high{};
    };
    const Box& primitiveBox(std::size_t mesh, std::size_t primitive);

    const Scene& scene_;
    std::vector<std::vector<std::optional<Box>>> boxes_;  // per mesh and primitive, made on first use
};

} // namespace ts2::scene
