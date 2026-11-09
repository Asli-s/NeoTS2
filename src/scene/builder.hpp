#pragma once

#include "formats/cobject.hpp"
#include "formats/scenegraph.hpp"
#include "scene/assets.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
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
// A change made to a model's mesh data before it is built. `key` names it.
struct MeshEdit {
    std::string key;
    std::function<void(scenegraph::Mesh&)> apply;
};

class SceneBuilder {
public:
    explicit SceneBuilder(Assets& assets) : assets_(assets) {}

    // The mesh of a model with these material overrides, built on first use. -1 when it has nothing
    // to draw. Each shape is placed where its reference sits in the model's transform tree. With
    // `edit`, the model's mesh data is changed first, as skinnedModel() does. With `at`, the model is
    // the resource at that key, whatever its name, as a part's reference list names it. With `hair`,
    // the model is a Sim's hair part (see material()).
    int model(const std::string& name, const std::vector<scenegraph::MaterialOverride>& overrides, const MeshEdit* edit = nullptr,
              const scenegraph::Link* at = nullptr, bool hair = false);
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
    // With `edit`, the model's mesh data is changed first (a newborn's blended face). Models built
    // with different edits are kept apart by the edit's key.
    int skinnedModel(const std::string& name, const std::vector<scenegraph::MaterialOverride>& overrides,
                     const MeshEdit* edit = nullptr, const scenegraph::Link* at = nullptr, bool hair = false);
    // A model's shape with where it sits in the model, the joint it follows (its own node when named,
    // else the nearest named one above, empty for none) and where it sits from that joint.
    struct ModelShape {
        scenegraph::Shape shape;
        Transform place;
        std::string node;
        Transform fromNode;
    };
    // A model's shapes and its joints: by the index meshes skin to, each one's rest place in the
    // model, and the joints that nodes drawing shapes follow.
    struct ModelShapes {
        std::vector<ModelShape> shapes;
        std::map<int, std::string> joints;
        Pose rests;
        std::set<std::string> shapeJoints;
    };
    // What a buildModel call asks for, as its parts read it.
    struct ModelRequest {
        const std::string& name;
        const std::vector<scenegraph::MaterialOverride>& overrides;
        const Pose* pose;
        bool skinned;
        bool hair;
    };
    std::optional<ModelShapes> readModelShapes(const std::string& name, const Pose* places, const std::string& boneKey,
                                               const scenegraph::Link* at);
    std::optional<scenegraph::Mesh> readShapeMesh(const scenegraph::Shape& shape, std::size_t item, const std::string& name, const MeshEdit* edit);
    std::optional<Primitive> groupPrimitive(const ModelRequest& request, const ModelShapes& model, const ModelShape& placed,
                                            const scenegraph::Mesh& data, const scenegraph::Group& group, Mesh& mesh);
    int composedFace(const scenegraph::Material& m);
    void readMaps(const scenegraph::Material& m, bool deprecated, Material& out);

    // Where a named node of a model sits in it, once the model has been built.
    const Transform* bone(const std::string& model, const std::string& node) const;
    // The joint (lower case) that every shape of a built model hangs under, unskinned, so that the
    // whole drawn model moves with it (a dining chair's seat joint). Null for other models.
    const std::string* followJoint(const std::string& model) const;

    // A material by name, -1 when there is no such material. With `hair`, for a Sim's hair part: the
    // game draws that part's SimStandardMaterial materials as SimHair.
    int material(const std::string& name, bool hair = false);

    // A game material (by name) whose texture is drawn over other textures (`under`, bottom first),
    // as one image: a Sim's clothing over its skin. Known afterwards as `name`.
    // Textures in `over` are drawn over it, bottom first.
    int layeredMaterial(const std::string& name, const std::string& material, const std::vector<std::string>& under,
                        const std::vector<std::string>& over = {});

    // A plain colored material.
    int colorMaterial(const std::string& name, const std::array<float, 3>& color, bool doubleSided = false);
    // A plain see-through color (its alpha last), unlit, blended over what lies behind it, on draw
    // layer `layer`.
    int blendedColorMaterial(const std::string& name, const std::array<float, 4>& color, int layer);
    // A pool's water surface (see Material::poolSurface) waved by the bump map `bump`, on draw layer
    // `layer`, `tint` added to it (the game's pools add 0, 0.25, 0.5). `murk` (0 to 1, 0 in the game)
    // dims its mirror image and hides more of what is under it. -1 when the bump map is missing.
    int poolSurfaceMaterial(const std::string& name, const std::string& bump, const std::string& caustics, int layer,
                            const std::array<float, 3>& tint = {0, 0.25f, 0.5f}, float murk = 0);
    // The water in a lot's ponds (see Material::pondWater), on draw layer `layer`, its ripples the
    // normal maps of the bump maps `ripples`. -1 when its textures are missing.
    int pondWaterMaterial(const std::string& name, const std::string& cube, const std::array<std::string, 2>& ripples, int layer);
    // A pool wall's covering: the material `name`, lit by the pools' caustics. Returns the name to
    // find it by, or `name` when that material is missing.
    std::string poolWallMaterial(const std::string& name);
    // The pools' floor caustics alone, added over what is below: the ground under ponds drawn like
    // pools (not in the original game). Adds nothing when the pools' caustics aren't drawn.
    int causticsOverlayMaterial(const std::string& name);
    // A texture shown as it is, unlit, blended over what lies behind it by each vertex's alpha alone
    // (the texture's own alpha left out), on draw layer `layer`. -1 when the texture is missing.
    int blendedTextureMaterial(const std::string& texture, int layer);
    // The cube texture `cube` seen along the reflected view in the world, times `scale`, unlit,
    // `alpha` see-through, on draw layer `layer`. -1 when the cube is missing.
    int cubeReflectionMaterial(const std::string& name, const std::string& cube, const std::array<float, 3>& scale, float alpha,
                               int layer);
    // A beach lot's sea (see Material::skirtSea), rippled by the bump map `bump`, on draw layer
    // `layer`, with cubeReflectionMaterial's look where it can't be drawn. With `caustics`, the
    // flipbook the pools' caustics play from, for a sea drawn with them (not in the original game).
    // -1 when the bump map or the cube is missing.
    int skirtSeaMaterial(const std::string& name, const std::string& bump, const std::string& cube, const std::array<float, 3>& scale,
                         float alpha, int layer, const std::string& caustics = {});
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
    // over by the detail's alpha (see Material::terrain), tinted by `shade`. Canvas (opaque) or paint
    // (a layer).
    int terrainMaterial(const std::string& texture, int repeats, float shade, bool paint);
    // A wall covering with a door or window's wall mask cut out of it: alpha below half in the mask
    // is a hole (see Material::mask). `mirrored` when the mask runs right to left across the face. -1
    // when either is missing.
    int maskedMaterial(const std::string& covering, const std::string& mask, bool mirrored);
    // Each of the scene's images by what it was made from (a texture's name, or a name and how it was
    // made), empty for images made for this scene alone such as light maps. Builders on the same
    // assets make the same image from the same key, so scenes joined later can share them.
    std::vector<std::string> imageKeys() const;

    // Adds the collected surfaces as a mesh. -1 when there is nothing in them.
    int addMesh(const std::string& name, Surfaces surfaces);

    // Places a mesh and returns the instance, or nullptr when the mesh is -1.
    Instance* place(int mesh, const std::string& name, const Transform& transform, Part part = Part::Other, int level = 0);

    // Whether models' shadows on walls are drawn too (the neighborhood draws all of its trees' shadows).
    void setDrawShadows(bool draw) { drawShadows_ = draw; }
    // Whether lots' ponds are drawn like pools, with their surface, depth tint and caustics. Not in
    // the original game, which draws them with its waterOnTerrain material.
    void setPoolStylePonds(bool on) { poolStylePonds_ = on; }
    bool poolStylePonds() const { return poolStylePonds_; }
    // Whether the sea around a lot gets a colour that deepens with the water and the pools' caustics on
    // the ground under it, its surface left clear but for its reflection. Not in the original game.
    void setPoolStyleSea(bool on) { poolStyleSea_ = on; }
    bool poolStyleSea() const { return poolStyleSea_; }
    // Whether lots are joined to the ground around them without the game's seams: the skirt follows
    // the lot's edge between its tile corners, and the lot's paints reach its far edges, where saved
    // strength maps can hold 0. Not in the original game.
    void setSmoothLotSkirts(bool on) { smoothLotSkirts_ = on; }
    bool smoothLotSkirts() const { return smoothLotSkirts_; }
    // The neighborhood's water level in the lot's own heights, when the lot lies in a neighborhood
    // with water. With smooth lot skirts the lot's sea bed is rounded below it (see Heights::drawnGrid).
    void setLotSea(std::optional<float> z) { lotSea_ = z; }
    // The water level that rounds the lot's drawn sea bed: only with smooth lot skirts.
    std::optional<float> edgeSea() const { return smoothLotSkirts_ ? lotSea_ : std::nullopt; }

    std::size_t missingModels = 0, missingMaterials = 0;

    // The scene built so far.
    Scene& scene() { return scene_; }
    Assets& assets() { return assets_; }

private:
    // `places`: joints' places relative to their parents that replace the model's own (see
    // savedPoseModel), its bones then known as `boneKey`.
    int buildModel(const std::string& name, const std::vector<scenegraph::MaterialOverride>& overrides, const Pose* pose,
                   bool skinned = false, const Pose* places = nullptr, const std::string& boneKey = {},
                   const MeshEdit* edit = nullptr, const scenegraph::Link* at = nullptr, bool hair = false);
    int image(const std::string& texture);
    // A bump map's normal map (see Assets::normalMap), -1 when it is missing.
    int normalMap(const std::string& texture);
    // A wall covering's normal map, as the game gives walls one, -1 for none.
    int wallNormalMap(const scenegraph::Material& m);
    // Textures drawn one over another by their alpha, bottom first, at the smallest one's size, opaque.
    int layeredImage(const std::vector<std::string>& textures);
    // A wall covering's material with `maskTexture` as its wall mask (see maskedMaterial). -1 when
    // either is missing.
    int cutMaterial(const std::string& name, const std::string& covering, const std::string& maskTexture, bool mirrored);
    // A cube texture's index in Scene::envCubes, loaded once per name. -1 when it is missing or broken.
    int envCube(const std::string& texture);
    int addImage(Image image);
    int addMaterial(const std::string& key, Material m);

    Assets& assets_;
    Scene scene_;
    bool drawShadows_ = false;
    bool poolStylePonds_ = false;
    bool poolStyleSea_ = false;
    bool smoothLotSkirts_ = false;
    std::optional<float> lotSea_;
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
