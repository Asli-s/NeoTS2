#pragma once

#include <array>
#include <cstdint>
#include <optional>
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

// Layer: blended onto the opaque surface it lies on (same vertices), right after opaque surfaces
// and in the order placed, as terrain paint is.
enum class AlphaMode { Opaque, Mask, Blend, Layer };

// How a surface looks: a color, optionally times an image, and how its alpha is used.
struct Material {
    std::string name;
    std::array<float, 4> color{1, 1, 1, 1};
    int image = -1;  // index into Scene::images, -1 for none
    AlphaMode alpha = AlphaMode::Opaque;
    float alphaReference = 0.5f;  // Mask: drawn where alpha is at least this
    bool doubleSided = false;
    bool unlit = false;  // shown as its color and texture, without light
};

// Triangles sharing one material: per vertex a position, a normal, texture coordinates and,
// optionally, an alpha that multiplies the material's (1 when there are none).
struct Primitive {
    int material = -1;  // index into Scene::materials, -1 for none
    std::vector<float> positions, normals, uvs, alphas;
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

// A neighborhood's painted terrain, drawn as the game draws it: each paint's texture times its
// alpha map, added up. A vertex per tile corner, row by row; its texture coordinates are its
// column and row.
struct PaintedTerrain {
    std::uint32_t columns = 0, rows = 0;
    float tileSize = 10;
    std::vector<float> heights;          // per vertex
    std::vector<float> normals;          // x, y, z per vertex
    std::uint32_t alphaSize = 0;         // alpha maps are alphaSize square, a byte per texel
    std::vector<std::uint8_t> alphas;    // one map per paint, in paint order
    std::vector<Image> textures;         // every paint's every variant, all the same size
    float textureRepeat = 13;            // tiles a paint texture covers
    std::uint32_t cellTiles = 13, cellsX = 0, cellsY = 0;  // the variant grid
    std::vector<std::uint8_t> cellTextures;  // per paint, per cell (row by row): index into textures
    // The light map the painted terrain is multiplied by (a texel per tile), where the terrain is at
    // or above `litFrom` (the game's waterHeight); sampled at grid / lightMapScale.
    Image lightMap;
    float lightMapScale = 128;
    float litFrom = 312.45f;

    // The water (values from the game's "Shaders for neighborhood" script): a sheet at waterLevel
    // over the tiles with a corner under water, showing the reflection (sky and terrain mirrored in
    // it) twice, a little apart, mixed by the ripple map's alpha, tinted and see-through. Terrain under
    // the water is darkened by depth through the gradient.
    float waterLevel = 0;
    std::vector<std::uint8_t> waterTiles;  // per tile, row by row: 1 where the water sheet is
    Image ripples;                         // neighborhood-water-1
    Image underwaterGradient;              // neighborhood-underwater-gradient, top row at the surface
    std::vector<Image> sky;                // neighborhood-sky2-envcube's six faces, z up
    std::array<float, 3> reflectionTint{0.55f, 0.6f, 0.65f};  // nhoodWaterReflColour
    float reflectionStrength = 0.75f;      // nhoodWaterReflStrength (the sheet's opacity)
    float reflectionOffset = 0.002f;       // nhoodWaterReflOffset (the second sample's shift)
    float rippleScale = 7, rippleSpeed = 0.5f;  // nhoodWaterMap1Scale, nhoodWaterMap1Speed
    float gradientDepth = 40;              // waterGradientZoneSize

    // Roads: a quad per road tile, a little above the terrain, its texture blended over it and
    // multiplied by the light map. Corners (x0,y0) (x0,y1) (x1,y1) (x1,y0), split like the terrain.
    struct RoadQuad {
        std::array<std::array<float, 5>, 4> corners{};  // x, y, z, u, v
        std::uint32_t texture = 0;                      // index into roadTextures
    };
    std::vector<RoadQuad> roads;
    std::vector<Image> roadTextures;  // all the same size

    // The lots, each drawn as its imposter: flat models of its terrain, walls, object slices and
    // roofs, with textures rendered from the lot (so with alpha already multiplied in). As the
    // game's "Shaders for imposters" script draws them: terrain (its light map multiplied over the
    // neighborhood, then its texture over that) before the rest; walls and slices point-sampled and
    // alpha-tested (above 64 and 127); roofs opaque and two-sided.
    struct ImposterPart {
        enum class Kind : std::uint8_t { Terrain, Wall, Slice, Roof };
        Kind kind = Kind::Wall;
        std::uint32_t texture = 0;  // index into imposterTextures
        int lightMap = -1;          // terrain: its light map, index into imposterTextures; -1 for none
        std::array<float, 3> lightMapTint{1, 1, 1};
        std::vector<float> vertices;  // x, y, z in the world, u, v, then u, v of the second set
        std::vector<std::uint32_t> indices;
    };
    std::vector<ImposterPart> imposters;
    std::vector<Image> imposterTextures;
    std::size_t paints() const { return alphaSize ? alphas.size() / (static_cast<std::size_t>(alphaSize) * alphaSize) : 0; }
};

// Everything needed to draw a scene, independent of any graphics API.
struct Scene {
    std::vector<Image> images;
    std::vector<Material> materials;
    std::vector<Mesh> meshes;
    std::vector<Instance> instances;
    std::vector<WallInfo> walls;
    std::optional<PaintedTerrain> terrain;
};

} // namespace ts2::scene
