#pragma once

#include "formats/face_shape.hpp"
#include "scene/assets.hpp"
#include "scene/face_blend.hpp"
#include "scene/builder.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ts2::scene {

// The face region masks and pairs every newborn's face is blended with, read on first use and kept
// for the babies after.
struct NewbornFaceData {
    std::optional<FaceMasks> masks;
    std::vector<FacePair> pairs;
};

// The change that turns the base baby face (buFace) into a newborn's face blended from its parents'.
// Without picks, or when a parent's shape has no baby face, the base face stays, as in the game. Each
// region comes from parent `a` (true) or `b` as `picks` say, in the game's face pair order (FaceInfo),
// missing ones from `a`, its vertices moved toward the parent's by the region's mask weight. Hair and
// neck stay as the base has them, and normals are remade except where the base's are kept.
MeshEdit newbornFaceEdit(Assets& assets, std::shared_ptr<NewbornFaceData> data, std::string key, std::optional<faceshape::FaceShape> a,
                         std::optional<faceshape::FaceShape> b, std::vector<std::pair<std::string, bool>> picks);

// The mesh of a shape (SHPE) resource: the first of its geometry nodes that has one. None when
// missing.
std::optional<scenegraph::Mesh> shapeMesh(Assets& assets, const scenegraph::Link& link);

// The face region masks (face region sets) for a face of age flag `age`, by region name, the first
// of each name. Faces of every age share the "hairneck" and "normals" masks.
FaceMasks faceRegionMasks(Assets& assets, std::uint32_t age);

// A Sim's face shape, from its character file. None when it has none or it can't be read.
std::optional<faceshape::FaceShape> faceShapeOf(const std::filesystem::path& characterFile);

} // namespace ts2::scene
