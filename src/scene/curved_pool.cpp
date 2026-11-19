#include "scene/curved_pool.hpp"

#include <cmath>
#include <numbers>

namespace ts2::scene::curvedpool {

namespace {

constexpr int kCircleSides = 64;

// Keeps the part of a convex polygon on the left of the line from a to b (or on its right).
Polygon clip(const Polygon& polygon, const Point& a, const Point& b, bool left) {
    const float dx = b[0] - a[0], dy = b[1] - a[1];
    auto side = [&](const Point& p) {
        const float s = dx * (p[1] - a[1]) - dy * (p[0] - a[0]);
        return left ? s : -s;
    };
    Polygon out;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Point& p = polygon[i];
        const Point& q = polygon[(i + 1) % polygon.size()];
        const float sp = side(p), sq = side(q);
        if (sp >= 0) {
            out.push_back(p);
        }
        if ((sp >= 0) != (sq >= 0)) {
            const float t = sp / (sp - sq);
            out.push_back({p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t});
        }
    }
    return out;
}

float area(const Polygon& polygon) {
    float twice = 0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Point& p = polygon[i];
        const Point& q = polygon[(i + 1) % polygon.size()];
        twice += p[0] * q[1] - q[0] * p[1];
    }
    return std::abs(twice) / 2;
}

// A polygon minus the circle about `c`: the part outside each of its sides in turn.
std::vector<Polygon> minusCircle(const Polygon& polygon, const Point& c) {
    constexpr float kTiny = 1e-7f;  // pieces smaller than this (in square tiles) are dropped
    std::vector<Polygon> pieces;
    Polygon rest = polygon;
    for (int i = 0; i < kCircleSides && rest.size() >= 3; ++i) {
        const float a0 = 2 * std::numbers::pi_v<float> * static_cast<float>(i) / kCircleSides;
        const float a1 = 2 * std::numbers::pi_v<float> * static_cast<float>(i + 1) / kCircleSides;
        const Point a{c[0] + kRadius * std::cos(a0), c[1] + kRadius * std::sin(a0)};
        const Point b{c[0] + kRadius * std::cos(a1), c[1] + kRadius * std::sin(a1)};
        if (auto piece = clip(rest, a, b, false); piece.size() >= 3 && area(piece) > kTiny) {
            pieces.push_back(std::move(piece));
        }
        rest = clip(rest, a, b, true);
    }
    return pieces;
}

} // namespace

Point centre(const content::CurvedPoolCorner& corner) {
    const auto x = static_cast<float>(corner.x), y = static_cast<float>(corner.y);
    switch (corner.direction) {
    case 0: return {x - 1, y - 1};
    case 2: return {x + 1, y - 1};
    case 4: return {x + 1, y + 1};
    default: return {x - 1, y + 1};
    }
}

float turn(const content::CurvedPoolCorner& corner) {
    return static_cast<float>((corner.direction + 6) % 8) * std::numbers::pi_v<float> / 4;
}

std::array<float, 4> patternWall(const content::CurvedPoolCorner& corner) {
    const auto x = static_cast<float>(corner.x), y = static_cast<float>(corner.y);
    switch (corner.direction) {
    case 0: return {x, y + 1, x + 1, y + 1};
    case 2: return {x - 1, y, x - 1, y + 1};
    case 4: return {x, y - 1, x - 1, y - 1};
    default: return {x + 1, y, x + 1, y - 1};
    }
}

bool touches(const content::CurvedPoolCorner& corner, float x0, float y0, float x1, float y1) {
    const auto c = centre(corner);
    const float nx = std::fmax(x0, std::fmin(c[0], x1)) - c[0];
    const float ny = std::fmax(y0, std::fmin(c[1], y1)) - c[1];
    return nx * nx + ny * ny < kRadius * kRadius;
}

TileCorners byTile(const std::vector<content::CurvedPoolCorner>& corners) {
    TileCorners out;
    for (const auto& c : corners) {
        for (const int x : {c.x - 1, c.x}) {
            for (const int y : {c.y - 1, c.y}) {
                out[{x, y}].push_back(c);
            }
        }
    }
    return out;
}

std::vector<Polygon> outside(const Polygon& polygon, const std::vector<content::CurvedPoolCorner>& corners) {
    std::vector<Polygon> pieces{polygon};
    for (const auto& corner : corners) {
        std::vector<Polygon> next;
        for (const auto& piece : pieces) {
            float x0 = piece[0][0], y0 = piece[0][1], x1 = x0, y1 = y0;
            for (const auto& p : piece) {
                x0 = std::fmin(x0, p[0]);
                y0 = std::fmin(y0, p[1]);
                x1 = std::fmax(x1, p[0]);
                y1 = std::fmax(y1, p[1]);
            }
            if (!touches(corner, x0, y0, x1, y1)) {
                next.push_back(piece);
                continue;
            }
            for (auto& rest : minusCircle(piece, centre(corner))) {
                next.push_back(std::move(rest));
            }
        }
        pieces = std::move(next);
    }
    return pieces;
}

} // namespace ts2::scene::curvedpool
