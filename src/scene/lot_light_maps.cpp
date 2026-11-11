#include "scene/lot_light_maps.hpp"

#include "scene/incidence_map.hpp"
#include "scene/light_map_texels.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

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
// against the shadow. The bottom of the face is at the lower of its ends' floors, its height whole
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

// Writes a light map's incidence map into its image when the sizes agree.
void putIncidence(Image* target, const Image& lightMap, const lotlighting::Settings& settings, float heightClamp) {
    if (!target) return;
    Image incidence = incidenceMap(lightMap, settings.incidenceScale, heightClamp);
    if (incidence.width == target->width && incidence.height == target->height) {
        *target = std::move(incidence);
    }
}

} // namespace

GroundNormals groundNormals(const lotshadow::Inputs& lot) {
    constexpr int kMaxTiles = 256;
    GroundNormals out;
    if (!lot.corner || lot.levels <= 0 || lot.width <= 0 || lot.height <= 0 || lot.width > kMaxTiles || lot.height > kMaxTiles) {
        return out;
    }
    const int w = lot.width, h = lot.height;
    out.width = w;
    out.height = h;
    out.corners.assign(static_cast<std::size_t>(w + 1) * static_cast<std::size_t>(h + 1), {0, 0, 0});
    auto corner = [&](int x, int y) -> std::array<float, 3>& {
        return out.corners[static_cast<std::size_t>(y) * static_cast<std::size_t>(w + 1) + static_cast<std::size_t>(x)];
    };
    auto add = [](std::array<float, 3>& to, const std::array<float, 3>& n) {
        for (std::size_t c = 0; c < 3; ++c) to[c] += n[c];
    };
    auto unit = [](std::array<float, 3> n) {
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        return len > 0 && std::isfinite(len) ? std::array<float, 3>{n[0] / len, n[1] / len, n[2] / len} : std::array<float, 3>{0, 0, 1};
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float h00 = lot.corner(x, y, 0);
            const float d10 = lot.corner(x + 1, y, 0) - h00, d01 = lot.corner(x, y + 1, 0) - h00, d11 = lot.corner(x + 1, y + 1, 0) - h00;
            // Triangle (x, y) (x + 1, y) (x + 1, y + 1), then (x, y) (x + 1, y + 1) (x, y + 1).
            const auto lower = unit({-d10, d10 - d11, 1});
            const auto upper = unit({d01 - d11, -d01, 1});
            add(corner(x, y), lower);
            add(corner(x + 1, y), lower);
            add(corner(x + 1, y + 1), lower);
            add(corner(x, y), upper);
            add(corner(x + 1, y + 1), upper);
            add(corner(x, y + 1), upper);
        }
    }
    for (auto& n : out.corners) n = unit(n);
    return out;
}

std::array<float, 3> underwaterNormal(const std::array<float, 3>& normal, float depth) {
    constexpr float kFlatDepth = 1;
    const float t = std::isfinite(depth) ? std::clamp(depth / kFlatDepth, 0.0f, 1.0f) : 0.0f;
    const std::array<float, 3> n{normal[0] * (1 - t), normal[1] * (1 - t), normal[2] * (1 - t) + t};
    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    return len > 0 && std::isfinite(len) ? std::array<float, 3>{n[0] / len, n[1] / len, n[2] / len} : std::array<float, 3>{0, 0, 1};
}

GroundNormals groundNormals(const Scene::DrawnGround& drawn) {
    constexpr std::uint32_t kMaxTiles = 256, kMaxSteps = 16;
    GroundNormals out;
    const std::uint32_t steps = drawn.steps, columns = drawn.columns, rows = drawn.rows;
    if (steps == 0 || steps > kMaxSteps || columns < 2 || rows < 2 || (columns - 1) % steps != 0 || (rows - 1) % steps != 0 ||
        (columns - 1) / steps > kMaxTiles || (rows - 1) / steps > kMaxTiles ||
        drawn.heights.size() != static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows)) {
        return out;
    }
    out.width = static_cast<int>((columns - 1) / steps);
    out.height = static_cast<int>((rows - 1) / steps);
    out.steps = static_cast<int>(steps);
    out.corners.resize(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows));
    const auto height = [&](std::int64_t i, std::int64_t j) {
        const auto ci = static_cast<std::size_t>(std::clamp<std::int64_t>(i, 0, columns - 1));
        const auto cj = static_cast<std::size_t>(std::clamp<std::int64_t>(j, 0, rows - 1));
        return drawn.heights[ci * rows + cj];
    };
    const float step = 1.0f / static_cast<float>(steps);
    for (std::int64_t j = 0; j < rows; ++j) {
        for (std::int64_t i = 0; i < columns; ++i) {
            const float sx = (height(i + 1, j) - height(i - 1, j)) / (2 * step), sy = (height(i, j + 1) - height(i, j - 1)) / (2 * step);
            const float len = std::sqrt(sx * sx + sy * sy + 1);
            const bool usable = std::isfinite(len) && len > 0;
            const std::array<float, 3> n = usable ? std::array<float, 3>{-sx / len, -sy / len, 1 / len} : std::array<float, 3>{0, 0, 1};
            out.corners[static_cast<std::size_t>(j) * columns + static_cast<std::size_t>(i)] = underwaterNormal(n, drawn.sea - height(i, j));
        }
    }
    return out;
}

float drawnHeight(const Scene::DrawnGround& drawn, float x, float y) {
    const std::uint32_t columns = drawn.columns, rows = drawn.rows;
    if (drawn.steps == 0 || columns < 2 || rows < 2 || drawn.heights.size() != static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows) ||
        !std::isfinite(x) || !std::isfinite(y)) {
        return 0;
    }
    const float u = std::clamp(x * static_cast<float>(drawn.steps), 0.0f, static_cast<float>(columns - 1));
    const float v = std::clamp(y * static_cast<float>(drawn.steps), 0.0f, static_cast<float>(rows - 1));
    const auto i = std::min(static_cast<std::uint32_t>(u), columns - 2), j = std::min(static_cast<std::uint32_t>(v), rows - 2);
    const float fx = u - static_cast<float>(i), fy = v - static_cast<float>(j);
    const auto at = [&](std::uint32_t c, std::uint32_t r) { return drawn.heights[static_cast<std::size_t>(c) * rows + r]; };
    return (at(i, j) * (1 - fx) + at(i + 1, j) * fx) * (1 - fy) + (at(i, j + 1) * (1 - fx) + at(i + 1, j + 1) * fx) * fy;
}

std::array<float, 3> GroundNormals::at(float x, float y) const {
    if (width <= 0 || height <= 0 || steps <= 0 ||
        corners.size() != static_cast<std::size_t>(width * steps + 1) * static_cast<std::size_t>(height * steps + 1) || !std::isfinite(x) ||
        !std::isfinite(y)) {
        return {0, 0, 1};
    }
    x *= static_cast<float>(steps);
    y *= static_cast<float>(steps);
    const int across = width * steps, down = height * steps;
    const float tx = std::clamp(std::floor(x), 0.0f, static_cast<float>(across - 1)), ty = std::clamp(std::floor(y), 0.0f, static_cast<float>(down - 1));
    const float u = std::clamp(x - tx, 0.0f, 1.0f), v = std::clamp(y - ty, 0.0f, 1.0f);
    const auto at = [&](int cx, int cy) { return corners[static_cast<std::size_t>(cy) * static_cast<std::size_t>(across + 1) + static_cast<std::size_t>(cx)]; };
    const int ix = static_cast<int>(tx), iy = static_cast<int>(ty);
    const auto a = at(ix, iy), b = at(ix + 1, iy), c = at(ix, iy + 1), d = at(ix + 1, iy + 1);
    std::array<float, 3> n{};
    for (std::size_t k = 0; k < 3; ++k) {
        n[k] = a[k] * (1 - u) * (1 - v) + b[k] * u * (1 - v) + c[k] * (1 - u) * v + d[k] * u * v;
    }
    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    return len > 0 && std::isfinite(len) ? std::array<float, 3>{n[0] / len, n[1] / len, n[2] / len} : std::array<float, 3>{0, 0, 1};
}

// Writes the lamps' and windows' share of the ground's light around the lot: the lit ground less the
// unlit, in the light maps' scale and texel for texel with them. The lot's own grid ends at its edge,
// so its light map holds the edge's light flat there. Past the edge the fade is scaled to start from
// the lot's own light at the nearest edge texel, so the skirt meets the lot without a step. The alpha
// is how much of the sun reaches each texel under the buildings' shadow, all of it without one.
void fillSkirtLamps(Image& target, const lotlighting::Result& light, const Sunlight* sunlight) {
    const int ground = -light.lowest;
    if (ground < 0 || static_cast<std::size_t>(ground) >= light.floors.size()) {
        return;
    }
    const auto lot = lightmaptexels::floorTexels(light.floors[static_cast<std::size_t>(ground)], {0, 0, 0, 255});
    const auto lit = lightmaptexels::floorTexels(light.around, {0, 0, 0, 255});
    const auto unlit = lightmaptexels::floorTexels(light.aroundUnlit, {0, 0, 0, 255});
    if (lot.width <= 0 || lot.height <= 0 || lit.width != unlit.width || lit.height != unlit.height || lit.width != static_cast<int>(target.width) ||
        lit.height != static_cast<int>(target.height) || lit.width < lot.width || lit.height < lot.height) {
        return;
    }
    // Where the lot's texels start in the image: the margin, as many texels on each side.
    const int x0 = (lit.width - lot.width) / 2, y0 = (lit.height - lot.height) / 2;
    const auto share = [](std::uint8_t a, std::uint8_t b) { return std::max(0, a - b); };
    const lotshadow::Grid* shadow = sunlight && sunlight->around && sunlight->around->width == lit.width && sunlight->around->height == lit.height &&
                                            sunlight->around->shadow.size() == lit.texels.size() && sunlight->around->ground.size() == lit.texels.size()
                                        ? sunlight->around
                                        : nullptr;
    for (int y = 0; y < lit.height; ++y) {
        for (int x = 0; x < lit.width; ++x) {
            const int lx = std::clamp(x - x0, 0, lot.width - 1), ly = std::clamp(y - y0, 0, lot.height - 1);
            const bool inside = lx == x - x0 && ly == y - y0;
            const auto* at = lit.at(x, y);
            const auto* off = unlit.at(x, y);
            const auto* edge = lit.at(lx + x0, ly + y0);
            const auto* edgeOff = unlit.at(lx + x0, ly + y0);
            const auto* own = lot.at(lx, ly);
            const auto channel = [](const walllight::Pixel& p, std::size_t c) { return c == 0 ? p.r : c == 1 ? p.g : p.b; };
            std::array<std::uint8_t, 3> out{};
            for (std::size_t c = 0; c < 3; ++c) {
                const int ownShare = share(channel(*own, c), channel(*edgeOff, c));
                if (inside) {
                    out[c] = static_cast<std::uint8_t>(ownShare);
                    continue;
                }
                const int here = share(channel(*at, c), channel(*off, c)), atEdge = share(channel(*edge, c), channel(*edgeOff, c));
                const int scaled = atEdge > 0 ? here * ownShare / atEdge : here;
                out[c] = static_cast<std::uint8_t>(std::clamp(scaled, 0, 255));
            }
            std::uint8_t sun = 255;
            if (shadow) {
                // The shadow grid's rows run as the light map's texels do, 8 a tile.
                const std::size_t s = static_cast<std::size_t>(y) * static_cast<std::size_t>(lit.width) + static_cast<std::size_t>(x);
                const float visible = lotshadow::sunVisible(shadow->shadow[s] - shadow->ground[s], sunlight->zone);
                sun = static_cast<std::uint8_t>(std::clamp(std::lround(255.0f * visible), 0L, 255L));
            }
            put(target, static_cast<std::size_t>(x), static_cast<std::size_t>(y), {out[0], out[1], out[2], sun});
        }
    }
}

void fill(Scene& scene, const lotlighting::Result& light, const lotlighting::Settings& settings, const Sunlight* sunlight) {
    if (!scene.lightMaps) {
        return;
    }
    const auto& maps = *scene.lightMaps;
    auto image = [&](int index) -> Image* {
        return index >= 0 && static_cast<std::size_t>(index) < scene.images.size() ? &scene.images[static_cast<std::size_t>(index)] : nullptr;
    };
    if (Image* skirt = image(maps.skirtLamps)) {
        fillSkirtLamps(*skirt, light, sunlight);
    }
    auto roomAdd = [&](std::int16_t room) {
        const auto it = light.rooms.find(room);
        return room <= 0 || it == light.rooms.end() ? walllight::Pixel{} : lightmaptexels::roomAdd(settings.unlit.color, it->second.unlitShare);
    };

    // Floors, level by level (`level` counts from the ground storey).
    for (std::size_t i = 0; i < light.floors.size(); ++i) {
        const int level = light.lowest + static_cast<int>(i);
        const int m = level - maps.lowest;
        if (m < 0 || static_cast<std::size_t>(m) >= maps.floors.size()) {
            continue;
        }
        Image* target = image(maps.floors[static_cast<std::size_t>(m)]);
        const auto& grid = light.floors[i];
        auto texels = lightmaptexels::floorTexels(grid, {128, 128, 128, 255});
        if (!target || texels.width != static_cast<int>(target->width) || texels.height != static_cast<int>(target->height)) {
            continue;
        }
        lightmaptexels::addRoomLight(texels, grid, roomAdd);
        // As the game: the incidence map from the light before the sunlight outdoors is added.
        if (static_cast<std::size_t>(m) < maps.floorIncidence.size()) {
            Image before{target->width, target->height, std::vector<std::uint8_t>(target->rgba.size())};
            for (int y = 0; y < texels.height; ++y) {
                for (int x = 0; x < texels.width; ++x) {
                    put(before, static_cast<std::size_t>(x), static_cast<std::size_t>(y), *texels.at(x, y));
                }
            }
            putIncidence(image(maps.floorIncidence[static_cast<std::size_t>(m)]), before, settings, settings.incidenceHeightClamp);
        }
        lightmaptexels::OutdoorAt at;
        if (sunlight && sunlight->shadow && sunlight->lot) {
            at = [&, level](int tx, int ty, int i, int k) { return floorTexel(*sunlight, level, tx, ty, i, k); };
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
    Image before = *walls;  // the wall light before the sunlight outdoors, for the incidence map
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
            auto keep = [&] {
                for (int row = 0; row < size.height; ++row) {
                    for (int x = 0; x < size.width; ++x) {
                        put(before, static_cast<std::size_t>(r.x0 + x), static_cast<std::size_t>(r.y1 - 1 - row),
                            texels[static_cast<std::size_t>(row * size.width + x)]);
                    }
                }
            };
            if (run.room > 0) {
                lightmaptexels::addRoomLight(texels, roomAdd(run.room));
                keep();
            } else {
                keep();
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
    for (const auto& key : maps.outdoorFaces) {
        const auto rect = maps.wallRects.find(key);
        const float dx = static_cast<float>(key[3] - key[1]), dy = static_cast<float>(key[4] - key[2]);
        const float len = std::sqrt(dx * dx + dy * dy);
        if (rect == maps.wallRects.end() || len <= 0) continue;
        // Outdoors where the half tile in front of the face's middle is (a quarter tile out).
        const float nx = dy / len, ny = -dx / len;
        const float px = (key[1] + key[3]) / 2.0f + nx / 4, py = (key[2] + key[4]) / 2.0f + ny / 4;
        const int level = key[0] - light.lowest;
        if (level < 0 || static_cast<std::size_t>(level) >= light.floors.size()) continue;
        const auto* cell = light.floors[static_cast<std::size_t>(level)].at(static_cast<int>(std::floor(2 * px)), static_cast<int>(std::floor(2 * py)));
        if (cell == nullptr || !cell->halves[0].outside) continue;
        auto add = lightmaptexels::outdoorAdd(settings.outdoor, {nx, ny, 0});
        add.a = 255;
        const auto& r = rect->second;
        for (int y = r.y0; y < r.y1; ++y) {
            for (int x = r.x0; x < r.x1; ++x) {
                put(*walls, static_cast<std::size_t>(x), static_cast<std::size_t>(y), add);
            }
        }
    }
    // Walls take 0.8 for the height clamp.
    putIncidence(image(maps.wallIncidence), before, settings, 0.8f);
}

void addSkirtLamps(Scene& scene) {
    if (!scene.lightMaps || scene.lightMaps->skirtLamps >= 0) {
        return;
    }
    auto& maps = *scene.lightMaps;
    const int ground = -maps.lowest;
    if (ground < 0 || static_cast<std::size_t>(ground) >= maps.floors.size()) {
        return;
    }
    const int index = maps.floors[static_cast<std::size_t>(ground)];
    if (index < 0 || static_cast<std::size_t>(index) >= scene.images.size()) {
        return;
    }
    // The floor light maps hold 8 texels a tile.
    const int width = static_cast<int>(scene.images[static_cast<std::size_t>(index)].width / 8);
    const int height = static_cast<int>(scene.images[static_cast<std::size_t>(index)].height / 8);
    const int margin = lotlighting::aroundMargin(width, height);
    if (margin <= 0) {
        return;
    }
    Image lamps;
    lamps.width = static_cast<std::uint32_t>(8 * (width + 2 * margin));
    lamps.height = static_cast<std::uint32_t>(8 * (height + 2 * margin));
    lamps.rgba.assign(static_cast<std::size_t>(lamps.width) * lamps.height * 4, 0);
    maps.skirtLamps = static_cast<int>(scene.images.size());
    maps.skirtMargin = margin;
    scene.images.push_back(std::move(lamps));
}

void linkSkirtLamps(Scene& scene, const Transform& place) {
    if (!scene.lightMaps || scene.lightMaps->skirtLamps < 0 || static_cast<std::size_t>(scene.lightMaps->skirtLamps) >= scene.images.size()) {
        return;
    }
    const auto& maps = *scene.lightMaps;
    const auto& image = scene.images[static_cast<std::size_t>(maps.skirtLamps)];
    if (image.width == 0 || image.height == 0) {
        return;
    }
    // From the scene to the lot (tiles), moved by the margin and scaled to the image.
    const float margin = static_cast<float>(maps.skirtMargin);
    const Transform toLot = place.rigidInverse();
    const std::array<float, 2> scale{8.0f / static_cast<float>(image.width), 8.0f / static_cast<float>(image.height)};
    std::array<std::array<float, 4>, 2> rows{};
    for (std::size_t r = 0; r < 2; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            rows[r][c] = toLot.m[r * 4 + c] * scale[r];
        }
        rows[r][3] += margin * scale[r];
    }
    for (auto& instance : scene.instances) {
        if (instance.part != Part::Skirt || instance.name == "lot skirt sea") {
            continue;
        }
        instance.lightMap = maps.skirtLamps;
        instance.skirtLampMap = rows;
        // As the lot's terrain takes its light map: times twice the material's diffuse (half its
        // skirt ambient), the ground's 0.9 and the roads' 1.
        instance.skirtLampGain = instance.skirtAmbient / 2;
    }
}

} // namespace ts2::scene::lotlightmaps
