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

// What a placed mesh is, for choosing what a view shows.
enum class Part : std::uint8_t { Other, Object, Floor, Roof, Wall, WallHub };

// A mesh placed in the world.
struct Instance {
    int mesh = -1;
    std::string name;
    Transform transform;
    Part part = Part::Other;
    int level = 0;  // the storey it is on, as the lot's height grid counts them
    // Walls and hubs come in a full and a cut-down version (`cut`). `walls` (indexes into
    // Scene::walls) are a wall's own, the walls meeting at a hub, or those a door or window sits in.
    bool cut = false;
    std::vector<int> walls;
};

// A wall as views need it: where it runs, whether there is a room (not outdoors) on each side
// (the left side faces (dy, -dx)), and whether it is ever cut down.
struct WallInfo {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int level = 0;
    bool roomLeft = false, roomRight = false;
    bool cuttable = true;
};

// Everything needed to draw a scene, independent of any graphics API.
struct Scene {
    std::vector<Image> images;
    std::vector<Material> materials;
    std::vector<Mesh> meshes;
    std::vector<Instance> instances;
    std::vector<WallInfo> walls;
};

} // namespace ts2::scene
