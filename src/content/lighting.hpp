#pragma once

#include "content/install.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ts2::content {

// The game's lighting settings (TSData/Res/Lights/Lighting.txt of every product, in release
// order, with the files they include). A later product's lighting state adds to and overrides an
// earlier one's of the same name rather than replacing it.
struct Lighting {
    struct Light {
        std::array<float, 3> direction{0, 0, 1};  // toward the light, as written (not normalized)
        std::array<float, 3> color{1, 1, 1};
        float strength = 1;
    };
    // How strongly one kind of light shows in a state ("modifiers <kind> ..."): kinds are
    // insidePortals, outsidePortals, insidePortalSpills, outsidePortalSpills, insideObjects and
    // outsideObjects. "Practical" is the light drawn on things, "diffusion" its share of the floor
    // and wall light.
    struct Modifier {
        float practicalStrength = 0, diffusionStrength = 0;
        float practicalFalloff = 1, diffusionFalloff = 1;  // kept when a line leaves them out
    };
    // Light given to rooms no lamp or window lights enough ("unlitRoomColor"): `color` times how far
    // a room's brightest channel is below `threshold` (0: every unlit room gets all of it).
    struct UnlitRoom {
        std::array<float, 3> color{0, 0, 0};
        float threshold = 0;
    };
    struct State {
        std::map<std::string, Light> lights;     // by name ("sun", "sky", ...)
        std::array<float, 3> skylight{0, 0, 0};  // the sky's colour times its strength
        std::optional<std::uint32_t> lightMap;   // the image a light map is looked up in
        float lightMapStrength = 1;
        std::array<float, 3> lightMapTint{1, 1, 1};
        std::map<std::string, Modifier> modifiers;
        std::map<std::string, std::vector<float>> diffParams;  // "diffParam <name> <value>", by name
        std::optional<UnlitRoom> unlitRoom;
    };
    // One object light's settings from the "lightAttribOverride <light> <setting> <value>" lines,
    // the last line for each setting winning; unset ones keep the game's defaults.
    struct LightAttributes {
        // The light as drawn.
        std::optional<float> intensity, falloffRate, relSpecIntensity, range;
        std::optional<float> coneAngle, conePenumbra;  // in degrees, as written
        std::optional<std::array<float, 3>> color;
        // Its share of the floor and wall light (the "diffusionSource..." settings).
        std::optional<std::array<float, 2>> diffusionMin, diffusionMax;
        std::optional<float> diffusionIntensity, diffusionFalloff, diffusionDirectionalFalloff;
        std::optional<float> diffusionWallScale, diffusionFloorScale, diffusionInterFloorFalloff;
        std::optional<std::array<float, 3>> diffusionColor;
        std::optional<bool> diffusionDirectional;
    };
    std::map<std::string, std::string> values;  // set / setf / seti / setb, by name
    std::map<std::string, State> states;
    std::map<std::string, LightAttributes> lights;  // by light name in lower case

    float number(const std::string& name, float fallback) const;
    // A light's settings by its name in a model (any case); null when no line names it.
    const LightAttributes* light(const std::string& name) const;
};

Lighting readLighting(const std::vector<Product>& products);
// The same read from one file's text (its includes are not followed).
Lighting readLightingText(const std::string& text);

// An image from the products' lighting packages (TSData/Res/Lighting/lighting.package) by
// instance, the newest product's copy winning, as RGBA rows; empty when none has it.
struct LightingImage {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};
std::optional<LightingImage> lightingImage(const std::vector<Product>& products, std::uint32_t instance);

} // namespace ts2::content
