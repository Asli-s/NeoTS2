#include "render/mesh_pack.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ts2::render {

GpuMesh packMesh(const scene::Mesh& mesh, std::size_t materials, std::uint32_t buffers, MeshData& data) {
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
            (data.indices.size() + p.indices.size()) * sizeof(std::uint32_t) > kMaxBytes || (data.skin.size() + n * kSkinBytes) > kMaxBytes) {
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
        data.indices.insert(data.indices.end(), p.indices.begin(), p.indices.end());
        g.count = static_cast<unsigned>(p.indices.size());
        g.vertices = static_cast<unsigned>(n);
        g.material = p.material >= 0 && static_cast<std::size_t>(p.material) < materials ? p.material : -1;
        g.lightMapped = p.lightUvs.size() == n * 2;
        g.tangents = p.tangents.size() == n * 3 && p.uvs.size() == n * 2;
        g.source = static_cast<std::size_t>(&p - mesh.primitives.data());
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
            for (std::size_t i = 0; i < n; ++i) {
                std::memcpy(&data.skin[g.firstSkin + i * kSkinBytes], &p.joints[i * 4], 4);
                std::memcpy(&data.skin[g.firstSkin + i * kSkinBytes + 4], &p.weights[i * 4], 4 * sizeof(float));
            }
        }
        const auto finite = [](const std::array<float, 3>& a) { return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]); };
        g.hasBox = finite(g.low) && finite(g.high);
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
