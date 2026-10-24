#include "sim/path_passes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>

namespace ts2::sim {

namespace {

// Costs above this count as blocked.
constexpr double kBlockedAbove = 1.7014117e38;
// The quadtree walks give up below this many levels. Real grids are far shallower.
constexpr int kMaxDepth = 16;

float distance(float ax, float ay, float bx, float by) { return std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay)); }
float distance(const RouteWaypoint& a, const RouteWaypoint& b) { return distance(a.x, a.y, b.x, b.y); }

// Starts, goals and the points either side of a change of level never move.
bool fixedPoint(WaypointKind k) {
    return k == WaypointKind::Start || k == WaypointKind::Goal || k == WaypointKind::LevelEnd || k == WaypointKind::LevelStart;
}
// Door points are kept too, though they may be nudged.
bool keptPoint(WaypointKind k) { return fixedPoint(k) || k == WaypointKind::DoorEntry || k == WaypointKind::DoorExit; }

// Clips the segment (x0, y0)-(x1, y1) to `r` widened by `tolerance`. True when a piece of it at
// least a hundred-thousandth of its length lies inside. `length` gets that piece's length.
bool clip(const RouteRect& r, float tolerance, float x0, float y0, float x1, float y1, float* length) {
    float t0 = 0, t1 = 1;
    const auto side = [&](float lo, float hi, float a, float b) {
        if (a <= lo && b <= lo) {
            return false;
        }
        if (a < lo && lo < b) {
            t0 = std::max(t0, (lo - a) / (b - a));
        } else if (b < lo && lo < a) {
            t1 = std::min(t1, (lo - a) / (b - a));
        }
        if (t1 < t0 || (hi <= a && hi <= b)) {
            return false;
        }
        if (a < hi && hi < b) {
            t1 = std::min(t1, (hi - a) / (b - a));
        } else if (b < hi && hi < a) {
            t0 = std::max(t0, (hi - a) / (b - a));
        }
        return t0 <= t1;
    };
    if (!side(r.x0 - tolerance, r.x1 + tolerance, x0, x1) || !side(r.y0 - tolerance, r.y1 + tolerance, y0, y1) || t1 - t0 < 1e-5f) {
        return false;
    }
    if (length != nullptr) {
        *length = distance(x0, y0, x1, y1) * (t1 - t0);
    }
    return true;
}

} // namespace

// Visits, depth first and children in order, every leaf below the root that `touches` accepts,
// descending only into cells it accepts. `visit` returns false to stop. False when stopped, or when
// the grid is deeper than the walk goes.
template <class Touches, class Visit> bool RouteLines::walk(Touches&& touches, Visit&& visit) const {
    const auto& cells = grid_.cells();
    if (cells.empty()) {
        return true;
    }
    std::array<std::pair<int, int>, kMaxDepth> stack{};
    int depth = 1;
    stack[0] = {0, 0};
    while (depth > 0) {
        auto& [cell, next] = stack[static_cast<std::size_t>(depth - 1)];
        if (next >= 4) {
            --depth;
            continue;
        }
        const int child = cells[static_cast<std::size_t>(cell)].children[static_cast<std::size_t>(next++)];
        if (child < 0 || static_cast<std::size_t>(child) >= cells.size()) {
            continue;
        }
        const RouteGrid::Cell& c = cells[static_cast<std::size_t>(child)];
        if (!touches(c)) {
            continue;
        }
        if (c.isLeaf()) {
            if (!visit(child, c)) {
                return false;
            }
        } else {
            if (depth >= kMaxDepth) {
                return false;
            }
            stack[static_cast<std::size_t>(depth++)] = {child, 0};
        }
    }
    return true;
}

bool RouteLines::blocked(float x0, float y0, float x1, float y1, float tolerance) const {
    bool hit = false;
    const bool done = walk([&](const RouteGrid::Cell& c) { return clip(c.rect(), tolerance, x0, y0, x1, y1, nullptr); },
                           [&](int cell, const RouteGrid::Cell&) {
                               hit = !passage_(cell).passable;
                               return !hit;
                           });
    return hit || !done;
}

bool RouteLines::evenCost(float x0, float y0, float x1, float y1, float tolerance) const {
    bool first = true;
    double cost = 0;
    return walk([&](const RouteGrid::Cell& c) { return clip(c.rect(), tolerance, x0, y0, x1, y1, nullptr); },
                [&](int cell, const RouteGrid::Cell&) {
                    const LeafPassage p = passage_(cell);
                    if (!p.passable || !(p.cost <= kBlockedAbove)) {
                        return false;
                    }
                    if (first) {
                        first = false;
                        cost = p.cost;
                        return true;
                    }
                    return std::fabs(cost - p.cost) <= 0.001;
                });
}

double RouteLines::cost(float x0, float y0, float x1, float y1) const {
    double total = 0;
    float length = 0;  // inside the cell last touched, which is the leaf visited
    const bool done = walk([&](const RouteGrid::Cell& c) { return clip(c.rect(), 0.001f, x0, y0, x1, y1, &length); },
                           [&](int cell, const RouteGrid::Cell&) {
                               const double cost = passage_(cell).cost;
                               if (!(cost <= kBlockedAbove)) {
                                   total = kBlockedCost;
                                   return false;
                               }
                               total += cost * length;
                               return true;
                           });
    return done ? total : kBlockedCost;
}

std::pair<float, float> RouteLines::pushClear(float x, float y, float tolerance) const {
    float pushX = 0, pushY = 0;
    const auto offset = [&](const RouteGrid::Cell& c, float& dx, float& dy) {
        dx = x - (static_cast<float>(c.x) + 0.5f) * c.size;
        dy = y - (static_cast<float>(c.y) + 0.5f) * c.size;
        return c.size * 0.5f + tolerance;
    };
    walk(
        [&](const RouteGrid::Cell& c) {
            float dx = 0, dy = 0;
            const float reach = offset(c, dx, dy);
            return std::fabs(dx) < reach && std::fabs(dy) < reach;
        },
        [&](int cell, const RouteGrid::Cell& c) {
            if (passage_(cell).passable) {
                return true;
            }
            // Out through the side nearest the point. Pushes the same way keep the largest. A push the
            // other way replaces the last.
            float dx = 0, dy = 0;
            const float reach = offset(c, dx, dy);
            if (dx + dy < 0) {
                if (dx - dy < 0) {
                    pushX = std::min(pushX, -(reach + dx));
                } else {
                    pushY = std::min(pushY, -(reach + dy));
                }
            } else if (dx - dy < 0) {
                pushY = std::max(pushY, reach - dy);
            } else {
                pushX = std::max(pushX, reach - dx);
            }
            return true;
        });
    return {x + pushX, y + pushY};
}

void layClearOfObstacles(std::vector<RouteWaypoint>& points, const RouteLines& lines) {
    if (points.size() < 2) {
        return;
    }
    std::vector<RouteWaypoint> out;
    out.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        RouteWaypoint cur = points[i];
        // The leg into the first point after a change of level comes from another level.
        if (i == 0 || cur.kind == WaypointKind::LevelStart) {
            out.push_back(cur);
            continue;
        }
        const RouteWaypoint& prev = points[i - 1];
        if (lines.blocked(prev.x, prev.y, cur.x, cur.y, 0.04f)) {
            // A point every eighth of a tile, leaving out the ends, and a tenth of a tile next to a
            // change of level.
            const float perTile = 1.0f / distance(prev, cur);
            const float step = perTile * 0.125f;
            if (step <= 0.5f) {
                const float first = prev.kind == WaypointKind::LevelStart ? std::max(perTile * 0.1f, step) : step;
                const float last = cur.kind == WaypointKind::LevelEnd ? std::min(1.0f - perTile * 0.1f, 1.0f - step) : 1.0f - step;
                for (float t = first; t < last && out.size() < kMaxPathPoints; t += step) {
                    RouteWaypoint p = cur;
                    p.kind = WaypointKind::Plain;
                    p.facing = kNoFacing;
                    std::tie(p.x, p.y) = lines.pushClear(t * cur.x + (1.0f - t) * prev.x, t * cur.y + (1.0f - t) * prev.y, 0.0625f);
                    out.push_back(p);
                }
            }
        }
        if (i + 1 != points.size() && !fixedPoint(cur.kind)) {
            std::tie(cur.x, cur.y) = lines.pushClear(cur.x, cur.y, 0.0625f);
        }
        out.push_back(cur);
    }
    points = std::move(out);

    // At most five rounds.
    for (int round = 0; round < 5; ++round) {
        bool dropped = false;
        out.clear();
        for (std::size_t i = 0; i < points.size(); ++i) {
            const RouteWaypoint& cur = points[i];
            if (i == 0 || i + 1 == points.size() || keptPoint(cur.kind)) {
                out.push_back(cur);
                continue;
            }
            const RouteWaypoint& last = out.back();
            const RouteWaypoint& next = points[i + 1];
            if (distance(last, cur) > 0.1f || distance(cur, next) > 0.1f || lines.blocked(last.x, last.y, next.x, next.y, 0.04f)) {
                out.push_back(cur);
            } else {
                dropped = true;
            }
        }
        points.swap(out);
        if (!dropped) {
            break;
        }
    }
}

void keepTurns(std::vector<RouteWaypoint>& points, const RouteLines& lines) {
    if (points.size() < 3) {
        return;
    }
    std::vector<RouteWaypoint> out;
    // The game carries each dropped stretch's cost difference into the next kept point's cost and
    // adds them all up on every point kept after it. Later tests compare that sum with the next
    // point's own cost, so past the first few turns only points whose straight line is blocked stay.
    float carry = 0, total = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        RouteWaypoint cur = points[i];
        bool keep = i == 0 || i + 1 == points.size() || keptPoint(cur.kind);
        if (!keep) {
            const RouteWaypoint& last = out.back();
            const RouteWaypoint& next = points[i + 1];
            const double straight = lines.cost(last.x, last.y, next.x, next.y);
            if (!(straight < kBlockedAbove)) {
                keep = true;
            } else {
                const float along = next.cost - last.cost;
                keep = along <= static_cast<float>(straight) - 0.5f && distance(last, cur) >= 0.1f;
                if (!keep) {
                    carry = static_cast<float>(straight) - along;
                }
            }
        }
        if (keep) {
            cur.cost += carry + total;
            total += carry;
            carry = 0;
            out.push_back(cur);
        }
    }
    points = std::move(out);
}

void splitLongLegs(std::vector<RouteWaypoint>& points) {
    if (points.size() < 2) {
        return;
    }
    std::vector<RouteWaypoint> out;
    // Each round halves every long leg once. 24 rounds split any leg a lot can hold.
    for (int round = 0; round < 24; ++round) {
        bool split = false;
        out.clear();
        for (std::size_t i = 0; i < points.size(); ++i) {
            const RouteWaypoint& cur = points[i];
            if (i != 0 && cur.kind != WaypointKind::LevelStart && out.size() < kMaxPathPoints && distance(points[i - 1], cur) > 0.5f) {
                RouteWaypoint mid = cur;
                mid.x = (points[i - 1].x + cur.x) * 0.5f;
                mid.y = (points[i - 1].y + cur.y) * 0.5f;
                mid.kind = WaypointKind::Plain;
                mid.facing = kNoFacing;
                out.push_back(mid);
                split = true;
            }
            out.push_back(cur);
        }
        points.swap(out);
        if (!split) {
            break;
        }
    }
}

void roundCorners(std::vector<RouteWaypoint>& points, const RouteLines& lines, float share, bool evenCost, bool byHold) {
    if (points.size() < 3) {
        return;
    }
    const auto clear = [&](float x0, float y0, float x1, float y1) {
        return evenCost ? lines.evenCost(x0, y0, x1, y1, 0.04f) : !lines.blocked(x0, y0, x1, y1, 0.04f);
    };
    std::vector<RouteWaypoint> out;
    out.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        RouteWaypoint cur = points[i];
        const float move = byHold ? (1.0f - std::max(cur.startHold, cur.endHold)) * share : share;
        if (i != 0 && i + 1 != points.size() && !fixedPoint(cur.kind) && move > 0) {
            // Toward the middle of the neighbours as they were before this round. The legs are tested
            // from the point before as already moved.
            const RouteWaypoint& prev = points[i - 1];
            const RouteWaypoint& next = points[i + 1];
            const float x = ((next.x + prev.x) * 0.5f - cur.x) * move + cur.x;
            const float y = ((next.y + prev.y) * 0.5f - cur.y) * move + cur.y;
            const RouteWaypoint& last = out.back();
            if (distance(cur.x, cur.y, x, y) > 0.0025f && clear(last.x, last.y, x, y) && clear(x, y, next.x, next.y)) {
                cur.x = x;
                cur.y = y;
            }
        }
        out.push_back(cur);
    }
    points = std::move(out);
}

float RouteHeading::leaving(float angle, HeadingEnd end) const {
    if (!snap || !std::isfinite(angle) || !std::isfinite(facing)) {
        return angle;
    }
    constexpr float kPi = 3.14159274f, kDegree = 0.0174532924f, kEighth = 0.785398185f;
    const auto eighths = [](float a) { return static_cast<float>(std::nearbyint(static_cast<double>(a * 1.27323949f))); };
    if (end != HeadingEnd::Start) {
        return eighths(angle) * kEighth;
    }
    // From the Sim's own facing, in eighths of a turn until the way out is within 40 degrees.
    float d = angle - facing;
    while (d > kPi) {
        d -= 2 * kPi;
    }
    while (d < -kPi) {
        d += 2 * kPi;
    }
    int steps = 0;
    while (d < -40 * kDegree) {
        --steps;
        d += 45 * kDegree;
    }
    while (d > 40 * kDegree) {
        ++steps;
        d -= 45 * kDegree;
    }
    return (eighths(facing) + static_cast<float>(steps)) * kEighth;
}

void holdToEnds(std::vector<RouteWaypoint>& points, const RouteLines& lines, const RouteHeading& heading) {
    const std::size_t n = points.size();
    if (n < 3) {
        return;
    }
    // What is left of a hold after a tile walked away from its end.
    constexpr float kFadePerTile = 0.35f;
    const auto end = [&](std::size_t i) { return i == 0 || i + 1 == n || fixedPoint(points[i].kind); };
    struct Pull {
        float x = 0, y = 0;  // where the end's direction would put the point
        float hold = 0;
    };
    std::vector<Pull> ahead(n), behind(n);
    float anchorX = 0, anchorY = 0, dirX = 0, dirY = 0;
    const auto setDirection = [&](const RouteWaypoint& p, float angle) {
        anchorX = p.x;
        anchorY = p.y;
        dirX = std::cos(angle);
        dirY = std::sin(angle);
    };

    // Ahead: from each start (or first point after a change of level) on.
    float hold = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const RouteWaypoint& cur = points[i];
        if (end(i)) {
            hold = 0;
            if (cur.kind == WaypointKind::Start || cur.kind == WaypointKind::LevelStart) {
                if (cur.facing != kNoFacing) {
                    setDirection(cur, cur.facing);
                    hold = cur.kind == WaypointKind::Start ? cur.facingHold : 1.0f;
                } else if (heading.active && i + 1 < n) {
                    const RouteWaypoint& next = points[i + 1];
                    const HeadingEnd which = cur.kind == WaypointKind::Start ? HeadingEnd::Start : HeadingEnd::LevelStart;
                    setDirection(cur, heading.leaving(std::atan2(next.y - cur.y, next.x - cur.x), which));
                    hold = 0.2f;
                }
            }
            ahead[i] = {cur.x, cur.y, hold};
            continue;
        }
        Pull pull{cur.x, cur.y, hold};
        if (hold > 0.01f) {
            hold *= std::pow(kFadePerTile, distance(cur, points[i - 1]));
            const float along = distance(cur.x, cur.y, anchorX, anchorY);
            pull = {(along * dirX + anchorX) * hold + (1.0f - hold) * cur.x, (along * dirY + anchorY) * hold + (1.0f - hold) * cur.y, hold};
        }
        ahead[i] = pull;
    }

    // Behind: back from each goal (or last point before a change of level).
    hold = 0;
    for (std::size_t i = n; i-- > 0;) {
        const RouteWaypoint& cur = points[i];
        if (end(i)) {
            hold = 0;
            if (cur.kind == WaypointKind::Goal || cur.kind == WaypointKind::LevelEnd) {
                if (cur.facing != kNoFacing) {
                    setDirection(cur, cur.facing);
                    hold = 1.0f;
                } else if (heading.active && i > 0) {
                    const RouteWaypoint& prev = points[i - 1];
                    const HeadingEnd which = cur.kind == WaypointKind::Goal ? HeadingEnd::Goal : HeadingEnd::LevelEnd;
                    setDirection(cur, heading.leaving(std::atan2(cur.y - prev.y, cur.x - prev.x), which));
                    hold = 1.0f;
                }
            }
            behind[i] = {cur.x, cur.y, hold};
            continue;
        }
        Pull pull{cur.x, cur.y, hold};
        if (hold > 0.01f) {
            hold *= std::pow(kFadePerTile, distance(cur, points[i + 1]));
            const float along = distance(cur.x, cur.y, anchorX, anchorY);
            pull = {(anchorX - along * dirX) * hold + (1.0f - hold) * cur.x, (anchorY - along * dirY) * hold + (1.0f - hold) * cur.y, hold};
        }
        behind[i] = pull;
    }

    // Each point but the ends moves to the blend of the two when the legs to and from it stay clear, else a
    // third of the way there when those legs do.
    std::vector<RouteWaypoint> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        RouteWaypoint cur = points[i];
        cur.startHold = ahead[i].hold;
        cur.endHold = behind[i].hold;
        const bool pulled = ahead[i].x != cur.x || ahead[i].y != cur.y || behind[i].x != cur.x || behind[i].y != cur.y;
        if (!end(i) && pulled) {
            const float a = ahead[i].hold, b = behind[i].hold;
            float x = cur.x, y = cur.y;
            if (a > 0.001f && b > 0.001f) {
                x = (b * behind[i].x + a * ahead[i].x) / (a + b);
                y = (b * behind[i].y + a * ahead[i].y) / (a + b);
            } else if (a > 0.001f) {
                x = ahead[i].x;
                y = ahead[i].y;
            } else if (b > 0.001f) {
                x = behind[i].x;
                y = behind[i].y;
            }
            const RouteWaypoint& last = out.back();
            const RouteWaypoint& next = points[i + 1];
            const auto clear = [&](float px, float py) {
                return !lines.blocked(last.x, last.y, px, py, 0.04f) && !lines.blocked(px, py, next.x, next.y, 0.04f);
            };
            if (distance(cur.x, cur.y, x, y) > 0.0025f) {
                const float tx = cur.x * 0.6666f + x * 0.3334f, ty = cur.y * 0.6666f + y * 0.3334f;
                if (clear(x, y)) {
                    cur.x = x;
                    cur.y = y;
                } else if (distance(cur.x, cur.y, tx, ty) > 0.0025f && clear(tx, ty)) {
                    cur.x = tx;
                    cur.y = ty;
                }
            }
        }
        out.push_back(cur);
    }
    points = std::move(out);
}

void smoothPath(std::vector<RouteWaypoint>& points, const RouteLines& lines, const RouteHeading& heading) {
    for (const RouteWaypoint& p : points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.cost)) {
            return;
        }
    }
    layClearOfObstacles(points, lines);
    keepTurns(points, lines);
    splitLongLegs(points);
    for (int i = 0; i < 8; ++i) {
        roundCorners(points, lines, 0.45f, true, false);
    }
    holdToEnds(points, lines, heading);
    for (int i = 0; i < 2; ++i) {
        roundCorners(points, lines, 0.325f, false, true);
    }
}

} // namespace ts2::sim
