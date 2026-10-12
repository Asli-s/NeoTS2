#pragma once

#include <array>
#include <cstdint>

namespace ts2::scene {

// A light that lights a lot's floors and walls indoors: a lamp, or the sun beam or sky spill
// coming in through a window.
struct LightSource {
    std::array<float, 3> color{0, 0, 0};
    int level = 0;
    std::int16_t room = 0;            // it only lights this room
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // the area it lights evenly, in tiles
    float falloff = 1;                // how fast it fades with distance
    float floorIntensity = 1;
    float wallIntensity = 1;
    float interFloorFalloff = 0.5f;   // lost per floor it shines down through; 1 or more: none passes

    // A window's light shines one way: toward `angle` (radians, around the vertical), more so
    // the higher `directionalMix` (0..1).
    bool directional = false;
    float angle = 0;
    float directionalMix = 0;
};

} // namespace ts2::scene
