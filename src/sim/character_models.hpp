#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The scenegraph records of a new Sim's face in its character file, one set per age: its shape
// (SHPE) and the near and far-away geometry nodes (GMND) it draws. Each record's key is the hash
// of its name, in the file's own group.
namespace ts2::sim {

// A name in a character file's own group as the game writes it: "#0x<group>!<name>". The game puts
// the number it gave the open file that run (0x6F000000 and a count). Later runs give the file other
// numbers and still load the names, so any number of that kind works.
std::string ownGroupName(std::uint32_t group, std::string_view name);

// The catalog age flag of the base face for character file age number `age` (0 toddler, 1 child,
// 2 teen, 3 adult, 4 elder, 5 baby, 7 young adult). A young adult has the adult's face, as the face
// shape has no young adult face. 0 for other numbers.
std::uint32_t baseFaceAgeFlag(int age);

// A face geometry node: a copy of the catalog's base face node `catalogNode` that names the new
// Sim's own mesh ("age<N>_0_gmdc", with LOD15 before "_gmdc" for the far-away one) and is named
// "age<N>_0_gmnd" (or "age<N>_0LOD15_gmnd") in `group`. The game writes the node's data lists after
// it in the opposite order (face, tsDesignModeEnabled, tsNoShadow in every face node, which sorting
// by name would give too). Throws std::runtime_error when the catalog node is not one geometry
// node, a mesh link and data lists.
std::vector<std::uint8_t> newFaceNode(std::span<const std::uint8_t> catalogNode, int age, bool far, std::uint32_t group);

// A face shape: the near node at level of detail 0 and the far-away one at 15, its one mesh group
// "face" drawn with material `material`, named "age<N>_00_shpe" in `group`.
std::vector<std::uint8_t> newFaceShape(int age, std::uint32_t group, std::string_view material);

// A face mesh: a copy of the catalog's base face mesh `catalogMesh` named "age<N>_0_gmdc" (or
// "age<N>_0LOD15_gmdc") in `group`, its face's positions and normals replaced by `positions` and
// `normals` (three floats per vertex). Everything else, tangents too, stays as the catalog has it.
// Throws std::runtime_error when the catalog mesh isn't one mesh whose first triangle group draws
// from positions and normals of that many vertices.
std::vector<std::uint8_t> newFaceMesh(std::span<const std::uint8_t> catalogMesh, int age, bool far, std::uint32_t group,
                                      std::span<const float> positions, std::span<const float> normals);

// The name of a new Sim's face material at age number `age`:
// "ss_customPart_0x<counter>_<age>_<paramGender>_cmpm", with 0 for a male and 1 for a female. The game
// counts with time for the counter, one more for each age's material in turn.
std::string faceMaterialName(std::uint32_t counter, int age, std::uint32_t paramGender);

// An overlay's texture and its drawing order, lowest first.
struct FaceTexture {
    std::string texture;
    std::uint32_t layer = 0;
};

// What a new Sim's face material at one age shows: its name (faceMaterialName), the textures of its
// eye colour and eyebrow overlays (empty for none), the GUIDs of its eye colour, eyebrow family,
// hair tone and skin tone, its age number and gender (0 male, 1 female).
struct FaceMaterialLook {
    std::string name;
    FaceTexture eye, brow;
    std::string eyeColour, browFamily, hairTone, skinTone;
    // Facial hair, after the eye colour and eyebrows in the overlay list, each family with its texture.
    std::vector<std::string> facialHairFamilies;
    std::vector<FaceTexture> facialHair;
    // Mesh overlays (glasses) last in the overlay list, with no texture.
    std::vector<std::string> meshOverlayFamilies;
    int age = 0;
    std::uint32_t paramGender = 0;
    // A made Sim's material lists its overlays eyebrows first, a newborn's eye colour first. The
    // textures go over the base texture by layer, lowest first, those of one layer in the list's order.
    bool browFirst = false;
};

// A face material (TXMT) "<name>_txmt" in `group`: a copy of the catalog base face material
// `catalogMaterial` that the game composites with the eye, eyebrow and facial hair overlays (SimSkin). Its
// properties gain the composite's textures, overlays, tones, age, gender and species, kept in key
// order, and its textures are the composite and the base texture. Throws std::runtime_error when
// the catalog material isn't one material.
std::vector<std::uint8_t> newFaceMaterial(std::span<const std::uint8_t> catalogMaterial, std::uint32_t group, const FaceMaterialLook& look);

// A mesh's morphs as a model's shape reference names them, "group::name", without the empty one.
std::vector<std::string> meshMorphs(std::span<const std::uint8_t> mesh);

// The catalog skeleton model a new Sim's face model of age number `age` is built on:
// "buSkel_cres" for a baby, "puSkel_cres" toddler, "cuSkel_cres" child, "auSkel_cres" from teen on.
// Empty for other numbers.
std::string skeletonModelName(int age);

// Where a grip (glasses_grip, mouth_grip) sits on a new Sim's face, a bone adjustment of its model.
struct GripAdjust {
    std::string bone;
    std::array<float, 3> translation{};
    std::array<float, 4> rotation{};  // quaternion x, y, z, w
};

// A face model (CRES) "age<N>_0_cres" in `group`: the age's skeleton model `skeleton` with a shape
// reference drawing the face shape ("age<N>_00_shpe"), its morphs `morphs` ("group::name") all at
// weight 0, and a "bone_adjust" data list per grip, in the order given. The game writes every child
// and extension list in the opposite order to the catalog file, the grips' lists first among the
// model's extensions, and the shape reference after the top joint's children, which it lists from
// head_ikctr on when the joint has one. Blocks follow breadth first from the model, each node's
// extensions before its children. Bone data is written at version 5, and the model's last number is
// a face model's. A teen's upper arms' bone data gets 1 as its third word. Throws std::runtime_error for
// a skeleton that isn't a model whose one child is its top joint, or with blocks it doesn't reach.
// A mesh overlay's model, part `part` from 1, is built the same way with no grips and its own mesh's
// morphs, named "age<N>_<part>_cres" and drawing its shape "age<N>_<part>0_shpe". Its shape
// reference has no data.
std::vector<std::uint8_t> newFaceModel(std::span<const std::uint8_t> skeleton, int age, std::uint32_t group,
                                       std::span<const GripAdjust> grips, std::span<const std::string> morphs, int part = 0);

// A mesh overlay (glasses) the Sim wears as part `part` (from 1) of its look at age number `age`:
// its geometry node, a copy of the catalog's `catalogNode` named "age<N>_<part>_gmnd" in `group` and
// naming its own mesh, with its data lists in the opposite order as a face node's. The node's
// references to its lists stay as the catalog's, so they name the lists in the other order, as in
// the game's own files.
std::vector<std::uint8_t> newOverlayNode(std::span<const std::uint8_t> catalogNode, int age, int part, std::uint32_t group);

// Its mesh: a copy of the catalog's `catalogMesh` named "age<N>_<part>_gmdc" in `group`, the
// positions of every vertex list it draws from replaced by `positions` (three floats per vertex, the
// lists in the mesh's order). Throws std::runtime_error when the count doesn't fit.
std::vector<std::uint8_t> newOverlayMesh(std::span<const std::uint8_t> catalogMesh, int age, int part, std::uint32_t group,
                                         std::span<const float> positions);

// Its shape "age<N>_<part>0_shpe" in `group`: its node at level of detail 0 and each mesh group
// (subset) drawn with its material, in the order given.
struct OverlaySubset {
    std::string subset, material;
};
std::vector<std::uint8_t> newOverlayShape(int age, int part, std::uint32_t group, std::span<const OverlaySubset> subsets);

// One of its materials: a copy of the catalog material `catalogMaterial` named "<name>_txmt" in
// `group`, its properties in key order with the species number (0 human) and drawn blended as
// hair is (simHairFlag), as the game writes a mesh overlay's materials. Throws std::runtime_error
// when it isn't one material.
std::vector<std::uint8_t> newOverlayMaterial(std::span<const std::uint8_t> catalogMaterial, std::uint32_t group, std::string_view name,
                                             int species = 0);

} // namespace ts2::sim
