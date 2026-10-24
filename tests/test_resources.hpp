// Game resources made by hand, so asset loading and model building can be tested without the game's
// files: a package writer, the smallest scenegraph resources our readers accept (each written to
// match its reader in formats/scenegraph.cpp), and a game folder holding them.
#pragma once

#include "formats/hash.hpp"
#include "formats/scenegraph.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace testres {

constexpr std::uint32_t kCres = 0xE519C933, kShpe = 0xFC6EB1F7, kGmnd = 0x7BA3838C, kGmdc = 0xAC4F8687;
constexpr std::uint32_t kTxmt = 0x49596978, kTxtr = 0x1C4A276C;

// Little-endian bytes, as the game's files store numbers.
struct Bytes {
    std::vector<std::uint8_t> out;
    void u8(std::uint8_t v) { out.push_back(v); }
    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v));
        u8(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v) {
        u16(static_cast<std::uint16_t>(v));
        u16(static_cast<std::uint16_t>(v >> 16));
    }
    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
    // A string after its length in 7-bit groups.
    void text(const std::string& s) {
        std::size_t n = s.size();
        do {
            u8(static_cast<std::uint8_t>((n & 0x7F) | (n > 0x7F ? 0x80 : 0)));
            n >>= 7;
        } while (n > 0);
        out.insert(out.end(), s.begin(), s.end());
    }
    void bytes(const std::vector<std::uint8_t>& b) { out.insert(out.end(), b.begin(), b.end()); }
};

// A scenegraph resource's key by its name, in the scenegraph group.
inline ts2::scenegraph::Link keyOf(std::uint32_t type, const std::string& name) {
    const auto key = ts2::hash::scenegraphKey(name);
    return {type, ts2::scenegraph::kScenegraphGroup, key.instance, key.instanceHigh};
}

// The container every scenegraph resource starts with: its links (with their high instance words)
// and the type ids of its blocks.
inline void container(Bytes& b, const std::vector<ts2::scenegraph::Link>& links, const std::vector<std::uint32_t>& blockTypes) {
    b.u32(0xFFFF0001);
    b.u32(static_cast<std::uint32_t>(links.size()));
    for (const auto& l : links) {
        b.u32(l.group);
        b.u32(l.instance);
        b.u32(l.instanceHigh);
        b.u32(l.type);
    }
    b.u32(static_cast<std::uint32_t>(blockTypes.size()));
    for (const auto t : blockTypes) {
        b.u32(t);
    }
}

inline void blockHeader(Bytes& b, const std::string& name, std::uint32_t version) {
    b.text(name);
    b.u32(0);
    b.u32(version);
}

inline void sgResource(Bytes& b, const std::string& name) {
    blockHeader(b, "cSGResource", 2);
    b.text(name);
}

// A texture (TXTR, version 9) of one 32-bit mip level, given as RGBA rows from the top.
inline std::vector<std::uint8_t> texture(const std::string& name, std::uint32_t width, std::uint32_t height,
                                         const std::vector<std::uint8_t>& rgba) {
    Bytes b;
    container(b, {}, {kTxtr});
    blockHeader(b, "cImageData", 9);
    sgResource(b, name);
    b.u32(width);
    b.u32(height);
    b.u32(1);  // raw 32-bit
    b.u32(1);  // mip levels
    b.f32(1);
    b.u32(1);  // textures
    b.u32(0);
    b.text("");
    b.u32(1);  // this texture's mip levels
    b.u8(0);   // inline
    b.u32(static_cast<std::uint32_t>(rgba.size()));
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {  // stored blue, green, red, alpha
        b.u8(rgba[i + 2]);
        b.u8(rgba[i + 1]);
        b.u8(rgba[i]);
        b.u8(rgba[i + 3]);
    }
    b.u32(0);  // colour
    b.f32(1);  // bump amplitude
    return b.out;
}

// A material (TXMT) of a type with name-value properties.
inline std::vector<std::uint8_t> material(const std::string& name, const std::string& type,
                                          const std::vector<std::pair<std::string, std::string>>& properties) {
    Bytes b;
    container(b, {}, {kTxmt});
    blockHeader(b, "cMaterialDefinition", 11);
    sgResource(b, name);
    b.text(name);
    b.text(type);
    b.u32(static_cast<std::uint32_t>(properties.size()));
    for (const auto& [key, value] : properties) {
        b.text(key);
        b.text(value);
    }
    b.u32(0);  // texture names
    return b.out;
}

// A mesh (GMDC, version 4) of one triangle-list group: positions (three floats per vertex), and
// texture coordinates (two per vertex) when given.
inline std::vector<std::uint8_t> mesh(const std::string& name, const std::string& group, const std::vector<float>& positions,
                                      const std::vector<float>& uvs, const std::vector<std::uint16_t>& indices) {
    Bytes b;
    container(b, {}, {kGmdc});
    blockHeader(b, "cGeometryDataContainer", 4);
    sgResource(b, name);
    const std::uint32_t vertices = static_cast<std::uint32_t>(positions.size() / 3);
    auto element = [&](std::uint32_t identity, std::uint32_t blockFormat, const std::vector<float>& values) {
        b.u32(vertices);
        b.u32(identity);
        b.u32(1);  // repetition
        b.u32(blockFormat);
        b.u32(0);
        b.u32(static_cast<std::uint32_t>(values.size() * 4));
        for (const float v : values) {
            b.f32(v);
        }
        b.u32(0);  // its index list
    };
    const bool withUvs = !uvs.empty();
    b.u32(withUvs ? 2 : 1);
    element(ts2::scenegraph::kPositions, 2, positions);
    if (withUvs) {
        element(ts2::scenegraph::kTextureCoordinates, 1, uvs);
    }
    b.u32(1);  // links
    b.u32(withUvs ? 2 : 1);
    b.u16(0);
    if (withUvs) {
        b.u16(1);
    }
    b.u32(vertices);
    b.u32(withUvs ? 2 : 1);
    for (int alias = 0; alias < 3; ++alias) {
        b.u32(0);
    }
    b.u32(1);  // groups
    b.u32(ts2::scenegraph::kTriangleList);
    b.u32(0);  // its link
    b.text(group);
    b.u32(static_cast<std::uint32_t>(indices.size()));
    for (const auto i : indices) {
        b.u16(i);
    }
    b.u32(0xFFFFFFFF);  // opacity
    b.u32(0);           // bones
    b.u32(0);           // inverse binds
    return b.out;
}

// A geometry node (GMND): only its link to the mesh is read.
inline std::vector<std::uint8_t> geometryNode(const std::string& meshName) {
    Bytes b;
    container(b, {keyOf(kGmdc, meshName)}, {});
    return b.out;
}

// A shape (SHPE, version 8) drawing one geometry node, by name, with `material` on mesh group `group`.
inline std::vector<std::uint8_t> shape(const std::string& name, const std::string& geometryNodeName, const std::string& group,
                                       const std::string& materialName) {
    Bytes b;
    container(b, {}, {kShpe});
    blockHeader(b, "cShape", 8);
    sgResource(b, name);
    blockHeader(b, "cReferentNode", 1);
    blockHeader(b, "cObjectGraphNode", 4);
    b.u32(0);  // extensions
    b.text(name);
    b.u32(0);  // detail levels
    b.u32(1);  // items
    b.u32(0);  // its detail level
    b.u8(1);   // named
    b.text(geometryNodeName);
    b.u32(1);  // parts
    b.text(group);
    b.text(materialName);
    b.u8(0);
    b.u32(0);
    b.u32(0);
    return b.out;
}

// A resource node (CRES) with no node tree: the model then draws the first shape it links to.
inline std::vector<std::uint8_t> resourceNode(const std::string& shapeName) {
    Bytes b;
    container(b, {keyOf(kShpe, shapeName)}, {});
    return b.out;
}

// A resource node (CRES) whose tree is a root with one shape reference node, moved by `offset`, that
// draws the shape. A tree's blocks are found by their type id, which follows each class name.
inline std::vector<std::uint8_t> placedResourceNode(const std::string& name, const std::string& shapeName,
                                                    const std::array<float, 3>& offset) {
    constexpr std::uint32_t kResourceNodeBlock = 0xE519C933, kShapeRefBlock = 0x65245517;
    Bytes b;
    container(b, {keyOf(kShpe, shapeName)}, {kResourceNodeBlock, kShapeRefBlock});
    auto header = [&](const std::string& className, std::uint32_t id, std::uint32_t version) {
        b.text(className);
        b.u32(id);
        b.u32(version);
    };
    auto compositionTree = [&](const std::string& nodeName, const std::vector<std::uint32_t>& children) {
        header("cCompositionTreeNode", 0, 11);
        header("cObjectGraphNode", 0, 4);
        b.u32(0);  // extensions
        b.text(nodeName);
        b.u32(static_cast<std::uint32_t>(children.size()));
        for (const auto c : children) {
            b.u8(1);
            b.u8(0);
            b.u32(c);
        }
    };
    // The root: block 0, with the shape reference (block 1) as its child.
    header("cResourceNode", kResourceNodeBlock, 7);
    b.u8(1);
    sgResource(b, name);
    compositionTree(name, {1});
    // The shape reference: a transform node placed at `offset`, drawing the container's link 0.
    header("cShapeRefNode", kShapeRefBlock, 21);
    header("cRenderableNode", 0, 5);
    header("cBoundedNode", 0, 5);
    header("cTransformNode", 0, 7);
    compositionTree("shape", {});
    for (const float v : offset) {
        b.f32(v);
    }
    for (const float v : {0.0f, 0.0f, 0.0f, 1.0f}) {
        b.f32(v);
    }
    b.u32(0xFFFFFFFF);  // no joint
    b.u16(1);
    b.u32(1);  // render groups
    b.text("Practical");
    b.u32(0);
    b.u8(0);
    b.u32(1);  // shapes
    b.u8(1);
    b.u8(0);
    b.u32(0);  // the container's link 0
    return b.out;
}

struct Resource {
    ts2::scenegraph::Link key;
    std::vector<std::uint8_t> data;
};

// A package (DBPF 1.1, index 7.2 with high instance words) holding the resources uncompressed.
inline std::vector<std::uint8_t> package(const std::vector<Resource>& resources) {
    constexpr std::uint32_t kHeaderSize = 96;
    std::vector<std::uint8_t> data;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> places;
    for (const auto& r : resources) {
        places.push_back({kHeaderSize + static_cast<std::uint32_t>(data.size()), static_cast<std::uint32_t>(r.data.size())});
        data.insert(data.end(), r.data.begin(), r.data.end());
    }
    Bytes b;
    b.bytes({'D', 'B', 'P', 'F'});
    b.u32(1);
    b.u32(1);
    for (int i = 0; i < 5; ++i) {
        b.u32(0);
    }
    b.u32(7);  // index version
    b.u32(static_cast<std::uint32_t>(resources.size()));
    b.u32(kHeaderSize + static_cast<std::uint32_t>(data.size()));
    b.u32(static_cast<std::uint32_t>(resources.size() * 24));
    b.u32(0);
    b.u32(0);
    b.u32(0);
    b.u32(2);  // index minor version: entries carry the high instance word
    b.out.resize(kHeaderSize, 0);
    b.bytes(data);
    for (std::size_t i = 0; i < resources.size(); ++i) {
        const auto& k = resources[i].key;
        b.u32(k.type);
        b.u32(k.group);
        b.u32(k.instance);
        b.u32(k.instanceHigh);
        b.u32(places[i].first);
        b.u32(places[i].second);
    }
    return b.out;
}

inline void writeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// A game folder of one product (the base game) whose models package holds `models`. Returns its root.
inline std::filesystem::path gameFolder(const std::string& folderName, const std::vector<Resource>& models) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / folderName;
    std::filesystem::remove_all(root);
    const std::filesystem::path res = root / "base" / "TSData" / "Res";
    writeFile(res / "Objects" / "objects.package", package({}));
    writeFile(res / "Sims3D" / "Models.package", package(models));
    return root;
}

// A textured square model named `model`: its resource node, shape, geometry node, mesh, material and
// a 2 x 2 texture (red, green, blue, white).
inline std::vector<Resource> squareModel(const std::string& model) {
    const std::string shapeName = model + "_shpe", node = model + "_tslocator_gmnd", meshName = model + "_tslocator_gmdc";
    const std::string materialName = model + "_material", textureName = model + "_texture";
    const std::vector<float> positions{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    const std::vector<float> uvs{0, 1, 1, 1, 1, 0, 0, 0};
    const std::vector<std::uint8_t> pixels{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    return {
        {keyOf(kCres, model + "_cres"), resourceNode(shapeName)},
        {keyOf(kShpe, shapeName), shape(shapeName, node, "square", materialName)},
        {keyOf(kGmnd, node), geometryNode(meshName)},
        {keyOf(kGmdc, meshName), mesh(meshName, "square", positions, uvs, {0, 1, 2, 0, 2, 3})},
        {keyOf(kTxmt, materialName + "_txmt"),
         material(materialName + "_txmt", "StandardMaterial",
                  {{"stdMatBaseTextureName", textureName}, {"stdMatBaseTextureEnabled", "true"}})},
        {keyOf(kTxtr, textureName + "_txtr"), texture(textureName + "_txtr", 2, 2, pixels)},
    };
}

} // namespace testres
