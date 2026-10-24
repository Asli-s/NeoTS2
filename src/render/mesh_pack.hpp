// Meshes laid out for the GPU: the vertex, index and skin data every renderer uploads, and where each
// primitive lies in it. Nothing here depends on a graphics API.
#pragma once

#include "scene/scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ts2::render {

constexpr std::uint32_t kVertexFloats = 17;  // position, normal, texture coordinates, alpha, light-map coordinates, color, tangent

constexpr std::uint32_t kSkinBytes = 4 + 4 * sizeof(float);  // four joints, four weights

// Where a primitive lies in its set of mesh buffers (see MeshData). Meshes share a few buffers,
// because binding a different buffer for every draw is slow.
struct GpuPrimitive {
    std::uint32_t buffers = 0;  // index into the scene's mesh buffer sets
    std::uint32_t firstVertex = 0;
    std::uint32_t firstIndex = 0;
    std::uint32_t firstSkin = 0;  // in the set's skin data, for skinned primitives
    bool skinned = false;         // has joints and weights per vertex
    unsigned count = 0;     // indices
    unsigned vertices = 0;  // vertices, which the indices count from 0
    int material = -1;
    bool lightMapped = false;  // has light-map coordinates
    bool tangents = false;     // has tangents (and texture coordinates), for normal maps
    std::size_t source = 0;    // its index among the scene mesh's primitives
    // The box around its vertices in model space. Skinned primitives move out of it, so they are
    // never culled.
    std::array<float, 3> low{}, high{};
    bool hasBox = false;  // the box is finite
    bool cullable = false;
};

struct GpuMesh {
    std::vector<GpuPrimitive> primitives;
    // The box around its cullable primitives' boxes, tested first so most primitives need no test.
    std::array<float, 3> low{}, high{};
    bool anyCullable = false;
};

// Meshes laid out for one set of mesh buffers.
struct MeshData {
    std::vector<float> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<std::uint8_t> skin;
    bool tooBig = false;  // some primitives were left out (a buffer's size is 32-bit)
};

// Makes room in `data` for all the meshes from index `first` on, so that packing them doesn't grow its arrays again and
// again (each growth holds the old array and the new together, which at a lot's size is the peak of its load).
void reserveMeshData(const std::vector<scene::Mesh>& meshes, std::size_t first, MeshData& data);

// Appends a mesh's primitives to `data`, the set of mesh buffers numbered `buffers`.
GpuMesh packMesh(const scene::Mesh& mesh, std::size_t materials, std::uint32_t buffers, MeshData& data);

} // namespace ts2::render
