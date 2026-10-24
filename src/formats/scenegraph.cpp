#include "formats/scenegraph.hpp"

#include "formats/binary_reader.hpp"
#include "formats/tga.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <string>

namespace ts2::scenegraph {

namespace {

constexpr std::uint32_t kMaxCount = 1u << 20;

void fail(const std::string& what) {
    throw std::runtime_error("scenegraph: " + what);
}

std::size_t count(BinaryReader& r, std::size_t minBytes) {
    const std::size_t n = r.u32Count(minBytes);
    if (n > kMaxCount) {
        fail("bad count " + std::to_string(n));
    }
    return n;
}

Container readContainer(BinaryReader& r) {
    Container c;
    std::uint32_t links = r.u32();
    const bool withHigh = (links & 0xFFFF0000) == 0xFFFF0000;
    if (withHigh) {
        links = r.u32();
    }
    if (links > 4096) {
        fail("too many links");
    }
    for (std::uint32_t i = 0; i < links; ++i) {
        Link l;
        l.group = r.u32();
        l.instance = r.u32();
        if (withHigh) {
            l.instanceHigh = r.u32();
        }
        l.type = r.u32();
        c.links.push_back(l);
    }
    c.blockTypes.resize(count(r, 4));
    for (auto& t : c.blockTypes) {
        t = r.u32();
    }
    return c;
}

struct BlockHeader {
    std::string name;
    std::uint32_t id = 0;
    std::uint32_t version = 0;
};

BlockHeader blockHeader(BinaryReader& r, std::string_view expected) {
    BlockHeader h;
    h.name = r.varString();
    h.id = r.u32();
    h.version = r.u32();
    if (h.name != expected) {
        fail("expected " + std::string(expected) + ", found " + h.name);
    }
    return h;
}

// cSGResource: the resource's own name.
std::string sgResource(BinaryReader& r) {
    blockHeader(r, "cSGResource");
    return r.varString();
}

// cObjectGraphNode: extensions (skipped) and, from version 4, a name.
void objectGraphNode(BinaryReader& r) {
    const BlockHeader h = blockHeader(r, "cObjectGraphNode");
    const std::size_t extensions = count(r, 6);
    for (std::size_t i = 0; i < extensions; ++i) {
        r.u8();
        r.u8();
        r.u32();
    }
    if (h.version >= 4) {
        r.varString();
    }
}

} // namespace

Container parseContainer(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    return readContainer(r);
}

namespace {

// A cCompositionTreeNode: the object graph node (its extensions as enabled, kind, block index), then
// children the same way.
std::vector<std::uint32_t> compositionTree(BinaryReader& r, std::string* name, std::vector<std::uint32_t>* extensionBlocks = nullptr) {
    blockHeader(r, "cCompositionTreeNode");
    const BlockHeader graph = blockHeader(r, "cObjectGraphNode");
    const std::size_t extensions = count(r, 6);
    for (std::size_t i = 0; i < extensions; ++i) {
        r.u8();
        r.u8();
        const std::uint32_t block = r.u32();
        if (extensionBlocks) {
            extensionBlocks->push_back(block);
        }
    }
    if (graph.version >= 4) {
        *name = r.varString();
    }
    std::vector<std::uint32_t> children;
    const std::size_t n = count(r, 6);
    for (std::size_t i = 0; i < n; ++i) {
        r.u8();
        r.u8();
        children.push_back(r.u32());
    }
    return children;
}

void transformNode(BinaryReader& r, SceneNode& node) {
    blockHeader(r, "cTransformNode");
    node.children = compositionTree(r, &node.name, &node.extensions);
    for (auto& v : node.translation) {
        v = std::bit_cast<float>(r.u32());
    }
    for (auto& v : node.rotation) {
        v = std::bit_cast<float>(r.u32());
    }
    node.joint = static_cast<std::int32_t>(r.u32());
}

// Where block `type` starts at or after `from`: its class name (a short string starting with
// 'c') followed by the type id. Returns the offset of the name's length byte.
std::optional<std::size_t> findBlock(std::span<const std::uint8_t> data, std::size_t from, std::uint32_t type) {
    for (std::size_t i = from + 2; i + 4 <= data.size(); ++i) {
        if (data[i] != static_cast<std::uint8_t>(type) || data[i + 1] != static_cast<std::uint8_t>(type >> 8) ||
            data[i + 2] != static_cast<std::uint8_t>(type >> 16) || data[i + 3] != static_cast<std::uint8_t>(type >> 24)) {
            continue;
        }
        for (std::size_t length = 4; length < 64 && length < i; ++length) {
            if (data[i - length - 1] == length && data[i - length] == 'c' && i - length - 1 >= from) {
                return i - length - 1;
            }
        }
    }
    return std::nullopt;
}

} // namespace

ResourceNode parseResourceNode(std::span<const std::uint8_t> data) {
    constexpr std::uint32_t kResourceNode = 0xE519C933, kTransformNode = 0x65246462, kShapeRefNode = 0x65245517, kLightRefNode = 0x253D2018,
                            kBoneData = 0xE9075BC5;
    BinaryReader header(data);
    ResourceNode rn;
    rn.container = readContainer(header);
    std::size_t at = data.size() - header.remaining();
    for (const std::uint32_t type : rn.container.blockTypes) {
        SceneNode node;
        const auto start = findBlock(data, at, type);
        if (!start) {
            break;
        }
        BinaryReader r(data.subspan(*start));
        try {
            if (type == kResourceNode) {
                node.kind = SceneNode::Kind::Root;
                blockHeader(r, "cResourceNode");
                r.u8();
                node.name = sgResource(r);
                std::string ignored;
                node.children = compositionTree(r, &ignored);
            } else if (type == kTransformNode) {
                node.kind = SceneNode::Kind::Transform;
                transformNode(r, node);
            } else if (type == kShapeRefNode) {
                node.kind = SceneNode::Kind::ShapeRef;
                blockHeader(r, "cShapeRefNode");
                blockHeader(r, "cRenderableNode");
                blockHeader(r, "cBoundedNode");
                transformNode(r, node);
                r.u16();
                const std::size_t groups = count(r, 1);
                for (std::size_t i = 0; i < groups; ++i) {
                    r.varString();  // render group, e.g. "Practical"
                }
                r.u32();
                r.u8();
                const std::size_t shapes = count(r, 6);
                for (std::size_t i = 0; i < shapes; ++i) {
                    r.u8();
                    r.u8();
                    node.shapeLinks.push_back(r.u32());
                }
            } else if (type == kLightRefNode) {
                node.kind = SceneNode::Kind::LightRef;
                blockHeader(r, "cLightRefNode");
                blockHeader(r, "cRenderableNode");
                blockHeader(r, "cBoundedNode");
                transformNode(r, node);
            } else if (type == kBoneData) {
                blockHeader(r, "cBoneDataExtension");
                blockHeader(r, "cExtension");
                SceneNode::BoneData bone;
                for (auto& w : bone.words) {
                    w = r.u32();
                }
                bone.weight = std::bit_cast<float>(r.u32());
                for (auto& v : bone.rotation) {
                    v = std::bit_cast<float>(r.u32());
                }
                node.boneData = bone;
            } else {
                r.bytes(5);
            }
        } catch (const std::exception&) {
            node.kind = SceneNode::Kind::Other;
        }
        at = *start + (data.subspan(*start).size() - r.remaining());
        rn.nodes.push_back(std::move(node));
    }
    return rn;
}

namespace {

// Limits for data lists, which files nest and size freely: how deep lists may nest (each level is a
// call), and how many values one resource may hold in all (each costs far more memory than its bytes).
constexpr int kMaxDataListDepth = 16;
constexpr std::size_t kMaxDataValues = 1u << 16;

DataValue dataValue(BinaryReader& r, int depth, std::size_t& budget) {
    if (depth > kMaxDataListDepth) {
        fail("data list too deep");
    }
    if (budget == 0) {
        fail("too many data values");
    }
    --budget;
    DataValue v;
    v.type = static_cast<DataValue::Type>(r.u8());
    v.name = r.varString();
    auto floats = [&](int n) {
        for (int i = 0; i < n; ++i) {
            v.floats.push_back(std::bit_cast<float>(r.u32()));
        }
    };
    switch (v.type) {
    case DataValue::Type::Bool: v.integer = r.u8() != 0 ? 1 : 0; break;
    case DataValue::Type::Int: v.integer = static_cast<std::int32_t>(r.u32()); break;
    case DataValue::Type::Float: floats(1); break;
    case DataValue::Type::Vector2: floats(2); break;
    case DataValue::Type::Vector: floats(3); break;
    case DataValue::Type::Rotation: floats(4); break;
    case DataValue::Type::String: v.text = r.varString(); break;
    case DataValue::Type::Bytes: {
        const auto bytes = r.bytes(r.u32Count(1));
        v.bytes.assign(bytes.begin(), bytes.end());
        break;
    }
    case DataValue::Type::List: {
        const std::size_t n = count(r, 3);  // the smallest value (an empty string) takes 3 bytes
        v.items.reserve(std::min(n, budget));
        for (std::size_t i = 0; i < n; ++i) {
            v.items.push_back(dataValue(r, depth + 1, budget));
        }
        break;
    }
    default: fail("unknown data value type " + std::to_string(static_cast<int>(v.type)));
    }
    return v;
}

} // namespace

const DataValue* DataValue::find(std::string_view itemName) const {
    for (const auto& item : items) {
        if (item.name == itemName) {
            return &item;
        }
    }
    return nullptr;
}

std::vector<DataValue> parseDataLists(std::span<const std::uint8_t> data) {
    constexpr std::uint32_t kDataList = 0x6A836D56;
    BinaryReader header(data);
    const Container container = readContainer(header);
    std::vector<DataValue> lists;
    std::size_t budget = kMaxDataValues;
    std::size_t at = data.size() - header.remaining();
    for (const std::uint32_t type : container.blockTypes) {
        if (type != kDataList) {
            continue;
        }
        const auto start = findBlock(data, at, type);
        if (!start) {
            break;
        }
        BinaryReader r(data.subspan(*start));
        blockHeader(r, "cDataListExtension");
        blockHeader(r, "cExtension");
        lists.push_back(dataValue(r, 0, budget));
        at = *start + (data.subspan(*start).size() - r.remaining());
    }
    return lists;
}

const Element* Mesh::find(const ElementLink& link, std::uint32_t identity) const {
    for (const std::uint16_t i : link.elements) {
        if (i < elements.size() && elements[i].identity == identity) {
            return &elements[i];
        }
    }
    return nullptr;
}

Mesh parseMesh(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    Mesh m;
    m.container = readContainer(r);
    m.version = blockHeader(r, "cGeometryDataContainer").version;
    if (m.version < 1 || m.version > 4) {
        fail("mesh version " + std::to_string(m.version) + " not supported");
    }
    // Before version 3 every index list stores its entries as u32s. They are read as u16s.
    const bool wideIndices = m.version < 3;
    auto indexList = [&](std::vector<std::uint16_t>& out) {
        out.resize(count(r, wideIndices ? 4 : 2));
        for (auto& i : out) {
            i = wideIndices ? static_cast<std::uint16_t>(r.u32()) : r.u16();
        }
    };
    std::vector<std::uint16_t> skipped;
    m.name = sgResource(r);
    m.elements.resize(count(r, 28));
    for (auto& e : m.elements) {
        e.count = r.u32();
        e.identity = r.u32();
        r.u32();  // repetition
        e.blockFormat = r.u32();
        e.setFormat = r.u32();
        const auto bytes = r.bytes(r.u32Count(1));
        e.data.assign(bytes.begin(), bytes.end());
        indexList(skipped);
    }
    m.links.resize(count(r, 24));
    for (auto& l : m.links) {
        indexList(l.elements);
        l.vertexCount = r.u32();
        r.u32();  // active elements
        for (auto& alias : l.aliases) {
            indexList(alias);
        }
    }
    m.groups.resize(count(r, 17));
    for (auto& g : m.groups) {
        g.primitive = r.u32();
        g.link = r.u32();
        g.name = r.varString();
        indexList(g.indices);
        g.opacity = r.u32();
        if (m.version > 1) {
            indexList(g.bones);
        }
    }
    m.inverseBinds.resize(r.remaining() >= 4 ? count(r, 28) : 0);
    for (auto& b : m.inverseBinds) {
        for (auto& v : b.rotation) {
            v = std::bit_cast<float>(r.u32());
        }
        for (auto& v : b.translation) {
            v = std::bit_cast<float>(r.u32());
        }
    }
    // Blend group names, a bounding mesh and per-joint bounding hulls follow. They aren't read yet.
    return m;
}

std::vector<float> floats(const Element& element) {
    const std::size_t perItem = element.blockFormat == 0 ? 1 : element.blockFormat == 1 ? 2
                              : element.blockFormat == 2 ? 3 : 0;
    if (perItem == 0) {
        fail("element is not made of floats");
    }
    std::vector<float> out(element.data.size() / 4);
    for (std::size_t i = 0; i < out.size(); ++i) {
        std::uint32_t bits = 0;
        for (int b = 0; b < 4; ++b) {
            bits |= static_cast<std::uint32_t>(element.data[i * 4 + b]) << (8 * b);
        }
        out[i] = std::bit_cast<float>(bits);
    }
    return out;
}

std::vector<float> vertexFloats(const Mesh& mesh, const ElementLink& link, std::uint32_t identity, std::size_t nth) {
    const int kind = identity == kPositions ? 0 : identity == kNormals ? 1 : identity == kTextureCoordinates ? 2 : -1;
    for (const std::uint16_t e : link.elements) {
        if (e >= mesh.elements.size() || mesh.elements[e].identity != identity) {
            continue;
        }
        if (nth-- > 0) {
            continue;
        }
        std::vector<float> items = floats(mesh.elements[e]);
        if (kind < 0 || link.aliases[static_cast<std::size_t>(kind)].empty()) {
            return items;
        }
        const auto& aliases = link.aliases[static_cast<std::size_t>(kind)];
        const std::uint32_t f = mesh.elements[e].blockFormat + 1;
        std::vector<float> out;
        out.reserve(aliases.size() * f);
        for (const std::uint16_t a : aliases) {
            if ((a + 1u) * f > items.size()) {
                return {};
            }
            out.insert(out.end(), items.begin() + a * f, items.begin() + (a + 1) * f);
        }
        return out;
    }
    return {};
}

Shape parseShape(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    Shape s;
    s.container = readContainer(r);
    s.version = blockHeader(r, "cShape").version;
    s.name = sgResource(r);
    blockHeader(r, "cReferentNode");
    objectGraphNode(r);
    if (s.version > 6) {
        const std::size_t lods = count(r, 4);
        for (std::size_t i = 0; i < lods; ++i) {
            r.u32();
        }
    }
    const std::size_t items = count(r, 6);
    for (std::size_t i = 0; i < items; ++i) {
        s.detailLevels.push_back(s.version > 5 ? r.u32() : 0);
        if (s.version >= 8 && r.u8() != 0) {
            s.geometryNodes.push_back(r.varString());
            s.geometryLinks.push_back(std::nullopt);
        } else {
            r.u8();
            r.u8();
            s.geometryNodes.emplace_back();
            s.geometryLinks.push_back(r.u32());
        }
    }
    s.parts.resize(count(r, 11));
    for (auto& p : s.parts) {
        p.group = r.varString();
        p.material = r.varString();
        r.u8();
        const std::size_t extraNames = count(r, 1);
        for (std::size_t i = 0; i < extraNames; ++i) {
            r.varString();
        }
        r.u32();
    }
    return s;
}

Material parseMaterial(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    Material m;
    readContainer(r);
    m.version = blockHeader(r, "cMaterialDefinition").version;
    m.name = sgResource(r);
    m.description = r.varString();
    m.type = r.varString();
    const std::size_t properties = count(r, 2);
    for (std::size_t i = 0; i < properties; ++i) {
        std::string key = r.varString();
        m.properties[std::move(key)] = r.varString();
    }
    if (m.version > 8 && r.remaining() >= 4) {
        const std::size_t textures = count(r, 1);
        for (std::size_t i = 0; i < textures; ++i) {
            m.textures.push_back(r.varString());
        }
    }
    return m;
}

namespace {

// Bytes of one mip level of each texture format, as the game sizes them (0: unknown format).
std::size_t levelBytes(std::uint32_t format, std::size_t width, std::size_t height) {
    const std::size_t blocks = ((width + 3) / 4) * ((height + 3) / 4);
    switch (format) {
    case 1: return width * height * 4;
    case 2:
    case 9: return width * height * 3;
    case 3:
    case 6: return width * height;
    case 7: return width * height * 2;
    case 4: return blocks * 8;
    case 5:
    case 8: return blocks * 16;
    default: return 0;
    }
}

// Before version 9 a texture's mip levels are one block, largest first. Each level halves the
// last (down to 1) and the game stops at a side that isn't a power of two. Returned smallest first.
std::vector<MipLevel> packedMipLevels(BinaryReader& r, const Image& img, std::size_t mipCount) {
    if (img.width > tga::kMaxSide || img.height > tga::kMaxSide) {
        fail("texture size " + std::to_string(img.width) + " x " + std::to_string(img.height) + " too large");
    }
    std::vector<std::size_t> sizes;
    std::size_t w = img.width, h = img.height;
    for (std::size_t i = 0; i < mipCount; ++i) {
        if (w == 0 || h == 0 || (w & (w - 1)) != 0 || (h & (h - 1)) != 0) {
            break;
        }
        const std::size_t bytes = levelBytes(img.format, w, h);
        if (bytes == 0) {
            fail("texture format " + std::to_string(img.format) + " not supported");
        }
        sizes.push_back(bytes);
        if (w < 2 && h < 2) {
            break;
        }
        w = std::max<std::size_t>(1, w / 2);
        h = std::max<std::size_t>(1, h / 2);
    }
    r.u32();  // the block's size, which the game ignores for the size it works out
    std::vector<MipLevel> levels(sizes.size());
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const auto bytes = r.bytes(sizes[i]);
        levels[sizes.size() - 1 - i].data.assign(bytes.begin(), bytes.end());
    }
    return levels;
}

} // namespace

Image parseImage(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    Image img;
    readContainer(r);
    img.version = blockHeader(r, "cImageData").version;
    if (img.version > 9) {
        fail("image version " + std::to_string(img.version) + " not supported");
    }
    img.name = sgResource(r);
    img.width = r.u32();
    img.height = r.u32();
    img.format = r.u32();
    const std::size_t mipCount = r.u32();
    r.u32();  // a float, normally 1.0
    const std::size_t textures = count(r, 4);
    r.u32();
    if (img.version > 6) {
        r.varString();
    }
    if (textures == 0) {
        return img;
    }
    // Each texture: its mip levels, then (from version 6) a colour and (from version 8) a float.
    for (std::size_t t = 0; t < textures; ++t) {
        std::vector<MipLevel> levels;
        if (img.version < 9) {
            levels = packedMipLevels(r, img, mipCount);
        } else {
            const std::size_t mips = count(r, 1);
            for (std::size_t i = 0; i < mips; ++i) {
                MipLevel level;
                const std::uint8_t kind = r.u8();
                if (kind == 0) {
                    const auto bytes = r.bytes(r.u32Count(1));
                    level.data.assign(bytes.begin(), bytes.end());
                } else if (kind == 1) {
                    level.largeMipName = r.varString();
                } else {
                    fail("unknown mip level kind " + std::to_string(kind));
                }
                levels.push_back(std::move(level));
            }
        }
        img.faces.push_back(std::move(levels));
        if (img.version >= 6) {
            r.u32();
        }
        if (img.version > 7) {
            const float amplitude = std::bit_cast<float>(r.u32());
            if (t == 0) {
                img.bumpAmplitude = amplitude;
            }
        }
    }
    img.mips = img.faces.front();
    return img;
}

LevelInfo parseLevelInfo(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    LevelInfo l;
    readContainer(r);
    blockHeader(r, "cLevelInfo");
    sgResource(r);
    l.width = r.u32();
    l.height = r.u32();
    r.u32();  // z level
    const auto bytes = r.bytes(r.u32Count(1));
    l.data.assign(bytes.begin(), bytes.end());
    return l;
}

LightResource parseLight(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    readContainer(r);
    LightResource l;
    l.kind = r.varString();
    r.u32();
    r.u32();
    if (l.kind != "cPointLight" && l.kind != "cSpotLight") {
        fail("light kind " + l.kind + " is not read");
    }
    blockHeader(r, "cStandardLightBase");
    sgResource(r);
    blockHeader(r, "cLightT");
    sgResource(r);
    blockHeader(r, "cReferentNode");
    // The object graph node holds the light's name.
    const BlockHeader graph = blockHeader(r, "cObjectGraphNode");
    const std::size_t extensions = count(r, 6);
    for (std::size_t i = 0; i < extensions; ++i) {
        r.u8();
        r.u8();
        r.u32();
    }
    r.u8();  // a byte lights have here before the name (0 in the game's files)
    if (graph.version >= 4) {
        l.name = r.varString();
    }
    auto f = [&] { return std::bit_cast<float>(r.u32()); };
    l.intensity = f();
    l.specular = f();
    l.color = {f(), f(), f()};
    l.falloffRate = f();
    if (l.kind == "cSpotLight") {
        l.coneAngle = f();
        l.conePenumbra = f();
    }
    l.range = f();
    return l;
}

SceneObject parseSceneObject(std::span<const std::uint8_t> data) {
    // The cObject block: its type id, version and class name, found wherever the record put it.
    static constexpr std::uint8_t kHeader[] = {0xF7, 0x39, 0x1C, 0xFA};
    static constexpr std::string_view kClass = "\x07" "cObject";
    std::size_t block = data.size();
    for (std::size_t i = 0; i + 8 + kClass.size() <= data.size(); ++i) {
        if (std::equal(std::begin(kHeader), std::end(kHeader), data.begin() + static_cast<std::ptrdiff_t>(i)) &&
            std::equal(kClass.begin(), kClass.end(), data.begin() + static_cast<std::ptrdiff_t>(i + 8))) {
            block = i;
            break;
        }
    }
    if (block == data.size()) {
        fail("no cObject block");
    }
    BinaryReader r(data.subspan(block + 4));
    const std::uint32_t version = r.u32();
    r.skip(kClass.size());
    SceneObject o;
    o.model = r.varString();
    if (o.model.empty()) {
        return o;
    }
    const std::size_t entries = count(r, 5);
    for (std::size_t e = 0; e < entries; ++e) {
        r.varString();
        r.u32();
        if (version < 0x11) {
            continue;  // overrides came with version 0x11
        }
        const std::size_t overrides = count(r, 2);
        for (std::size_t i = 0; i < overrides; ++i) {
            MaterialOverride m;
            m.material = r.varString();
            m.group = r.varString();
            o.overrides.push_back(std::move(m));
        }
    }
    return o;
}

namespace {

std::array<std::array<std::uint8_t, 4>, 4> dxtColors(const std::uint8_t* block, bool fourColors) {
    const auto c0 = static_cast<std::uint16_t>(block[0] | block[1] << 8);
    const auto c1 = static_cast<std::uint16_t>(block[2] | block[3] << 8);
    auto expand = [](std::uint16_t c) {
        const int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
        return std::array<int, 3>{(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
    };
    const auto a = expand(c0), b = expand(c1);
    std::array<std::array<std::uint8_t, 4>, 4> out{};
    for (int k = 0; k < 3; ++k) {
        out[0][k] = static_cast<std::uint8_t>(a[k]);
        out[1][k] = static_cast<std::uint8_t>(b[k]);
        if (fourColors || c0 > c1) {
            out[2][k] = static_cast<std::uint8_t>((2 * a[k] + b[k]) / 3);
            out[3][k] = static_cast<std::uint8_t>((a[k] + 2 * b[k]) / 3);
        } else {
            out[2][k] = static_cast<std::uint8_t>((a[k] + b[k]) / 2);
            out[3][k] = 0;
        }
    }
    for (int i = 0; i < 4; ++i) {
        out[i][3] = 255;
    }
    if (!fourColors && c0 <= c1) {
        out[3][3] = 0;
    }
    return out;
}

} // namespace

// The bytes a texture of this size and format needs.
std::size_t textureBytes(std::uint32_t width, std::uint32_t height, std::uint32_t format) {
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    switch (format) {
    case 4: case 5: case 8: {
        const std::size_t blocks = static_cast<std::size_t>(std::max(1u, (width + 3) / 4)) * std::max(1u, (height + 3) / 4);
        return blocks * (format == 4 ? 8 : 16);
    }
    case 1: return pixels * 4;
    case 2: return pixels * 3;
    case 3: case 6: return pixels;
    default:
        fail("texture format " + std::to_string(format) + " not supported");
        return 0;
    }
}

std::vector<std::uint8_t> decodeRgba(std::span<const std::uint8_t> data, std::uint32_t width, std::uint32_t height,
                                     std::uint32_t format) {
    if (width > tga::kMaxSide || height > tga::kMaxSide) {
        fail("texture size " + std::to_string(width) + " x " + std::to_string(height) + " too large");
    }
    // Checked before allocating, so a short resource claiming a huge size costs nothing.
    if (data.size() < textureBytes(width, height, format)) {
        fail("texture data too short");
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * 4, 255);
    auto pixel = [&](std::uint32_t x, std::uint32_t y) { return out.data() + (static_cast<std::size_t>(y) * width + x) * 4; };
    if (format == 4 || format == 5 || format == 8) {
        const std::size_t blockBytes = format == 4 ? 8 : 16;
        const std::uint32_t bw = std::max(1u, (width + 3) / 4), bh = std::max(1u, (height + 3) / 4);
        for (std::uint32_t by = 0; by < bh; ++by) {
            for (std::uint32_t bx = 0; bx < bw; ++bx) {
                const std::uint8_t* block = data.data() + (static_cast<std::size_t>(by) * bw + bx) * blockBytes;
                const std::uint8_t* colorBlock = format == 4 ? block : block + 8;
                const auto colors = dxtColors(colorBlock, format != 4);
                const std::uint32_t bits = static_cast<std::uint32_t>(colorBlock[4]) | colorBlock[5] << 8 |
                                           colorBlock[6] << 16 | static_cast<std::uint32_t>(colorBlock[7]) << 24;
                std::array<std::uint8_t, 8> alphas{};
                if (format == 8) {
                    const int a0 = block[0], a1 = block[1];
                    alphas[0] = static_cast<std::uint8_t>(a0);
                    alphas[1] = static_cast<std::uint8_t>(a1);
                    for (int i = 2; i < 8; ++i) {
                        alphas[i] = static_cast<std::uint8_t>(
                            a0 > a1 ? ((8 - i) * a0 + (i - 1) * a1) / 7
                                    : i < 6 ? ((6 - i) * a0 + (i - 1) * a1) / 5 : (i == 6 ? 0 : 255));
                    }
                }
                std::uint64_t alphaBits = 0;
                for (int i = 0; i < 6; ++i) {
                    alphaBits |= static_cast<std::uint64_t>(block[2 + i]) << (8 * i);
                }
                for (std::uint32_t py = 0; py < 4; ++py) {
                    for (std::uint32_t px = 0; px < 4; ++px) {
                        const std::uint32_t x = bx * 4 + px, y = by * 4 + py;
                        if (x >= width || y >= height) {
                            continue;
                        }
                        const int i = static_cast<int>(py * 4 + px);
                        std::uint8_t* p = pixel(x, y);
                        const auto& c = colors[(bits >> (2 * i)) & 3];
                        std::copy(c.begin(), c.end(), p);
                        if (format == 5) {
                            const int nibble = (block[i / 2] >> (4 * (i % 2))) & 0xF;
                            p[3] = static_cast<std::uint8_t>(nibble * 17);
                        } else if (format == 8) {
                            p[3] = alphas[(alphaBits >> (3 * i)) & 7];
                        }
                    }
                }
            }
        }
        return out;
    }
    if (format == 1 || format == 2) {
        const std::size_t bytesPerPixel = format == 1 ? 4 : 3;
        for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
            const std::uint8_t* s = data.data() + i * bytesPerPixel;
            std::uint8_t* p = out.data() + i * 4;
            p[0] = s[2];
            p[1] = s[1];
            p[2] = s[0];
            p[3] = bytesPerPixel == 4 ? s[3] : 255;
        }
        return out;
    }
    if (format == 3) {
        // Alpha only: a byte per pixel, black where drawn.
        for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
            std::uint8_t* p = out.data() + i * 4;
            p[0] = p[1] = p[2] = 0;
            p[3] = data[i];
        }
        return out;
    }
    if (format == 6) {
        // A byte per pixel (all bump maps' heights), shown as grey.
        for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
            std::uint8_t* p = out.data() + i * 4;
            p[0] = p[1] = p[2] = data[i];
        }
        return out;
    }
    return out;
}

} // namespace ts2::scenegraph
