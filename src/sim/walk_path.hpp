#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace ts2::sim {

// The line a walking Sim follows along its route, as the game lays it: a uniform cubic B-spline
// whose control points are the route's points with each stretch cut into equal pieces of a fifth of
// a tile or a little more, plus a point just ahead of the Sim along its facing and one just before
// the end along the facing it ends in, so it sets off and arrives facing those ways. The first and last points
// count four times over so that it starts and ends on them. It rounds each corner smoothly,
// cutting inside it, rather than turning on the spot there. Distances are in tiles, in route
// coordinates. Headings are as in turn.hpp.
// The points the curve is laid over, as the game makes them from a route's points: the first, the
// point ahead along `startHeading`, each stretch's pieces, the point before the end along
// `endHeading` (taken to 45 degrees), and the last.
std::vector<std::pair<float, float>> walkPathPoints(const std::vector<std::pair<float, float>>& points, float startHeading, float endHeading);

// How the game walks a route, from the Sim's facing to the facing it ends in:
// - most routes are one walk along the path above.
// - a short route (up to 0.75 tiles, ending within half a tile of the start) is a turn on the spot
//   to the end facing, then a walk straight from the start to the end keeping that facing.
// - a short route of up to a quarter tile is a turn to face the path (in 45-degree steps), the
//   walk along the path, and a turn to the end facing. For a Sim with step clips this needs the
//   route to change the facing and not run back past both facings.
struct RouteShape {
    bool straight = false;               // the walk goes straight from the start to the end
    float turnTo = 0;                    // the facing to turn to before the walk (none when it is the Sim's)
    float walkFrom = 0, walkTo = 0;      // the facings the walk starts and ends in
    std::optional<float> turnAfter;      // the facing to turn to after it
};
RouteShape routeShape(const std::vector<std::pair<float, float>>& points, float startHeading, float endHeading, bool hasSteps);

class WalkPath {
public:
    WalkPath() = default;
    // From the first point (where the Sim stands, facing `startHeading`) through the rest in order,
    // ending facing `endHeading`.
    // `straight`: the path is laid straight from the first point to the last.
    WalkPath(const std::vector<std::pair<float, float>>& points, float startHeading, float endHeading, bool straight = false);

    float length() const { return lengths_.empty() ? 0.0f : lengths_.back(); }

    // Where the path is `distance` along it (clamped to its ends), and the way it runs there
    // (see turn.hpp), none where it has no length.
    struct Point {
        float x = 0, y = 0;
        std::optional<float> heading;
    };
    Point at(float distance) const;

private:
    std::array<float, 2> curve(std::size_t piece, float t) const;
    std::vector<std::array<float, 2>> control_;
    // The path sampled evenly in each piece's parameter, with the length walked to each sample.
    std::vector<float> lengths_;
};

} // namespace ts2::sim
