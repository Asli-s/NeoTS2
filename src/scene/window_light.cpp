#include "scene/window_light.hpp"

#include "diag/log.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

// The game works these out on the x87 in double precision and rounds to float only where it
// stores a value, so the code below does the same: double in between, float where the game
// keeps a float.

namespace ts2::scene::windowlight {

namespace {

using Color = std::array<float, 3>;

// Portals per room that share light, more than any room has, so the pairing stays quick.
constexpr std::size_t kMaxSharing = 1024;

Color times(const Color& a, const Color& b) {
    return {a[0] * b[0], a[1] * b[1], a[2] * b[2]};
}

Color scaled(const Color& a, float k) {
    return {a[0] * k, a[1] * k, a[2] * k};
}

Color plus(const Color& a, const Color& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

bool lit(const Color& c) {
    return c[0] != 0 || c[1] != 0 || c[2] != 0;
}

// A floor and wall light source from the portal's own: its colour times `color` times `share`
// (rounded as the game rounds it: red and green in one go, blue in two steps), its falloff
// scaled. None when it would be black.
std::optional<LightSource> sourceFrom(const LightSource& base, const Color& color, float share, float falloffScale) {
    LightSource s = base;
    s.color[0] = static_cast<float>(static_cast<double>(color[0]) * base.color[0] * share);
    s.color[1] = static_cast<float>(static_cast<double>(color[1]) * base.color[1] * share);
    const float blue = color[2] * base.color[2];
    s.color[2] = static_cast<float>(static_cast<double>(blue) * share);
    s.falloff *= falloffScale;
    if (!lit(s.color)) {
        return std::nullopt;
    }
    return s;
}

} // namespace

void suppress(std::vector<Portal>& portals, const Suppression& s) {
    for (auto& p : portals) {
        p.suppression = 1;
    }
    if (!(s.facing > 0) || !(s.radius > 0)) {
        return;
    }
    const std::size_t count = std::min(portals.size(), kMaxSharing);
    if (portals.size() > kMaxSharing) {
        diag::warnOnce("window light: windows and doors past 1024 share no light");
    }
    auto middle = [](float a, float b) { return static_cast<float>((static_cast<double>(a) + b) * 0.5); };
    for (std::size_t i = 0; i < count; ++i) {
        Portal& p = portals[i];
        if (p.otherRoom == 0) {
            continue;
        }
        const float px = middle(p.x0, p.x1), py = middle(p.y0, p.y1);
        for (std::size_t j = 0; j < count; ++j) {
            const Portal& q = portals[j];
            if (j == i || q.otherRoom != p.otherRoom || std::bit_cast<std::uint32_t>(q.height) != std::bit_cast<std::uint32_t>(p.height)) {
                continue;
            }
            const float vx = middle(q.x0, q.x1) - px, vy = middle(q.y0, q.y1) - py;
            const double ahead = static_cast<double>(p.ny) * vy + static_cast<double>(p.nx) * vx;
            const double behind = -(static_cast<double>(q.ny) * vy + static_cast<double>(q.nx) * vx);
            const double distance = std::sqrt(static_cast<double>(vx) * vx + static_cast<double>(vy) * vy);
            const float d = static_cast<float>(distance);
            // Only portals beside each other, looking the same way, within the radius.
            if (ahead < -0.001f || behind < -0.001f || !(distance <= s.radius) || d < 0.1f) {
                continue;
            }
            const double facing = static_cast<double>(p.ny) * q.ny + static_cast<double>(p.nx) * q.nx;
            const double share = (facing * facing) * (static_cast<double>(s.facing) - s.corner) + s.corner;
            const double k = (1.0 - static_cast<double>(d) / s.radius) * share;
            p.suppression = k < p.suppression ? static_cast<float>(p.suppression - k) : 0.0f;
        }
    }
}

std::optional<PortalLights> lightsFor(const Portal& p, bool fromOutside, bool sun, const Daylight& day, const Color& lightMap) {
    Modifiers direct = p.otherRoom == 0 ? day.outsidePortals : day.insidePortals;
    Modifiers spill = p.otherRoom == 0 ? day.outsidePortalSpills : day.insidePortalSpills;
    direct.practical *= p.suppression;
    spill.practical *= p.suppression;

    // The direct light (colour and how much of it) and the diffuse light (colour and weight).
    Color directColor{0, 0, 0}, diffuseColor{0, 0, 0};
    float directShare = 1, diffuseShare = 0;
    if (fromOutside && p.otherRoom != 0) {
        if (!sun) {
            return std::nullopt;
        }
        const double dot = (0.0 * day.sunDirection[2] + static_cast<double>(-p.ny) * day.sunDirection[1]) +
                           static_cast<double>(-p.nx) * day.sunDirection[0];
        const float facing = static_cast<float>(dot);
        directShare = facing > 0 ? facing : 0;  // also NaN
        directColor = plus(scaled(day.sunColor, day.sunStrength), lightMap);
        diffuseColor = plus(day.sky, scaled(lightMap, 0.5f));
        diffuseShare = 1;
    } else {
        directColor = scaled(lightMap, 0.5f);
        if (sun) {
            diffuseColor = scaled(lightMap, 0.5f);
            diffuseShare = 1;
        }
    }

    PortalLights out;
    out.roomColor = plus(scaled(diffuseColor, diffuseShare), scaled(directColor, directShare));
    if (day.suppressionDimsRoomLight) {
        out.roomColor = scaled(out.roomColor, p.suppression);
    }
    if (p.doorOrArch) {
        directShare *= day.doorOrArchDirect;
    }

    const float jitter = 1;  // portalLightJitter is 0 in every shipped file (see the open questions)
    const float beamStrength = static_cast<float>(static_cast<double>(direct.practical) * p.size * jitter * directShare);
    out.beam.color = scaled(times(directColor, p.tint), beamStrength);
    out.beam.falloff = direct.practicalFalloff * p.reach;
    if (auto s = sourceFrom(p.source, directColor, direct.diffusion * directShare, direct.diffusionFalloff)) {
        out.sources.push_back(*s);
    }
    const float spillStrength = static_cast<float>(static_cast<double>(spill.practical) * p.size * jitter * diffuseShare);
    if (spillStrength > 0) {
        out.spill = Light{scaled(times(diffuseColor, p.tint), spillStrength), spill.practicalFalloff * p.reach};
    }
    if (auto s = sourceFrom(p.source, diffuseColor, spill.diffusion * diffuseShare, spill.diffusionFalloff)) {
        out.sources.push_back(*s);
    }
    return out;
}

} // namespace ts2::scene::windowlight
