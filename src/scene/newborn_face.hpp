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

// The change that turns the base baby face (buFace) into a newborn's face blended from its parents'
// face shapes. Without picks, or when either parent's shape has no baby face, the base face stays as
// it is, as in the game. Each region is
// taken from parent `a` (true) or `b` as `picks` say, in the order of the face pairs the game reads
// (FaceInfo), a region missing from `picks` from `a`. Copied vertices move toward the parent's by
// the region's mask weight. The hair and neck stay as the base has them, normals are made again
// from the new triangles except where the base's are kept. `key` names the result.
MeshEdit newbornFaceEdit(Assets& assets, std::shared_ptr<NewbornFaceData> data, std::string key, std::optional<faceshape::FaceShape> a,
                         std::optional<faceshape::FaceShape> b, std::vector<std::pair<std::string, bool>> picks);

// A Sim's face shape, from its character file. None when it has none or it can't be read.
std::optional<faceshape::FaceShape> faceShapeOf(const std::filesystem::path& characterFile);

} // namespace ts2::scene
