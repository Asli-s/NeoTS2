#pragma once

#include "formats/cobject.hpp"
#include "formats/scenegraph.hpp"
#include "scene/assets.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ts2::scene {

// Triangles being collected for one mesh, by material index.
using Surfaces = std::map<int, Primitive>;

// Where a skeleton's joints are in a pose, in the model's space, by lowercase joint name.
using Pose = std::map<std::string, Transform>;

// Whether a model's group is one of the game's shadows on nearby walls, which it shows or hides by
// the walls around the object (not worked out yet: they stay hidden on lots). Its ground shadow and
// shadows under other names are ordinary parts of the model.
bool isWallShadowGroup(const std::string& group);
// A standard material's environment cube reflection: its cube texture's name, the colour scale
// (stdMatEnvCubeCoef) and whether the cube turns with the camera (stdMatEnvCubeLockedToCamera).
struct EnvCubeSettings {
    std::string texture;
    std::array<float, 3> coef{0, 0, 0};
    bool lockedToCamera = false;
};
// The material's reflection, as the game's standard material turns it on: a mode other than
// "none", a cube named and a coefficient whose parts add to at least 0.003. Nothing otherwise.
std::optional<EnvCubeSettings> envCubeSettings(const scenegraph::Material& m);
// A lit standard material's specular colour scale (stdMatSpecCoef) and power, the game's
// stdMatSpecPower times 0.4. The game draws none unless the power is above 1 and the scale's parts
// add to more than 0.3.
struct SpecularSettings {
    std::array<float, 3> coef{0, 0, 0};
    float power = 0;
};
std::optional<SpecularSettings> specularSettings(const scenegraph::Material& m);
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

    // The mesh of a model with these material overrides, built on first use. -1 when it has nothing
    // to draw. Each shape is placed where its reference sits in the model's transform tree.
    int model(const std::string& name, const std::vector<scenegraph::MaterialOverride>& overrides);
    // The same with its meshes skinned to a pose (named `poseName`, for sharing): each vertex follows
    // its joints, from the bind pose to where `pose` has them.
    int posedModel(const std::string& name, const std::vector<scenegraph::MaterialOverride>& overrides, const Pose& pose,
                   const std::string& poseName);

    // The same with its joints placed as a lot saved them: each saved joint's place relative to its
    // parent replaces the model's own, and the shapes under it move along (a dining chair pushed under
    // a table). Skinned vertices stay as model() leaves them. `boneKey` gets the name bone() knows the
    // result by, which is the model's own name when no joint moved.
    int savedPoseModel(const std::string& name, const std::vector<scenegraph::MaterialOverride>& overrides,
                       std::span<const cobject::SavedJoint> joints, std::string& boneKey);

    // The same left in its bind pose, skinned to be posed when drawn (see Primitive and Mesh::skin).
    // Groups without skinning data are placed as model() places them.
    int skinnedModel(const std::string& name, const std::vector<scenegraph::MaterialOverride>& overrides);

    // Where a named node of a model sits in it, once the model has been built.
    const Transform* bone(const std::string& model, const std::string& node) const;
    // The joint (lower case) that every shape of a built model hangs under, unskinned, so that the
    // whole drawn model moves with it (a dining chair's seat joint). Null for other models.
    const std::string* followJoint(const std::string& model) const;

    // A material by name, -1 when there is no such material.
    int material(const std::string& name);

    // A game material (by name) whose texture is drawn over other textures (`under`, bottom first),
    // as one image: a Sim's clothing over its skin. Known afterwards as `name`.
    int layeredMaterial(const std::string& name, const std::string& material, const std::vector<std::string>& under);

    // A plain colored material.
    int colorMaterial(const std::string& name, const std::array<float, 3>& color, bool doubleSided = false);
    // A plain see-through color (its alpha last), unlit, blended over what lies behind it, on draw
    // layer `layer`.
    int blendedColorMaterial(const std::string& name, const std::array<float, 4>& color, int layer);
    // A particle sprite as the game's StandardParticlesMaterial draws it: the texture (clamped)
    // times `color`, unlit, from both sides, added over what lies behind it or blended, on draw
    // layer `layer`. -1 when the texture is missing.
    int particleMaterial(const std::string& texture, const std::array<float, 4>& color, bool added, int layer);
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
    // over by the detail's alpha, as one image, tinted by `shade`. Canvas (opaque) or paint (a layer).
    int terrainMaterial(const std::string& texture, int repeats, float shade, bool paint);
    // A wall covering with a door or window's wall mask cut out of it: alpha below half in the mask
    // is a hole. `mirrored` when the mask runs right to left across the face. -1 when either is missing.
    int maskedMaterial(const std::string& covering, const std::string& mask, bool mirrored);

    // Adds the collected surfaces as a mesh. -1 when there is nothing in them.
    int addMesh(const std::string& name, Surfaces surfaces);

    // Places a mesh and returns the instance, or nullptr when the mesh is -1.
    Instance* place(int mesh, const std::string& name, const Transform& transform, Part part = Part::Other, int level = 0);

    // Whether models' shadows on walls are drawn too (the neighborhood draws all of its trees' shadows).
    void setDrawShadows(bool draw) { drawShadows_ = draw; }

    std::size_t missingModels = 0, missingMaterials = 0;

    // The scene built so far.
    Scene& scene() { return scene_; }
    Assets& assets() { return assets_; }

private:
    // `places`: joints' places relative to their parents that replace the model's own (see
    // savedPoseModel), its bones then known as `boneKey`.
    int buildModel(const std::string& name, const std::vector<scenegraph::MaterialOverride>& overrides, const Pose* pose,
                   bool skinned = false, const Pose* places = nullptr, const std::string& boneKey = {});
    int image(const std::string& texture);
    // A bump map's normal map (see Assets::normalMap), -1 when it is missing.
    int normalMap(const std::string& texture);
    // A wall covering's normal map, as the game gives walls one, -1 for none.
    int wallNormalMap(const scenegraph::Material& m);
    // Textures drawn one over another by their alpha, bottom first, at the smallest one's size, opaque.
    int layeredImage(const std::vector<std::string>& textures);
    // A material's texture with alpha below half in `cut` as holes (see maskedMaterial). -1 when
    // either is missing.
    int cutMaterial(const std::string& name, const std::string& covering, const std::optional<Image>& cut, bool mirrored);
    // A cube texture's index in Scene::envCubes, loaded once per name. -1 when it is missing or broken.
    int envCube(const std::string& texture);
    int addImage(Image image);
    int addMaterial(const std::string& key, Material m);

    Assets& assets_;
    Scene scene_;
    bool drawShadows_ = false;
    std::map<std::string, int> models_, materials_, images_, envCubes_;
    std::map<std::string, std::map<std::string, Transform>> bones_;
    std::map<std::string, std::string> followJoints_;  // see followJoint, by the name bone() takes
    // Each model's joints at rest relative to their parents, by lower-case model and joint name, read once.
    struct RestJoint {
        std::array<float, 3> translation{};
        std::array<float, 4> rotation{0, 0, 0, 1};
    };
    std::map<std::string, std::map<std::string, RestJoint>> restJoints_;
    std::map<std::string, std::string> baseTextures_;  // game materials' base textures, by material key
};

} // namespace ts2::scene
