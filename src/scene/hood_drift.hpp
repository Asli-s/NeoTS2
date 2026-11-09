#pragma once

#include "formats/effects.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace ts2::scene {

// The neighborhood camera's slow drift, as the game plays it. After mouse and keys rest for more than 30
// game updates (a quarter second, counted in seconds here so frame rate doesn't matter), the "nhoodcam"
// effect starts where the camera looks. It waits, then a wandering particle carries a camera block that
// sets the camera's goal and sways its yaw. Any input ends it, the sway staying until the next drift.
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
