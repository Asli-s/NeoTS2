#pragma once

#include "formats/face_shape.hpp"
#include "scene/assets.hpp"
#include "scene/face_blend.hpp"
#include "scene/outfit_catalog.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ts2::scene {

// A newborn's face shape (LxNR) for its character file, blended from its parents' as the game does
// at a birth: for every age, gender and level of detail, the base face of the newborn's skin tone
// takes each region from the parent `picks` names (parent `a` when it names none), in FaceInfo's
// pair order. Each region is copied once, moving vertices toward the parent's face by the region's
// mask weight. Then the seam between the pair's regions is kept within FaceInfo's range by moving
// its up or down region in height. Hair and neck stay as the base has them.
//
// The arithmetic is the game's at double precision, as it runs until its graphics device is reset
// (after a reset its results differ in the last bits, open question LT-3).
//
// None, with a warning, when a base face, a parent's mesh or FaceInfo is missing, or a parent's
// mesh doesn't fit its base face.
std::optional<faceshape::FaceShape> newbornFaceShape(Assets& assets, const faceshape::FaceShape& a, const faceshape::FaceShape& b,
                                                     const FacePicks& picks, const std::string& skinTone);

// A Sim's face shape as Make New Character makes it: for every age, gender and level of detail, the
// base face of skin tone `skinTone` moved to the face template of `family` (catalog order, see
// OutfitCatalog) for that age and gender, as applyFaceTemplate moves it. A mesh with no such template
// (every baby's) and every mesh of a Sim with no family move nothing. None, with a warning, when a
// base face is missing or a template can't be read.
std::optional<faceshape::FaceShape> madeFaceShape(Assets& assets, const std::vector<CatalogPart>& faceTemplates, const std::string& family,
                                                  const std::string& skinTone);

// The geometry node (GMND) of the base face a new Sim's face is built on: the catalog's skin face
// part of skin tone `skinTone` (else the first) for catalog age flag `ageFlag` and gender flag
// `genderFlag` (1 female, 2 male), its near or far-away level. None when the catalog has none.
std::optional<std::vector<std::uint8_t>> baseFaceNode(Assets& assets, std::uint32_t ageFlag, std::uint32_t genderFlag, bool far,
                                                      const std::string& skinTone);

// One mesh of a newborn's face (face shape age and gender, near or far away): the base face mesh's
// vertices moved to the blended face, and their normals, three floats each per vertex. Each
// triangle corner takes its triangle's unit normal, or the base's where the "normals" mask is
// above 0, and each vertex the unit-length sum of its corners' normals.
struct NewbornFaceVertices {
    std::vector<float> positions, normals;
};

// None when a base face or a parent's mesh is missing or doesn't fit.
std::optional<NewbornFaceVertices> newbornFaceVertices(Assets& assets, const faceshape::FaceShape& a, const faceshape::FaceShape& b,
                                                       const FacePicks& picks, const std::string& skinTone, std::uint8_t age,
                                                       std::uint8_t gender, bool far);

// One mesh of a made Sim's face (face shape age and gender, near or far away), as madeFaceShape
// moves it, with its normals made by the newborn rule. Without a template the base's own normals
// are kept. None when the base face is missing or a template can't be read.
std::optional<NewbornFaceVertices> madeFaceVertices(Assets& assets, const std::vector<CatalogPart>& faceTemplates, const std::string& family,
                                                    const std::string& skinTone, std::uint8_t age, std::uint8_t gender, bool far);

// A grip's place on a new Sim's face (glasses_grip, mouth_grip), for its model's bone adjustments.
struct FaceGrip {
    std::string bone;
    std::array<float, 3> translation{};
    std::array<float, 4> rotation{};  // quaternion x, y, z, w
};

// The grips of a newborn's face model at character file age number `age` (7 young adult) and
// catalog gender flag `genderFlag`, glasses then mouth. Each starts from the grip's node in the
// catalog face model of the age (the adult's for a baby and a young adult), else in the skeleton
// model `skeletonName`. Apart from a baby's, each then moves by the mean move of its face region's
// full-weight triangle corners in the blended face ("eyes" for the glasses, "mouth" for the mouth),
// turned into the grip's parent joint's space. Its rotation is the node's. A grip whose node is
// missing, or whose region has no full-weight corner, is left out. `singlePrecision` gives the
// game's results after its graphics device is reset (open question LT-3), for checks against such
// saves. Throws std::runtime_error when the skeleton or face model can't be read.
std::vector<FaceGrip> newbornFaceGrips(Assets& assets, const faceshape::FaceShape& a, const faceshape::FaceShape& b, const FacePicks& picks,
                                       const std::string& skinTone, int age, std::uint32_t genderFlag, const std::string& skeletonName,
                                       bool singlePrecision = false);

// The grips of a made Sim's face model, by the newborn rule with its face moved to the face template
// (see madeFaceShape) in place of the blended face. Throws when a template can't be read.
std::vector<FaceGrip> madeFaceGrips(Assets& assets, const std::vector<CatalogPart>& faceTemplates, const std::string& family,
                                    const std::string& skinTone, int age, std::uint32_t genderFlag, const std::string& skeletonName);

// An overlay's base texture and its drawing order, lowest first.
struct FaceOverlayTexture {
    std::string texture;
    std::uint32_t layer = 0;
};

// What a new Sim's face material at one age is made from: the catalog base face part's material
// (by age flag, gender flag and skin tone, as the base face mesh is picked), and the base textures
// of the first eye overlay of its eye colour and the first eyebrow overlay of its brow family and
// hair tone for the age, gender and skin tone, in catalog order. A texture is empty when the
// catalog has no such overlay. The other overlays (facial hair) are found as the eyebrows are, one
// for each family asked for, in that order.
struct FaceMaterialParts {
    std::vector<std::uint8_t> catalogMaterial;
    FaceOverlayTexture eye, brow;
    std::vector<FaceOverlayTexture> others;
};

// None when the catalog has no base face part or material for the age, gender and tone.
std::optional<FaceMaterialParts> newbornFaceMaterialParts(Assets& assets, std::uint32_t ageFlag, std::uint32_t genderFlag, const std::string& skinTone,
                                                          const std::string& eyeColour, const std::string& hairTone, const std::string& browFamily,
                                                          std::span<const std::string> otherFamilies = {});

} // namespace ts2::scene
