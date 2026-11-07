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

// Four joints, four weights, the vertex's place in the morph data (kNoMorphVertex for none) and its
// eye coordinates (see scene::Primitive::eyeUvs, 0 for none).
constexpr std::uint32_t kSkinBytes = 4 + 4 * sizeof(float) + sizeof(std::uint32_t) + 2 * sizeof(float);
constexpr std::uint32_t kSkinEyeOffset = 4 + 4 * sizeof(float) + sizeof(std::uint32_t);
constexpr std::uint32_t kNoMorphVertex = 0xFFFFFFFF;
// A morphing vertex's floats in the morph data: per stream its position delta and the morph it takes
// (x, y, z, morph), then per stream its normal delta (x, y, z, 0).
constexpr std::uint32_t kMorphVertexFloats = 2 * scene::Primitive::kMorphStreams * 4;

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
    bool shadowGroup = false;  // one of the model's ground or wall shadow groups (see scene::objectshadow)
    int alphaSort = -1;  // see scene::Primitive::alphaSort
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
    std::vector<float> morphs;  // the morphing vertices' deltas (see kMorphVertexFloats)
    bool tooBig = false;  // some primitives were left out (a buffer's size is 32-bit)
};

// Appends a mesh's primitives to `data`, the set of mesh buffers numbered `buffers`.
GpuMesh packMesh(const scene::Mesh& mesh, std::size_t materials, std::uint32_t buffers, MeshData& data);

} // namespace ts2::render
