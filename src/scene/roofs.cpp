#include "scene/lot_parts.hpp"

#include <algorithm>
#include <cmath>

// Roofs: the roof list's type is the roofs.txt id plus one (1 gable shed, 2 hipped shed, 3 tall
// gable, 4 gable, 5 hipped, 6 mansard...). A roof's surface is the lowest of the planes rising from
// its sloped edges, so each face is the part of the rectangle where its plane is lowest. Sloped
// edges reach out by the overhang, with a fascia under them; a shed's high side (toward the second
// corner, across the shorter span) has no eave and meets the wall there; gable ends are left to the
// walls under them, which carry on up to the roof. Mansards and dormers are drawn as hipped.

namespace ts2::scene {

namespace {

constexpr float kRoofOverhang = 0.5f;  // how far eaves reach out past the walls, in tiles
constexpr float kRoofFascia = 0.28f;   // the roof slab's thickness at its edges (the fascia)
// The roof texture is laid as seen from above (its hip pieces are drawn at 45 degrees, as hips look
// in plan), one cell (two shingles across, three courses) to 0.45 of a tile, so courses run longer
// along the slope.
constexpr float kRoofTextureCell = 0.45f;
// Ridge and hip caps are a band down each face from the line where it meets the next face, showing
// one side of the cap piece (37 rows of a 102.4-row texture cell).
constexpr float kRoofCapWidth = 0.36f * kRoofTextureCell;

using Polygon = std::vector<std::array<float, 2>>;

// The part of a convex polygon where a*x + b*y + c <= 0.
Polygon clip(const Polygon& poly, float a, float b, float c) {
    Polygon out;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const auto& p = poly[i];
        const auto& q = poly[(i + 1) % poly.size()];
        const float dp = a * p[0] + b * p[1] + c, dq = a * q[0] + b * q[1] + c;
        if (dp <= 0) {
            out.push_back(p);
        }
        if ((dp < 0 && dq > 0) || (dp > 0 && dq < 0)) {
            const float t = dp / (dp - dq);
            out.push_back({p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t});
        }
    }
    return out;
}

} // namespace

float RoofShape::height(float x, float y) const {
    float z = 1e9f;
    for (const auto& p : planes) {
        z = std::min(z, eave + slope * ((x - p[0]) * p[2] + (y - p[1]) * p[3]));
    }
    return z;
}

std::vector<float> RoofShape::bends(const std::array<float, 2>& p, const std::array<float, 2>& q) const {
    std::vector<float> ts{0, 1};
    // Each plane's height along the way is c + d t; two planes cross where those are equal.
    for (std::size_t i = 0; i < planes.size(); ++i) {
        for (std::size_t j = i + 1; j < planes.size(); ++j) {
            auto line = [&](const std::array<float, 4>& e) {
                const float c = (p[0] - e[0]) * e[2] + (p[1] - e[1]) * e[3];
                const float d = (q[0] - p[0]) * e[2] + (q[1] - p[1]) * e[3];
                return std::array<float, 2>{c, d};
            };
            const auto a = line(planes[i]), b = line(planes[j]);
            if (std::abs(a[1] - b[1]) > 1e-6f) {
                const float t = (b[0] - a[0]) / (a[1] - b[1]);
                if (t > 1e-4f && t < 1 - 1e-4f) {
                    ts.push_back(t);
                }
            }
        }
    }
    std::sort(ts.begin(), ts.end());
    return ts;
}

void addValleys(Surfaces& surfaces, const RoofShape& a, const RoofShape& b, int material) {
    if (material < 0) {
        return;
    }
    constexpr float kValleyWidth = kRoofCapWidth;
    auto z = [](const RoofShape::Face& f, float x, float y) { return f.plane[0] + f.plane[1] * x + f.plane[2] * y; };
    for (const auto& fa : a.faces) {
        for (const auto& fb : b.faces) {
            // The planes meet where (cx, cy) . (x, y) + c = 0, from their difference.
            const float cx = fa.plane[1] - fb.plane[1], cy = fa.plane[2] - fb.plane[2], c = fa.plane[0] - fb.plane[0];
            const float len = std::sqrt(cx * cx + cy * cy);
            if (len < 1e-4f) {
                continue;
            }
            const std::array<float, 2> n{cx / len, cy / len}, d{-n[1], n[0]};
            const std::array<float, 2> o{-c / len * n[0], -c / len * n[1]};  // a point on the line
            // The part of the line inside both faces' outlines (both convex).
            float t0 = -1e6f, t1 = 1e6f;
            for (const auto* outline : {&fa.outline, &fb.outline}) {
                // Which way round the outline runs, for its inside.
                float area = 0;
                for (std::size_t k = 0; k < outline->size(); ++k) {
                    const auto& p = (*outline)[k];
                    const auto& q = (*outline)[(k + 1) % outline->size()];
                    area += p[0] * q[1] - q[0] * p[1];
                }
                const float sign = area > 0 ? 1.0f : -1.0f;
                for (std::size_t k = 0; k < outline->size() && t0 < t1; ++k) {
                    const auto& p = (*outline)[k];
                    const auto& q = (*outline)[(k + 1) % outline->size()];
                    // Inside: sign * cross(q - p, x - p) >= 0, linear in t along the line.
                    const float ex = q[0] - p[0], ey = q[1] - p[1];
                    const float at0 = sign * (ex * (o[1] - p[1]) - ey * (o[0] - p[0]));
                    const float slopeT = sign * (ex * d[1] - ey * d[0]);
                    if (std::abs(slopeT) < 1e-7f) {
                        if (at0 < 0) {
                            t1 = t0 - 1;
                        }
                    } else if (slopeT > 0) {
                        t0 = std::max(t0, -at0 / slopeT);
                    } else {
                        t1 = std::min(t1, -at0 / slopeT);
                    }
                }
            }
            if (t1 - t0 < 1e-3f) {
                continue;
            }
            // A strip across the line, lying on whichever surface is on top on each side.
            auto top = [&](float x, float y) { return std::max(z(fa, x, y), z(fb, x, y)) + 0.015f; };
            const float h = kValleyWidth / 2;
            std::array<std::array<float, 3>, 4> corners{};
            const std::array<std::array<float, 2>, 4> at{{{t0, -h}, {t1, -h}, {t1, h}, {t0, h}}};
            for (std::size_t k = 0; k < 4; ++k) {
                const float x = o[0] + d[0] * at[k][0] + n[0] * at[k][1], y = o[1] + d[1] * at[k][0] + n[1] * at[k][1];
                corners[k] = {x, y, top(x, y)};
            }
            // Folded along the line: each half on its own surface.
            const std::array<float, 3> mid0{o[0] + d[0] * t0, o[1] + d[1] * t0, top(o[0] + d[0] * t0, o[1] + d[1] * t0)};
            const std::array<float, 3> mid1{o[0] + d[0] * t1, o[1] + d[1] * t1, top(o[0] + d[0] * t1, o[1] + d[1] * t1)};
            const float u0 = t0 / kRoofTextureCell, u1 = t1 / kRoofTextureCell;
            addQuad(surfaces[material], {{corners[0], corners[1], mid1, mid0}}, {0, 0, 1}, {{{u0, 0}, {u1, 0}, {u1, 0.5f}, {u0, 0.5f}}});
            addQuad(surfaces[material], {{mid0, mid1, corners[2], corners[3]}}, {0, 0, 1}, {{{u0, 0.5f}, {u1, 0.5f}, {u1, 1}, {u0, 1}}});
        }
    }
}

RoofShape addRoof(Surfaces& surfaces, const roofs::Roof& roof, float eave, float slope, const RoofLook& look) {
    const float lx = std::min(roof.x0, roof.x1), hx = std::max(roof.x0, roof.x1);
    const float ly = std::min(roof.y0, roof.y1), hy = std::max(roof.y0, roof.y1);
    const bool wideX = hx - lx >= hy - ly;
    // Edges: 0 low x, 1 high x, 2 low y, 3 high y; which slope and which is a shed's high side.
    std::array<bool, 4> sloped{true, true, true, true};
    int high = -1;
    if (roof.type == 1 || roof.type == 2) {
        high = wideX ? (roof.y1 >= roof.y0 ? 3 : 2) : (roof.x1 >= roof.x0 ? 1 : 0);
        sloped[static_cast<std::size_t>(high)] = false;
        if (roof.type == 1) {
            const int low = high ^ 1;
            for (int e = 0; e < 4; ++e) {
                sloped[static_cast<std::size_t>(e)] = e == low;
            }
        }
    } else if (roof.type == 3 || roof.type == 4) {
        // Gable: the ridge along the longer side (tall gable: along the shorter one).
        const bool ridgeAlongX = (roof.type == 4) == wideX;
        sloped = ridgeAlongX ? std::array<bool, 4>{false, false, true, true} : std::array<bool, 4>{true, true, false, false};
    }
    const float o = kRoofOverhang;
    const float xa = lx - (high == 0 ? 0 : o), xb = hx + (high == 1 ? 0 : o);
    const float ya = ly - (high == 2 ? 0 : o), yb = hy + (high == 3 ? 0 : o);
    // Each edge's line: a point on it, its direction (counterclockwise) and inward normal.
    struct Edge {
        std::array<float, 2> p, d, n;
    };
    const std::array<Edge, 4> edges{{{{xa, yb}, {0, -1}, {1, 0}},
                                     {{xb, ya}, {0, 1}, {-1, 0}},
                                     {{xa, ya}, {1, 0}, {0, 1}},
                                     {{xb, yb}, {-1, 0}, {0, -1}}}};
    auto height = [&](int e, const std::array<float, 2>& v) {
        const auto& E = edges[static_cast<std::size_t>(e)];
        return eave + slope * ((v[0] - E.p[0]) * E.n[0] + (v[1] - E.p[1]) * E.n[1]);
    };
    const Polygon rect{{xa, ya}, {xb, ya}, {xb, yb}, {xa, yb}};
    RoofShape shape;
    shape.eave = eave;
    shape.slope = slope;
    shape.thickness = kRoofFascia;
    for (int e = 0; e < 4; ++e) {
        const auto& E = edges[static_cast<std::size_t>(e)];
        if (sloped[static_cast<std::size_t>(e)]) {
            shape.planes.push_back({E.p[0], E.p[1], E.n[0], E.n[1]});
        } else if (e != high) {
            // Along the rectangle's side, under the overhang.
            const std::array<std::array<float, 2>, 4> ends{{{lx, ly}, {hx, ly}, {lx, hy}, {hx, hy}}};
            const auto& a = e == 0 ? ends[0] : e == 1 ? ends[1] : e == 2 ? ends[0] : ends[2];
            const auto& b = e == 0 ? ends[2] : e == 1 ? ends[3] : e == 2 ? ends[1] : ends[3];
            shape.gables.push_back({a, b, {-E.n[0], -E.n[1]}});
        }
    }
    const float rise = std::sqrt(1 + slope * slope);
    for (int i = 0; i < 4; ++i) {
        if (!sloped[static_cast<std::size_t>(i)]) {
            continue;
        }
        Polygon poly = rect;
        const auto& Ei = edges[static_cast<std::size_t>(i)];
        for (int j = 0; j < 4 && !poly.empty(); ++j) {
            if (j == i || !sloped[static_cast<std::size_t>(j)]) {
                continue;
            }
            // height_i - height_j <= 0, linear in (x, y); ties go to the lower index.
            const auto& Ej = edges[static_cast<std::size_t>(j)];
            const float a = slope * (Ei.n[0] - Ej.n[0]), b = slope * (Ei.n[1] - Ej.n[1]);
            const float c = slope * (-(Ei.p[0] * Ei.n[0] + Ei.p[1] * Ei.n[1]) + (Ej.p[0] * Ej.n[0] + Ej.p[1] * Ej.n[1])) +
                            (j < i ? 1e-5f : 0.0f);
            poly = clip(poly, a, b, c);
        }
        if (poly.size() < 3) {
            continue;
        }
        shape.faces.push_back({poly, {eave - slope * (Ei.p[0] * Ei.n[0] + Ei.p[1] * Ei.n[1]), slope * Ei.n[0], slope * Ei.n[1]}});
        const std::array<float, 3> up{-Ei.n[0] * slope / rise, -Ei.n[1] * slope / rise, 1 / rise};
        // Caps along the edges this face shares with another (those not on the rectangle's border).
        if (look.cap >= 0) {
            auto onBorder = [&](const std::array<float, 2>& v, const std::array<float, 2>& w) {
                const float eps = 1e-4f;
                return (std::abs(v[0] - xa) < eps && std::abs(w[0] - xa) < eps) ||
                       (std::abs(v[0] - xb) < eps && std::abs(w[0] - xb) < eps) ||
                       (std::abs(v[1] - ya) < eps && std::abs(w[1] - ya) < eps) ||
                       (std::abs(v[1] - yb) < eps && std::abs(w[1] - yb) < eps);
            };
            // Each shared edge: its start, direction and the perpendicular into the face.
            struct CapEdge {
                std::array<float, 2> v, d, in;
            };
            std::vector<CapEdge> caps;
            for (std::size_t k = 0; k < poly.size(); ++k) {
                const auto& v = poly[k];
                const auto& w = poly[(k + 1) % poly.size()];
                const float ex = w[0] - v[0], ey = w[1] - v[1], len = std::sqrt(ex * ex + ey * ey);
                if (len < 1e-3f || onBorder(v, w)) {
                    continue;
                }
                caps.push_back({v, {ex / len, ey / len}, {-ey / len, ex / len}});
            }
            auto& capSurface = surfaces[look.cap];
            for (std::size_t k = 0; k < caps.size(); ++k) {
                const auto& c = caps[k];
                auto distance = [](const CapEdge& e, const std::array<float, 2>& q) {
                    return (q[0] - e.v[0]) * e.in[0] + (q[1] - e.v[1]) * e.in[1];
                };
                // The band: within the face, within its width of the edge, and nearer this edge than
                // any other shared edge (so bands meet in a mitre).
                Polygon band = clip(poly, c.in[0], c.in[1], -(c.v[0] * c.in[0] + c.v[1] * c.in[1]) - kRoofCapWidth);
                for (std::size_t j = 0; j < caps.size() && band.size() >= 3; ++j) {
                    if (j == k) {
                        continue;
                    }
                    const auto& other = caps[j];
                    band = clip(band, c.in[0] - other.in[0], c.in[1] - other.in[1],
                                -(c.v[0] * c.in[0] + c.v[1] * c.in[1]) + (other.v[0] * other.in[0] + other.v[1] * other.in[1]));
                }
                if (band.size() < 3) {
                    continue;
                }
                // Measured along the line from the same origin and direction for both faces sharing
                // it, so the cap's joints run straight across both halves.
                const float sign = c.d[0] > 1e-4f || (std::abs(c.d[0]) <= 1e-4f && c.d[1] > 0) ? 1.0f : -1.0f;
                for (std::size_t t = 1; t + 1 < band.size(); ++t) {
                    for (const auto* q : {&band[0], &band[t], &band[t + 1]}) {
                        addVertex(capSurface,
                                  {(*q)[0] + up[0] * 0.01f, (*q)[1] + up[1] * 0.01f, height(i, *q) + up[2] * 0.01f}, up,
                                  {sign * ((*q)[0] * c.d[0] + (*q)[1] * c.d[1]) / kRoofTextureCell,
                                   distance(c, *q) / kRoofCapWidth});
                    }
                }
            }
        }
        for (std::size_t k = 1; k + 1 < poly.size(); ++k) {
            if (look.top >= 0) {
                for (const auto* v : {&poly[0], &poly[k], &poly[k + 1]}) {
                    const float u = ((*v)[0] - Ei.p[0]) * Ei.d[0] + ((*v)[1] - Ei.p[1]) * Ei.d[1];
                    const float w = ((*v)[0] - Ei.p[0]) * Ei.n[0] + ((*v)[1] - Ei.p[1]) * Ei.n[1];
                    addVertex(surfaces[look.top], {(*v)[0], (*v)[1], height(i, *v)}, up,
                              {u / kRoofTextureCell, w / kRoofTextureCell});
                }
            }
            // The underside, the fascia's depth lower (a roof is a slab that thick) and facing down (the
            // same triangle wound the other way).
            if (look.under >= 0) {
                for (const auto* v : {&poly[0], &poly[k + 1], &poly[k]}) {
                    addVertex(surfaces[look.under], {(*v)[0], (*v)[1], height(i, *v) - kRoofFascia}, {-up[0], -up[1], -up[2]},
                              {(*v)[0], (*v)[1]});
                }
            }
        }
    }
    // The slab's edges: level along the eaves, following the slopes along gable ends (the rakes).
    for (int e = 0; e < 4; ++e) {
        const auto& E = edges[static_cast<std::size_t>(e)];
        if (look.trim < 0 || e == high) {
            continue;
        }
        const float len = e < 2 ? yb - ya : xb - xa;
        const std::array<float, 2> a = E.p, b{E.p[0] + E.d[0] * len, E.p[1] + E.d[1] * len};
        const std::array<float, 3> out{-E.n[0], -E.n[1], 0};
        if (sloped[static_cast<std::size_t>(e)]) {
            addQuad(surfaces[look.trim],
                    {{{a[0], a[1], eave - kRoofFascia}, {b[0], b[1], eave - kRoofFascia}, {b[0], b[1], eave}, {a[0], a[1], eave}}}, out,
                    {{{0, 1}, {len, 1}, {len, 0}, {0, 0}}});
            continue;
        }
        const auto ts = shape.bends(a, b);
        for (std::size_t k = 0; k + 1 < ts.size(); ++k) {
            const std::array<float, 2> p{a[0] + (b[0] - a[0]) * ts[k], a[1] + (b[1] - a[1]) * ts[k]};
            const std::array<float, 2> q{a[0] + (b[0] - a[0]) * ts[k + 1], a[1] + (b[1] - a[1]) * ts[k + 1]};
            const float zp = shape.height(p[0], p[1]), zq = shape.height(q[0], q[1]);
            addQuad(surfaces[look.trim],
                    {{{p[0], p[1], zp - kRoofFascia}, {q[0], q[1], zq - kRoofFascia}, {q[0], q[1], zq}, {p[0], p[1], zp}}}, out,
                    {{{ts[k] * len, 1}, {ts[k + 1] * len, 1}, {ts[k + 1] * len, 0}, {ts[k] * len, 0}}});
        }
    }
    return shape;
}

} // namespace ts2::scene
