#include "sim/character_models.hpp"

#include "formats/hash.hpp"
#include "formats/rcol.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <map>
#include <functional>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace ts2::sim {

namespace {

constexpr std::uint32_t kOwnGroup = 0xFFFFFFFF;
constexpr std::uint32_t kMeshType = 0xAC4F8687, kShapeType = 0xFC6EB1F7, kDataListType = 0x6A836D56;

std::string ageName(int age, bool far, std::string_view suffix) {
    return "age" + std::to_string(age) + "_0" + (far ? "LOD15" : "") + std::string(suffix);
}

} // namespace

std::uint32_t baseFaceAgeFlag(int age) {
    switch (age) {
    case 0: return 0x01;
    case 1: return 0x02;
    case 2: return 0x04;
    case 3:
    case 7: return 0x08;
    case 4: return 0x10;
    case 5: return 0x20;
    default: return 0;
    }
}

std::string ownGroupName(std::uint32_t group, std::string_view name) {
    char prefix[16];
    std::snprintf(prefix, sizeof prefix, "#0x%08x!", group);
    return prefix + std::string(name);
}

std::vector<std::uint8_t> newFaceNode(std::span<const std::uint8_t> catalogNode, int age, bool far, std::uint32_t group) {
    rcol::Resource node = rcol::read(catalogNode);
    auto* geometry = node.blocks.empty() ? nullptr : std::get_if<rcol::GeometryNode>(&node.blocks[0]);
    const bool lists = std::all_of(node.blockTypes.begin() + (node.blocks.empty() ? 0 : 1), node.blockTypes.end(),
                                   [](std::uint32_t t) { return t == kDataListType; });
    if (geometry == nullptr || node.links.size() != 1 || node.links[0].type != kMeshType || !lists) {
        throw std::runtime_error("a catalog face node that isn't one geometry node, a mesh link and data lists");
    }
    const hash::ScenegraphKey mesh = hash::scenegraphKey(ageName(age, far, "_gmdc"));
    node.links[0] = {kMeshType, kOwnGroup, mesh.instance, mesh.instanceHigh};
    geometry->name.name = ownGroupName(group, ageName(age, far, "_gmnd"));
    std::reverse(node.blocks.begin() + 1, node.blocks.end());
    return rcol::write(node);
}

std::vector<std::uint8_t> newFaceShape(int age, std::uint32_t group, std::string_view material) {
    rcol::Shape shape;
    shape.header = {"cShape", kShapeType, 8};
    shape.name = {{"cSGResource", 0, 2}, ownGroupName(group, "age" + std::to_string(age) + "_00_shpe")};
    shape.referent = {"cReferentNode", 0, 1};
    shape.graph.header = {"cObjectGraphNode", 0, 4};
    shape.items.push_back({0, 1, ownGroupName(group, ageName(age, false, "_gmnd")), 0, 0});
    shape.items.push_back({15, 1, ownGroupName(group, ageName(age, true, "_gmnd")), 0, 0});
    shape.parts.push_back({"face", std::string(material), 0, {}, 0});
    rcol::Resource resource;
    resource.blocks.push_back(std::move(shape));
    resource.blockTypes.push_back(kShapeType);
    return rcol::write(resource);
}

} // namespace ts2::sim

namespace ts2::sim {

namespace {

constexpr std::uint32_t kShapeRefType = 0x65245517, kTransformType = 0x65246462, kBoneDataVersion = 5;
// The number at a model's end, which every face model has (a skeleton has 2, hair 10). What it
// means is not known yet.
constexpr std::uint32_t kFaceModelValue = 9;
// A teen's upper arms take this as their bone data's third word, where the skeleton they share with
// adults has 3. What the word means is not known yet.
constexpr std::uint32_t kTeenUpperArmWord = 1;
constexpr int kTeenAge = 2;
// A shape reference's joint when it follows none.
constexpr std::uint32_t kNoJoint = 0x7FFFFFFF;
constexpr std::size_t kShapeRefDataBytes = 16;

// The lists of block references a block holds: its children and its extensions. Null for blocks
// without them.
struct BlockRefs {
    std::vector<rcol::Ref>* children = nullptr;
    std::vector<rcol::Ref>* extensions = nullptr;
};

BlockRefs refsOf(rcol::Block& block) {
    return std::visit(
        [](auto& b) -> BlockRefs {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, rcol::ResourceNode>) {
                return b.root != 0 ? BlockRefs{&b.tree.children, &b.tree.graph.extensions} : BlockRefs{nullptr, &b.graph.extensions};
            } else if constexpr (std::is_same_v<T, rcol::TransformBlock>) {
                return {&b.transform.tree.children, &b.transform.tree.graph.extensions};
            } else if constexpr (std::is_same_v<T, rcol::ShapeRef> || std::is_same_v<T, rcol::LightRef> || std::is_same_v<T, rcol::ViewerRef>) {
                return {&b.node.transform.tree.children, &b.node.transform.tree.graph.extensions};
            } else {
                return {};
            }
        },
        block);
}

std::string nodeName(const rcol::Block& block) {
    if (const auto* t = std::get_if<rcol::TransformBlock>(&block)) {
        return t->transform.tree.graph.name;
    }
    return {};
}

rcol::DataList gripList(const GripAdjust& grip) {
    rcol::DataList list;
    list.header = {"cDataListExtension", kDataListType, 1};
    list.extension = {"cExtension", 0, 3};
    list.value.type = 7;
    list.value.name = "bone_adjust";
    rcol::Value tag, translation, rotation;
    tag.type = 6;
    tag.name = "bone_adjust_bone_tag";
    tag.text = grip.bone;
    translation.type = 5;
    translation.name = "bone_adjust_translation";
    translation.floats.assign(grip.translation.begin(), grip.translation.end());
    rotation.type = 8;
    rotation.name = "bone_adjust_rotation";
    rotation.floats.assign(grip.rotation.begin(), grip.rotation.end());
    list.value.items = {tag, translation, rotation};
    return list;
}

rcol::ShapeRef faceShapeRef(std::span<const std::string> morphs) {
    rcol::ShapeRef ref;
    ref.node.header = {"cShapeRefNode", kShapeRefType, 21};
    ref.node.renderable = {"cRenderableNode", 0, 5};
    ref.node.bounded = {"cBoundedNode", 0, 5};
    ref.node.transform.header = {"cTransformNode", kTransformType, 7};
    ref.node.transform.tree.header = {"cCompositionTreeNode", 0, 11};
    ref.node.transform.tree.graph.header = {"cObjectGraphNode", 0, 4};
    ref.node.transform.rotation = {0, 0, 0, 1};
    ref.node.transform.joint = kNoJoint;
    ref.node.flags = 1;
    ref.node.groups = {"Practical"};
    ref.node.byte = 1;
    ref.shapes = {{1, 1, 0}};  // the model's first link, the face shape
    ref.values.assign(morphs.size(), 0);
    ref.names.assign(morphs.begin(), morphs.end());
    ref.data.assign(kShapeRefDataBytes, 0);
    ref.c = 0xFFFFFFFF;
    return ref;
}

void forEachRef(BlockRefs refs, const std::function<void(rcol::Ref&)>& visit) {
    for (auto* list : {refs.extensions, refs.children}) {
        if (list != nullptr) {
            for (auto& r : *list) {
                visit(r);
            }
        }
    }
}

} // namespace

std::vector<std::uint8_t> newFaceMesh(std::span<const std::uint8_t> catalogMesh, int age, bool far, std::uint32_t group,
                                      std::span<const float> positions, std::span<const float> normals) {
    constexpr std::uint32_t kPositions = 0x5B830781, kNormals = 0x3B83078B, kTriangleList = 2;
    rcol::Resource mesh = rcol::read(catalogMesh);
    auto* m = mesh.blocks.size() == 1 ? std::get_if<rcol::Mesh>(&mesh.blocks[0]) : nullptr;
    const auto faceGroup = m != nullptr ? std::find_if(m->groups.begin(), m->groups.end(), [](const rcol::Mesh::Group& g) { return g.primitive == kTriangleList; })
                                        : std::vector<rcol::Mesh::Group>::iterator{};
    if (m == nullptr || faceGroup == m->groups.end() || faceGroup->link >= m->links.size()) {
        throw std::runtime_error("a catalog face mesh without a face to draw");
    }
    const auto replace = [&](std::uint32_t identity, std::span<const float> values) {
        for (const std::uint16_t e : m->links[faceGroup->link].elements) {
            if (e < m->elements.size() && m->elements[e].identity == identity && m->elements[e].stream == 0) {
                auto& data = m->elements[e].data;
                if (data.size() != values.size() * sizeof(float)) {
                    throw std::runtime_error("a face mesh's vertices don't fit the catalog face mesh");
                }
                std::memcpy(data.data(), values.data(), data.size());
                return;
            }
        }
        throw std::runtime_error("a catalog face mesh without positions or normals");
    };
    replace(kPositions, positions);
    replace(kNormals, normals);
    m->name.name = ownGroupName(group, "age" + std::to_string(age) + "_0" + (far ? "LOD15" : "") + "_gmdc");
    return rcol::write(mesh);
}

std::string faceMaterialName(std::uint32_t counter, int age, std::uint32_t paramGender) {
    char name[64];
    std::snprintf(name, sizeof name, "ss_customPart_0x%x_%d_%u_cmpm", counter, age, paramGender);
    return name;
}

std::vector<std::uint8_t> newFaceMaterial(std::span<const std::uint8_t> catalogMaterial, std::uint32_t group, const FaceMaterialLook& look) {
    rcol::Resource resource = rcol::read(catalogMaterial);
    auto* material = resource.blocks.size() == 1 ? std::get_if<rcol::Material>(&resource.blocks[0]) : nullptr;
    if (material == nullptr) {
        throw std::runtime_error("a catalog face material that isn't one material");
    }
    std::map<std::string, std::string> properties(material->properties.begin(), material->properties.end());
    const std::string base = properties["stdMatBaseTextureName"];
    std::vector<std::string> composite{base};
    for (const std::string* texture : {&look.eyeTexture, &look.browTexture}) {
        if (!texture->empty()) {
            composite.push_back(*texture);
        }
    }
    std::string compositeName;
    for (std::size_t i = 0; i < composite.size(); ++i) {
        properties["baseTexture" + std::to_string(i)] = composite[i];
        compositeName += "_" + composite[i];
    }
    std::replace(compositeName.begin(), compositeName.end(), '-', '_');
    compositeName += "-diffuse";
    // The overlays are the eye colour and the eyebrow family, each a list of one GUID.
    std::vector<std::string> overlays;
    const std::array<const std::string*, 2> order = look.browFirst ? std::array{&look.browFamily, &look.eyeColour}
                                                                  : std::array{&look.eyeColour, &look.browFamily};
    for (const std::string* family : order) {
        if (!family->empty()) {
            overlays.push_back(*family + ",");
        }
    }
    properties["cafHairTone"] = look.hairTone;
    properties["cafNumOverlays"] = std::to_string(overlays.size());
    for (std::size_t i = 0; i < overlays.size(); ++i) {
        properties["cafOverlay" + std::to_string(i)] = overlays[i];
    }
    properties["cafSkinTone"] = look.skinTone;
    properties["compositeBaseTextureName"] = compositeName;
    properties["eyeGizmoEnable"] = "true";
    properties["numTexturesToComposite"] = std::to_string(composite.size());
    properties["paramAge"] = std::to_string(look.age);
    properties["paramGender"] = std::to_string(look.paramGender);
    properties["paramSpecies"] = "0";
    material->properties.assign(properties.begin(), properties.end());
    material->type = "SimSkin";
    std::string lower = look.name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    material->description = lower;
    material->name.name = ownGroupName(group, look.name + "_txmt");
    material->textures = {compositeName, base};
    return rcol::write(resource);
}

std::string skeletonModelName(int age) {
    switch (age) {
    case 5: return "buSkel_cres";
    case 0: return "puSkel_cres";
    case 1: return "cuSkel_cres";
    case 2:
    case 3:
    case 4:
    case 7: return "auSkel_cres";
    default: return {};
    }
}

std::vector<std::uint8_t> newFaceModel(std::span<const std::uint8_t> skeleton, int age, std::uint32_t group,
                                       std::span<const GripAdjust> grips, std::span<const std::string> morphs) {
    rcol::Resource model = rcol::read(skeleton);
    auto* root = model.blocks.empty() ? nullptr : std::get_if<rcol::ResourceNode>(&model.blocks[0]);
    if (root == nullptr || root->root == 0 || root->tree.children.size() != 1) {
        throw std::runtime_error("a skeleton model that isn't a model with one top joint");
    }
    for (auto& block : model.blocks) {
        const BlockRefs refs = refsOf(block);
        if (refs.children != nullptr) {
            std::reverse(refs.children->begin(), refs.children->end());
        }
        if (refs.extensions != nullptr) {
            std::reverse(refs.extensions->begin(), refs.extensions->end());
        }
        if (auto* bone = std::get_if<rcol::BoneData>(&block)) {
            bone->header.version = kBoneDataVersion;
        }
        if (age == kTeenAge && (nodeName(block) == "l_upperarm" || nodeName(block) == "r_upperarm") && refs.extensions != nullptr) {
            for (const auto& r : *refs.extensions) {
                if (auto* bone = r.index < model.blocks.size() ? std::get_if<rcol::BoneData>(&model.blocks[r.index]) : nullptr) {
                    bone->words[2] = kTeenUpperArmWord;
                }
            }
        }
    }
    // The grips' lists and the shape reference go after the skeleton's blocks until the blocks are
    // put in order.
    const std::size_t skeletonBlocks = model.blocks.size();
    std::vector<rcol::Ref> gripRefs;
    for (const auto& grip : grips) {
        gripRefs.push_back({1, 0, static_cast<std::uint32_t>(model.blocks.size())});
        model.blocks.push_back(gripList(grip));
        model.blockTypes.push_back(kDataListType);
    }
    const auto shapeRef = static_cast<std::uint32_t>(model.blocks.size());
    model.blocks.push_back(faceShapeRef(morphs));
    model.blockTypes.push_back(kShapeRefType);
    root = &std::get<rcol::ResourceNode>(model.blocks[0]);
    root->tree.graph.extensions.insert(root->tree.graph.extensions.begin(), gripRefs.begin(), gripRefs.end());
    const std::uint32_t top = root->tree.children[0].index;
    const BlockRefs topRefs = top < skeletonBlocks ? refsOf(model.blocks[top]) : BlockRefs{};
    if (topRefs.children == nullptr) {
        throw std::runtime_error("a skeleton model whose top joint isn't a joint");
    }
    topRefs.children->push_back({1, 0, shapeRef});
    const auto head = std::find_if(topRefs.children->begin(), topRefs.children->end(), [&](const rcol::Ref& r) {
        return r.index < model.blocks.size() && nodeName(model.blocks[r.index]) == "head_ikctr";
    });
    std::rotate(topRefs.children->begin(), head, topRefs.children->end());
    // Breadth first from the model, extensions before children.
    std::vector<std::uint32_t> order{0};
    std::vector<std::int64_t> newIndex(model.blocks.size(), -1);
    newIndex[0] = 0;
    for (std::size_t at = 0; at < order.size(); ++at) {
        forEachRef(refsOf(model.blocks[order[at]]), [&](rcol::Ref& r) {
            if (r.index >= model.blocks.size()) {
                throw std::runtime_error("a skeleton model naming a block it doesn't have");
            }
            if (newIndex[r.index] < 0) {
                newIndex[r.index] = static_cast<std::int64_t>(order.size());
                order.push_back(r.index);
            }
        });
    }
    if (order.size() != model.blocks.size()) {
        throw std::runtime_error("a skeleton model with blocks its tree doesn't reach");
    }
    rcol::Resource out;
    const hash::ScenegraphKey shapeKey = hash::scenegraphKey("age" + std::to_string(age) + "_00_shpe");
    out.links.push_back({kShapeType, kOwnGroup, shapeKey.instance, shapeKey.instanceHigh});
    for (const std::uint32_t old : order) {
        out.blocks.push_back(std::move(model.blocks[old]));
        out.blockTypes.push_back(model.blockTypes[old]);
    }
    for (auto& block : out.blocks) {
        forEachRef(refsOf(block), [&](rcol::Ref& r) { r.index = static_cast<std::uint32_t>(newIndex[r.index]); });
    }
    auto& model0 = std::get<rcol::ResourceNode>(out.blocks[0]);
    model0.name.name = ownGroupName(group, "age" + std::to_string(age) + "_0_cres");
    model0.value = kFaceModelValue;
    return rcol::write(out);
}

} // namespace ts2::sim
