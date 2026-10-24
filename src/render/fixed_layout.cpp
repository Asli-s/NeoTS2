#include "render/fixed_layout.hpp"

#include "render/image_levels.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::render {

namespace {

// The underwater gradient's colour at a height (white without one): a column of the image at its
// centre, sampled between two rows as a linear sampler clamped at the edges does.
std::array<float, 3> gradientColor(const scene::PaintedTerrain& t, float height) {
    if (!complete(t.underwaterGradient) || t.gradientDepth <= 0) {
        return {1, 1, 1};
    }
    const scene::Image& image = t.underwaterGradient;
    const float v = (t.litFrom - height) / t.gradientDepth;
    const float at = std::clamp(v * static_cast<float>(image.height) - 0.5f, 0.0f, static_cast<float>(image.height - 1));
    const auto row0 = static_cast<std::size_t>(at);
    const std::size_t row1 = std::min<std::size_t>(row0 + 1, image.height - 1);
    const float share = at - static_cast<float>(row0);
    const std::size_t column = image.width / 2;
    std::array<float, 3> out{};
    for (std::size_t k = 0; k < 3; ++k) {
        const float a = image.rgba[(row0 * image.width + column) * 4 + k], b = image.rgba[(row1 * image.width + column) * 4 + k];
        out[k] = (a + (b - a) * share) / 255;
    }
    return out;
}

// Channel `k` (0 to 1) of an RGBA picture sampled linearly at (x, y), in texels with texel i's centre at i + 0.5,
// wrapping round its edges or clamped at them.
float sampleTexels(const std::uint8_t* rgba, std::size_t width, std::size_t height, float x, float y, std::size_t k, bool wrap) {
    const float fx = x - 0.5f, fy = y - 0.5f;
    const float x0 = std::floor(fx), y0 = std::floor(fy);
    const float tx = fx - x0, ty = fy - y0;
    const auto w = static_cast<long>(width), h = static_cast<long>(height);
    const auto texel = [&](float xf, float yf) {
        long xi = std::lround(xf), yi = std::lround(yf);
        if (wrap) {
            xi = ((xi % w) + w) % w;
            yi = ((yi % h) + h) % h;
        } else {
            xi = std::clamp(xi, 0L, w - 1);
            yi = std::clamp(yi, 0L, h - 1);
        }
        return static_cast<float>(rgba[(static_cast<std::size_t>(yi) * width + static_cast<std::size_t>(xi)) * 4 + k]) / 255;
    };
    const float top = texel(x0, y0) * (1 - tx) + texel(x0 + 1, y0) * tx;
    const float bottom = texel(x0, y0 + 1) * (1 - tx) + texel(x0 + 1, y0 + 1) * tx;
    return top * (1 - ty) + bottom * ty;
}

// The terrain light map at a place in grid (or, for roads, light-map) units, wrapping as it is sampled.
std::array<float, 3> lightAt(const scene::PaintedTerrain& t, float x, float y) {
    if (!complete(t.lightMap)) {
        return {1, 1, 1};
    }
    const scene::Image& image = t.lightMap;
    const float u = x / t.lightMapScale * static_cast<float>(image.width), v = y / t.lightMapScale * static_cast<float>(image.height);
    return {sampleTexels(image.rgba.data(), image.width, image.height, u, v, 0, true),
            sampleTexels(image.rgba.data(), image.width, image.height, u, v, 1, true),
            sampleTexels(image.rgba.data(), image.width, image.height, u, v, 2, true)};
}

std::uint8_t byteOf(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255));
}

} // namespace

std::vector<float> fixedTerrainVertices(const IndexedMesh& grid, const scene::PaintedTerrain& t) {
    const std::size_t count = grid.vertices.size() / kTerrainFloats;
    std::vector<float> out(count * kFixedTerrainFloats);
    const float paint = 1 / t.textureRepeat, weights = t.alphaSize ? 1.0f / static_cast<float>(t.alphaSize) : 1.0f;
    const float light = 1 / t.lightMapScale;
    for (std::size_t i = 0; i < count; ++i) {
        const float* in = &grid.vertices[i * kTerrainFloats];
        float* v = &out[i * kFixedTerrainFloats];
        v[0] = in[0];
        v[1] = in[1];
        v[2] = in[2];
        v[3] = in[6] * paint;
        v[4] = in[7] * paint;
        v[5] = in[6] * weights;
        v[6] = in[7] * weights;
        v[7] = in[6] * light;
        v[8] = in[7] * light;
    }
    return out;
}

std::vector<std::uint8_t> fixedTerrainColors(const IndexedMesh& grid, const scene::PaintedTerrain& t) {
    const std::size_t count = grid.vertices.size() / kTerrainFloats;
    std::vector<std::uint8_t> out(count * 4, 255);
    for (std::size_t i = 0; i < count; ++i) {
        const auto color = gradientColor(t, grid.vertices[i * kTerrainFloats + 2]);
        for (std::size_t k = 0; k < 3; ++k) {
            out[i * 4 + k] = byteOf(color[k]);
        }
    }
    return out;
}

std::vector<std::uint8_t> fixedSliceColors(const IndexedMesh& grid, const scene::PaintedTerrain& t, unsigned slice) {
    const std::size_t count = grid.vertices.size() / kTerrainFloats;
    std::vector<std::uint8_t> out(count * 4, 255);
    const std::vector<std::uint8_t> weights = fixedPaintWeights(t, slice);
    for (std::size_t i = 0; i < count; ++i) {
        const float* v = &grid.vertices[i * kTerrainFloats];
        const float weight = t.alphaSize ? sampleTexels(weights.data(), t.alphaSize, t.alphaSize, v[6], v[7], 0, false) : 0.0f;
        const auto gradient = gradientColor(t, v[2]);
        // The light map is left off what is under the water, as the shader does.
        const auto light = v[2] >= t.litFrom ? lightAt(t, v[6], v[7]) : std::array<float, 3>{1, 1, 1};
        for (std::size_t k = 0; k < 3; ++k) {
            out[i * 4 + k] = byteOf(weight * gradient[k] * light[k]);
        }
    }
    return out;
}

std::vector<std::uint8_t> fixedPaintWeights(const scene::PaintedTerrain& t, unsigned slice) {
    const std::size_t size = t.alphaSize, texels = size * size;
    std::vector<std::uint8_t> out(texels * 4, 255);
    const std::size_t paints = t.paints(), cells = std::size_t{t.cellsX} * t.cellsY;
    if (t.cellsX == 0 || t.cellsY == 0 || t.cellTiles == 0 || t.cellTextures.size() < paints * cells) {
        std::fill(out.begin(), out.end(), 0);
        return out;
    }
    for (std::size_t y = 0; y < size; ++y) {
        for (std::size_t x = 0; x < size; ++x) {
            // A texel is a tile wide, and the variant grid is cellTiles tiles to a cell.
            const std::size_t cx = std::min<std::size_t>(x / t.cellTiles, t.cellsX - 1), cy = std::min<std::size_t>(y / t.cellTiles, t.cellsY - 1);
            float weight = 0;
            for (std::size_t layer = 0; layer < paints; ++layer) {
                if (t.cellTextures[layer * cells + cy * t.cellsX + cx] == slice) {
                    weight += static_cast<float>(t.alphas[layer * texels + y * size + x]) / 255;
                }
            }
            const std::uint8_t b = byteOf(weight);
            std::uint8_t* at = &out[(y * size + x) * 4];
            at[0] = at[1] = at[2] = b;
        }
    }
    return out;
}

std::vector<float> fixedRoadVertices(const RoadMesh& roads, float lightMapSize) {
    const std::size_t count = roads.mesh.vertices.size() / kRoadFloats;
    std::vector<float> out(count * kFixedRoadFloats);
    for (std::size_t i = 0; i < count; ++i) {
        const float* in = &roads.mesh.vertices[i * kRoadFloats];
        float* v = &out[i * kFixedRoadFloats];
        std::copy_n(in, 5, v);  // position, texture coordinates
        v[5] = in[0] / lightMapSize;
        v[6] = in[1] / lightMapSize;
    }
    return out;
}

std::vector<std::uint8_t> fixedRoadColors(const RoadMesh& roads, const scene::PaintedTerrain& t, float lightMapSize) {
    const std::size_t count = roads.mesh.vertices.size() / kRoadFloats;
    std::vector<std::uint8_t> out(count * 4, 255);
    for (std::size_t i = 0; i < count; ++i) {
        const float* v = &roads.mesh.vertices[i * kRoadFloats];
        // A road vertex's place in the light map, which covers lightMapSize world units, in the grid's units.
        const auto light = lightAt(t, v[0] / lightMapSize * t.lightMapScale, v[1] / lightMapSize * t.lightMapScale);
        for (std::size_t k = 0; k < 3; ++k) {
            out[i * 4 + k] = byteOf(light[k]);
        }
    }
    return out;
}

std::array<float, 3> averageColor(const std::vector<scene::Image>& images) {
    double sum[3] = {};
    std::size_t taken = 0;
    for (const auto& image : images) {
        if (!complete(image)) {
            continue;
        }
        const std::size_t texels = std::size_t{image.width} * image.height, step = std::max<std::size_t>(1, texels / 4096);
        for (std::size_t i = 0; i < texels; i += step, ++taken) {
            for (std::size_t k = 0; k < 3; ++k) {
                sum[k] += image.rgba[i * 4 + k];
            }
        }
    }
    if (taken == 0) {
        return {1, 1, 1};
    }
    const auto n = static_cast<double>(taken) * 255;
    return {static_cast<float>(sum[0] / n), static_cast<float>(sum[1] / n), static_cast<float>(sum[2] / n)};
}

} // namespace ts2::render
