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
    struct State {
        std::map<std::string, Light> lights;     // by name ("sun", "sky", ...)
        std::optional<std::uint32_t> lightMap;   // the image a light map is looked up in
        float lightMapStrength = 1;
        std::array<float, 3> lightMapTint{1, 1, 1};
    };
    std::map<std::string, std::string> values;  // set / setf / seti / setb, by name
    std::map<std::string, State> states;

    float number(const std::string& name, float fallback) const;
};

Lighting readLighting(const std::vector<Product>& products);

// An image from the products' lighting packages (TSData/Res/Lighting/lighting.package) by
// instance, the newest product's copy winning, as RGBA rows; empty when none has it.
struct LightingImage {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};
std::optional<LightingImage> lightingImage(const std::vector<Product>& products, std::uint32_t instance);

} // namespace ts2::content
