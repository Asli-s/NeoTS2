#include "scene/neighborhood_paint.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::scene {

namespace {

// A distance map over the vertices: cells start at 0 (sources) or `far`; each pass gives the four
// neighbours of the cells at one level the next level, up to `far - 1`.
void spreadDistances(std::vector<std::uint8_t>& map, std::uint32_t columns, std::uint32_t rows, std::uint8_t far) {
    std::uint8_t lowest = 0xff;
    for (const auto v : map) {
        lowest = std::min(lowest, v);
    }
    if (lowest >= far) {
        return;
    }
    auto at = [&](std::uint32_t x, std::uint32_t y) -> std::uint8_t& { return map[static_cast<std::size_t>(y) * columns + x]; };
    for (std::uint8_t level = lowest; level + 1 < far; ++level) {
        bool changed = false;
        const std::uint8_t next = level + 1;
        for (std::uint32_t x = 0; x < columns; ++x) {
            for (std::uint32_t y = 0; y < rows; ++y) {
                if (at(x, y) != level) {
                    continue;
                }
                for (auto* n : {&at(x > 0 ? x - 1 : 0, y), &at(x + 1 < columns ? x + 1 : x, y), &at(x, y > 0 ? y - 1 : 0),
                                &at(x, y + 1 < rows ? y + 1 : y)}) {
                    if (next < *n) {
                        *n = next;
                        changed = true;
                    }
                }
            }
        }
        if (!changed) {
            break;
        }
    }
}

// Puts a paint on a vertex: a paint already there keeps the larger weight; a new one takes a free
// slot, or else the slot with the smallest weight if it is smaller.
void addPaint(VertexPaints& v, std::uint8_t paint, float strength) {
    // Worked out in more than float precision, as the game's x87 code does.
    const auto w = static_cast<std::uint16_t>(static_cast<double>(strength) * 255.0 + 1.0);
    for (int i = 0; i < 3; ++i) {
        if (v.id[i] == 0) {
            v.id[i] = paint;
            v.weight[i] = w;
            return;
        }
        if (v.id[i] == paint) {
            v.weight[i] = std::max(v.weight[i], w);
            return;
        }
    }
    int smallest = v.weight[1] < v.weight[0] ? 1 : 0;
    if (v.weight[2] < v.weight[smallest]) {
        smallest = 2;
    }
    if (v.weight[smallest] < w) {
        v.id[smallest] = paint;
        v.weight[smallest] = w;
    }
}

} // namespace

std::vector<float> terrainNormals(const nhood::Terrain& t, float tileSize) {
    // Worked out as the game's x87 code does: sums and products in more than float precision,
    // rounded to float where the game stores a value.
    const std::uint32_t cols = t.columns, rows = t.rows;
    const double s = tileSize;
    auto h = [&](std::uint32_t x, std::uint32_t y) { return static_cast<double>(t.heights[static_cast<std::size_t>(y) * cols + x]); };
    auto unit = [](float* f, double x, double y, double z) {
        const auto inv = static_cast<float>(1.0 / std::sqrt(z * z + y * y + x * x));
        f[0] = static_cast<float>(inv * static_cast<double>(static_cast<float>(x)));
        f[1] = static_cast<float>(inv * static_cast<double>(static_cast<float>(y)));
        f[2] = static_cast<float>(inv * static_cast<double>(static_cast<float>(z)));
    };
    // Two triangles per tile: (x,y)(x+1,y+1)(x,y+1) and (x,y)(x+1,y)(x+1,y+1).
    std::vector<float> faces(static_cast<std::size_t>(cols - 1) * (rows - 1) * 6);
    for (std::uint32_t y = 0; y + 1 < rows; ++y) {
        for (std::uint32_t x = 0; x + 1 < cols; ++x) {
            const double h0 = h(x, y);
            const double up = h(x, y + 1) - h0, diag = h(x + 1, y + 1) - h0, right = h(x + 1, y) - h0;
            float* f = &faces[(static_cast<std::size_t>(y) * (cols - 1) + x) * 6];
            unit(f, s * up - diag * s, -up * s, s * s);
            unit(f + 3, -right * s, right * s - diag * s, s * s);
        }
    }
    // The last row of vertices is left without a normal (0, 0, 0), as the game leaves it.
    std::vector<float> normals(static_cast<std::size_t>(cols) * rows * 3);
    for (std::uint32_t y = 0; y + 1 < rows; ++y) {
        for (std::uint32_t x = 0; x < cols; ++x) {
            double n[3] = {0, 0, 0};
            auto face = [&](std::uint32_t fx, std::uint32_t fy, int which) {
                return &faces[(static_cast<std::size_t>(fy) * (cols - 1) + fx) * 6 + which * 3];
            };
            auto add2 = [&](const float* a, const float* b) {
                for (int i = 0; i < 3; ++i) {
                    n[i] = static_cast<double>(static_cast<float>(n[i] + a[i] + b[i]));
                }
            };
            if (x > 0) {
                const float* f = face(x - 1, y, 1);
                for (int i = 0; i < 3; ++i) {
                    n[i] = f[i];
                }
                if (y > 0) {
                    add2(face(x - 1, y - 1, 1), face(x - 1, y - 1, 0));
                }
            }
            if (x + 1 < cols) {
                add2(face(x, y, 0), face(x, y, 1));
                if (y > 0) {
                    const float* f = face(x, y - 1, 0);
                    for (int i = 0; i < 3; ++i) {
                        n[i] = static_cast<double>(static_cast<float>(n[i] + f[i]));
                    }
                }
            }
            float* out = &normals[(static_cast<std::size_t>(y) * cols + x) * 3];
            const double len = std::sqrt(n[0] * n[0] + n[2] * n[2] + n[1] * n[1]);
            if (len == 0) {
                n[0] = n[1] = n[2] = 1;
            }
            const auto inv = static_cast<float>(1.0 / std::sqrt(n[0] * n[0] + n[2] * n[2] + n[1] * n[1]));
            for (int i = 0; i < 3; ++i) {
                out[i] = static_cast<float>(inv * n[i]);
            }
        }
    }
    return normals;
}

TerrainPaints paintTerrain(const nhood::Terrain& t, const nhood::PaintRules& rules, float tile, std::span<const std::uint8_t> roads,
                           std::span<const float> normals) {
    TerrainPaints out;
    const std::uint32_t cols = t.columns, rows = t.rows;
    out.columns = cols;
    out.rows = rows;
    out.vertices.resize(static_cast<std::size_t>(cols) * rows);
    out.normals = normals.size() == out.vertices.size() * 3 ? std::vector<float>(normals.begin(), normals.end()) : terrainNormals(t, tile);
    const auto typeIt = rules.types.find(t.type);
    if (typeIt == rules.types.end() || cols < 2 || rows < 2) {
        return out;
    }
    const auto& type = typeIt->second;
    auto index = [&](std::uint32_t x, std::uint32_t y) { return static_cast<std::size_t>(y) * cols + x; };
    auto height = [&](std::uint32_t x, std::uint32_t y) { return t.heights[index(x, y)]; };
    auto depth = [&](std::uint32_t x, std::uint32_t y) { return t.seaLevel - height(x, y); };

    // How far each vertex is from the water, in steps, up to the beach width + 2.
    const auto beachFar = static_cast<std::uint8_t>(rules.maxBeachWidthInCells + 2);
    std::vector<std::uint8_t> water(out.vertices.size());
    for (std::uint32_t y = 0; y < rows; ++y) {
        for (std::uint32_t x = 0; x < cols; ++x) {
            water[index(x, y)] = depth(x, y) <= 0 ? beachFar : 0;
        }
    }
    spreadDistances(water, cols, rows, beachFar);

    // How far each vertex is from a road, in steps, scaled to the table's 16 columns.
    const auto roadFar = static_cast<std::uint8_t>(rules.maxRoadProximity);
    std::vector<std::uint8_t> road(out.vertices.size(), roadFar);
    const std::uint32_t tilesX = cols - 1, tilesY = rows - 1;
    if (roads.size() == static_cast<std::size_t>(tilesX) * tilesY) {
        for (std::uint32_t y = 0; y < tilesY; ++y) {
            for (std::uint32_t x = 0; x < tilesX; ++x) {
                if (roads[static_cast<std::size_t>(y) * tilesX + x] != 0) {
                    road[index(x, y)] = 0;
                }
            }
        }
    }
    spreadDistances(road, cols, rows, roadFar);
    for (auto& r : road) {
        r = static_cast<std::uint8_t>(std::min(15.0f, std::floor(static_cast<float>(r) * (16.0f / static_cast<float>(roadFar)))));
    }

    // The game turns degrees into radians with 3.14 for pi.
    auto cosDegrees = [](float degrees) {
        return static_cast<float>(std::cos(static_cast<double>(degrees) * static_cast<double>(3.14f) * static_cast<double>(1.0f / 180)));
    };
    const float cliffStart = cosDegrees(rules.cliffAngleMin), cliffFull = cosDegrees(rules.cliffAngleMax);
    const float cliffScale = cliffStart <= cliffFull ? 0.0f : static_cast<float>(1.0 / (static_cast<double>(cliffStart) - cliffFull));
    const float beachTop = t.seaLevel + rules.maxBeachAltitude;
    const float rowsPerHeight = 31.0f / rules.heightRangeForTemperature;
    const auto beachWide = static_cast<std::uint8_t>(rules.maxBeachWidthInCells);

    // The game paints twice when it loads a neighborhood, the second time over the first's
    // results, which it does not clear: weights merge and are scaled to 250 again.
    for (int pass = 0; pass < 2; ++pass) {
        for (std::uint32_t y = 0; y < rows; ++y) {
            for (std::uint32_t x = 0; x < cols; ++x) {
                if (depth(x, y) > rules.waterDepthThresholdForPaint) {
                    continue;
                }
                const float h = height(x, y);
                const float nz = std::fabs(out.normals[index(x, y) * 3 + 2]);
                std::uint8_t paint = 0;
                bool spread = true, limitDepth = true;
                float cliff = 0;
                if (nz >= cliffStart) {
                    if (water[index(x, y)] > beachWide || h > beachTop) {
                        const int step = static_cast<int>(std::floor((h - t.seaLevel) * rowsPerHeight));
                        const int row = std::clamp(31 - step, 0, 31);
                        paint = type.table[static_cast<std::size_t>(row)][road[index(x, y)]];
                    } else {
                        paint = type.beach;
                        addPaint(out.vertices[index(x, y)], paint, 1.0f);
                        limitDepth = false;
                    }
                } else {
                    cliff = nz >= cliffFull ? static_cast<float>((static_cast<double>(cliffStart) - nz) * cliffScale) : 1.0f;
                    paint = type.cliff;
                    spread = false;
                }
                const auto info = rules.paints.find(paint);
                if (info == rules.paints.end()) {
                    continue;
                }
                const auto& p = info->second;
                if (!spread) {
                    const float w = cliff * p.maxStrength;
                    if (w > 0) {
                        addPaint(out.vertices[index(x, y)], paint, w);
                    }
                    continue;
                }
                const float radius = p.spread.empty() || !std::isfinite(p.spread.back().first) ? 0 : p.spread.back().first;
                // No further than across the whole terrain, whatever the rules say.
                const int cells = static_cast<int>(std::min<double>(std::ceil(radius * 0.1), std::max(cols, rows)));
                const int x0 = std::max(0, static_cast<int>(x) - cells), x1 = std::min(static_cast<int>(cols) - 1, static_cast<int>(x) + cells);
                const int y0 = std::max(0, static_cast<int>(y) - cells), y1 = std::min(static_cast<int>(rows) - 1, static_cast<int>(y) + cells);
                for (int yy = y0; yy <= y1; ++yy) {
                    for (int xx = x0; xx <= x1; ++xx) {
                        const auto ux = static_cast<std::uint32_t>(xx), uy = static_cast<std::uint32_t>(yy);
                        const float d = depth(ux, uy);
                        if (limitDepth && d > rules.waterDepthThresholdForPaint) {
                            continue;
                        }
                        if (!(height(ux, uy) > beachTop || water[index(ux, uy)] > beachWide || d > 0)) {
                            continue;
                        }
                        const int dx = xx - static_cast<int>(x), dy = yy - static_cast<int>(y);
                        const auto dist = static_cast<float>(std::sqrt(static_cast<double>(dy * dy + dx * dx)) * tile);
                        if (dist >= radius) {
                            continue;
                        }
                        const auto w = static_cast<float>(nhood::spreadAt(p, dist) * p.maxStrength);
                        if (w > 0) {
                            addPaint(out.vertices[index(ux, uy)], paint, w);
                        }
                    }
                }
            }
        }

        // Weights scaled to add up to 250.
        for (auto& v : out.vertices) {
            unsigned sum = 0;
            for (int i = 0; i < 3 && v.id[i] != 0; ++i) {
                sum += v.weight[i];
            }
            if (sum == 0) {
                continue;
            }
            for (int i = 0; i < 3 && v.id[i] != 0; ++i) {
                v.weight[i] = static_cast<std::uint16_t>(v.weight[i] * 250u / sum);
            }
        }
    }
    return out;
}

} // namespace ts2::scene
