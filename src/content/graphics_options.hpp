#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace ts2::content {

// A radio button of the Graphics/Performance Options panel (UI script 0x49060F03) and the Graphics
// Rules setting number it picks.
struct GraphicsChoice {
    std::uint32_t button = 0;
    int setting = 0;
};

// A settings group of that panel. Its choice is saved in userProps.xml as "option" + the option's
// name, holding the setting number.
struct GraphicsGroup {
    std::uint32_t window = 0;
    std::string_view option;  // the Graphics Rules option
    int count = 0;
    std::array<GraphicsChoice, 3> choices{};
    // The setting Graphics Rules gives this option at its High base level, used until one is saved.
    int highLevel = 0;
};

// In the order the game applies them. Sim/Object Detail has no Low setting in the rules: its Low
// button is their Medium. UI Transparency is the opposite of the OpaqueUI option it sets.
inline constexpr GraphicsGroup kGraphicsGroups[] = {
    {0x90, "Shadows", 3, {{{0x91, 1}, {0x92, 2}, {0x93, 3}}}, 3},
    {0x80, "LightingQuality", 3, {{{0x81, 1}, {0x82, 2}, {0x83, 3}}}, 3},
    {0x50, "EffectsQuality", 3, {{{0x51, 1}, {0x52, 2}, {0x53, 3}}}, 3},
    {0x60, "ObjectDetail", 2, {{{0x61, 2}, {0x62, 3}}}, 3},
    {0xA0, "ObjectHiding", 2, {{{0xA1, 0}, {0xA2, 1}}}, 0},
    {0x30, "SnowOnGround", 2, {{{0xC1, 0}, {0xC2, 1}}}, 1},
    {0x40, "MaterialDetail", 3, {{{0x41, 1}, {0x42, 2}, {0x43, 3}}}, 3},
    {0x70, "Reflection", 2, {{{0x71, 0}, {0x72, 1}}}, 1},
    {0x20, "OpaqueUI", 2, {{{0x21, 1}, {0x22, 0}}}, 0},
};

// The button that shows a setting number, 0 when none of the group's does (a broken file's number).
constexpr std::uint32_t buttonForSetting(const GraphicsGroup& group, int setting) {
    for (int i = 0; i < group.count; ++i) {
        if (group.choices[static_cast<std::size_t>(i)].setting == setting) {
            return group.choices[static_cast<std::size_t>(i)].button;
        }
    }
    return 0;
}

// The levels of NeoTS2's Smooth Edges: the samples each pixel takes, with 1 for off.
inline constexpr std::array<int, 4> kSmoothEdgeSamples{1, 2, 4, 8};

// The level a saved Smooth Edges choice asks for, off for anything that isn't a level.
inline int savedSmoothEdges(std::string_view text) {
    int samples = 0;
    const char* end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, samples);
    const bool isLevel = error == std::errc{} && stop == end && std::find(kSmoothEdgeSamples.begin(), kSmoothEdgeSamples.end(), samples) != kSmoothEdgeSamples.end();
    return isLevel ? samples : 1;
}

// The levels of NeoTS2's Ambient Occlusion, by the number saved: off, then ever darker. Each is the power
// how open a pixel is gets raised to.
inline constexpr std::array<float, 4> kAmbientOcclusionStrengths{0.0f, 0.6f, 1.4f, 2.0f};

// The level a saved Ambient Occlusion choice asks for, off for anything that isn't a level.
inline int savedAmbientOcclusion(std::string_view text) {
    int level = 0;
    const char* end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, level);
    return error == std::errc{} && stop == end && level >= 0 && level < static_cast<int>(kAmbientOcclusionStrengths.size()) ? level : 0;
}

} // namespace ts2::content
