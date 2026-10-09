#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ts2::scene {

// A rigid transform as a 3x4 matrix (rotation, then translation). Game coordinates: x and y along
// the ground, z up.
struct Transform {
    std::array<float, 12> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

    // From a translation and a rotation quaternion (x, y, z, w).
    static Transform of(const std::array<float, 3>& t, const std::array<float, 4>& q);
    // At (x, y, z), turned about the vertical axis by `angle` radians.
    static Transform at(float x, float y, float z, float angle);

    Transform operator*(const Transform& b) const;

    // Transforms a point (with translation) or a direction (without) in place.
    void apply(float* v, bool point) const;
};

// A decoded image, 8-bit RGBA, rows from the top.
struct Image {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};

enum class AlphaMode { Opaque, Mask, Blend };

// How a surface looks: a color, optionally times an image, and how its alpha is used.
struct Material {
    std::string name;
    std::array<float, 4> color{1, 1, 1, 1};
    int image = -1;  // index into Scene::images, -1 for none
    AlphaMode alpha = AlphaMode::Opaque;
    bool doubleSided = false;
};

// Triangles sharing one material: per vertex a position, a normal and texture coordinates.
struct Primitive {
    int material = -1;  // index into Scene::materials, -1 for none
    std::vector<float> positions, normals, uvs;
    std::vector<std::uint32_t> indices;
};

struct Mesh {
    std::string name;
    std::vector<Primitive> primitives;
};

// A mesh placed in the world.
struct Instance {
    int mesh = -1;
    std::string name;
    Transform transform;
};

// Everything needed to draw a scene, independent of any graphics API.
struct Scene {
    std::vector<Image> images;
    std::vector<Material> materials;
    std::vector<Mesh> meshes;
    std::vector<Instance> instances;
};

} // namespace ts2::scene
