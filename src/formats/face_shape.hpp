#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ts2::faceshape {

constexpr std::uint32_t kType = 0xCCCEF852;

// One point of a face shape: how far a corner of the base face's triangles is moved from the base
// face, and the texture coordinates that name the corner.
struct Point {
    std::array<float, 3> offset{};
    std::array<float, 2> uv{};
};

// A Sim's face shape (LxNR, in its character file): for each age, gender and face mesh, the points
// of every triangle corner of that age's base face, in the order of its indices.
struct FaceShape {
    std::uint32_t version = 0;
    struct Mesh {
        std::uint8_t age = 0;     // 0 toddler, 1 child, 2 teen, 3 adult, 4 elder, 5 baby
        std::uint8_t gender = 0;  // 0 or 1
        std::uint8_t group = 0;   // 0 the face, 15 its far-away mesh
        std::vector<Point> points;
    };
    std::vector<Mesh> meshes;

    const Mesh* find(std::uint8_t age, std::uint8_t gender, std::uint8_t group) const;
};

// Versions 3 to 5 are read. The baby meshes of versions 3 and 4 move nothing, as the game takes those
// faces as the base face. Version 3's far-away face of an elder of gender 1 is left out, as the game
// leaves it.
// Throws on other versions and broken data, points that aren't finite numbers among it.
FaceShape parse(std::span<const std::uint8_t> data);

// A face shape record as the game writes it, at version 5 with the meshes in the order given. Each
// offset and texture coordinate pair goes into the pool as the first entry within 0.0001 on every
// axis, or is added after the last. Throws std::runtime_error when the pool or a mesh needs more
// entries than the record's 16-bit counts hold.
std::vector<std::uint8_t> write(const FaceShape& shape);

} // namespace ts2::faceshape
