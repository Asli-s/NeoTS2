#include "scene/room_light.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

// The game works these out on the x87 in double precision and rounds to float only where it
// stores a value, so the code below does the same: double in between, float where the game
// keeps a float.

namespace ts2::scene::roomlight {

namespace {

using Color = std::array<float, 3>;

// The direction each sample's light travels, as the game stores directional lights (the
// negative ones are negated whole, so their zeros are -0).
constexpr std::array<std::array<float, 3>, 6> kDirections{
    {{1, 0, 0}, {-1, -0.0f, -0.0f}, {0, 1, 0}, {-0.0f, -1, -0.0f}, {0, 0, 1}, {-0.0f, -0.0f, -1}}};

float brightest(const Color& c) {
    return std::max({std::fabs(c[0]), std::fabs(c[1]), std::fabs(c[2])});
}

// The strongest of the first `count` directions; the first one wins a tie.
std::size_t strongest(const std::array<Color, 6>& d, std::size_t count) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < count; ++i) {
        if (brightest(d[best]) < brightest(d[i])) {
            best = i;
        }
    }
    return best;
}

} // namespace

void Samples::add(Direction d, const Color& color, float w) {
    const auto i = static_cast<std::size_t>(d);
    if (i >= sum.size()) {
        return;
    }
    for (std::size_t c = 0; c < 3; ++c) {
        sum[i][c] += color[c] * w;
    }
    weight[i] += w;
}

void Samples::addFacing(float nx, float ny, const Color& color) {
    if (nx > 0) {
        add(kPlusX, color, nx);
    } else {
        add(kMinusX, color, -nx);
    }
    if (ny > 0) {
        add(kPlusY, color, ny);
    } else {
        add(kMinusY, color, -ny);
    }
}

std::array<Color, 6> Samples::averages() const {
    std::array<Color, 6> out = sum;
    for (std::size_t i = 0; i < 6; ++i) {
        if (weight[i] != 0) {
            const double scale = 1.0 / static_cast<double>(weight[i]);
            for (auto& v : out[i]) {
                v = static_cast<float>(static_cast<double>(v) * scale);
            }
        }
    }
    return out;
}

// The sum of the six samples, each channel added in the game's own order: it keeps some partial
// sums in double and stores others as floats, differently per channel.
Color average(const std::array<Color, 6>& s) {
    const float x012 = (s[0][0] + s[1][0]) + s[2][0];
    const float x = static_cast<float>(((static_cast<double>(x012) + s[3][0]) + s[4][0]) + s[5][0]);
    const float y012 = static_cast<float>((static_cast<double>(s[0][1]) + s[1][1]) + s[2][1]);
    const float y0123 = y012 + s[3][1];
    const float y = static_cast<float>((static_cast<double>(y0123) + s[4][1]) + s[5][1]);
    const float z0123 = static_cast<float>(((static_cast<double>(s[0][2]) + s[1][2]) + s[2][2]) + s[3][2]);
    const float z = (z0123 + s[4][2]) + s[5][2];
    return {x * (1.0f / 6.0f), y * (1.0f / 6.0f), z * (1.0f / 6.0f)};
}

float unlitShare(const std::array<Color, 6>& samples, const Color& color, float threshold, bool litBySource) {
    if (color[0] == 0 && color[1] == 0 && color[2] == 0) {
        return 0;  // black adds nothing
    }
    if (threshold > 0) {
        const float b = brightest(average(samples));
        if (!(b < threshold)) {
            return 0;
        }
        return static_cast<float>(1.0 - static_cast<double>(b) / static_cast<double>(threshold));
    }
    return litBySource ? 0.0f : 1.0f;
}

void addUnlitLight(std::array<Color, 6>& samples, const Color& color, float threshold, bool litBySource) {
    const float share = unlitShare(samples, color, threshold, litBySource);
    if (!(share > 0)) {
        return;
    }
    for (std::size_t i = 0; i <= static_cast<std::size_t>(kFromFloor); ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            samples[i][c] += color[c] * share;
        }
    }
}

std::optional<Lights> lightsFor(const std::array<Color, 6>& samples, const Settings& s) {
    if (s.type < 2 || s.type > 8) {
        return std::nullopt;
    }
    const int count = s.type - 2;

    // Four sides, each with some of the floor's light; then up and down, from all four sides
    // bounced plus their own sample.
    std::array<Color, 6> d{};
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            d[i][c] = (samples[i][c] + samples[kFromFloor][c] * s.sideCrossover) * s.side * s.overall;
        }
    }
    for (std::size_t k = 0; k < 2; ++k) {
        for (std::size_t c = 0; c < 3; ++c) {
            const float sides = samples[0][c] + samples[1][c] + samples[2][c] + samples[3][c];
            d[4 + k][c] = (sides * s.bounceCrossover + samples[4 + k][c]) * s.bounce * s.overall;
        }
    }

    Lights out;
    out.ambient = average(d);
    out.ambientFromLightMap = s.fromLightMapMix != 0;
    out.specular = s.specular;
    for (auto& v : d) {
        for (std::size_t c = 0; c < 3; ++c) {
            v[c] -= out.ambient[c];
        }
    }
    for (int n = 0; n < count; ++n) {
        const std::size_t i = strongest(d, count < 3 ? 4 : 6);
        Directional l;
        for (std::size_t c = 0; c < 3; ++c) {
            l.color[c] = d[i][c] * s.directionalBoost;
        }
        l.direction = kDirections[i];
        if (i < 4) {
            l.direction[2] -= s.sideSlope;
        }
        out.directionals.push_back(l);
        d[i] = {0, 0, 0};
    }
    return out;
}

} // namespace ts2::scene::roomlight
