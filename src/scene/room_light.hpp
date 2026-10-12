#pragma once

#include <array>
#include <optional>
#include <vector>

namespace ts2::scene::roomlight {

// How the game turns a room's light into the lights its objects, Sims and doors/windows get.
// A room keeps six coloured samples of the light reaching it from each direction; each is a
// weighted average of what its floors, walls and windows show.

enum Direction { kPlusX, kMinusX, kPlusY, kMinusY, kFromFloor, kFromCeiling };

struct Samples {
    std::array<std::array<float, 3>, 6> sum{};
    std::array<float, 6> weight{};

    void add(Direction d, const std::array<float, 3>& color, float w);
    // Light from a surface facing (nx, ny): split onto the side samples by how much it faces each.
    void addFacing(float nx, float ny, const std::array<float, 3>& color);
    // The averages (a sample with no weight keeps its sum, which is black).
    std::array<std::array<float, 3>, 6> averages() const;
};

// A room's light as each group of things is lit by it ("object:", "sim:", "wall:" in the
// lighting settings; the wall group lights doors, windows and arches).
struct Settings {
    int type = 4;                  // roomLightingType: 2 + the number of directional lights
    float overall = 0.4f;          // roomOverallStrength
    float bounce = 0.25f;          // roomBounceStrength
    float bounceCrossover = 1;     // roomBounceCrossover
    float side = 1.2f;             // roomSideStrength
    float sideCrossover = 1;       // roomSideCrossover
    float sideSlope = 0.75f;       // roomSideSlope
    float specular = 0.8f;         // roomSpecular
    float fromLightMapMix = 0;     // roomFromLightMapMix
    float fromLightMapStrength = 1;
    float directionalBoost = 1;    // roomDirectionalBoost
};

struct Directional {
    std::array<float, 3> color{0, 0, 0};      // already times the directional boost
    std::array<float, 3> direction{0, 0, 1};  // as the game stores it: not normalized
};

struct Lights {
    std::array<float, 3> ambient{0, 0, 0};
    bool ambientFromLightMap = false;  // the game then reads the ambient from the light map instead
    std::vector<Directional> directionals;
    float specular = 0;
};

// The ambient is the average of the six directions; the brightest remaining directions become
// directional lights, side ones tilted down by the slope. Types 0 and 1 (five or four
// directionals and no ambient) are not supported; they give nothing.
std::optional<Lights> lightsFor(const std::array<std::array<float, 3>, 6>& samples, const Settings& settings);

// The room's plain average light, which the game gives everything in the room as well.
std::array<float, 3> average(const std::array<std::array<float, 3>, 6>& samples);

// How much of the unlit room light (`color`) a room gets: 1 - its brightest average channel /
// `threshold` below the threshold, else 0; with no threshold all of it unless a lamp or window
// lights the room. The game also adds half of it (times this share) to the room's light-map texels.
float unlitShare(const std::array<std::array<float, 3>, 6>& samples, const std::array<float, 3>& color, float threshold, bool litBySource);
// Light added to rooms darker than `threshold` (their brightest channel of the average):
// `color` times how far below the threshold they are, on the four sides and the floor sample.
// With no threshold (0 or less) every room that no lamp or window lights gets the whole colour.
void addUnlitLight(std::array<std::array<float, 3>, 6>& samples, const std::array<float, 3>& color, float threshold,
                   bool litBySource);

} // namespace ts2::scene::roomlight
