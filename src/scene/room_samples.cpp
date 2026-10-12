#include "scene/room_samples.hpp"

#include "content/lot.hpp"

#include <cmath>
#include <cstddef>
#include <set>
#include <tuple>

// The game works these out on the x87 in double precision and rounds to float only where it
// stores a value, so the code below does the same: double in between, float where the game
// keeps a float.

namespace ts2::scene::roomsamples {

namespace {

float r24(double v) {
    return static_cast<float>(v);
}

// The covering's colour with only `saturation` of its difference from white kept, times the light.
Color lit(const Color& covering, const Color& light, const Settings& s) {
    Color out{};
    for (std::size_t c = 0; c < 3; ++c) {
        const float kept = r24(1.0 + static_cast<double>(r24(static_cast<double>(r24(covering[c] - 1.0)) * s.saturation)));
        out[c] = r24(static_cast<double>(light[c]) * kept);
    }
    return out;
}

// Adds `color` times `share` to a side sample and `share` to its weight when the share is
// positive, else takes them away from the opposite side (negative shares there add up).
void toward(roomlight::Samples& samples, roomlight::Direction positive, roomlight::Direction negative, const Color& color, double share) {
    const bool ahead = share > 0;
    const auto i = static_cast<std::size_t>(ahead ? positive : negative);
    for (std::size_t c = 0; c < 3; ++c) {
        const float part = r24(static_cast<double>(color[c]) * share);
        samples.sum[i][c] = ahead ? r24(static_cast<double>(part) + samples.sum[i][c]) : r24(static_cast<double>(samples.sum[i][c]) - part);
    }
    samples.weight[i] = ahead ? r24(share + samples.weight[i]) : r24(static_cast<double>(samples.weight[i]) - share);
}

} // namespace

void addFloor(roomlight::Samples& samples, const Color& covering, const Color& floorLight, const Settings& s) {
    Color quarterLight{};
    for (std::size_t c = 0; c < 3; ++c) {
        quarterLight[c] = r24(static_cast<double>(floorLight[c]) * 0.25f);
    }
    const Color below = lit(covering, quarterLight, s);
    const auto floor = static_cast<std::size_t>(roomlight::kFromFloor), ceiling = static_cast<std::size_t>(roomlight::kFromCeiling);
    for (std::size_t c = 0; c < 3; ++c) {
        samples.sum[floor][c] = r24(static_cast<double>(below[c]) + samples.sum[floor][c]);
        samples.sum[ceiling][c] = r24(static_cast<double>(r24(static_cast<double>(quarterLight[c]) * s.ceiling)) + samples.sum[ceiling][c]);
    }
    samples.weight[floor] = r24(static_cast<double>(samples.weight[floor]) + 1.0f);
    samples.weight[ceiling] = r24(static_cast<double>(samples.weight[ceiling]) + 1.0f);
}

void addWall(roomlight::Samples& samples, float x0, float y0, float x1, float y1, const Color& covering, const Color& wallLight, const Settings& s) {
    Color half{};
    for (std::size_t c = 0; c < 3; ++c) {
        half[c] = r24(static_cast<double>(wallLight[c]) * 0.5f);
    }
    const Color color = lit(covering, half, s);
    // It looks along (dy, -dx); the game keeps the x share as a float and the y share unrounded.
    const float minusDx = r24(-(static_cast<double>(x1) - x0));
    const float dy = r24(static_cast<double>(y1) - y0);
    const float length = r24(std::sqrt(static_cast<double>(dy) * dy + static_cast<double>(minusDx) * minusDx));
    if (!(length > 0)) {
        return;
    }
    const double inverse = 1.0 / length;
    const float nx = r24(dy * inverse);
    const double ny = inverse * minusDx;
    toward(samples, roomlight::kPlusX, roomlight::kMinusX, color, nx);
    toward(samples, roomlight::kPlusY, roomlight::kMinusY, color, ny);
}

void addPortal(roomlight::Samples& samples, float nx, float ny, const Color& light, const Settings& s) {
    Color color{};
    for (std::size_t c = 0; c < 3; ++c) {
        color[c] = r24(static_cast<double>(light[c]) * s.portal);
    }
    toward(samples, roomlight::kPlusX, roomlight::kMinusX, color, nx);
    toward(samples, roomlight::kPlusY, roomlight::kMinusY, color, ny);
}

std::map<std::int16_t, roomlight::Samples> gather(const content::Lot& lot, const LotInputs& in, const std::vector<PortalInput>& portals,
                                                  const Settings& settings) {
    std::map<std::int16_t, roomlight::Samples> rooms;
    auto covering = [&](bool floor, const std::string& name) {
        return (in.covering ? in.covering(floor, name) : std::nullopt).value_or(kNoCoveringColor);
    };
    // Floors: every quarter with a floor covering, its light read a third of a tile from the
    // tile's middle toward the quarter.
    const auto floors = lot.grids.find(content::kFloorGridInstance);
    if (floors != lot.grids.end() && in.quarterRoom && in.floorLight) {
        const auto& grid = floors->second;
        for (std::uint32_t level = 0; level < grid.levels; ++level) {
            for (std::uint32_t x = 0; x < grid.width; ++x) {
                for (std::uint32_t y = 0; y < grid.height; ++y) {
                    for (int bit = 0; bit < 4; ++bit) {
                        const int l = static_cast<int>(level), tx = static_cast<int>(x), ty = static_cast<int>(y);
                        const auto room = in.quarterRoom(l, tx, ty, bit);
                        const auto pattern = static_cast<std::uint16_t>(grid.i16(l, tx, ty, static_cast<std::size_t>(bit)));
                        const auto name = lot.floorPatterns.find(pattern);
                        if (room <= 0 || pattern == 0 || name == lot.floorPatterns.end()) {
                            continue;
                        }
                        float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
                        if (bit == 0) px -= 1.0f / 3;
                        if (bit == 1) py += 1.0f / 3;
                        if (bit == 2) px += 1.0f / 3;
                        if (bit == 3) py -= 1.0f / 3;
                        addFloor(rooms[room], covering(true, name->second.name), in.floorLight(l, px, py), settings);
                    }
                }
            }
        }
    }
    // Walls: each wall once, both faces.
    if (lot.roomGraph && in.faceRoom && in.wallLight) {
        std::map<std::int32_t, const walllayer::Entry*> coveringOf;
        for (const auto& c : lot.wallCoverings) {
            coveringOf[static_cast<std::int32_t>(c.wall)] = &c;
        }
        std::map<std::int32_t, const wallgraph::Point*> points;
        for (const auto& p : lot.roomGraph->points) {
            points.emplace(p.id, &p);
        }
        auto tileLine = [](float v) { return std::isfinite(v) && std::fabs(v) <= 4096.0f; };
        std::set<std::tuple<std::int32_t, float, float, float, float>> seen;
        for (const auto& w : lot.roomGraph->walls) {
            const auto a = points.find(w.from), b = points.find(w.to);
            if (a == points.end() || b == points.end()) {
                continue;
            }
            const auto& p = *a->second;
            const auto& q = *b->second;
            if (p.level != q.level || !tileLine(p.x) || !tileLine(p.y) || !tileLine(q.x) || !tileLine(q.y)) {
                continue;
            }
            const bool forward = std::tie(p.x, p.y) < std::tie(q.x, q.y);
            if (!seen.insert(forward ? std::tuple{p.level, p.x, p.y, q.x, q.y} : std::tuple{p.level, q.x, q.y, p.x, p.y}).second) {
                continue;
            }
            const auto entry = coveringOf.find(w.id);
            if (entry == coveringOf.end()) {
                continue;
            }
            for (int face = 0; face < 2; ++face) {
                const auto& s = face ? q : p;
                const auto& e = face ? p : q;
                const auto key = wallruns::keyOf(p.level, static_cast<int>(s.x), static_cast<int>(s.y), static_cast<int>(e.x), static_cast<int>(e.y));
                const auto room = in.faceRoom(key);
                const auto name = lot.wallPatterns.find(face ? entry->second->right : entry->second->left);
                if (room <= 0 || name == lot.wallPatterns.end() || name->second.name.empty()) {
                    continue;
                }
                addWall(rooms[room], s.x, s.y, e.x, e.y, covering(false, name->second.name), in.wallLight(key), settings);
            }
        }
    }
    for (const auto& portal : portals) {
        if (portal.into > 0) {
            addPortal(rooms[portal.into], portal.nx, portal.ny, portal.light, settings);
        }
    }
    return rooms;
}

} // namespace ts2::scene::roomsamples
