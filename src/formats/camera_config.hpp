#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ts2::camera {

// One zoom level of a camera from the game's camera files (TSData/Res/UserData/Cameras). Angles are in
// degrees. `distance` is the straight-line distance from the camera to what it looks at.
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
    // A camera without zoom levels (the free camera) gives one near and far clip.
    std::optional<std::array<float, 2>> clip;
    // How far in from each edge of the neighborhood the camera's target is kept, as fractions of its
    // size (`constrain`). Which value is which edge is not known yet. Every file gives four equal values.
    std::optional<std::array<float, 4>> constrain;
    // The camera's named settings (`setparam <name> <value>`), by name in lower case.
    std::map<std::string, float> params;
    // A named setting, or `otherwise` when the file doesn't set it.
    float param(const std::string& key, float otherwise) const {
        const auto it = params.find(key);
        return it == params.end() ? otherwise : it->second;
    }
};

// Reads the `camera` blocks of one camera file (the syntax is described in cameras.txt).
std::vector<Config> parse(std::string_view text);

// The pitch, in degrees, at a distance: the game blends the pitches of the two zoom levels around
// it by distance, holding the end values beyond the first and last levels. `blend` (0 to 1) goes
// from the levels' minimum pitch to their maximum.
float pitchAt(const Config& config, float distance, float blend = 0);
// The near clip distance at a distance, blended between zoom levels the same way.
float nearClipAt(const Config& config, float distance);
// The zoom level a distance is at: the first level, farthest first, at or nearer than it, else the
// nearest level. -1 for a camera without levels.
int zoomLevelAt(const Config& config, float distance);

} // namespace ts2::camera
