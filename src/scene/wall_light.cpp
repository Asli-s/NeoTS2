#include "scene/wall_light.hpp"

#include "scene/portable_math.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

// The game works these out on the x87 in double precision and rounds to float only where it
// stores a value, so the code below does the same: double in between, float where the game
// keeps a float.

namespace ts2::scene::walllight {

namespace {

constexpr int kMaxSteps = 1000;
constexpr int kMaxDepth = 32;          // walls stacked through levels; more means bad links
constexpr int kMaxLevelsAbove = 32;
constexpr std::size_t kMaxSegments = 4096;  // per run; longer runs are left unlit
// Safety limits far above what any real lot needs. Spreading counts segment visits: walls
// stacked over many levels and branching at each can otherwise be revisited exponentially.
// Lighting from sources counts each light against each wall face's segments.
constexpr long long kMaxSegmentVisits = 16'000'000;
constexpr long long kMaxLightChecks = 4'000'000;

// The eight neighbours inside a segment's grid, as steps in row and in half; odd ones are diagonal.
constexpr int kStepRow[8] = {0, -1, -1, -1, 0, 1, 1, 1};
constexpr int kStepHalf[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

// Rounds to the nearest whole number, halves to even, whatever the processor's rounding mode.
double roundHalfEven(double v) {
    const double down = std::floor(v);
    const double rest = v - down;
    if (rest > 0.5 || (rest == 0.5 && std::fmod(down, 2.0) != 0)) {
        return down + 1;
    }
    return down;
}

std::uint8_t channel(float v) {
    if (std::isnan(v)) {
        return std::signbit(v) ? 0 : 255;
    }
    const double x = roundHalfEven(static_cast<double>(v * 256.0f));
    if (!(x > 0)) {
        return 0;
    }
    return static_cast<std::uint8_t>(std::min(x, 255.0));
}

// The game fades stored colours with 16-bit fixed point: value * f / 4096, rounded down and
// kept within a byte.
std::uint8_t fadeChannel(std::uint8_t v, int f) {
    return static_cast<std::uint8_t>(std::clamp(((v << 4) * f) >> 16, 0, 255));
}

Pixel fade(const Pixel& p, int f) {
    return {fadeChannel(p.b, f), fadeChannel(p.g, f), fadeChannel(p.r, f), fadeChannel(p.a, f)};
}

// The fade per step for a falloff, as the game's fixed-point factor: 4096 / (1 + falloff),
// rounded by adding 1.5 * 2^23 as a float and reading the low 16 bits as a signed number.
int fadeFactor(double falloff) {
    const float biased = static_cast<float>(0.0625 / (1.0 + falloff) * 65536.0 + 12582912.0);
    const std::uint32_t k = std::bit_cast<std::uint32_t>(biased) - 0x4B400000u;
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(k & 0xFFFFu));
}

// Raises each channel of `to` to `from` where it is darker; returns whether anything changed.
bool brighten(Pixel& to, const Pixel& from) {
    bool changed = false;
    auto one = [&](std::uint8_t& a, std::uint8_t b) {
        if (b > a) {
            a = b;
            changed = true;
        }
    };
    one(to.b, from.b);
    one(to.g, from.g);
    one(to.r, from.r);
    one(to.a, from.a);
    return changed;
}

// The segment a link leads to: the same wall exactly one level up or down, or none (links
// that go anywhere else are ignored, so they can never loop).
Segment* linked(Walls& walls, const Run& from, const Segment& s, bool up) {
    const Link& l = up ? s.up : s.down;
    if (l.run < 0 || l.segment < 0 || static_cast<std::size_t>(l.run) >= walls.runs.size()) {
        return nullptr;
    }
    const Run& to = walls.runs[static_cast<std::size_t>(l.run)];
    if (static_cast<long long>(to.level) != static_cast<long long>(from.level) + (up ? 1 : -1)) {
        return nullptr;
    }
    auto& segments = walls.runs[static_cast<std::size_t>(l.run)].segments;
    return static_cast<std::size_t>(l.segment) < segments.size() ? &segments[static_cast<std::size_t>(l.segment)] : nullptr;
}

// ---- Lamps and windows ----

// Where a light stands against a run: its distance in front of the face (negative behind),
// the nearest point on the run, and the half segment there.
struct Facing {
    float distance = 0;
    float nearestX = 0, nearestY = 0;
    int half = 0;
};

Facing facing(const Run& run, float x, float y) {
    const Segment& s0 = run.segments.front();
    const int n = static_cast<int>(run.segments.size());
    const double dx = static_cast<double>(s0.x1) - s0.x0, dy = static_cast<double>(s0.y1) - s0.y0;
    const double len0 = std::sqrt(dy * dy + dx * dx);
    Facing f;
    if (!(len0 > 0)) {
        return f;  // a wall of no length: nothing in front of it
    }
    const double inv = 1.0 / len0;
    const float ux = static_cast<float>(dx * inv), uy = static_cast<float>(dy * inv);
    const float rx = x - s0.x0, ry = y - s0.y0;
    const float away = static_cast<float>(std::sqrt(static_cast<double>(ry) * ry + static_cast<double>(rx) * rx));
    const float cross = static_cast<float>(static_cast<double>(uy) * rx - static_cast<double>(ux) * ry);
    const float length = static_cast<float>(n * (run.diagonal ? std::sqrt(2.0) : 1.0));
    const float t = static_cast<float>((static_cast<double>(uy) * ry + static_cast<double>(ux) * rx) / length);
    if (!(t >= 0) || t >= 1) {  // past either end (and NaN, as in the game)
        f.distance = cross < 0 || std::isnan(cross) ? -away : away;
        if (t < 0) {
            f.nearestX = s0.x0;
            f.nearestY = s0.y0;
            f.half = 0;
        } else {
            f.nearestX = static_cast<float>(static_cast<double>(ux) * length + s0.x0);
            f.nearestY = static_cast<float>(static_cast<double>(uy) * length + s0.y0);
            f.half = 2 * n - 1;
        }
        return f;
    }
    f.distance = cross;
    f.nearestX = static_cast<float>(static_cast<double>(ux) * t * length + s0.x0);
    f.nearestY = static_cast<float>(static_cast<double>(uy) * t * length + s0.y0);
    f.half = std::clamp(static_cast<int>(std::floor(static_cast<double>(2 * n) * t)), 0, 2 * n - 1);
    return f;
}

// Brightens the middle rows of one half segment; the top and bottom rows take the next,
// faded value. Returns whether anything brightened.
bool lightHalf(Run& run, int half, Pixel& color, int fadeBy) {
    Row& row = run.segments[static_cast<std::size_t>(half / 2)].halves[static_cast<std::size_t>(half % 2)];
    bool changed = false;
    for (std::size_t i = 1; i <= 4; ++i) {
        changed |= brighten(row[i], color);
    }
    if (!changed) {
        return false;
    }
    color = fade(color, fadeBy);
    row[0] = row[5] = color;
    return true;
}

// Lights a run from the half nearest the light, fading half by half both ways until it no
// longer brightens anything.
void lightAlong(Run& run, int nearest, const Pixel& color, int fadeBy) {
    Pixel down = color;
    for (int h = nearest; h >= 0; --h) {
        if (!lightHalf(run, h, down, fadeBy)) {
            break;
        }
    }
    Pixel upward = fade(color, fadeBy);
    const int halves = 2 * static_cast<int>(run.segments.size());
    for (int h = nearest + 1; h < halves; ++h) {
        if (!lightHalf(run, h, upward, fadeBy)) {
            break;
        }
    }
}

void markLamplit(Walls& walls, int run, int depth) {
    if (depth > kMaxDepth || run < 0 || static_cast<std::size_t>(run) >= walls.runs.size()) {
        return;
    }
    Run& r = walls.runs[static_cast<std::size_t>(run)];
    r.lamplit = true;
    for (const auto& s : r.segments) {
        if (linked(walls, r, s, true)) {
            markLamplit(walls, s.up.run, depth + 1);
        }
    }
}

// One light on one run; returns whether it reached the run at all.
bool lightRun(Walls& walls, Run& run, const LightSource& light, const FloorAt& floorAt) {
    const float x = (light.x0 + light.x1) * 0.5f, y = (light.y0 + light.y1) * 0.5f;
    const Facing f = facing(run, x, y);
    if (!(f.distance > 0)) {
        return false;  // behind the face
    }
    float intensity = light.wallIntensity;
    for (int level = light.level; level > run.level; --level) {
        if (floorAt && floorAt(x, y, level)) {
            break;
        }
        intensity = static_cast<float>(static_cast<double>(intensity) * (1.0 - static_cast<double>(light.interFloorFalloff)));
    }
    float falloff = light.falloff * walls.falloffScale;
    double shape = 1;   // < 1 stretches the light's reach the way it points
    float towards = 0;
    if (light.directional && f.distance > 1e-4f) {
        const double inv = 1.0 / static_cast<double>(f.distance);
        const float vx = static_cast<float>(static_cast<double>(f.nearestX - x) * inv);
        const float vy = static_cast<float>(static_cast<double>(f.nearestY - y) * inv);
        const float pointsX = static_cast<float>(portable::cosine(light.angle));
        const float pointsY = static_cast<float>(portable::sine(light.angle));
        const double dot = static_cast<double>(vy) * pointsY + static_cast<double>(vx) * pointsX;
        const float dotF = static_cast<float>(dot);
        shape = (6.0 - 2.0 * dot) / (6.0 + 2.0 * dot);
        if (dotF > 0) {
            towards = dotF;
        }
        const double mix = std::clamp(light.directionalMix, 0.0f, 1.0f);
        intensity = static_cast<float>(static_cast<double>(intensity) * (mix * ((static_cast<double>(dotF) + 1.0) * 0.5) + (1.0 - mix)));
    }
    const double reachD = 2.0 * (shape * static_cast<double>(f.distance));
    const float reach = reachD > 1 ? static_cast<float>(reachD) : 1.0f;
    const double strength = portable::power(1.0 / (1.0 + static_cast<double>(falloff)), static_cast<double>(reach)) *
                            static_cast<double>(intensity);
    const std::array<float, 3> color{static_cast<float>(light.color[0] * strength), static_cast<float>(light.color[1] * strength),
                                     static_cast<float>(light.color[2] * strength)};
    if (light.directional) {
        const double d = falloff;
        falloff = static_cast<float>((2.0 * (d / reach) - d) * towards + d);
    }
    if (!(falloff > 0)) {
        falloff = 0;
    }
    lightAlong(run, f.half, pack(color), fadeFactor(static_cast<double>(falloff) * (run.diagonal ? 1.4 : 1.0)));
    return true;
}

// ---- Spreading ----

struct Spreader {
    Walls& walls;
    int fadeSide;
    int fadeDiagonal;
    std::vector<bool> visited;
    long long budget = kMaxSegmentVisits;

    // Counts a visit to a run of `count` segments; false once the safety limit is used up.
    bool spend(std::size_t count) {
        if (budget <= 0) {
            return false;
        }
        budget -= static_cast<long long>(std::min(count, kMaxSegments)) + 1;
        return true;
    }

    bool within(Segment& s) {
        bool changed = false;
        for (int half = 0; half < 2; ++half) {
            for (int d = 0; d < 8; ++d) {
                const int other = half + kStepHalf[d];
                if (other < 0 || other > 1) {
                    continue;
                }
                const int f = (d & 1) ? fadeDiagonal : fadeSide;
                for (int row = 0; row < 6; ++row) {
                    const int from = row + kStepRow[d];
                    if (from >= 0 && from < 6) {
                        changed |= brighten(s.halves[static_cast<std::size_t>(half)][static_cast<std::size_t>(row)],
                                            fade(s.halves[static_cast<std::size_t>(other)][static_cast<std::size_t>(from)], f));
                    }
                }
            }
        }
        return changed;
    }

    // Light from the neighbouring segment's touching half into this one's.
    bool across(Row& into, const Row& from) {
        bool changed = false;
        for (std::size_t i = 1; i < 6; ++i) {
            changed |= brighten(into[i], fade(from[i - 1], fadeDiagonal));
        }
        for (std::size_t i = 0; i < 6; ++i) {
            changed |= brighten(into[i], fade(from[i], fadeSide));
        }
        for (std::size_t i = 0; i < 5; ++i) {
            changed |= brighten(into[i], fade(from[i + 1], fadeDiagonal));
        }
        return changed;
    }

    bool along(Segment& s, const Segment* before, const Segment* after) {
        bool changed = false;
        if (before) {
            changed |= across(s.halves[0], before->halves[1]);
        }
        if (after) {
            changed |= across(s.halves[1], after->halves[0]);
        }
        return changed;
    }

    // Between a segment and the same wall a level up (its top row meets their bottom row) or down.
    bool vertical(Segment& s, Segment& other, bool up) {
        bool changed = false;
        const std::size_t mine = up ? 0 : 5, theirs = up ? 5 : 0;
        for (std::size_t half = 0; half < 2; ++half) {
            changed |= brighten(s.halves[half][mine], fade(other.halves[half][theirs], fadeSide));
            changed |= brighten(other.halves[half][theirs], fade(s.halves[half][mine], fadeSide));
        }
        return changed;
    }

    void clearLinked(Run& run, bool up) {
        for (const auto& s : run.segments) {
            if (linked(walls, run, s, up)) {
                visited[static_cast<std::size_t>((up ? s.up : s.down).run)] = false;
            }
        }
    }

    // Within each segment of a run and of the runs stacked above (or below) it.
    bool withinStack(int runIndex, bool up, bool self, int depth) {
        Run& run = walls.runs[static_cast<std::size_t>(runIndex)];
        if (depth > kMaxDepth || !spend(run.segments.size())) {
            return false;
        }
        clearLinked(run, up);
        bool changed = false;
        for (auto& s : run.segments) {
            if (self) {
                changed |= within(s);
            }
            const int next = (up ? s.up : s.down).run;
            if (linked(walls, run, s, up) && !visited[static_cast<std::size_t>(next)]) {
                visited[static_cast<std::size_t>(next)] = true;
                changed |= withinStack(next, up, true, depth + 1);
            }
        }
        return changed;
    }

    // Along a run, and between it and the runs stacked above (or below) it.
    bool betweenStack(int runIndex, bool up, bool self, int depth) {
        Run& run = walls.runs[static_cast<std::size_t>(runIndex)];
        if (depth > kMaxDepth || !spend(run.segments.size())) {
            return false;
        }
        clearLinked(run, up);
        bool changed = false;
        auto& segs = run.segments;
        const std::size_t n = segs.size();
        if (self && n >= 2) {
            changed |= along(segs[0], nullptr, &segs[1]);
            changed |= along(segs[n - 1], &segs[n - 2], nullptr);
            for (std::size_t i = 1; i + 1 < n; ++i) {
                changed |= along(segs[i], &segs[i - 1], &segs[i + 1]);
            }
        }
        for (auto& s : segs) {
            if (Segment* other = linked(walls, run, s, up)) {
                changed |= vertical(s, *other, up);
            }
        }
        for (auto& s : segs) {
            const int next = (up ? s.up : s.down).run;
            if (linked(walls, run, s, up) && !visited[static_cast<std::size_t>(next)]) {
                visited[static_cast<std::size_t>(next)] = true;
                changed |= betweenStack(next, up, true, depth + 1);
            }
        }
        return changed;
    }
};

} // namespace

Pixel pack(const std::array<float, 3>& color) {
    return {channel(color[2]), channel(color[1]), channel(color[0]), 255};
}

void resetToAmbient(Walls& walls) {
    const Pixel inside = pack(walls.insideAmbient), outside = pack(walls.outsideAmbient);
    for (auto& run : walls.runs) {
        const Pixel& p = run.room == 0 ? outside : inside;
        for (auto& s : run.segments) {
            for (auto& half : s.halves) {
                half.fill(p);
            }
        }
        run.lamplit = false;
    }
}

bool lightFromSources(Walls& walls, const std::vector<LightSource>& sources, const FloorAt& floorAt, std::set<int>& changed) {
    long long budget = kMaxLightChecks;
    for (const auto& light : sources) {
        const long long levelsAbove = static_cast<long long>(light.level);
        for (std::size_t i = 0; i < walls.runs.size() && budget > 0; ++i) {
            Run& run = walls.runs[i];
            budget -= 1 + static_cast<long long>(std::min(run.segments.size(), kMaxSegments));
            if (run.segments.empty() || run.segments.size() > kMaxSegments || run.level > light.level || run.room != light.room ||
                levelsAbove - run.level > kMaxLevelsAbove) {
                continue;
            }
            if (lightRun(walls, run, light, floorAt)) {
                markLamplit(walls, static_cast<int>(i), 0);
                changed.insert(static_cast<int>(i));
            }
        }
    }
    return budget > 0;
}

void lightFromFloor(Walls& walls, const FloorLight& floorLight, std::set<int>& changed) {
    if (!floorLight) {
        return;
    }
    for (std::size_t i = 0; i < walls.runs.size(); ++i) {
        Run& run = walls.runs[i];
        const float share = run.lamplit ? walls.brightFloor : walls.darkFloor;
        if (share == 0 || run.segments.empty()) {
            continue;
        }
        const Segment& s0 = run.segments.front();
        const float dx = s0.x1 - s0.x0, dy = s0.y1 - s0.y0;
        const double len = std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy);
        float nx = dy, ny = -dx;
        if (len > 0) {
            const double inv = 1.0 / len;
            nx = static_cast<float>(static_cast<double>(dy) * inv);
            ny = static_cast<float>(static_cast<double>(-dx) * inv);
        }
        // Sample an eighth of a tile in front of the middle of each half segment.
        const double n = static_cast<double>(run.segments.size());
        const float step = static_cast<float>(1.0 / (2.0 * n));
        bool any = false;
        for (std::size_t k = 0; k < run.segments.size(); ++k) {
            Segment& s = run.segments[k];
            if (linked(walls, run, s, false)) {
                continue;  // only walls standing on a floor take its light
            }
            for (std::size_t half = 0; half < 2; ++half) {
                const float t = std::clamp(static_cast<float>((static_cast<double>(2 * k + half) + 0.5) * step), 0.0f, 1.0f);
                const double tn = static_cast<double>(t) * n;
                const float ax = static_cast<float>(static_cast<double>(dx) * tn), ay = static_cast<float>(static_cast<double>(dy) * tn);
                const float x = (s0.x0 + ax) + nx * 0.125f, y = (s0.y0 + ay) + ny * 0.125f;
                auto c = floorLight(x, y, run.level);
                for (auto& v : c) {
                    v *= share * 0.5f;
                }
                const Pixel p = pack(c);
                for (auto& px : s.halves[half]) {
                    any |= brighten(px, p);
                }
            }
        }
        if (any) {
            changed.insert(static_cast<int>(i));
        }
    }
}

bool spread(Walls& walls, const std::set<int>& changed) {
    Spreader s{walls, fadeFactor(walls.falloffScale), fadeFactor(1.4 * walls.falloffScale), std::vector<bool>(walls.runs.size(), false)};
    const int steps = std::clamp(walls.steps, 0, kMaxSteps);
    for (const int run : changed) {
        if (run < 0 || static_cast<std::size_t>(run) >= walls.runs.size()) {
            continue;
        }
        for (int i = 0; i < steps && s.budget > 0; ++i) {
            bool any = s.withinStack(run, true, true, 0);
            any |= s.withinStack(run, false, false, 0);
            any |= s.betweenStack(run, true, true, 0);
            any |= s.betweenStack(run, false, false, 0);
            if (!any) {
                break;
            }
        }
    }
    return s.budget > 0;
}

void pad(Walls& walls) {
    // A segment by run and index, or none.
    auto at = [&](std::size_t run, std::size_t index) -> const Segment* {
        const auto& segments = walls.runs[run].segments;
        return index < segments.size() ? &segments[index] : nullptr;
    };
    // The segment a segment's link leads to, with its run and index, or none.
    struct Linked {
        const Segment* segment = nullptr;
        std::size_t run = 0, index = 0;
    };
    auto follow = [&](const Run& from, const Segment* s, bool up) {
        Linked l;
        if (s) {
            l.segment = linked(walls, from, *s, up);
            if (l.segment) {
                l.run = static_cast<std::size_t>((up ? s->up : s->down).run);
                l.index = static_cast<std::size_t>((up ? s->up : s->down).segment);
            }
        }
        return l;
    };
    for (std::size_t r = 0; r < walls.runs.size(); ++r) {
        const Run& run = walls.runs[r];
        for (std::size_t i = 0; i < run.segments.size(); ++i) {
            // The neighbours, as the game numbers them: 0 previous, 4 next, 2 above, 6 below,
            // and the corners 1 (above previous), 3 (above next), 5 (below next), 7 (below previous).
            std::array<const Segment*, 8> nb{};
            nb[0] = i > 0 ? at(r, i - 1) : nullptr;
            nb[4] = at(r, i + 1);
            if (const auto up = follow(run, &run.segments[i], true); up.segment) {
                nb[1] = up.index > 0 ? at(up.run, up.index - 1) : nullptr;
                nb[2] = up.segment;
                nb[3] = at(up.run, up.index + 1);
            }
            if (const auto down = follow(run, &run.segments[i], false); down.segment) {
                nb[7] = down.index > 0 ? at(down.run, down.index - 1) : nullptr;
                nb[6] = down.segment;
                nb[5] = at(down.run, down.index + 1);
            }
            // Where this segment has no level above or below, its neighbours along the run may.
            if (!nb[2]) {
                nb[1] = follow(run, nb[0], true).segment;
                nb[3] = follow(run, nb[4], true).segment;
            }
            if (!nb[6]) {
                nb[7] = follow(run, nb[0], false).segment;
                nb[5] = follow(run, nb[4], false).segment;
            }

            Segment& s = walls.runs[r].segments[i];
            auto& t = s.texels;
            for (std::size_t row = 0; row < 6; ++row) {
                t[1][row + 1] = s.halves[0][row];
                t[2][row + 1] = s.halves[1][row];
                t[0][row + 1] = nb[0] ? nb[0]->halves[1][row] : t[1][row + 1];
                t[3][row + 1] = nb[4] ? nb[4]->halves[0][row] : t[2][row + 1];
            }
            t[1][0] = nb[2] ? nb[2]->halves[0][5] : t[1][1];
            t[2][0] = nb[2] ? nb[2]->halves[1][5] : t[2][1];
            t[1][7] = nb[6] ? nb[6]->halves[0][0] : t[1][6];
            t[2][7] = nb[6] ? nb[6]->halves[1][0] : t[2][6];
            // The corners, each falling back in the game's own order.
            t[0][0] = nb[1] ? nb[1]->halves[1][5] : nb[2] ? nb[2]->halves[0][5] : nb[0] ? nb[0]->halves[1][0] : t[1][1];
            t[3][0] = nb[3] ? nb[3]->halves[0][5] : nb[4] ? nb[4]->halves[0][0] : nb[2] ? nb[2]->halves[1][5] : t[2][1];
            t[3][7] = nb[5] ? nb[5]->halves[0][0] : nb[6] ? nb[6]->halves[1][0] : nb[4] ? nb[4]->halves[0][5] : t[2][6];
            t[0][7] = nb[7] ? nb[7]->halves[1][0] : nb[0] ? nb[0]->halves[1][5] : nb[6] ? nb[6]->halves[0][0] : t[1][6];
        }
    }
}

bool light(Walls& walls, const std::vector<LightSource>& sources, const FloorAt& floorAt, const FloorLight& floorLight) {
    std::set<int> changed;
    resetToAmbient(walls);
    const bool lit = lightFromSources(walls, sources, floorAt, changed);
    lightFromFloor(walls, floorLight, changed);
    const bool spreadAll = spread(walls, changed);
    pad(walls);
    return lit && spreadAll;
}

} // namespace ts2::scene::walllight
