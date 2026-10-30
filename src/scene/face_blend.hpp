#pragma once

#include "formats/face_shape.hpp"
#include "formats/scenegraph.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ts2::scene {

// A face region's weights: each point of its mask mesh, by texture coordinates.
struct MaskPoint {
    float u = 0, v = 0, weight = 0;
};
using FaceMasks = std::map<std::string, std::vector<MaskPoint>>;

// Two regions the blend takes together, in FaceInfo's order.
struct FacePair {
    std::string a, b;
};

// Each face region, from parent `a` (true) or `b`.
using FacePicks = std::vector<std::pair<std::string, bool>>;

// A mask mesh's points. A point's weight is its vertex colour's alpha, and points weighing nothing
// are left out.
std::vector<MaskPoint> maskPoints(const scenegraph::Mesh& mesh);

// The face pairs a FaceInfo resource lists, in order. Throws when it can't be read.
std::vector<FacePair> readFacePairs(std::span<const std::uint8_t> bytes);

// Whether a newborn's face is blended at all: without picks, or without a baby face in either
// parent's shape, the game leaves the base face as it is, its normals too.
bool blendsBabyFace(const std::optional<faceshape::FaceShape>& a, const std::optional<faceshape::FaceShape>& b, const FacePicks& picks);

// Turns the base baby face `mesh` into a newborn's: each region is taken from parent `a` or `b` as
// `picks` say (a region missing from them from `a`), in the order of `pairs`, both regions of a pair
// only when both have masks. Copied vertices move toward the parent's by the region's mask weight.
// The hair and neck stay as the base has them, and normals are made again from the new triangles
// except where the "normals" mask keeps the base's.
void blendBabyFace(scenegraph::Mesh& mesh, const std::optional<faceshape::FaceShape>& a, const std::optional<faceshape::FaceShape>& b,
                   const FacePicks& picks, const FaceMasks& masks, const std::vector<FacePair>& pairs);

} // namespace ts2::scene
