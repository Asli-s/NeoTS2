#include "sim/walk_path.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::sim {

namespace {

// Samples per piece for measuring along the curve. The game measures to 0.005 tiles, which this
// matches for the short pieces routes have.
constexpr std::size_t kSamplesPerPiece = 32;
// A route longer than this is cut short: the router makes far fewer points, so more is broken.
constexpr std::size_t kMostPoints = 100000;
// The way the path runs is read over this distance.
constexpr float kHeadingSpan = 0.01f;

constexpr float kPi = 3.14159265358979f;
constexpr float kQuarterPi = kPi / 4;
// The facing points lie a third of the first and last pieces from the ends, at most this far.
constexpr float kFacingShare = 0.3333f;
constexpr float kFacingMost = 0.25f;
// Each stretch is cut into whole pieces, this many to a tile (rounded down).
constexpr float kPiecesPerTile = 5.0f;
// A facing more than this far off the path's way is first turned towards it in 45-degree steps.
constexpr float kFacingSlack = 40.0f * kPi / 180.0f;
// Short routes: up to this long, ending this close to where they start. Up to this long they may
// turn to face the path first. A facing change below this counts as none.
constexpr float kShortRoute = 0.75f;
constexpr float kShortReach = 0.5f;
constexpr float kShortTurnRoute = 0.25f;
constexpr float kSameFacing = 0.01f;
constexpr float kHalfPi = 3.14159265358979f / 2;

using Point2 = std::array<float, 2>;

float wrapped(float a) {
    a = std::remainder(a, 2 * kPi);
    return a <= -kPi ? a + 2 * kPi : a;
}
float headingOf(const Point2& v) {
    return std::atan2(-v[1], v[0]);  // the world's x and y are the route's y and x
}
Point2 facingOf(float heading) {
    return {std::cos(heading), -std::sin(heading)};
}
// `heading` turned in 45-degree steps until it lies within 40 degrees of the way `v` runs.
float towards(float heading, const Point2& v) {
    if (!(std::hypot(v[0], v[1]) > 0)) {
        return heading;
    }
    float d = wrapped(headingOf(v) - heading);
    int steps = 0;
    while (d < -kFacingSlack && steps > -8) { --steps; d += kQuarterPi; }
    while (d > kFacingSlack && steps < 8) { ++steps; d -= kQuarterPi; }
    return heading + static_cast<float>(steps) * kQuarterPi;
}

} // namespace

namespace {

// The path's points (see walkPathPoints), and the facing its last piece arrives in.
std::vector<std::pair<float, float>> layPoints(const std::vector<std::pair<float, float>>& raw, float startHeading, float endHeading, float& arrival) {
    std::vector<std::pair<float, float>> out;
    arrival = endHeading;
    std::vector<Point2> route;
    for (std::size_t i = 0; i < raw.size() && i < kMostPoints; ++i) {
        route.push_back({std::isfinite(raw[i].first) ? raw[i].first : 0.0f, std::isfinite(raw[i].second) ? raw[i].second : 0.0f});
    }
    if (route.empty()) {
        return out;
    }
    const float h0 = std::isfinite(startHeading) ? startHeading : 0.0f;
    // The end facing is taken to the nearest 45 degrees.
    const float h2 = std::isfinite(endHeading) ? std::floor(endHeading / kQuarterPi + 0.5f) * kQuarterPi : 0.0f;
    float length = 0;
    for (std::size_t i = 1; i < route.size(); ++i) {
        length += std::hypot(route[i][0] - route[i - 1][0], route[i][1] - route[i - 1][1]);
    }
    const Point2 first = route.front(), last = route.back();
    const auto add = [&](const Point2& p, int times) {
        for (int i = 0; i < times && out.size() < kMostPoints; ++i) {
            out.emplace_back(p[0], p[1]);
        }
    };
    add(first, 1);
    if (route.size() >= 2) {
        // Just ahead of the Sim along its facing (turned towards the first stretch), a third of it on.
        const Point2 v{route[1][0] - first[0], route[1][1] - first[1]};
        const float firstLength = std::hypot(v[0], v[1]);
        Point2 from = first;
        float before = 1;
        if (firstLength > 0) {
            const float d = std::min(kFacingMost, firstLength * kFacingShare);
            const Point2 f = facingOf(towards(h0, v));
            from = {first[0] + d * f[0], first[1] + d * f[1]};
            before = d;
        }
        // Each stretch from the last point laid, cut into equal pieces.
        Point2 lastPiece{0, 0};
        for (std::size_t i = 1; i < route.size(); ++i) {
            if (before > 0) {
                add(from, 1);
            }
            const Point2 seg{route[i][0] - from[0], route[i][1] - from[1]};
            const float segLength = std::hypot(seg[0], seg[1]);
            const int pieces = static_cast<int>(std::min(kPiecesPerTile * segLength, static_cast<float>(kMostPoints)));
            for (int j = 1; j < pieces; ++j) {
                add({from[0] + seg[0] * static_cast<float>(j) / static_cast<float>(pieces), from[1] + seg[1] * static_cast<float>(j) / static_cast<float>(pieces)}, 1);
            }
            lastPiece = pieces > 0 ? Point2{seg[0] / static_cast<float>(pieces), seg[1] / static_cast<float>(pieces)} : seg;
            before = segLength;
            from = route[i];
        }
        // Just before the end along the end facing (turned towards the last piece), a third of it back.
        const float pieceLength = std::hypot(lastPiece[0], lastPiece[1]);
        const float way = pieceLength > 0 ? headingOf(lastPiece) : h0;
        const float d = std::min(kFacingMost, pieceLength * kFacingShare);
        arrival = h2 - (towards(way, facingOf(h2)) - way);
        const Point2 f = facingOf(arrival);
        add({last[0] - d * f[0], last[1] - d * f[1]}, 1);
    }
    if (route.size() >= 2) {
        add(last, 1);
    }
    return out;
}

} // namespace

std::vector<std::pair<float, float>> walkPathPoints(const std::vector<std::pair<float, float>>& raw, float startHeading, float endHeading) {
    float arrival = 0;
    return layPoints(raw, startHeading, endHeading, arrival);
}

RouteShape routeShape(const std::vector<std::pair<float, float>>& raw, float startHeading, float endHeading, bool hasSteps) {
    const float h0 = std::isfinite(startHeading) ? startHeading : 0.0f;
    const float h2 = std::isfinite(endHeading) ? std::floor(endHeading / kQuarterPi + 0.5f) * kQuarterPi : 0.0f;
    RouteShape shape;
    shape.turnTo = shape.walkFrom = h0;
    shape.walkTo = h2;
    float length = 0;
    for (std::size_t i = 1; i < raw.size() && i < kMostPoints; ++i) {
        length += std::hypot(raw[i].first - raw[i - 1].first, raw[i].second - raw[i - 1].second);
    }
    if (raw.size() < 2 || !(length > 0) || !std::isfinite(length)) {
        return shape;
    }
    const Point2 move{raw.back().first - raw.front().first, raw.back().second - raw.front().second};
    if (length > kShortRoute || !(std::hypot(move[0], move[1]) <= kShortReach)) {
        return shape;
    }
    bool turnsOnTheSpot = length > kShortTurnRoute;
    if (!turnsOnTheSpot && hasSteps) {
        const float s = headingOf(move);
        const bool behindBoth = std::abs(wrapped(s - h0)) >= kHalfPi && std::abs(wrapped(s - h2)) >= kHalfPi;
        turnsOnTheSpot = behindBoth || std::abs(wrapped(h2 - h0)) < kSameFacing;
    }
    if (turnsOnTheSpot) {
        shape.straight = true;
        shape.turnTo = shape.walkFrom = h2;
        return shape;
    }
    // Facing the path, along it, then to the end facing.
    const Point2 first{raw[1].first - raw[0].first, raw[1].second - raw[0].second};
    shape.turnTo = shape.walkFrom = towards(h0, first);
    float arrival = h2;
    layPoints(raw, shape.walkFrom, h2, arrival);
    shape.walkTo = arrival;
    shape.turnAfter = h2;
    return shape;
}

WalkPath::WalkPath(const std::vector<std::pair<float, float>>& raw, float startHeading, float endHeading, bool straight) {
    auto points = straight && raw.size() >= 2 ? std::vector<std::pair<float, float>>{raw.front(), raw.back()} : walkPathPoints(raw, startHeading, endHeading);
    for (auto& p : points) {
        p = {std::isfinite(p.first) ? p.first : 0.0f, std::isfinite(p.second) ? p.second : 0.0f};
    }
    if (points.empty()) {
        return;
    }
    // The first and last count four times over, so the curve starts and ends on them.
    for (std::size_t i = 0; i < points.size(); ++i) {
        const int times = i == 0 || i + 1 == points.size() ? 4 : 1;
        for (int k = 0; k < times; ++k) {
            control_.push_back({points[i].first, points[i].second});
        }
    }
    const std::size_t pieces = control_.size() >= 4 ? control_.size() - 3 : 0;
    lengths_.reserve(pieces * kSamplesPerPiece + 1);
    lengths_.push_back(0);
    auto previous = control_.front();
    for (std::size_t p = 0; p < pieces; ++p) {
        for (std::size_t k = 1; k <= kSamplesPerPiece; ++k) {
            const auto c = curve(p, static_cast<float>(k) / kSamplesPerPiece);
            lengths_.push_back(lengths_.back() + std::hypot(c[0] - previous[0], c[1] - previous[1]));
            previous = c;
        }
    }
}

// Piece p's point at t (0-1): the uniform cubic B-spline over control points p to p + 3.
std::array<float, 2> WalkPath::curve(std::size_t piece, float t) const {
    const float u = 1 - t, t2 = t * t, t3 = t2 * t;
    const float w[4] = {u * u * u / 6, (3 * t3 - 6 * t2 + 4) / 6, (-3 * t3 + 3 * t2 + 3 * t + 1) / 6, t3 / 6};
    std::array<float, 2> out{0, 0};
    for (int i = 0; i < 4; ++i) {
        out[0] += w[i] * control_[piece + i][0];
        out[1] += w[i] * control_[piece + i][1];
    }
    return out;
}

WalkPath::Point WalkPath::at(float distance) const {
    if (control_.empty()) {
        return {};
    }
    if (lengths_.size() < 2) {
        return {control_.front()[0], control_.front()[1], std::nullopt};
    }
    const float total = length();
    const auto place = [&](float d) {
        d = std::clamp(std::isfinite(d) ? d : 0.0f, 0.0f, total);
        const std::size_t i = std::min<std::size_t>(std::upper_bound(lengths_.begin(), lengths_.end(), d) - lengths_.begin(), lengths_.size() - 1);
        const std::size_t sample = i == 0 ? 0 : i - 1;
        const float span = lengths_[sample + 1] - lengths_[sample];
        const float within = span > 0 ? (d - lengths_[sample]) / span : 0.0f;
        const std::size_t piece = std::min(sample / kSamplesPerPiece, lengths_.size() / kSamplesPerPiece - 1);
        const float t = (static_cast<float>(sample - piece * kSamplesPerPiece) + within) / kSamplesPerPiece;
        return curve(piece, std::clamp(t, 0.0f, 1.0f));
    };
    const auto here = place(distance);
    Point out{here[0], here[1], std::nullopt};
    // The way it runs: towards a point a little ahead (behind at the very end).
    const float d = std::clamp(std::isfinite(distance) ? distance : 0.0f, 0.0f, total);
    const float a = std::min(d, std::max(0.0f, total - kHeadingSpan));
    const auto from = place(a), to = place(a + kHeadingSpan);
    const float dx = to[0] - from[0], dy = to[1] - from[1];
    if (std::hypot(dx, dy) > 1e-6f) {
        out.heading = std::atan2(-dy, dx);  // the world's x and y are the route's y and x
    }
    return out;
}

} // namespace ts2::sim
