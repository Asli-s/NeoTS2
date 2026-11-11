#pragma once

// How grid roofs cover and meet each other, used by roof_grid.cpp.

#include "scene/lot_parts.hpp"

#include <array>
#include <vector>

namespace ts2::scene {

// A top triangle's corner flags: on the ridge, on a hip, joining another roof.
constexpr int kCornerRidge = 1, kCornerHip = 2, kCornerJunction = 4;

// Whether another roof covers `asked` at a point, as the game tests a top triangle's centre and an
// edge board step's middle. askLevel is the level of the roof being built. roofs are those within
// its reach, its own included.
bool roofCoveredAt(const RoofGrid& asked, float x, float y, int askLevel, const std::vector<const RoofGrid*>& roofs);

// Another roof at the same height as a triangle's corner, with its ridge and hip masks there.
struct CornerRoof {
    const RoofGrid* roof = nullptr;
    int ridge = 0, hip = 0;
};

// A top triangle's corners: their flags (the roof's own ridge and hip bits, and the junction bit
// when another roof is at the same height there) and the other roofs at each.
struct TriangleCorners {
    std::array<int, 3> flags{};
    std::array<std::vector<CornerRoof>, 3> meeting;
};

// Settles which other roofs join the corners, leaving the junction bits as the triangle's pieces
// use them, and returns whether the triangle is left out because another roof's top lies on it.
// face: the triangle's facing bits. centre: its centre. roofs: as for roofCoveredAt.
bool settleMeeting(TriangleCorners& corners, const RoofGrid& roof, int face, std::array<float, 2> centre,
                   const std::vector<const RoofGrid*>& roofs);

} // namespace ts2::scene
