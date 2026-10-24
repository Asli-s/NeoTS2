#include "scene/lot_light_maps.hpp"

#include "scene/light_map_texels.hpp"

#include <cmath>
#include <cstddef>
#include <algorithm>

namespace ts2::scene::lotlightmaps {

namespace {

void put(Image& image, std::size_t x, std::size_t y, const walllight::Pixel& p) {
    if (x >= image.width || y >= image.height) return;
    auto* rgba = &image.rgba[(y * image.width + x) * 4];
    rgba[0] = p.r;
    rgba[1] = p.g;
    rgba[2] = p.b;
    rgba[3] = p.a;
}

// Rounds to the nearest whole number, halves to even, whatever the processor's rounding mode.
double roundHalfEven(double v) {
    const double down = std::floor(v);
    const double rest = v - down;
    return rest > 0.5 || (rest == 0.5 && std::fmod(down, 2.0) != 0) ? down + 1 : down;
}

// The sun on a floor texel of a level: under the shadow it fades, and the game keeps how much
// reaches the texel in its alpha (the square root of it, as a byte).
lightmaptexels::OutdoorTexel floorTexel(const Sunlight& sun, int level, int tx, int ty, int i, int k) {
    lightmaptexels::OutdoorTexel out;
    const int sx = lotshadow::kSamplesPerTile * tx + i, sy = lotshadow::kSamplesPerTile * ty + k;
    const auto& grid = *sun.shadow;
    if (sx < 0 || sy < 0 || sx >= grid.width || sy >= grid.height) return out;
    const bool terrain = level == 0 && sun.terrain && sun.terrain(tx, ty);
    float ground;
    if (terrain) {
        ground = grid.groundAt(sx, sy);
        if (sun.terrainNormal) out.normal = sun.terrainNormal(static_cast<float>(tx) + i / 8.0f, static_cast<float>(ty) + k / 8.0f);
    } else {
        ground = sun.lot->corner ? sun.lot->corner(tx, ty, level) : 0;
    }
    const float depth = grid.shadow[static_cast<std::size_t>(sy) * static_cast<std::size_t>(grid.width) + static_cast<std::size_t>(sx)] - ground;
    out.sun = lotshadow::sunVisible(depth, sun.zone);
    if (depth > 0) {
        out.alpha = static_cast<std::uint8_t>(std::clamp(roundHalfEven(256.0 * std::sqrt(std::max(1.0 - static_cast<double>(depth) * sun.zone, 0.0))), 0.0, 255.0));
    }
    return out;
}

// A wall face turned to the sun: each texel's point on the face, an eighth of a tile out from it,
// against the shadow; the bottom of the face at the lower of its ends' floors, its height whole
// storeys up to the floor above.
void shadeWall(std::vector<walllight::Pixel>& texels, lightmaptexels::Size size, const Sunlight& sun, const lightmaptexels::Outdoor& outdoor,
               int level, const walllight::Segment& seg, const std::array<float, 3>& n) {
    const auto& lot = *sun.lot;
    const float bottom = std::min(lotshadow::elevation(lot, seg.x0, seg.y0, level), lotshadow::elevation(lot, seg.x1, seg.y1, level));
    const float top = std::max(lotshadow::elevation(lot, seg.x0, seg.y0, level + 1), lotshadow::elevation(lot, seg.x1, seg.y1, level + 1));
    const float height = std::ceil(top - bottom);
    const bool small = size.width == 4 && size.height == 8;
    const float u0 = small ? -0.25f : 0.0f, du = small ? 0.5f : static_cast<float>(1.0 / (size.width - 1));
    const float v0 = small ? -1.0f / 12 : 0.0f, dv = small ? 1.0f / 6 : static_cast<float>(1.0 / (size.height - 1));
    float v = v0;
    for (int row = 0; row < size.height; ++row) {
        float u = u0;
        for (int x = 0; x < size.width; ++x) {
            const float px = seg.x0 + (seg.x1 - seg.x0) * u + n[0] * lotshadow::kCell;
            const float py = seg.y0 + (seg.y1 - seg.y0) * u + n[1] * lotshadow::kCell;
            const float pz = bottom + height * v;
            const float depth = sun.shadow->shadowAt(px, py) - pz;
            auto& t = texels[static_cast<std::size_t>(row * size.width + x)];
            const auto add = lightmaptexels::outdoorAdd(outdoor, n, lotshadow::sunVisible(depth, sun.zone));
            t = {static_cast<std::uint8_t>(std::min(255, t.b + add.b)), static_cast<std::uint8_t>(std::min(255, t.g + add.g)),
                 static_cast<std::uint8_t>(std::min(255, t.r + add.r)), t.a};
            u += du;
        }
        v += dv;
    }
}

} // namespace

void fill(Scene& scene, const lotlighting::Result& light, const lotlighting::Settings& settings, const Sunlight* sunlight) {
    if (!scene.lightMaps) {
        return;
    }
    const auto& maps = *scene.lightMaps;
    auto image = [&](int index) -> Image* {
        return index >= 0 && static_cast<std::size_t>(index) < scene.images.size() ? &scene.images[static_cast<std::size_t>(index)] : nullptr;
    };
    auto roomAdd = [&](std::int16_t room) {
        const auto it = light.rooms.find(room);
        return room <= 0 || it == light.rooms.end() ? walllight::Pixel{} : lightmaptexels::roomAdd(settings.unlit.color, it->second.unlitShare);
    };

    // Floors, level by level.
    for (std::size_t level = 0; level < light.floors.size() && level < maps.floors.size(); ++level) {
        Image* target = image(maps.floors[level]);
        const auto& grid = light.floors[level];
        auto texels = lightmaptexels::floorTexels(grid, {128, 128, 128, 255});
        if (!target || texels.width != static_cast<int>(target->width) || texels.height != static_cast<int>(target->height)) {
            continue;
        }
        lightmaptexels::addRoomLight(texels, grid, roomAdd);
        lightmaptexels::OutdoorAt at;
        if (sunlight && sunlight->shadow && sunlight->lot) {
            at = [&, level](int tx, int ty, int i, int k) { return floorTexel(*sunlight, static_cast<int>(level), tx, ty, i, k); };
        }
        lightmaptexels::addOutdoorLight(texels, grid, settings.outdoor, at);
        for (int y = 0; y < texels.height; ++y) {
            for (int x = 0; x < texels.width; ++x) {
                put(*target, static_cast<std::size_t>(x), static_cast<std::size_t>(y), *texels.at(x, y));
            }
        }
    }

    // Wall faces, each into its rect, the bottom of the wall at the rect's bottom.
    Image* walls = image(maps.walls);
    if (!walls) {
        return;
    }
    for (const auto& run : light.walls.runs) {
        for (const auto& seg : run.segments) {
            const std::array<int, 5> key{run.level, static_cast<int>(std::lround(seg.x0)), static_cast<int>(std::lround(seg.y0)),
                                         static_cast<int>(std::lround(seg.x1)), static_cast<int>(std::lround(seg.y1))};
            const auto rect = maps.wallRects.find(key);
            if (rect == maps.wallRects.end()) continue;
            const auto& r = rect->second;
            const lightmaptexels::Size size{r.x1 - r.x0, r.y1 - r.y0};
            const auto& ambient = run.room == 0 ? light.walls.outsideAmbient : std::array<float, 3>{0, 0, 0};
            auto texels = lightmaptexels::panelTexels(seg.texels, size, ambient, 0.5f, false, light.walls.falloffScale);
            if (texels.size() != static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height)) continue;
            if (run.room > 0) {
                lightmaptexels::addRoomLight(texels, roomAdd(run.room));
            } else {
                const float dx = seg.x1 - seg.x0, dy = seg.y1 - seg.y0, len = std::sqrt(dx * dx + dy * dy);
                if (len > 0) {
                    const std::array<float, 3> n{dy / len, -dx / len, 0};
                    const auto& sunDir = settings.outdoor.lights.empty() ? std::array<float, 3>{0, 0, 1} : settings.outdoor.lights.front().direction;
                    if (sunlight && sunlight->shadow && sunlight->lot && n[0] * sunDir[0] + n[1] * sunDir[1] > 0) {
                        shadeWall(texels, size, *sunlight, settings.outdoor, run.level, seg, n);
                    } else {
                        lightmaptexels::addRoomLight(texels, lightmaptexels::outdoorAdd(settings.outdoor, n));
                    }
                }
            }
            for (int row = 0; row < size.height; ++row) {
                for (int x = 0; x < size.width; ++x) {
                    put(*walls, static_cast<std::size_t>(r.x0 + x), static_cast<std::size_t>(r.y1 - 1 - row),
                        texels[static_cast<std::size_t>(row * size.width + x)]);
                }
            }
        }
    }
}

} // namespace ts2::scene::lotlightmaps
