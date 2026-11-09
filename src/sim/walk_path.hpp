#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace ts2::sim {

// A walking Sim follows a uniform cubic B-spline over its route, rounding corners inside rather than
// turning on the spot. Its control points are the route's points with stretches cut into equal pieces
// of a fifth of a tile or a little more, the first and last counted four times. Distances are tiles
// in route coordinates, headings as in turn.hpp.
// The control points: the first, a point ahead along `startHeading`, each stretch's pieces, a point
// before the end along `endHeading` (taken to 45 degrees), and the last.
std::vector<std::pair<float, float>> walkPathPoints(const std::vector<std::pair<float, float>>& points, float startHeading, float endHeading);

// How a route is walked, from the Sim's facing to its end facing. Most are one walk along the path.
// Up to 0.75 tiles, ending within half a tile of the start: a turn to the end facing, then a straight
// walk keeping it. Up to a quarter tile: a 45-degree-step turn to face the path, the walk, and a turn
// to the end facing (for a Sim with step clips, only when the route changes facing and doesn't run
// back past both facings).
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
