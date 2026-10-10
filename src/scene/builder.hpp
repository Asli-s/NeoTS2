#pragma once

#include "formats/scenegraph.hpp"
#include "scene/assets.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace ts2::scene {

// Triangles being collected for one mesh, by material index.
using Surfaces = std::map<int, Primitive>;

// Where a skeleton's joints are in a pose, in the model's space, by lowercase joint name.
using Pose = std::map<std::string, Transform>;

// Adds a vertex to a primitive, indexed in order.
void addVertex(Primitive& p, const std::array<float, 3>& position, const std::array<float, 3>& normal,
               const std::array<float, 2>& uv);
// Adds a flat quad (corners in order around it) facing along `facing`, wound so that it does.
void addQuad(Primitive& p, const std::array<std::array<float, 3>, 4>& corners, const std::array<float, 3>& facing,
             const std::array<std::array<float, 2>, 4>& uv);

// Builds a Scene from the game's assets: models, materials and textures are loaded once and shared
// by everything that uses them. Materials can be looked up by name, whether they come from the game
// (TXMT) or were made here (plain colors, texture cells, wall cutouts).
class SceneBuilder {
public:
    explicit SceneBuilder(Assets& assets) : assets_(assets) {}

    // The mesh of a model with these material overrides, built on first use; -1 when it has nothing
    // to draw. Each shape is placed where its reference sits in the model's transform tree.
    int model(const std::string& name, const std::vector<scenegraph::MaterialOverride>& overrides);
    // The same with its meshes skinned to a pose (named `poseName`, for sharing): each vertex follows
    // its joints, from the bind pose to where `pose` has them.
    int posedModel(const std::string& name, const std::vector<scenegraph::MaterialOverride>& overrides, const Pose& pose,
                   const std::string& poseName);

    // Where a named node of a model sits in it, once the model has been built.
    const Transform* bone(const std::string& model, const std::string& node) const;

    // A material by name; -1 when there is no such material.
    int material(const std::string& name);

    // A game material (by name) whose texture is drawn over other textures (`under`, bottom first),
    // as one image: a Sim's clothing over its skin. Known afterwards as `name`.
    int layeredMaterial(const std::string& name, const std::string& material, const std::vector<std::string>& under);

    // A plain colored material.
    int colorMaterial(const std::string& name, const std::array<float, 3>& color, bool doubleSided = false);
    // A texture shown as it is, tinted by `shade`.
    int textureMaterial(const std::string& texture, float shade = 1.0f, bool doubleSided = false);
    // One cell of a texture laid out as a grid (counted from the top left), resampled to 128 pixels
    // square so that the cell repeats on its own.
    int textureCellMaterial(const std::string& texture, int columns, int rows, int column, int row,
                            const std::array<float, 3>& color);
    // The part of a texture between fractions (u0, v0) and (u1, v1) of its size, resampled.
    int textureRegionMaterial(const std::string& texture, double u0, double v0, double u1, double v1, std::uint32_t width,
                              std::uint32_t height, const std::array<float, 3>& color);
    // A terrain texture with its detail texture ("<name>_detail", `repeats` times across it) laid
    // over by the detail's alpha, as one image; tinted by `shade`. Canvas (opaque) or paint (a layer).
    int terrainMaterial(const std::string& texture, int repeats, float shade, bool paint);
    // A wall covering with a door or window's wall mask cut out of it: alpha below half in the mask
    // is a hole. `mirrored` when the mask runs right to left across the face. -1 when either is missing.
    int maskedMaterial(const std::string& covering, const std::string& mask, bool mirrored);

    // Adds the collected surfaces as a mesh; -1 when there is nothing in them.
    int addMesh(const std::string& name, Surfaces surfaces);

    // Places a mesh; returns the instance, or nullptr when the mesh is -1.
    Instance* place(int mesh, const std::string& name, const Transform& transform, Part part = Part::Other, int level = 0);

    // Whether models' shadow groups are drawn (the neighborhood draws its trees' ground shadows).
    void setDrawShadows(bool draw) { drawShadows_ = draw; }

    std::size_t missingModels = 0, missingMaterials = 0;

    // The scene built so far.
    Scene& scene() { return scene_; }
    Assets& assets() { return assets_; }

private:
    int buildModel(const std::string& name, const std::vector<scenegraph::MaterialOverride>& overrides, const Pose* pose);
    int image(const std::string& texture);
    // Textures drawn one over another by their alpha, bottom first, at the first one's size; opaque.
    int layeredImage(const std::vector<std::string>& textures);
    // A material's texture with alpha below half in `cut` as holes (see maskedMaterial); -1 when
    // either is missing.
    int cutMaterial(const std::string& name, const std::string& covering, const std::optional<Image>& cut, bool mirrored);
    int addImage(Image image);
    int addMaterial(const std::string& key, Material m);

    Assets& assets_;
    Scene scene_;
    bool drawShadows_ = false;
    std::map<std::string, int> models_, materials_, images_;
    std::map<std::string, std::map<std::string, Transform>> bones_;
    std::map<std::string, std::string> baseTextures_;  // game materials' base textures, by material key
};

} // namespace ts2::scene
