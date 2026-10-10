#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ts2::camera {

// One zoom level of a camera from the game's camera files (TSData/Res/UserData/Cameras). Angles in
// degrees; `distance` is the straight-line distance from the camera to what it looks at.
struct ZoomLevel {
    float distance = 0;
    float fov = 0;  // across the screen
    float nearClip = 0, farClip = 0;
    float pitchMin = 0, pitchMax = 0;
};

struct Config {
    std::string name, controller;
    int defaultZoom = 0;
    std::vector<ZoomLevel> levels;  // as listed: farthest first
    int defaultOrientation = 0;
    std::vector<float> orientations;  // yaw angles
};

// Reads the `camera` blocks of one camera file (the syntax is described in cameras.txt).
std::vector<Config> parse(std::string_view text);

// The pitch, in degrees, at a distance: the game blends the minimum pitches of the two zoom levels
// around it by distance, holding the end values beyond the first and last levels.
float pitchAt(const Config& config, float distance);

} // namespace ts2::camera
