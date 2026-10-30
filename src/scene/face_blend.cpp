#include "scene/face_blend.hpp"

#include "diag/log.hpp"
#include "formats/binary_reader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <set>

namespace ts2::scene {

namespace sg = scenegraph;

namespace {

constexpr std::uint8_t kBabyShapeAge = 5;
// Texture coordinates this close name the same corner.
constexpr float kSameUv = 0.0001f;

bool sameSign(float x, float y) {
    return std::signbit(x) == std::signbit(y);
}

// Each vertex's weight in a region: the last mask point at its texture coordinates.
std::vector<float> weights(const std::vector<MaskPoint>& mask, const std::vector<float>& uv) {
    std::vector<float> w(uv.size() / 2, 0.0f);
    for (const auto& p : mask) {
        for (std::size_t v = 0; v < w.size(); ++v) {
            if (std::fabs(uv[v * 2] - p.u) < kSameUv && std::fabs(uv[v * 2 + 1] - p.v) < kSameUv) {
                w[v] = p.weight;
            }
        }
    }
    return w;
}

// A parent's baby face, vertex by vertex: the base plus the offset of the first point of its shape
// with the vertex's texture coordinates. The base where it has none.
std::vector<float> parentFace(const std::optional<faceshape::FaceShape>& shape, const std::vector<float>& base,
                              const std::vector<float>& uv) {
    std::vector<float> face = base;
    const auto* mesh = shape ? shape->find(kBabyShapeAge, 0, 0) : nullptr;
    if (mesh == nullptr) {
        return face;
    }
    for (std::size_t v = 0; v < uv.size() / 2; ++v) {
        const float u = uv[v * 2], w = uv[v * 2 + 1];
        for (const auto& p : mesh->points) {
            if (sameSign(p.uv[0], u) && sameSign(p.uv[1], w) && std::fabs(p.uv[0] - u) < kSameUv && std::fabs(p.uv[1] - w) < kSameUv) {
                for (std::size_t k = 0; k < 3; ++k) {
                    face[v * 3 + k] += p.offset[k];
                }
                break;
            }
        }
    }
    return face;
}

void writeFloats(sg::Element& element, const std::vector<float>& values) {
    if (element.data.size() == values.size() * sizeof(float)) {
        std::memcpy(element.data.data(), values.data(), element.data.size());
    }
}

// Normals made from the triangles: each vertex takes the sum of the unit normals of the triangles
// using it, made unit length. Vertices `keep` marks keep their own.
std::vector<float> faceNormals(const sg::Mesh& mesh, std::size_t link, const std::vector<float>& positions,
                               const std::vector<float>& own, const std::vector<float>& keep) {
    std::vector<float> sum(positions.size(), 0.0f);
    const std::size_t count = positions.size() / 3;
    for (const auto& group : mesh.groups) {
        if (group.link != link || group.primitive != sg::kTriangleList) {
            continue;
        }
        for (std::size_t t = 0; t + 2 < group.indices.size(); t += 3) {
            const std::array<std::size_t, 3> c{group.indices[t], group.indices[t + 1], group.indices[t + 2]};
            if (c[0] >= count || c[1] >= count || c[2] >= count) {
                continue;
            }
            auto at = [&](std::size_t v, std::size_t k) { return positions[c[v] * 3 + k]; };
            const std::array<float, 3> e1{at(1, 0) - at(2, 0), at(1, 1) - at(2, 1), at(1, 2) - at(2, 2)};
            const std::array<float, 3> e2{at(1, 0) - at(0, 0), at(1, 1) - at(0, 1), at(1, 2) - at(0, 2)};
            std::array<float, 3> n{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (!(length > 0)) {
                continue;
            }
            for (const std::size_t v : c) {
                for (std::size_t k = 0; k < 3; ++k) {
                    sum[v * 3 + k] += n[k] / length;
                }
            }
        }
    }
    std::vector<float> normals = own;
    for (std::size_t v = 0; v < count; ++v) {
        const float length = std::sqrt(sum[v * 3] * sum[v * 3] + sum[v * 3 + 1] * sum[v * 3 + 1] + sum[v * 3 + 2] * sum[v * 3 + 2]);
        if (keep[v] > 0 || !(length > 0)) {
            continue;
        }
        for (std::size_t k = 0; k < 3; ++k) {
            normals[v * 3 + k] = sum[v * 3 + k] / length;
        }
    }
    return normals;
}

} // namespace

std::vector<MaskPoint> maskPoints(const sg::Mesh& mesh) {
    std::vector<MaskPoint> points;
    for (const auto& link : mesh.links) {
        const auto uv = sg::vertexFloats(mesh, link, sg::kTextureCoordinates);
        const auto* colours = mesh.find(link, sg::kColors);
        if (colours == nullptr || colours->data.size() < uv.size() / 2 * 4) {
            continue;
        }
        for (std::size_t v = 0; v < uv.size() / 2; ++v) {
            const float weight = colours->data[v * 4 + 3] / 255.0f;
            if (weight >= 0.0001f) {
                points.push_back({uv[v * 2], uv[v * 2 + 1], weight});
            }
        }
    }
    return points;
}

std::vector<FacePair> readFacePairs(std::span<const std::uint8_t> bytes) {
    BinaryReader r(bytes);
    r.u32();  // version
    // Each pair is four strings of at least a length each.
    const std::size_t count = r.u32Count(16);
    auto text = [&r] { return r.text(r.u32Count(1)); };
    std::vector<FacePair> pairs;
    for (std::size_t i = 0; i < count; ++i) {
        FacePair pair{text(), text()};
        text();  // the regions moved up and down to keep the pair's seam
        text();
        pairs.push_back(std::move(pair));
    }
    return pairs;
}

bool blendsBabyFace(const std::optional<faceshape::FaceShape>& a, const std::optional<faceshape::FaceShape>& b, const FacePicks& picks) {
    const auto babyFace = [](const std::optional<faceshape::FaceShape>& shape) { return shape && shape->find(kBabyShapeAge, 0, 0) != nullptr; };
    return !picks.empty() && babyFace(a) && babyFace(b);
}

void blendBabyFace(sg::Mesh& mesh, const std::optional<faceshape::FaceShape>& a, const std::optional<faceshape::FaceShape>& b,
                   const FacePicks& picks, const FaceMasks& masks, const std::vector<FacePair>& pairs) {
    if (!blendsBabyFace(a, b, picks)) {
        return;
    }
    const auto group = std::find_if(mesh.groups.begin(), mesh.groups.end(),
                                    [&](const sg::Group& g) { return g.primitive == sg::kTriangleList && g.link < mesh.links.size(); });
    if (group == mesh.groups.end()) {
        return;
    }
    const std::size_t link = group->link;
    const auto& elements = mesh.links[link];
    if (std::any_of(elements.aliases.begin(), elements.aliases.end(), [](const auto& alias) { return !alias.empty(); })) {
        diag::warnOnce("newborn face: the base face's vertices are shared in a way not read, so it isn't blended");
        return;
    }
    auto element = [&](std::uint32_t identity) -> sg::Element* {
        for (const std::uint16_t e : elements.elements) {
            if (e < mesh.elements.size() && mesh.elements[e].identity == identity) {
                return &mesh.elements[e];
            }
        }
        return nullptr;
    };
    sg::Element* positionElement = element(sg::kPositions);
    sg::Element* normalElement = element(sg::kNormals);
    const auto base = sg::vertexFloats(mesh, elements, sg::kPositions);
    const auto uv = sg::vertexFloats(mesh, elements, sg::kTextureCoordinates);
    const auto ownNormals = sg::vertexFloats(mesh, elements, sg::kNormals);
    if (positionElement == nullptr || base.empty() || uv.size() / 2 != base.size() / 3) {
        return;
    }
    const auto faceA = parentFace(a, base, uv), faceB = parentFace(b, base, uv);
    std::map<std::string, std::vector<float>> regionWeights;
    auto weightsOf = [&](const std::string& region) -> const std::vector<float>* {
        if (const auto it = regionWeights.find(region); it != regionWeights.end()) {
            return &it->second;
        }
        const auto mask = masks.find(region);
        return mask == masks.end() ? nullptr : &(regionWeights[region] = weights(mask->second, uv));
    };
    std::vector<float> face = base;
    std::set<std::string> done;
    auto copy = [&](const std::string& region) {
        if (!done.insert(region).second) {
            return;
        }
        const auto pick = std::find_if(picks.begin(), picks.end(), [&](const auto& p) { return p.first == region; });
        const auto& from = pick != picks.end() && !pick->second ? faceB : faceA;
        const auto& w = *weightsOf(region);
        for (std::size_t v = 0; v < w.size(); ++v) {
            if (w[v] >= 0.001f) {
                for (std::size_t k = 0; k < 3; ++k) {
                    face[v * 3 + k] = (1 - w[v]) * face[v * 3 + k] + w[v] * from[v * 3 + k];
                }
            }
        }
    };
    // A baby's face has no limits on its pairs' seams (FaceInfo gives none for babies), so nothing
    // is moved up or down after a pair.
    for (const auto& pair : pairs) {
        if (weightsOf(pair.a) == nullptr || weightsOf(pair.b) == nullptr) {
            continue;
        }
        copy(pair.a);
        copy(pair.b);
    }
    if (const auto* hairNeck = weightsOf("hairneck")) {
        for (std::size_t v = 0; v < hairNeck->size(); ++v) {
            if ((*hairNeck)[v] > 0) {
                std::copy_n(base.begin() + static_cast<std::ptrdiff_t>(v * 3), 3, face.begin() + static_cast<std::ptrdiff_t>(v * 3));
            }
        }
    }
    writeFloats(*positionElement, face);
    if (normalElement != nullptr && ownNormals.size() == face.size()) {
        const auto* keep = weightsOf("normals");
        const std::vector<float> none(face.size() / 3, 0.0f);
        writeFloats(*normalElement, faceNormals(mesh, link, face, ownNormals, keep != nullptr ? *keep : none));
    }
}

} // namespace ts2::scene
