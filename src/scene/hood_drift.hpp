#pragma once

#include "formats/effects.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace ts2::scene {

// The neighborhood camera's slow drift, as the game plays it. Once mouse and keys have rested for
// more than 30 of the game's updates (a quarter second at its 120 a second, counted here in
// seconds so the frame rate doesn't change it), it starts the "nhoodcam" effect where the camera
// looks. The effect's sequence waits, then a wandering particle carries a camera block whose
// position becomes the camera's goal and whose curve sways the camera's yaw. Any mouse movement,
// click, wheel turn or key press ends it, leaving the sway where it was until the next drift begins.
class HoodDrift {
public:
    // What one step asks of the camera.
    struct Step {
        bool foldSway = false;                       // add the current sway into the yaw, then zero it
        std::optional<float> sway;                   // the new yaw sway, in radians
        std::optional<std::array<float, 3>> goal;    // the new goal, before the camera's own limits
    };

    // The drift from the game's effects, or nothing when the effect chain isn't there or holds
    // values it can't use. `seed` starts its random numbers (0 is taken as 1).
    static std::optional<HoodDrift> load(const effects::File& file, std::uint32_t seed);

    // Mouse, wheel or keys were used: the drift ends where it is, and the wait for the next one
    // starts over.
    void interrupt();

    // One camera step of `seconds`, with the camera now looking at `target`.
    Step step(const std::array<float, 3>& target, float seconds);

    bool running() const { return running_; }

private:
    struct Channel {
        float timer = 0, speed = 0, yaw = 0;
    };

    bool sensible() const;
    float random(float low, float high);
    void start(const std::array<float, 3>& target);
    void spawn();
    void wander(float seconds);

    std::vector<std::array<float, 2>> waitRanges_;
    std::array<float, 2> playRange_{};
    effects::MetaParticleBlock particles_;
    effects::CameraBlock camera_;

    float rested_ = 0;  // seconds since the last input
    bool running_ = false, spawnPending_ = false;
    float waiting_ = 0, played_ = 0, playLength_ = 0;
    float age_ = 0, life_ = 0, clock_ = 0;
    std::array<float, 3> base_{}, position_{}, velocity_{};
    std::array<Channel, 2> channels_{};
    std::uint32_t seed_ = 1;
};

} // namespace ts2::scene
