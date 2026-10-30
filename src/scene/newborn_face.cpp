#include "scene/newborn_face.hpp"

#include "scene/face_blend.hpp"

#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "formats/propset.hpp"
#include "formats/reference_list.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>

namespace ts2::scene {

namespace {

namespace sg = scenegraph;

constexpr std::uint32_t kFaceRegion = 0x8C93BF6C, kFaceInfo = 0x46414345;
constexpr std::uint32_t kShpe = 0xFC6EB1F7, kGmnd = 0x7BA3838C, kGmdc = 0xAC4F8687;
constexpr std::uint32_t kBabyAgeFlag = 0x20;

// The first mesh a shape draws.
std::optional<sg::Mesh> shapeMesh(Assets& assets, const sg::Link& link) {
    const auto shpe = assets.byKey(link);
    if (!shpe) {
        return std::nullopt;
    }
    const sg::Shape shape = sg::parseShape(*shpe);
    for (std::size_t i = 0; i < shape.geometryNodes.size() || i < shape.geometryLinks.size(); ++i) {
        const auto linked = i < shape.geometryLinks.size() ? shape.geometryLinks[i] : std::nullopt;
        const auto gmnd = linked ? (*linked < shape.container.links.size() ? assets.byKey(shape.container.links[*linked]) : std::nullopt)
                                 : assets.byName(kGmnd, shape.geometryNodes[i]);
        if (!gmnd) {
            continue;
        }
        for (const auto& l : sg::parseContainer(*gmnd).links) {
            if (l.type == kGmdc) {
                if (const auto bytes = assets.byKey(l)) {
                    return sg::parseMesh(*bytes);
                }
            }
        }
    }
    return std::nullopt;
}

// The baby face's region masks (face region property sets for babies), by region name. A mask
// point's weight is its vertex colour's alpha.
FaceMasks babyMasks(Assets& assets) {
    FaceMasks masks;
    assets.forEachSkinResource(kFaceRegion, [&](const sg::Link& key, const std::vector<std::uint8_t>& bytes) {
        try {
            const auto p = propset::parse(bytes);
            const auto name = p.find("name");
            const auto age = p.find("age");
            if (name == p.end() || age == p.end() || !(std::strtoul(age->second.c_str(), nullptr, 10) & kBabyAgeFlag) ||
                masks.contains(name->second)) {
                return;
            }
            const auto refs = assets.skinResource({reflist::kType, key.group, key.instance, key.instanceHigh});
            if (!refs) {
                return;
            }
            for (const auto& l : reflist::parse(*refs)) {
                if (l.type != kShpe) {
                    continue;
                }
                const auto mesh = shapeMesh(assets, l);
                if (!mesh) {
                    break;
                }
                masks[name->second] = maskPoints(*mesh);
                break;
            }
        } catch (const std::exception& ex) {
            diag::warnOnce(std::string("newborn face: a face region that can't be read is left out: ") + ex.what());
        }
    });
    return masks;
}

// The face pairs, in the order the blend takes their regions (FaceInfo).
std::vector<FacePair> facePairs(Assets& assets) {
    std::vector<FacePair> pairs;
    assets.forEachSkinResource(kFaceInfo, [&](const sg::Link&, const std::vector<std::uint8_t>& bytes) {
        if (!pairs.empty()) {
            return;
        }
        try {
            pairs = readFacePairs(bytes);
        } catch (const std::exception& ex) {
            pairs.clear();
            diag::warnOnce(std::string("newborn face: the face pairs can't be read: ") + ex.what());
        }
    });
    return pairs;
}

// What a newborn's face is blended from.
struct Blend {
    std::optional<faceshape::FaceShape> a, b;
    std::vector<std::pair<std::string, bool>> picks;
    std::shared_ptr<NewbornFaceData> data;
};

void blend(Assets& assets, sg::Mesh& mesh, Blend& from) {
    if (!blendsBabyFace(from.a, from.b, from.picks)) {
        return;
    }
    auto& data = *from.data;
    if (!data.masks) {
        data.masks = babyMasks(assets);
        data.pairs = facePairs(assets);
    }
    blendBabyFace(mesh, from.a, from.b, from.picks, *data.masks, data.pairs);
}

} // namespace

MeshEdit newbornFaceEdit(Assets& assets, std::shared_ptr<NewbornFaceData> data, std::string key, std::optional<faceshape::FaceShape> a,
                         std::optional<faceshape::FaceShape> b, std::vector<std::pair<std::string, bool>> picks) {
    auto from = std::make_shared<Blend>(Blend{std::move(a), std::move(b), std::move(picks), data ? std::move(data) : std::make_shared<NewbornFaceData>()});
    return {std::move(key), [&assets, from](sg::Mesh& mesh) { blend(assets, mesh, *from); }};
}

std::optional<faceshape::FaceShape> faceShapeOf(const std::filesystem::path& characterFile) {
    try {
        dbpf::Package package{characterFile};
        for (const auto& e : package.entries()) {
            if (e.key.type == faceshape::kType && e.key.instance == 1) {
                return faceshape::parse(package.read(e));
            }
        }
    } catch (const std::exception& ex) {
        diag::warnOnce(std::string("newborn face: a parent's face shape can't be read: ") + ex.what());
    }
    return std::nullopt;
}

} // namespace ts2::scene
