#pragma once
// The shadows the game casts from Sims onto the ground with its Shadows option at High: each Sim is
// drawn from the sun into a small picture, blurred, and laid over the floor or ground under it.

#include "scene/window_light.hpp"

#include <array>

namespace ts2::scene::simshadows {

// The picture's size in texels, square.
constexpr int kResolution = 128;
// The flat colour a Sim is drawn in (the material scripts' stdMatShadowColor). Only its coverage
// is used later.
constexpr std::array<float, 3> kCasterColor{0.63f, 0.63f, 0.85f};
// How far the blur reaches outdoors, in texels each way.
constexpr int kOutdoorBlur = 2;
// The blurred picture keeps the shadow where its alpha is above this (out of 255).
constexpr int kAlphaCutoff = 100;

// What a Sim outdoors is drawn into its shadow picture by: world (lot) coordinates to the picture's
// clip space, row-major for column vectors. x and y run -1..1 across the picture (y up), z 0..1
// from 8 units toward the sun to 1 unit away. The picture looks along the sun line `toSun` (toward
// the sun, need not be unit length) at `root`, the Sim's root joint, and holds 0.7 to either side
// and from 1 below the root to 2 above it.
std::array<float, 16> camera(const std::array<float, 3>& toSun, const std::array<float, 3>& root);

// The weight of each blur tap, in 255ths: the taps share 255 in whole numbers, in the order the game
// adds them (across, then up), so the last ones take one more.
int blurWeight(int across, int up, int radius);

// The colour the shadow multiplies the ground by: the sky's light on flat ground with the sun's share
// dimmed by `strength` (a lighting state's projectiveShadowStrength), over the same light with the
// whole sun, channel by channel, as the graphics card is handed it (in 256ths, at most 1).
std::array<float, 3> shade(const windowlight::Daylight& day, float strength);

} // namespace ts2::scene::simshadows
