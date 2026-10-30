#include "formats/face_shape.hpp"
#include "formats/scenegraph.hpp"
#include "scene/face_blend.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

// The next piece of the input: a 16-bit length, then that many bytes (fewer at the end).
std::span<const std::uint8_t> piece(std::span<const std::uint8_t>& rest) {
    if (rest.size() < 2) {
        rest = {};
        return {};
    }
    const std::size_t length = std::min<std::size_t>(rest[0] | rest[1] << 8, rest.size() - 2);
    const auto out = rest.subspan(2, length);
    rest = rest.subspan(2 + length);
    return out;
}

template <typename T, typename Parse>
std::optional<T> tryParse(std::span<const std::uint8_t> bytes, Parse parse) {
    try {
        return parse(bytes);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace

// Blends a face mesh from two face shapes, with masks read from a mask mesh and pairs read from a
// FaceInfo resource, all from the input. The first byte picks each region's parent.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::span<const std::uint8_t> rest(data, size);
    if (rest.empty()) {
        return 0;
    }
    const std::uint8_t pickBits = rest[0];
    rest = rest.subspan(1);
    auto face = tryParse<ts2::scenegraph::Mesh>(piece(rest), [](auto bytes) { return ts2::scenegraph::parseMesh(bytes); });
    const auto a = tryParse<ts2::faceshape::FaceShape>(piece(rest), [](auto bytes) { return ts2::faceshape::parse(bytes); });
    const auto b = tryParse<ts2::faceshape::FaceShape>(piece(rest), [](auto bytes) { return ts2::faceshape::parse(bytes); });
    const auto maskMesh = tryParse<ts2::scenegraph::Mesh>(piece(rest), [](auto bytes) { return ts2::scenegraph::parseMesh(bytes); });
    const auto pairs = tryParse<std::vector<ts2::scene::FacePair>>(piece(rest), [](auto bytes) { return ts2::scene::readFacePairs(bytes); });
    if (!face) {
        return 0;
    }
    const std::vector<std::string> regions{"nose", "brow", "mouth", "cheek", "eyes", "cheekbones", "jaw", "ears"};
    ts2::scene::FacePicks picks;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        picks.push_back({regions[i], (pickBits >> i & 1) != 0});
    }
    ts2::scene::FaceMasks masks;
    if (maskMesh) {
        const auto points = ts2::scene::maskPoints(*maskMesh);
        for (const auto& region : regions) {
            masks[region] = points;
        }
        masks["hairneck"] = points;
        masks["normals"] = points;
    }
    std::vector<ts2::scene::FacePair> order = pairs ? *pairs : std::vector<ts2::scene::FacePair>{};
    if (order.empty()) {
        order = {{"nose", "mouth"}, {"eyes", "cheekbones"}, {"jaw", "cheek"}};
    }
    ts2::scene::blendBabyFace(*face, a, b, picks, masks, order);
    return 0;
}
