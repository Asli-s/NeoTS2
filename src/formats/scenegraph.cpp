#include "formats/scenegraph.hpp"

#include "formats/binary_reader.hpp"

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
    const std::uint32_t n = r.u32();
    if (n > kMaxCount || n > r.remaining() / std::max<std::size_t>(minBytes, 1)) {
        fail("bad count " + std::to_string(n));
    }
    return n;
}

// A byte array's length: it only has to fit in what is left.
std::size_t byteCount(BinaryReader& r) {
    const std::uint32_t n = r.u32();
    if (n > r.remaining()) {
        fail("bad byte count " + std::to_string(n));
    }
    return n;
}

// Strings carry their length in 7-bit groups (one byte below 128).
std::string string(BinaryReader& r) {
    std::uint32_t length = 0;
    for (int shift = 0;; shift += 7) {
        if (shift > 28) {
            fail("bad string length");
        }
        const std::uint8_t b = r.u8();
        length |= static_cast<std::uint32_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) {
            break;
        }
    }
    const auto bytes = r.bytes(length);
    return std::string(bytes.begin(), bytes.end());
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
    h.name = string(r);
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
    return string(r);
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
        string(r);
    }
}

} // namespace

Container parseContainer(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    return readContainer(r);
}

namespace {

// A cCompositionTreeNode: the object graph node, then children (enabled, kind, block index).
std::vector<std::uint32_t> compositionTree(BinaryReader& r, std::string* name) {
    blockHeader(r, "cCompositionTreeNode");
    const BlockHeader graph = blockHeader(r, "cObjectGraphNode");
    const std::size_t extensions = count(r, 6);
    for (std::size_t i = 0; i < extensions; ++i) {
        r.u8();
        r.u8();
        r.u32();
    }
    if (graph.version >= 4) {
        *name = string(r);
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
    node.children = compositionTree(r, &node.name);
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
    constexpr std::uint32_t kResourceNode = 0xE519C933, kTransformNode = 0x65246462, kShapeRefNode = 0x65245517;
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
                    string(r);  // render group, e.g. "Practical"
                }
                r.u32();
                r.u8();
                const std::size_t shapes = count(r, 6);
                for (std::size_t i = 0; i < shapes; ++i) {
                    r.u8();
                    r.u8();
                    node.shapeLinks.push_back(r.u32());
                }
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
    if (m.version != 4) {
        fail("mesh version " + std::to_string(m.version) + " not supported");
    }
    m.name = sgResource(r);
    m.elements.resize(count(r, 28));
    for (auto& e : m.elements) {
        e.count = r.u32();
        e.identity = r.u32();
        r.u32();  // repetition
        e.blockFormat = r.u32();
        e.setFormat = r.u32();
        const auto bytes = r.bytes(byteCount(r));
        e.data.assign(bytes.begin(), bytes.end());
        const std::size_t items = count(r, 2);
        for (std::size_t i = 0; i < items; ++i) {
            r.u16();
        }
    }
    m.links.resize(count(r, 24));
    for (auto& l : m.links) {
        l.elements.resize(count(r, 2));
        for (auto& e : l.elements) {
            e = r.u16();
        }
        l.vertexCount = r.u32();
        r.u32();  // active elements
        for (auto& alias : l.aliases) {
            alias.resize(count(r, 2));
            for (auto& i : alias) {
                i = r.u16();
            }
        }
    }
    m.groups.resize(count(r, 17));
    for (auto& g : m.groups) {
        g.primitive = r.u32();
        g.link = r.u32();
        g.name = string(r);
        g.indices.resize(count(r, 2));
        for (auto& i : g.indices) {
            i = r.u16();
        }
        g.opacity = r.u32();
        g.bones.resize(count(r, 2));
        for (auto& b : g.bones) {
            b = r.u16();
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
    // Blend group names, a bounding mesh and per-joint bounding hulls follow; they aren't read yet.
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
    const std::size_t lods = count(r, 4);
    for (std::size_t i = 0; i < lods; ++i) {
        r.u32();
    }
    const std::size_t items = count(r, 6);
    for (std::size_t i = 0; i < items; ++i) {
        s.detailLevels.push_back(r.u32());
        r.u8();
        s.geometryNodes.push_back(string(r));
    }
    s.parts.resize(count(r, 11));
    for (auto& p : s.parts) {
        p.group = string(r);
        p.material = string(r);
        r.bytes(9);
    }
    return s;
}

Material parseMaterial(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    Material m;
    readContainer(r);
    m.version = blockHeader(r, "cMaterialDefinition").version;
    m.name = sgResource(r);
    m.description = string(r);
    m.type = string(r);
    const std::size_t properties = count(r, 2);
    for (std::size_t i = 0; i < properties; ++i) {
        std::string key = string(r);
        m.properties[std::move(key)] = string(r);
    }
    if (m.version > 8 && r.remaining() >= 4) {
        const std::size_t textures = count(r, 1);
        for (std::size_t i = 0; i < textures; ++i) {
            m.textures.push_back(string(r));
        }
    }
    return m;
}

Image parseImage(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    Image img;
    readContainer(r);
    img.version = blockHeader(r, "cImageData").version;
    if (img.version < 9) {
        fail("image version " + std::to_string(img.version) + " not supported");
    }
    img.name = sgResource(r);
    img.width = r.u32();
    img.height = r.u32();
    img.format = r.u32();
    r.u32();  // mip level count
    r.u32();  // a float, normally 1.0
    const std::size_t textures = count(r, 4);
    r.u32();
    string(r);
    if (textures == 0) {
        return img;
    }
    // Each texture: its mip levels, then a colour and a float (8 bytes) before the next one.
    for (std::size_t t = 0; t < textures; ++t) {
        std::vector<MipLevel> levels;
        const std::size_t mips = count(r, 1);
        for (std::size_t i = 0; i < mips; ++i) {
            MipLevel level;
            const std::uint8_t kind = r.u8();
            if (kind == 0) {
                const auto bytes = r.bytes(byteCount(r));
                level.data.assign(bytes.begin(), bytes.end());
            } else if (kind == 1) {
                level.largeMipName = string(r);
            } else {
                fail("unknown mip level kind " + std::to_string(kind));
            }
            levels.push_back(std::move(level));
        }
        img.faces.push_back(std::move(levels));
        if (t + 1 < textures) {
            r.skip(8);
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
    const auto bytes = r.bytes(byteCount(r));
    l.data.assign(bytes.begin(), bytes.end());
    return l;
}

SceneObject parseSceneObject(std::span<const std::uint8_t> data) {
    static constexpr std::string_view kClass = "" "cObject";
    const auto at = std::search(data.begin(), data.end(), kClass.begin(), kClass.end());
    if (at == data.end()) {
        fail("no cObject block");
    }
    BinaryReader r(data.subspan(static_cast<std::size_t>(at - data.begin()) + kClass.size()));
    SceneObject o;
    o.model = string(r);
    if (o.model.empty()) {
        return o;
    }
    r.u32();
    string(r);
    r.u32();
    const std::size_t overrides = count(r, 2);
    for (std::size_t i = 0; i < overrides; ++i) {
        MaterialOverride m;
        m.material = string(r);
        m.group = string(r);
        o.overrides.push_back(std::move(m));
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

std::vector<std::uint8_t> decodeRgba(std::span<const std::uint8_t> data, std::uint32_t width, std::uint32_t height,
                                     std::uint32_t format) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * 4, 255);
    auto pixel = [&](std::uint32_t x, std::uint32_t y) { return out.data() + (static_cast<std::size_t>(y) * width + x) * 4; };
    if (format == 4 || format == 5 || format == 8) {
        const std::size_t blockBytes = format == 4 ? 8 : 16;
        const std::uint32_t bw = std::max(1u, (width + 3) / 4), bh = std::max(1u, (height + 3) / 4);
        if (data.size() < static_cast<std::size_t>(bw) * bh * blockBytes) {
            fail("texture data too short");
        }
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
        if (data.size() < static_cast<std::size_t>(width) * height * bytesPerPixel) {
            fail("texture data too short");
        }
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
        if (data.size() < static_cast<std::size_t>(width) * height) {
            fail("texture data too short");
        }
        for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
            std::uint8_t* p = out.data() + i * 4;
            p[0] = p[1] = p[2] = 0;
            p[3] = data[i];
        }
        return out;
    }
    fail("texture format " + std::to_string(format) + " not supported");
    return out;
}

} // namespace ts2::scenegraph
