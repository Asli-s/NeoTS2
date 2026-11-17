#include "scene/newborn_face_shape.hpp"

#include "scene/newborn_face.hpp"

#include "diag/log.hpp"
#include "formats/propset.hpp"
#include "formats/rcol.hpp"
#include "formats/reference_list.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace ts2::scene {

namespace {

namespace sg = scenegraph;

constexpr std::uint32_t kFaceRegion = 0x8C93BF6C, kFaceInfo = 0x46414345, kSkinPart = 0xEBCF3E27;
constexpr std::uint32_t kShpe = 0xFC6EB1F7, kGmnd = 0x7BA3838C, kGmdc = 0xAC4F8687;
constexpr std::uint8_t kFarGroup = 15;
// The face shape's ages (0 toddler to 5 baby) as catalog age flags.
constexpr std::array<std::uint32_t, 6> kAgeFlags{0x01, 0x02, 0x04, 0x08, 0x10, 0x20};
// The order the game writes the meshes in: baby first, then toddler to elder.
constexpr std::array<std::uint8_t, 6> kAgeOrder{5, 0, 1, 2, 3, 4};
constexpr float kSameUv = 0.0001f;
// A mask vertex's weight is its colour's alpha times this, the game's 1/255 as a float.
constexpr float kAlphaScale = 0.003921568859368563f;
// Weights below these leave a vertex out of a mask, a copy and a seam move.
constexpr float kMaskMinimum = 0.0001f, kCopyMinimum = 0.001f, kShiftMinimum = 0.0001f;
// A vertex of at least this weight counts toward its region's mean height.
constexpr float kFullWeight = 0.9f;
// The seam range when FaceInfo gives none for a pair.
constexpr float kNoRange = 9999.0f;

unsigned long number(const std::map<std::string, std::string>& p, const char* key) {
    const auto it = p.find(key);
    return it != p.end() ? std::strtoul(it->second.c_str(), nullptr, 0) : 0ul;
}

bool human(const std::map<std::string, std::string>& p) {
    return !p.contains("species") || (number(p, "species") & 1) != 0;
}

std::optional<sg::Mesh> meshOfNode(Assets& assets, const std::optional<std::vector<std::uint8_t>>& gmnd) {
    if (!gmnd) {
        return std::nullopt;
    }
    for (const auto& l : sg::parseContainer(*gmnd).links) {
        if (l.type == kGmdc) {
            if (const auto bytes = assets.byKey(l)) {
                return sg::parseMesh(*bytes);
            }
        }
    }
    return std::nullopt;
}

// A far-away part's name: the near one's with LOD15 inserted at `at`. Empty when there is no `at`.
std::string farName(std::string name, std::size_t at) {
    if (at == std::string::npos) {
        return {};
    }
    name.insert(at, "LOD15");
    return name;
}

struct FacePart {
    sg::Link key;
    std::map<std::string, std::string> properties;
};

// The catalog's human skin face parts (category 0x80, skin outfit).
std::vector<FacePart> faceParts(Assets& assets) {
    std::vector<FacePart> parts;
    assets.forEachSkinResource(kSkinPart, [&](const sg::Link& key, const std::vector<std::uint8_t>& bytes) {
        try {
            auto p = propset::parse(bytes);
            if ((number(p, "category") & 0x80) && (number(p, "outfit") & 2) && (number(p, "species") == 0 || (number(p, "species") & 1))) {
                parts.push_back({key, std::move(p)});
            }
        } catch (const std::exception& ex) {
            diag::warnOnce(std::string("newborn face shape: a skin part that can't be read is left out: ") + ex.what());
        }
    });
    return parts;
}

// The base face's geometry node for an age and gender: the face part of skin tone `tone`, else the
// first part for them. The far-away face is a shape of its own, named as the near one with LOD15
// before "_untagged".
std::optional<std::vector<std::uint8_t>> baseNode(Assets& assets, const std::vector<FacePart>& parts, std::uint32_t age, std::uint32_t gender,
                                                  bool far, const std::string& tone) {
    const FacePart* pick = nullptr;
    for (const auto& part : parts) {
        if (!(number(part.properties, "age") & age) || !(number(part.properties, "gender") & gender)) {
            continue;
        }
        if (pick == nullptr) {
            pick = &part;
        }
        if (const auto it = part.properties.find("skintone"); it != part.properties.end() && it->second == tone) {
            pick = &part;
            break;
        }
    }
    if (pick == nullptr) {
        return std::nullopt;
    }
    const auto refs = assets.skinResource({reflist::kType, pick->key.group, pick->key.instance, pick->key.instanceHigh});
    const auto links = refs ? reflist::parse(*refs) : std::vector<sg::Link>{};
    const std::size_t index = number(pick->properties, "shapekeyidx");
    const auto shpe = index < links.size() ? assets.byKey(links[index]) : std::nullopt;
    if (!shpe) {
        return std::nullopt;
    }
    sg::Shape shape = sg::parseShape(*shpe);
    if (far) {
        const std::string name = farName(shape.name, shape.name.find("_untagged"));
        const auto farShape = name.empty() ? std::nullopt : assets.byName(kShpe, name);
        if (!farShape) {
            return std::nullopt;
        }
        shape = sg::parseShape(*farShape);
    }
    for (std::size_t i = 0; i < shape.detailLevels.size(); ++i) {
        if (shape.detailLevels[i] != 0) {
            continue;
        }
        const auto linked = i < shape.geometryLinks.size() ? shape.geometryLinks[i] : std::nullopt;
        const auto gmnd = linked ? (*linked < shape.container.links.size() ? assets.byKey(shape.container.links[*linked]) : std::nullopt)
                                 : (i < shape.geometryNodes.size() ? assets.byName(kGmnd, shape.geometryNodes[i]) : std::nullopt);
        if (gmnd && meshOfNode(assets, gmnd)) {
            return gmnd;
        }
    }
    return std::nullopt;
}

std::optional<sg::Mesh> baseFace(Assets& assets, const std::vector<FacePart>& parts, std::uint32_t age, std::uint32_t gender, bool far,
                                 const std::string& tone) {
    return meshOfNode(assets, baseNode(assets, parts, age, gender, far, tone));
}

// Each human face region's mask points for an age, in the order of its mesh's triangle corners,
// the first region set of each name. A far-away mask is the geometry node named as the near
// mask's with LOD15 before its first '_'. Regions with no far mask are left out of far faces.
FaceMasks cornerMasks(Assets& assets, std::uint32_t age, bool far) {
    FaceMasks masks;
    assets.forEachSkinResource(kFaceRegion, [&](const sg::Link& key, const std::vector<std::uint8_t>& bytes) {
        try {
            const auto p = propset::parse(bytes);
            const auto name = p.find("name");
            if (name == p.end() || !(number(p, "age") & age) || !human(p) || masks.contains(name->second)) {
                return;
            }
            const auto refs = assets.skinResource({reflist::kType, key.group, key.instance, key.instanceHigh});
            for (const auto& l : refs ? reflist::parse(*refs) : std::vector<sg::Link>{}) {
                if (l.type != kShpe) {
                    continue;
                }
                std::optional<sg::Mesh> mesh;
                if (!far) {
                    mesh = shapeMesh(assets, l);
                } else if (const auto nearShape = assets.byKey(l)) {
                    const sg::Shape shape = sg::parseShape(*nearShape);
                    const std::string node = shape.geometryNodes.empty() ? std::string{} : farName(shape.geometryNodes[0], shape.geometryNodes[0].find('_'));
                    mesh = node.empty() ? std::nullopt : meshOfNode(assets, assets.byName(kGmnd, node));
                }
                if (!mesh) {
                    break;
                }
                auto& points = masks[name->second];
                for (const auto& group : mesh->groups) {
                    if (group.link >= mesh->links.size()) {
                        continue;
                    }
                    const auto uv = sg::vertexFloats(*mesh, mesh->links[group.link], sg::kTextureCoordinates);
                    const auto* colours = mesh->find(mesh->links[group.link], sg::kColors);
                    if (colours == nullptr) {
                        continue;
                    }
                    for (const std::uint16_t i : group.indices) {
                        if (i * 2u + 1 >= uv.size() || i * 4u + 3 >= colours->data.size()) {
                            continue;
                        }
                        const float w = static_cast<float>(colours->data[i * 4u + 3]) * kAlphaScale;
                        if (w >= kMaskMinimum) {
                            points.push_back({uv[i * 2u], uv[i * 2u + 1], w});
                        }
                    }
                }
                break;
            }
        } catch (const std::exception& ex) {
            diag::warnOnce(std::string("newborn face shape: a face region that can't be read is left out: ") + ex.what());
        }
    });
    return masks;
}

// One base face being blended: its vertices, triangle corners and texture coordinates.
class FaceMesh {
public:
    FaceMesh(std::vector<float> base, std::vector<float> uv, std::vector<std::uint16_t> corners)
        : base_(std::move(base)), uv_(std::move(uv)), corners_(std::move(corners)), byU_(uv_.size() / 2) {
        for (std::size_t v = 0; v < byU_.size(); ++v) {
            byU_[v] = {uv_[v * 2], v};
        }
        std::sort(byU_.begin(), byU_.end());
    }

    std::size_t vertices() const { return base_.size() / 3; }
    const std::vector<float>& base() const { return base_; }
    const std::vector<float>& uv() const { return uv_; }
    const std::vector<std::uint16_t>& corners() const { return corners_; }

    // Each vertex's weight in a mask: the last mask point at its texture coordinates.
    std::vector<float> weights(const std::vector<MaskPoint>& mask) const {
        std::vector<float> w(vertices(), 0.0f);
        for (const auto& p : mask) {
            // A wider window than the match, so rounding never leaves out a vertex that matches.
            auto it = std::lower_bound(byU_.begin(), byU_.end(), std::pair{p.u - 2 * kSameUv, std::size_t{0}});
            for (; it != byU_.end() && it->first <= p.u + 2 * kSameUv; ++it) {
                const std::size_t v = it->second;
                if (std::fabs(uv_[v * 2] - p.u) < kSameUv && std::fabs(uv_[v * 2 + 1] - p.v) < kSameUv) {
                    w[v] = p.weight;
                }
            }
        }
        return w;
    }

    // A parent's face, vertex by vertex: the base plus the offset of the vertex's last corner.
    std::vector<float> parentFace(const faceshape::FaceShape::Mesh& shape) const {
        std::vector<float> face = base_;
        for (std::size_t c = 0; c < corners_.size(); ++c) {
            for (std::size_t k = 0; k < 3; ++k) {
                face[corners_[c] * 3u + k] = base_[corners_[c] * 3u + k] + shape.points[c].offset[k];
            }
        }
        return face;
    }

private:
    std::vector<float> base_, uv_;
    std::vector<std::uint16_t> corners_;
    std::vector<std::pair<float, std::size_t>> byU_;
};

// The base face's triangle list: the first one whose link a broken file can't send out of range.
const sg::Group* faceGroup(const sg::Mesh& mesh) {
    const auto group = std::find_if(mesh.groups.begin(), mesh.groups.end(),
                                    [&](const sg::Group& g) { return g.primitive == sg::kTriangleList && g.link < mesh.links.size(); });
    return group != mesh.groups.end() ? &*group : nullptr;
}

// The base face's triangle list as a FaceMesh. None when it has none or its data doesn't fit.
std::optional<FaceMesh> faceMesh(const sg::Mesh& mesh) {
    const sg::Group* group = faceGroup(mesh);
    if (group == nullptr) {
        return std::nullopt;
    }
    auto base = sg::vertexFloats(mesh, mesh.links[group->link], sg::kPositions);
    auto uv = sg::vertexFloats(mesh, mesh.links[group->link], sg::kTextureCoordinates);
    const std::size_t vertices = base.size() / 3;
    // A broken mesh's value that isn't a finite number would spoil the face and the sort by texture
    // coordinates.
    const auto finite = [](const std::vector<float>& values) { return std::all_of(values.begin(), values.end(), [](float x) { return std::isfinite(x); }); };
    if (vertices == 0 || uv.size() / 2 != vertices || !finite(base) || !finite(uv) ||
        std::any_of(group->indices.begin(), group->indices.end(), [&](std::uint16_t i) { return i >= vertices; })) {
        return std::nullopt;
    }
    return FaceMesh(std::move(base), std::move(uv), group->indices);
}

// The game's x87 steps at double precision, each stored to a float where the game stores it.
float blendTo(float keep, float value, float weight, float target) {
    return static_cast<float>(static_cast<double>(keep) * value + static_cast<double>(weight) * target);
}

float movedBy(float by, float weight, float z) {
    return static_cast<float>(static_cast<double>(by) * weight + z);
}

// The mean height of the corners of weight 0.9 or more, a corner counted each time a triangle uses
// it. The sum is kept unrounded and stored as a float before it is divided.
float meanHeight(const FaceMesh& mesh, const std::vector<float>& face, const std::vector<float>& w) {
    double sum = 0;
    std::size_t n = 0;
    for (const std::uint16_t v : mesh.corners()) {
        if (w[v] >= kFullWeight) {
            sum += face[v * 3u + 2];
            ++n;
        }
    }
    return n == 0 ? 0.0f : static_cast<float>(static_cast<double>(static_cast<float>(sum)) * (1.0 / static_cast<double>(n)));
}

struct MeshInputs {
    std::uint8_t age, gender;
    const FaceInfo& info;
    const FaceMasks& masks;
    const FacePicks& picks;
};

// The blended face's vertex positions, three per vertex.
std::vector<float> blendFace(const FaceMesh& mesh, const faceshape::FaceShape::Mesh& a, const faceshape::FaceShape::Mesh& b,
                             const MeshInputs& in) {
    const auto faceA = mesh.parentFace(a), faceB = mesh.parentFace(b);
    std::vector<float> face = mesh.base();
    std::map<std::string, std::vector<float>> cache;
    const auto weightsOf = [&](const std::string& region) -> const std::vector<float>* {
        if (const auto it = cache.find(region); it != cache.end()) {
            return &it->second;
        }
        const auto mask = in.masks.find(region);
        return mask == in.masks.end() ? nullptr : &(cache[region] = mesh.weights(mask->second));
    };
    std::set<std::string> copied;
    const auto copy = [&](const std::string& region, const std::vector<float>& w) {
        if (!copied.insert(region).second) {
            return;
        }
        const auto pick = std::find_if(in.picks.begin(), in.picks.end(), [&](const auto& p) { return p.first == region; });
        const auto& from = pick != in.picks.end() && !pick->second ? faceB : faceA;
        for (std::size_t v = 0; v < mesh.vertices(); ++v) {
            if (w[v] >= kCopyMinimum) {
                const float keep = 1.0f - w[v];
                for (std::size_t k = 0; k < 3; ++k) {
                    face[v * 3 + k] = blendTo(keep, face[v * 3 + k], w[v], from[v * 3 + k]);
                }
            }
        }
    };
    for (const FacePair& pair : in.info.pairs) {
        const auto* wa = weightsOf(pair.a);
        const auto* wb = weightsOf(pair.b);
        if (wa == nullptr || wb == nullptr) {
            continue;
        }
        copy(pair.a, *wa);
        copy(pair.b, *wb);
        const float d = meanHeight(mesh, face, *wa) - meanHeight(mesh, face, *wb);
        float low = -kNoRange, high = kNoRange;
        for (const FaceRange& r : in.info.ranges) {
            if (r.age == in.age && r.gender == in.gender && r.a == pair.a && r.b == pair.b) {
                low = r.low;
                high = r.high;
            }
        }
        const std::string* moved = d > high ? &pair.up : d < low ? &pair.down : nullptr;
        const float by = d > high ? d - high : d - low;
        const auto* wm = moved != nullptr && !moved->empty() ? weightsOf(*moved) : nullptr;
        if (wm == nullptr) {
            continue;
        }
        for (std::size_t v = 0; v < mesh.vertices(); ++v) {
            if ((*wm)[v] >= kShiftMinimum) {
                face[v * 3 + 2] = movedBy(by, (*wm)[v], face[v * 3 + 2]);
            }
        }
    }
    if (const auto* hairNeck = weightsOf("hairneck")) {
        for (std::size_t v = 0; v < mesh.vertices(); ++v) {
            if ((*hairNeck)[v] > 0) {
                std::copy_n(mesh.base().begin() + static_cast<std::ptrdiff_t>(v * 3), 3, face.begin() + static_cast<std::ptrdiff_t>(v * 3));
            }
        }
    }
    return face;
}

// One mesh of the newborn's shape: the blended face as offsets from the base at each corner.
std::vector<faceshape::Point> blendMesh(const FaceMesh& mesh, const faceshape::FaceShape::Mesh& a, const faceshape::FaceShape::Mesh& b,
                                        const MeshInputs& in) {
    const std::vector<float> face = blendFace(mesh, a, b, in);
    std::vector<faceshape::Point> points;
    points.reserve(mesh.corners().size());
    for (const std::uint16_t v : mesh.corners()) {
        const std::size_t i = v * 3u;
        points.push_back({{face[i] - mesh.base()[i], face[i + 1] - mesh.base()[i + 1], face[i + 2] - mesh.base()[i + 2]},
                          {mesh.uv()[v * 2u], mesh.uv()[v * 2u + 1]}});
    }
    return points;
}

std::optional<FaceInfo> faceInfo(Assets& assets) {
    std::optional<FaceInfo> info;
    assets.forEachSkinResource(kFaceInfo, [&](const sg::Link&, const std::vector<std::uint8_t>& bytes) {
        if (info) {
            return;
        }
        try {
            info = readFaceInfo(bytes);
        } catch (const std::exception& ex) {
            diag::warnOnce(std::string("newborn face shape: FaceInfo can't be read: ") + ex.what());
        }
    });
    return info;
}

// The mesh of the face template of `family` for a catalog age and gender flag, near or far away: the
// first human template in catalog order with the family, age and gender. A far mesh is the geometry
// node named as the near shape's with LOD15 before its first '_'. None when there is no such template.
std::optional<sg::Mesh> templateMesh(Assets& assets, const std::vector<CatalogPart>& templates, const std::string& family, std::uint32_t age,
                                     std::uint32_t gender, bool far) {
    constexpr std::uint32_t kHuman = 1;
    const auto t = std::find_if(templates.begin(), templates.end(),
                                [&](const CatalogPart& p) { return p.family == family && (p.species & kHuman) && (p.age & age) && (p.gender & gender); });
    if (t == templates.end()) {
        return std::nullopt;
    }
    const auto bytes = assets.skinResource(t->key);
    const auto refs = assets.skinResource({reflist::kType, t->key.group, t->key.instance, t->key.instanceHigh});
    if (!bytes || !refs) {
        return std::nullopt;
    }
    const auto p = propset::parse(*bytes);
    const auto links = reflist::parse(*refs);
    const std::size_t index = number(p, "shapekeyidx");
    if (!p.contains("shapekeyidx") || index >= links.size()) {
        return std::nullopt;
    }
    if (!far) {
        return shapeMesh(assets, links[index]);
    }
    const auto nearShape = assets.byKey(links[index]);
    if (!nearShape) {
        return std::nullopt;
    }
    const sg::Shape shape = sg::parseShape(*nearShape);
    const std::string node = shape.geometryNodes.empty() ? std::string{} : farName(shape.geometryNodes[0], shape.geometryNodes[0].find('_'));
    return node.empty() ? std::nullopt : meshOfNode(assets, assets.byName(kGmnd, node));
}

// A base face moved to a face template's, as applyFaceTemplate moves it: each template vertex, group
// by group, takes the place of every base vertex at its texture coordinates, the last one winning,
// and the hair and neck stay as the base has them.
std::vector<float> templateFace(const FaceMesh& mesh, const sg::Mesh& faceTemplate, const FaceMasks& masks) {
    std::vector<float> face = mesh.base();
    const auto& uv = mesh.uv();
    for (const auto& from : faceTemplate.links) {
        const auto points = sg::vertexFloats(faceTemplate, from, sg::kPositions);
        const auto coords = sg::vertexFloats(faceTemplate, from, sg::kTextureCoordinates);
        if (points.empty() || coords.size() / 2 != points.size() / 3) {
            continue;
        }
        for (std::size_t t = 0; t < coords.size() / 2; ++t) {
            for (std::size_t v = 0; v < mesh.vertices(); ++v) {
                if (std::fabs(uv[v * 2] - coords[t * 2]) < kSameUv && std::fabs(uv[v * 2 + 1] - coords[t * 2 + 1]) < kSameUv) {
                    std::copy_n(points.begin() + static_cast<std::ptrdiff_t>(t * 3), 3, face.begin() + static_cast<std::ptrdiff_t>(v * 3));
                }
            }
        }
    }
    if (const auto hairNeck = masks.find("hairneck"); hairNeck != masks.end()) {
        const auto w = mesh.weights(hairNeck->second);
        for (std::size_t v = 0; v < mesh.vertices(); ++v) {
            if (w[v] > 0) {
                std::copy_n(mesh.base().begin() + static_cast<std::ptrdiff_t>(v * 3), 3, face.begin() + static_cast<std::ptrdiff_t>(v * 3));
            }
        }
    }
    return face;
}

} // namespace

std::optional<faceshape::FaceShape> madeFaceShape(Assets& assets, const std::vector<CatalogPart>& faceTemplates, const std::string& family,
                                                  const std::string& skinTone) {
    const auto parts = faceParts(assets);
    faceshape::FaceShape shape;
    shape.version = 5;
    try {
        for (const std::uint8_t age : kAgeOrder) {
            const std::array<FaceMasks, 2> masks{cornerMasks(assets, kAgeFlags[age], false), cornerMasks(assets, kAgeFlags[age], true)};
            for (const std::uint8_t gender : {std::uint8_t{0}, std::uint8_t{1}}) {
                const std::uint32_t genderFlag = gender == 0 ? 2 : 1;
                for (const std::uint8_t group : {std::uint8_t{0}, kFarGroup}) {
                    const bool far = group == kFarGroup;
                    const auto base = baseFace(assets, parts, kAgeFlags[age], genderFlag, far, skinTone);
                    const auto mesh = base ? faceMesh(*base) : std::nullopt;
                    if (!mesh) {
                        diag::warnOnce("made face shape: a base face is missing, so no face shape is made");
                        return std::nullopt;
                    }
                    const auto faceTemplate = family.empty() ? std::nullopt : templateMesh(assets, faceTemplates, family, kAgeFlags[age], genderFlag, far);
                    const std::vector<float> face = faceTemplate ? templateFace(*mesh, *faceTemplate, masks[far ? 1 : 0]) : mesh->base();
                    std::vector<faceshape::Point> points;
                    points.reserve(mesh->corners().size());
                    for (const std::uint16_t v : mesh->corners()) {
                        const std::size_t i = v * 3u;
                        points.push_back({{face[i] - mesh->base()[i], face[i + 1] - mesh->base()[i + 1], face[i + 2] - mesh->base()[i + 2]},
                                          {mesh->uv()[v * 2u], mesh->uv()[v * 2u + 1]}});
                    }
                    shape.meshes.push_back({age, gender, group, std::move(points)});
                }
            }
        }
    } catch (const std::exception& ex) {
        diag::warnOnce(std::string("made face shape: a face template can't be read, so no face shape is made: ") + ex.what());
        return std::nullopt;
    }
    return shape;
}

std::optional<std::vector<std::uint8_t>> baseFaceNode(Assets& assets, std::uint32_t ageFlag, std::uint32_t genderFlag, bool far,
                                                      const std::string& skinTone) {
    return baseNode(assets, faceParts(assets), ageFlag, genderFlag, far, skinTone);
}

std::optional<faceshape::FaceShape> newbornFaceShape(Assets& assets, const faceshape::FaceShape& a, const faceshape::FaceShape& b,
                                                     const FacePicks& picks, const std::string& skinTone) {
    const auto info = faceInfo(assets);
    if (!info) {
        diag::warnOnce("newborn face shape: no FaceInfo, so no face shape is made");
        return std::nullopt;
    }
    const auto parts = faceParts(assets);
    faceshape::FaceShape shape;
    shape.version = 5;
    for (const std::uint8_t age : kAgeOrder) {
        const std::array<FaceMasks, 2> masks{cornerMasks(assets, kAgeFlags[age], false), cornerMasks(assets, kAgeFlags[age], true)};
        for (const std::uint8_t gender : {std::uint8_t{0}, std::uint8_t{1}}) {
            // The face shape's gender 0 is male (catalog flag 2), 1 female (flag 1).
            const std::uint32_t genderFlag = gender == 0 ? 2 : 1;
            for (const std::uint8_t group : {std::uint8_t{0}, kFarGroup}) {
                const bool far = group == kFarGroup;
                const auto base = baseFace(assets, parts, kAgeFlags[age], genderFlag, far, skinTone);
                const auto mesh = base ? faceMesh(*base) : std::nullopt;
                const auto* fromA = a.find(age, gender, group);
                const auto* fromB = b.find(age, gender, group);
                if (!mesh || fromA == nullptr || fromB == nullptr || fromA->points.size() != mesh->corners().size() ||
                    fromB->points.size() != mesh->corners().size()) {
                    diag::warnOnce(std::string("newborn face shape: ") + (!mesh ? "a base face is missing" : "a parent's face doesn't fit the base face") +
                                   ", so no face shape is made");
                    return std::nullopt;
                }
                shape.meshes.push_back({age, gender, group, blendMesh(*mesh, *fromA, *fromB, {age, gender, *info, masks[far ? 1 : 0], picks})});
            }
        }
    }
    return shape;
}

namespace {

constexpr std::uint32_t kModelType = 0xE519C933;
constexpr std::uint8_t kBabyShapeAge = 5, kAdultShapeAge = 3;
constexpr int kYoungAdultAge = 7;

using Quat = std::array<float, 4>;  // x, y, z, w
using Vec = std::array<float, 3>;

// The catalog face model whose grip nodes a new Sim's grips start from: the toddler's, child's,
// teen's and elder's own, and the adult's for every other age (a baby and a young adult too).
std::string gripFaceModel(int age, std::uint32_t genderFlag) {
    const char g = genderFlag == 1 ? 'f' : 'm';
    switch (age) {
    case 0: return "puFace_cres";
    case 1: return "cuFace_cres";
    case 2: return std::string("t") + g + "Face_cres";
    case 4: return std::string("e") + g + "Face_cres";
    default: return std::string("a") + g + "Face_cres";
    }
}

bool sameName(const std::string& a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// A model's transform node of a name (in any case), by block index.
std::optional<std::size_t> findNode(const rcol::Resource& model, std::string_view name) {
    for (std::size_t i = 0; i < model.blocks.size(); ++i) {
        if (const auto* t = std::get_if<rcol::TransformBlock>(&model.blocks[i]); t != nullptr && sameName(t->transform.tree.graph.name, name)) {
            return i;
        }
    }
    return std::nullopt;
}

// The transform node holding a block among its children.
std::optional<std::size_t> parentOf(const rcol::Resource& model, std::size_t block) {
    for (std::size_t i = 0; i < model.blocks.size(); ++i) {
        if (const auto* t = std::get_if<rcol::TransformBlock>(&model.blocks[i])) {
            for (const auto& r : t->transform.tree.children) {
                if (r.index == block) {
                    return i;
                }
            }
        }
    }
    return std::nullopt;
}

// The game's float steps, with `R` the precision the FPU works at between stores: double as a
// game session starts, float after its graphics device is reset (open question LT-3).

// The rotation `b` then `a` (b on the left), made unit length.
template <class R>
Quat compose(const Quat& a, const Quat& b) {
    const auto [ax, ay, az, aw] = a;
    const auto [bx, by, bz, bw] = b;
    const auto m = [](float p, float q) { return static_cast<R>(p) * static_cast<R>(q); };
    const float rx = static_cast<float>(((m(bx, aw) + m(by, az)) + m(bw, ax)) - m(bz, ay));
    const float ry = static_cast<float>(((m(by, aw) + m(bw, ay)) + m(bz, ax)) - m(bx, az));
    const float rz = static_cast<float>(((m(bz, aw) + m(bw, az)) + m(bx, ay)) - m(by, ax));
    const float rw = static_cast<float>(((m(bw, aw) - m(bx, ax)) - m(by, ay)) - m(bz, az));
    const R inv = R{1} / std::sqrt(((m(rx, rx) + m(ry, ry)) + m(rz, rz)) + m(rw, rw));
    return {static_cast<float>(rx * inv), static_cast<float>(ry * inv), static_cast<float>(rz * inv), static_cast<float>(rw * inv)};
}

// A block's world rotation: the top joint's own, then each joint's below composed with its parent's.
template <class R>
Quat worldRotation(const rcol::Resource& model, std::size_t block) {
    std::vector<std::size_t> chain{block};
    for (std::size_t guard = 0; guard < model.blocks.size(); ++guard) {
        const auto parent = parentOf(model, chain.back());
        if (!parent) {
            break;
        }
        chain.push_back(*parent);
    }
    const auto local = [&](std::size_t i) { return std::get<rcol::TransformBlock>(model.blocks[i]).transform.rotation; };
    Quat q = local(chain.back());
    for (auto it = chain.rbegin() + 1; it != chain.rend(); ++it) {
        q = compose<R>(local(*it), q);
    }
    return q;
}

// Turns `v` by the unit quaternion `q`.
template <class R>
Vec rotate(const Quat& q, const Vec& v) {
    const auto [x, y, z, w] = q;
    const auto two = [](float a, float b) {
        const R p = static_cast<R>(a) * static_cast<R>(b);
        return p + p;
    };
    const R xx = two(x, x), xy = two(y, x), zz = two(z, z), zw = two(z, w);
    const float xz = static_cast<float>(two(z, x)), xw = static_cast<float>(two(w, x)), yy = static_cast<float>(two(y, y)),
                yz = static_cast<float>(two(z, y)), yw = static_cast<float>(two(w, y));
    const R vx = v[0], vy = v[1], vz = v[2];
    const float outX = static_cast<float>((((R{1} - yy) - zz) * vx + (xy - zw) * vy) + (R{yw} + xz) * vz);
    const R a = R{1} - xx;
    const float af = static_cast<float>(a);
    const float outY = static_cast<float>(((a - zz) * vy + (R{yz} - xw) * vz) + (zw + xy) * vx);
    const float outZ = static_cast<float>(((R{af} - yy) * vz + (R{xz} - yw) * vx) + (R{yz} + xw) * vy);
    return {outX, outY, outZ};
}

// The mean move of the region's full-weight corners (a vertex counted for each corner using it).
// Nothing when no corner has full weight.
template <class R>
std::optional<Vec> meanMove(const FaceMesh& mesh, const std::vector<float>& blended, const std::vector<float>& w) {
    Vec sum{};
    std::uint32_t n = 0;
    for (const std::uint16_t v : mesh.corners()) {
        if (w[v] != 1.0f) {
            continue;
        }
        for (std::size_t k = 0; k < 3; ++k) {
            const float d = static_cast<float>(static_cast<R>(blended[v * 3u + k]) - static_cast<R>(mesh.base()[v * 3u + k]));
            sum[k] = static_cast<float>(static_cast<R>(d) + static_cast<R>(sum[k]));
        }
        ++n;
    }
    if (n == 0) {
        return std::nullopt;
    }
    const R f = R{1} / static_cast<R>(n);
    return Vec{static_cast<float>(sum[0] * f), static_cast<float>(sum[1] * f), static_cast<float>(f * sum[2])};
}

// The grips of a face built on the base face `mesh` whose vertices became `blended`.
template <class R>
std::vector<FaceGrip> gripsOf(Assets& assets, const FaceMesh& mesh, const FaceMasks& masks, const std::vector<float>& blended, int age,
                              std::uint32_t genderFlag, const std::string& skeletonName) {
    std::vector<FaceGrip> grips;
    const auto skeletonBytes = assets.byName(kModelType, skeletonName);
    const auto faceBytes = assets.byName(kModelType, gripFaceModel(age, genderFlag));
    if (!skeletonBytes) {
        diag::warnOnce("new face grips: no skeleton model, so no grips are made");
        return grips;
    }
    const rcol::Resource skeleton = rcol::read(*skeletonBytes);
    const std::optional<rcol::Resource> face = faceBytes ? std::optional(rcol::read(*faceBytes)) : std::nullopt;
    const std::uint8_t shapeAge = age == kYoungAdultAge ? kAdultShapeAge : static_cast<std::uint8_t>(age);
    for (const auto& [bone, region] : {std::pair<const char*, const char*>{"glasses_grip", "eyes"}, {"mouth_grip", "mouth"}}) {
        const auto node = findNode(skeleton, bone);
        const auto mask = masks.find(region);
        const auto move = node && mask != masks.end() ? meanMove<R>(mesh, blended, mesh.weights(mask->second)) : std::nullopt;
        if (!move) {
            continue;
        }
        const auto& own = std::get<rcol::TransformBlock>(skeleton.blocks[*node]).transform;
        FaceGrip grip{bone, own.translation, own.rotation};
        if (const auto faceNode = face ? findNode(*face, bone) : std::nullopt) {
            const auto& t = std::get<rcol::TransformBlock>(face->blocks[*faceNode]).transform;
            grip.translation = t.translation;
            grip.rotation = t.rotation;
        }
        // A baby's grips keep the face model's place.
        if (shapeAge != kBabyShapeAge) {
            const auto parent = parentOf(skeleton, *node);
            Quat world = parent ? worldRotation<R>(skeleton, *parent) : Quat{0, 0, 0, 1};
            world = {-world[0], -world[1], -world[2], world[3]};
            const Vec turned = rotate<R>(world, *move);
            for (std::size_t k = 0; k < 3; ++k) {
                grip.translation[k] = static_cast<float>(static_cast<R>(turned[k]) + static_cast<R>(grip.translation[k]));
            }
        }
        grips.push_back(grip);
    }
    return grips;
}

template <class R>
std::vector<FaceGrip> faceGrips(Assets& assets, const faceshape::FaceShape& a, const faceshape::FaceShape& b, const FacePicks& picks,
                                const std::string& skinTone, int age, std::uint32_t genderFlag, const std::string& skeletonName) {
    const auto info = faceInfo(assets);
    if (!info) {
        diag::warnOnce("newborn face grips: no FaceInfo, so no grips are made");
        return {};
    }
    const std::uint8_t shapeAge = age == kYoungAdultAge ? kAdultShapeAge : static_cast<std::uint8_t>(age);
    const std::uint8_t gender = genderFlag == 1 ? 1 : 0;
    const auto base = baseFace(assets, faceParts(assets), kAgeFlags[shapeAge], genderFlag, false, skinTone);
    const auto mesh = base ? faceMesh(*base) : std::nullopt;
    const auto* fromA = a.find(shapeAge, gender, 0);
    const auto* fromB = b.find(shapeAge, gender, 0);
    if (!mesh || fromA == nullptr || fromB == nullptr || fromA->points.size() != mesh->corners().size() ||
        fromB->points.size() != mesh->corners().size()) {
        diag::warnOnce("newborn face grips: a base face or parent's face is missing or doesn't fit, so no grips are made");
        return {};
    }
    const FaceMasks masks = cornerMasks(assets, kAgeFlags[shapeAge], false);
    const std::vector<float> blended = blendFace(*mesh, *fromA, *fromB, {shapeAge, gender, *info, masks, picks});
    return gripsOf<R>(assets, *mesh, masks, blended, age, genderFlag, skeletonName);
}

} // namespace

std::vector<FaceGrip> madeFaceGrips(Assets& assets, const std::vector<CatalogPart>& faceTemplates, const std::string& family,
                                    const std::string& skinTone, int age, std::uint32_t genderFlag, const std::string& skeletonName) {
    const std::uint8_t shapeAge = age == kYoungAdultAge ? kAdultShapeAge : static_cast<std::uint8_t>(age);
    const auto base = shapeAge < kAgeFlags.size() ? baseFace(assets, faceParts(assets), kAgeFlags[shapeAge], genderFlag, false, skinTone) : std::nullopt;
    const auto mesh = base ? faceMesh(*base) : std::nullopt;
    if (!mesh) {
        diag::warnOnce("made face grips: a base face is missing, so no grips are made");
        return {};
    }
    const FaceMasks masks = cornerMasks(assets, kAgeFlags[shapeAge], false);
    const auto faceTemplate = family.empty() ? std::nullopt : templateMesh(assets, faceTemplates, family, kAgeFlags[shapeAge], genderFlag, false);
    const std::vector<float> face = faceTemplate ? templateFace(*mesh, *faceTemplate, masks) : mesh->base();
    return gripsOf<double>(assets, *mesh, masks, face, age, genderFlag, skeletonName);
}

namespace {

// Normals made from the blended triangles, as the game makes them: each triangle corner takes its
// triangle's unit normal, or keeps the base's where the "normals" mask is above 0, then each vertex
// the unit-length sum of its corners' normals in corner order (a vertex of one corner keeps it).
std::vector<float> faceNormals(const std::vector<float>& positions, const std::vector<std::uint16_t>& corners, const std::vector<float>& own,
                               const std::vector<float>& keep) {
    const std::size_t vertices = positions.size() / 3;
    std::vector<std::array<float, 3>> cornerNormal(corners.size());
    std::vector<bool> made(corners.size(), false);
    for (std::size_t t = 0; t + 2 < corners.size(); t += 3) {
        const auto p = [&](std::size_t i, std::size_t k) { return positions[corners[t + i] * 3u + k]; };
        const std::array<float, 3> a{p(1, 0) - p(0, 0), p(1, 1) - p(0, 1), p(1, 2) - p(0, 2)};
        const std::array<float, 3> b{p(1, 0) - p(2, 0), p(1, 1) - p(2, 1), p(1, 2) - p(2, 2)};
        const double nx = static_cast<double>(b[1]) * a[2] - static_cast<double>(b[2]) * a[1];
        const float ny = static_cast<float>(static_cast<double>(a[0]) * b[2] - static_cast<double>(b[0]) * a[2]);
        const double nz = static_cast<double>(b[0]) * a[1] - static_cast<double>(a[0]) * b[1];
        const double length = (nz * nz + static_cast<double>(ny) * ny) + nx * nx;
        std::array<float, 3> n{};
        if (length != 0) {
            const double inv = 1.0 / std::sqrt(length);
            n = {static_cast<float>(nx * inv), static_cast<float>(ny * inv), static_cast<float>(nz * inv)};
        }
        for (std::size_t i = 0; i < 3; ++i) {
            const std::size_t c = t + i, v = corners[c];
            const bool make = keep[v] <= 0;
            made[c] = make;
            cornerNormal[c] = make ? n : std::array<float, 3>{own[v * 3], own[v * 3 + 1], own[v * 3 + 2]};
        }
    }
    std::vector<std::vector<std::size_t>> cornersOf(vertices);
    for (std::size_t c = 0; c < corners.size(); ++c) {
        cornersOf[corners[c]].push_back(c);
    }
    std::vector<float> normals = own;
    for (std::size_t v = 0; v < vertices; ++v) {
        const auto& list = cornersOf[v];
        if (list.empty()) {
            continue;
        }
        std::array<float, 3> result = cornerNormal[list.back()];
        bool anyMade = made[list.back()];
        if (list.size() > 1) {
            std::array<float, 3> sum{};
            for (const std::size_t c : list) {
                for (std::size_t k = 0; k < 3; ++k) {
                    sum[k] = sum[k] + cornerNormal[c][k];
                }
                anyMade = anyMade || made[c];
            }
            const double length = (static_cast<double>(sum[2]) * sum[2] + static_cast<double>(sum[1]) * sum[1]) + static_cast<double>(sum[0]) * sum[0];
            if (length > 0) {
                const double inv = 1.0 / std::sqrt(length);
                for (std::size_t k = 0; k < 3; ++k) {
                    sum[k] = static_cast<float>(sum[k] * inv);
                }
            }
            result = sum;
        }
        if (anyMade) {
            std::copy(result.begin(), result.end(), normals.begin() + static_cast<std::ptrdiff_t>(v * 3));
        }
    }
    return normals;
}

} // namespace

std::optional<NewbornFaceVertices> newbornFaceVertices(Assets& assets, const faceshape::FaceShape& a, const faceshape::FaceShape& b,
                                                       const FacePicks& picks, const std::string& skinTone, std::uint8_t age,
                                                       std::uint8_t gender, bool far) {
    const auto info = faceInfo(assets);
    const std::uint32_t genderFlag = gender == 0 ? 2 : 1;
    const auto base = info && age < kAgeFlags.size() ? baseFace(assets, faceParts(assets), kAgeFlags[age], genderFlag, far, skinTone) : std::nullopt;
    const auto mesh = base ? faceMesh(*base) : std::nullopt;
    const std::uint8_t group = far ? kFarGroup : 0;
    const auto* fromA = a.find(age, gender, group);
    const auto* fromB = b.find(age, gender, group);
    if (!mesh || fromA == nullptr || fromB == nullptr || fromA->points.size() != mesh->corners().size() ||
        fromB->points.size() != mesh->corners().size()) {
        return std::nullopt;
    }
    const FaceMasks masks = cornerMasks(assets, kAgeFlags[age], far);
    NewbornFaceVertices out;
    out.positions = blendFace(*mesh, *fromA, *fromB, {age, gender, *info, masks, picks});
    const auto own = sg::vertexFloats(*base, base->links[faceGroup(*base)->link], sg::kNormals);
    const auto keep = masks.find("normals");
    out.normals = own.size() == out.positions.size()
                      ? faceNormals(out.positions, mesh->corners(), own, keep != masks.end() ? mesh->weights(keep->second) : std::vector<float>(mesh->vertices(), 0.0f))
                      : own;
    return out;
}

std::optional<NewbornFaceVertices> madeFaceVertices(Assets& assets, const std::vector<CatalogPart>& faceTemplates, const std::string& family,
                                                    const std::string& skinTone, std::uint8_t age, std::uint8_t gender, bool far) {
    const std::uint32_t genderFlag = gender == 0 ? 2 : 1;
    const auto base = age < kAgeFlags.size() ? baseFace(assets, faceParts(assets), kAgeFlags[age], genderFlag, far, skinTone) : std::nullopt;
    const auto mesh = base ? faceMesh(*base) : std::nullopt;
    if (!mesh) {
        return std::nullopt;
    }
    const FaceMasks masks = cornerMasks(assets, kAgeFlags[age], far);
    std::optional<sg::Mesh> faceTemplate;
    try {
        faceTemplate = family.empty() ? std::nullopt : templateMesh(assets, faceTemplates, family, kAgeFlags[age], genderFlag, far);
    } catch (const std::exception& ex) {
        diag::warnOnce(std::string("made face: a face template can't be read: ") + ex.what());
        return std::nullopt;
    }
    NewbornFaceVertices out;
    out.positions = faceTemplate ? templateFace(*mesh, *faceTemplate, masks) : mesh->base();
    const auto own = sg::vertexFloats(*base, base->links[faceGroup(*base)->link], sg::kNormals);
    const auto keep = masks.find("normals");
    out.normals = own.size() == out.positions.size() && faceTemplate
                      ? faceNormals(out.positions, mesh->corners(), own, keep != masks.end() ? mesh->weights(keep->second) : std::vector<float>(mesh->vertices(), 0.0f))
                      : own;
    return out;
}

std::vector<FaceGrip> newbornFaceGrips(Assets& assets, const faceshape::FaceShape& a, const faceshape::FaceShape& b, const FacePicks& picks,
                                       const std::string& skinTone, int age, std::uint32_t genderFlag, const std::string& skeletonName,
                                       bool singlePrecision) {
    return singlePrecision ? faceGrips<float>(assets, a, b, picks, skinTone, age, genderFlag, skeletonName)
                           : faceGrips<double>(assets, a, b, picks, skinTone, age, genderFlag, skeletonName);
}


namespace {

constexpr std::uint32_t kTextureOverlay = 0x2C1FD8A1;
constexpr std::uint32_t kEyebrowOverlay = 1, kEyeOverlay = 3;

bool noGuid(const std::string& guid) {
    return guid.empty() || guid == "00000000-0000-0000-0000-000000000000";
}

bool sameGuid(std::string a, std::string b) {
    const auto lower = [](std::string& t) {
        std::transform(t.begin(), t.end(), t.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    };
    lower(a);
    lower(b);
    return a == b;
}

std::string textOf(const std::map<std::string, std::string>& p, const char* key) {
    const auto it = p.find(key);
    return it != p.end() ? it->second : std::string();
}

// The material (TXMT) a catalog part or overlay names by its resource list index `indexKey`
// ("materialkeyidx" for an overlay, "override0resourcekeyidx" for a part's first material).
std::optional<std::vector<std::uint8_t>> partMaterial(Assets& assets, const sg::Link& key, const std::map<std::string, std::string>& p,
                                                      const char* indexKey = "materialkeyidx") {
    const auto refs = assets.skinResource({reflist::kType, key.group, key.instance, key.instanceHigh});
    const auto links = refs ? reflist::parse(*refs) : std::vector<sg::Link>{};
    const std::size_t index = number(p, indexKey);
    return index < links.size() ? assets.byKey(links[index]) : std::nullopt;
}

std::string baseTextureOf(const std::optional<std::vector<std::uint8_t>>& material) {
    if (!material) {
        return {};
    }
    try {
        return textOf(sg::parseMaterial(*material).properties, "stdMatBaseTextureName");
    } catch (const std::exception& ex) {
        diag::warnOnce(std::string("newborn face material: a material that can't be read is left out: ") + ex.what());
        return {};
    }
}

} // namespace

std::optional<FaceMaterialParts> newbornFaceMaterialParts(Assets& assets, std::uint32_t ageFlag, std::uint32_t genderFlag, const std::string& skinTone,
                                                          const std::string& eyeColour, const std::string& hairTone, const std::string& browFamily) {
    FaceMaterialParts out;
    // The base face part's own material, picked as its mesh is.
    const auto parts = faceParts(assets);
    const FacePart* pick = nullptr;
    for (const auto& part : parts) {
        if (!(number(part.properties, "age") & ageFlag) || !(number(part.properties, "gender") & genderFlag)) {
            continue;
        }
        if (pick == nullptr) {
            pick = &part;
        }
        if (sameGuid(textOf(part.properties, "skintone"), skinTone)) {
            pick = &part;
            break;
        }
    }
    const auto material = pick != nullptr ? partMaterial(assets, pick->key, pick->properties, "override0resourcekeyidx") : std::nullopt;
    if (!material) {
        return std::nullopt;
    }
    out.catalogMaterial = *material;
    // The first eye overlay of the eye colour and eyebrow overlay of the brow family and hair tone
    // for the age, gender and skin tone, in catalog order.
    assets.forEachSkinResource(kTextureOverlay, [&](const sg::Link& key, const std::vector<std::uint8_t>& bytes) {
        try {
            const auto p = propset::parse(bytes);
            if (!(number(p, "age") & ageFlag) || !(number(p, "gender") & genderFlag) ||
                !(noGuid(textOf(p, "skintone")) || sameGuid(textOf(p, "skintone"), skinTone))) {
                return;
            }
            const auto subtype = number(p, "subtype");
            if (subtype == kEyeOverlay && out.eyeTexture.empty() && sameGuid(textOf(p, "family"), eyeColour)) {
                out.eyeTexture = baseTextureOf(partMaterial(assets, key, p));
            } else if (subtype == kEyebrowOverlay && out.browTexture.empty() && sameGuid(textOf(p, "family"), browFamily) &&
                       (noGuid(textOf(p, "hairtone")) || sameGuid(textOf(p, "hairtone"), hairTone))) {
                out.browTexture = baseTextureOf(partMaterial(assets, key, p));
            }
        } catch (const std::exception& ex) {
            diag::warnOnce(std::string("newborn face material: an overlay that can't be read is left out: ") + ex.what());
        }
    });
    return out;
}

} // namespace ts2::scene
