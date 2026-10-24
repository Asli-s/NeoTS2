#pragma once

#include "scene/rig.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ts2::scene {

// How many UI poses a Sim of this age (its look's age: 1 toddler, 2 child, 0x20 baby...) can be
// drawn in for its portrait.
int portraitPoseCount(std::uint32_t age);

// The animation (without "_anim") of UI pose `pick` (1 to portraitPoseCount) for a Sim's age and
// gender (1 female, 2 male): "a-UIpose-M03" for an adult man, "c-..." for children, "p-..." for
// toddlers, "b-UIpose-02" for babies. Teens and elders use the adults' poses.
std::string portraitPose(std::uint32_t age, std::uint32_t gender, int pick);

// The camera a portrait is drawn with: in front of the Sim's head, looking straight back at it.
struct PortraitCamera {
    std::array<float, 3> eye{}, target{};
    float fovY = 0, nearPlane = 0, farPlane = 0;
    static constexpr unsigned kSize = 256;  // the picture is this many pixels square
};

// The portrait camera for a Sim standing at `place` whose head joint is at `head` in its model.
PortraitCamera portraitCamera(const Transform& place, const std::array<float, 3>& head);

// How a portrait is lit, whatever the lot: no ambient and five lights fixed to the camera (a bright
// key from the front left, others from below, the right and behind), turned into the world.
ObjectLighting portraitLighting(const PortraitCamera& camera);

// A Sim posed for its portrait: `pose` at its first frame (at rest when null). Gives each skinned
// instance's joint matrices, the camera and the lighting. False when the actor has no rig or no
// head joint.
struct PortraitShot {
    std::vector<std::pair<std::size_t, std::vector<Transform>>> joints;
    PortraitCamera camera;
    ObjectLighting lighting;
};
[[nodiscard]] bool portraitShot(const Scene& scene, const Actor& actor, const RigAnimation* pose, PortraitShot& out);

} // namespace ts2::scene
