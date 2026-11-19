// The rounded corners the curved pool tool makes (content::CurvedPoolCorner): where their quarter
// circle lies, and the shapes of the ground floor and terrain left outside it.
#pragma once

#include "content/lot.hpp"

#include <array>
#include <map>
#include <utility>
#include <vector>

namespace ts2::scene::curvedpool {

using Point = std::array<float, 2>;
using Polygon = std::vector<Point>;

// The radius of the quarter circle, in tiles. The game cuts the ground floor and terrain inside it
// (its poolShapeMask model) and draws its curved wall just inside it (poolShapeWall).
constexpr float kRadius = 2.0f;

// The centre of the corner's quarter circle: the inner corner of its 2 by 2 tiles.
Point centre(const content::CurvedPoolCorner& corner);

// The turn of the poolShapeWall and poolShapeMask models, in radians about the vertical axis, as the
// game turns them: an eighth turn for each step of direction + 6. They are placed at (x, y).
float turn(const content::CurvedPoolCorner& corner);

// The pool wall whose pattern the curved wall takes: the half of the outer wall beside the corner
// point on the side the game picks for each direction, from (x0, y0) to (x1, y1) on the pool's storey.
std::array<float, 4> patternWall(const content::CurvedPoolCorner& corner);

// Whether the corner's quarter circle reaches into the box from (x0, y0) to (x1, y1).
bool touches(const content::CurvedPoolCorner& corner, float x0, float y0, float x1, float y1);

// The corners by the tiles of their 2 by 2 blocks. A corner's circle reaches no other tile.
using TileCorners = std::map<std::pair<int, int>, std::vector<content::CurvedPoolCorner>>;
TileCorners byTile(const std::vector<content::CurvedPoolCorner>& corners);

// What is left of a convex polygon (counter-clockwise or clockwise) outside the quarter circles of
// the corners, as convex polygons. The circle is taken as a 64-sided polygon through it.
std::vector<Polygon> outside(const Polygon& polygon, const std::vector<content::CurvedPoolCorner>& corners);

} // namespace ts2::scene::curvedpool
