#include "render/mesh_pack.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ts2::render {

std::optional<std::array<float, 4>> flatPlane(const scene::Primitive& p) {
    const std::size_t n = p.positions.size() / 3;
    const auto at = [&](std::uint32_t k) { return std::array<double, 3>{p.positions[k * 3], p.positions[k * 3 + 1], p.positions[k * 3 + 2]}; };
    // The triangles' cross products summed: a normal weighted by area, and their corners' middle.
    std::array<double, 3> normal{}, middle{};
    double area = 0;
    for (std::size_t t = 0; t + 2 < p.indices.size(); t += 3) {
        if (p.indices[t] >= n || p.indices[t + 1] >= n || p.indices[t + 2] >= n) {
            return std::nullopt;
        }
        const auto a = at(p.indices[t]), b = at(p.indices[t + 1]), c = at(p.indices[t + 2]);
        const std::array<double, 3> u{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const std::array<double, 3> cross{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        const double twice = std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
        for (std::size_t k = 0; k < 3; ++k) {
            normal[k] += cross[k];
            middle[k] += twice * (a[k] + b[k] + c[k]) / 3;
        }
        area += twice;
    }
    const double length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    if (!(area > 1e-12) || !(length > 1e-6 * area) || !std::isfinite(length)) {
        return std::nullopt;
    }
    for (std::size_t k = 0; k < 3; ++k) {
        normal[k] /= length;
        middle[k] /= area;
    }
    const double offset = -(normal[0] * middle[0] + normal[1] * middle[1] + normal[2] * middle[2]);
    // A millimetre off its plane is still flat. A bent surface would mirror two ways at once.
    constexpr double kFlat = 0.001;
    for (std::size_t k = 0; k < n; ++k) {
        const auto q = at(static_cast<std::uint32_t>(k));
        if (std::abs(normal[0] * q[0] + normal[1] * q[1] + normal[2] * q[2] + offset) > kFlat) {
            return std::nullopt;
        }
    }
    return std::array<float, 4>{static_cast<float>(normal[0]), static_cast<float>(normal[1]), static_cast<float>(normal[2]), static_cast<float>(offset)};
}

void packVertex(const scene::Primitive& p, std::size_t i, float* v) {
    const std::size_t n = p.positions.size() / 3;
    std::fill_n(v, kVertexFloats, 0.0f);
    std::copy_n(&p.positions[i * 3], 3, v);
    if (p.normals.size() == p.positions.size()) {
        std::copy_n(&p.normals[i * 3], 3, v + 3);
    } else {
        v[5] = 1;
    }
    if (p.uvs.size() == n * 2) {
        std::copy_n(&p.uvs[i * 2], 2, v + 6);
    }
    v[8] = p.alphas.size() == n ? p.alphas[i] : 1.0f;
    if (p.lightUvs.size() == n * 2) {
        std::copy_n(&p.lightUvs[i * 2], 2, v + 9);
    }
    if (p.colors.size() == n * 3) {
        std::copy_n(&p.colors[i * 3], 3, v + 11);
    } else {
        std::fill_n(v + 11, 3, 1.0f);
    }
    if (p.tangents.size() == n * 3) {
        std::copy_n(&p.tangents[i * 3], 3, v + 14);
    }
}

GpuMesh packMesh(const scene::Mesh& mesh, std::span<const scene::Material> materials, std::uint32_t buffers, MeshData& data) {
    GpuMesh gm;
    for (const auto& p : mesh.primitives) {
        const std::size_t n = p.positions.size() / 3;
        if (n == 0 || p.indices.empty()) {
            continue;
        }
        // The scene builders check their indices. This keeps the GPU from ever reading past the
        // vertices should one be missed.
        if (std::any_of(p.indices.begin(), p.indices.end(), [n](std::uint32_t k) { return k >= n; })) {
            continue;
        }
        const bool skinned = p.joints.size() == n * 4 && p.weights.size() == n * 4;
        // A GPU buffer's size is 32-bit: a scene too big for that leaves out the rest of its primitives.
        constexpr std::size_t kMaxBytes = 0x7FFFFFFF;
        if ((data.vertices.size() + n * kVertexFloats) * sizeof(float) > kMaxBytes ||
            (data.indices.size() + p.indices.size()) * sizeof(std::uint32_t) > kMaxBytes || (data.skin.size() + n * kSkinBytes) > kMaxBytes ||
            (data.morphs.size() + n * kMorphVertexFloats) * sizeof(float) > kMaxBytes) {
            data.tooBig = true;
            continue;
        }
        GpuPrimitive g;
        g.buffers = buffers;
        g.firstVertex = static_cast<std::uint32_t>(data.vertices.size() / kVertexFloats);
        g.firstIndex = static_cast<std::uint32_t>(data.indices.size());
        data.vertices.resize(data.vertices.size() + n * kVertexFloats, 0.0f);
        for (std::size_t i = 0; i < n; ++i) {
            float* v = &data.vertices[(g.firstVertex + i) * kVertexFloats];
            packVertex(p, i, v);
        }
        data.indices.insert(data.indices.end(), p.indices.begin(), p.indices.end());
        g.count = static_cast<unsigned>(p.indices.size());
        g.vertices = static_cast<unsigned>(n);
        g.material = p.material >= 0 && static_cast<std::size_t>(p.material) < materials.size() ? p.material : -1;
        g.lightMapped = p.lightUvs.size() == n * 2;
        g.tangents = p.tangents.size() == n * 3 && p.uvs.size() == n * 2;
        g.source = static_cast<std::size_t>(&p - mesh.primitives.data());
        g.shadowGroup = scene::objectshadow::isShadowGroup(p.group);
        g.alphaSort = p.alphaSort;
        g.low = {p.positions[0], p.positions[1], p.positions[2]};
        g.high = g.low;
        for (std::size_t i = 1; i < n; ++i) {
            for (std::size_t k = 0; k < 3; ++k) {
                g.low[k] = std::min(g.low[k], p.positions[i * 3 + k]);
                g.high[k] = std::max(g.high[k], p.positions[i * 3 + k]);
            }
        }
        if (skinned) {
            g.skinned = true;
            g.firstSkin = static_cast<std::uint32_t>(data.skin.size());
            data.skin.resize(data.skin.size() + n * kSkinBytes);
            constexpr std::size_t streams = scene::Primitive::kMorphStreams;
            const bool morphed = p.morphs.size() == n * streams && p.positionDeltas.size() == n * streams * 3 &&
                                 p.normalDeltas.size() == n * streams * 3 && !mesh.morphs.empty();
            for (std::size_t i = 0; i < n; ++i) {
                std::uint8_t* at = &data.skin[g.firstSkin + i * kSkinBytes];
                std::memcpy(at, &p.joints[i * 4], 4);
                std::memcpy(at + 4, &p.weights[i * 4], 4 * sizeof(float));
                // Only vertices a morph moves get morph data.
                std::uint32_t morphVertex = kNoMorphVertex;
                const bool moves = morphed && std::any_of(&p.morphs[i * streams], &p.morphs[i * streams] + streams, [&](std::uint8_t m) {
                    return m < mesh.morphs.size();
                });
                if (moves && data.morphs.size() / kMorphVertexFloats < kNoMorphVertex) {
                    morphVertex = static_cast<std::uint32_t>(data.morphs.size() / kMorphVertexFloats);
                    for (std::size_t k = 0; k < streams; ++k) {
                        const std::size_t s = i * streams + k;
                        const bool used = p.morphs[s] < mesh.morphs.size();
                        data.morphs.insert(data.morphs.end(), {used ? p.positionDeltas[s * 3] : 0.0f, used ? p.positionDeltas[s * 3 + 1] : 0.0f,
                                                               used ? p.positionDeltas[s * 3 + 2] : 0.0f, used ? static_cast<float>(p.morphs[s]) : 0.0f});
                    }
                    for (std::size_t k = 0; k < streams; ++k) {
                        const std::size_t s = i * streams + k;
                        const bool used = p.morphs[s] < mesh.morphs.size();
                        data.morphs.insert(data.morphs.end(), {used ? p.normalDeltas[s * 3] : 0.0f, used ? p.normalDeltas[s * 3 + 1] : 0.0f,
                                                               used ? p.normalDeltas[s * 3 + 2] : 0.0f, 0.0f});
                    }
                }
                std::memcpy(at + 4 + 4 * sizeof(float), &morphVertex, sizeof morphVertex);
                if (p.eyeUvs.size() == n * 2) {
                    std::memcpy(at + kSkinEyeOffset, &p.eyeUvs[i * 2], 2 * sizeof(float));
                }
            }
        }
        const auto finite = [](const std::array<float, 3>& a) { return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]); };
        g.hasBox = finite(g.low) && finite(g.high);
        if (g.material >= 0 && materials[static_cast<std::size_t>(g.material)].mirror && !g.skinned) {
            if (const auto plane = p.mirrorPlane ? p.mirrorPlane : flatPlane(p)) {
                g.mirror = true;
                g.plane = *plane;
            }
        }
        g.cullable = !g.skinned && g.hasBox;
        if (g.cullable) {
            for (std::size_t k = 0; k < 3; ++k) {
                gm.low[k] = gm.anyCullable ? std::min(gm.low[k], g.low[k]) : g.low[k];
                gm.high[k] = gm.anyCullable ? std::max(gm.high[k], g.high[k]) : g.high[k];
            }
            gm.anyCullable = true;
        }
        gm.primitives.push_back(g);
    }
    return gm;
}

} // namespace ts2::render
