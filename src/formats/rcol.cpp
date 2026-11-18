#include "formats/rcol.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace ts2::rcol {

namespace {

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("scenegraph record: " + what);
}

// How deep data lists may nest (each level is a call), and how many values one resource may hold
// in all (each costs far more memory than its bytes), against broken files.
constexpr int kMaxDepth = 16;
constexpr std::size_t kMaxValues = 1u << 16;
// The memory one record's lists may take in all. An item can cost far more memory than its bytes in
// the file, so a broken count could otherwise ask for gigabytes.
constexpr std::size_t kMaxListMemory = std::size_t{256} << 20;
// The camera settings kept as stored after a viewer node's renderable part.
constexpr std::size_t kViewerSettings = 155, kRecursiveViewerSettings = 64;
constexpr std::uint32_t kHighInstances = 0xFFFF0001;

constexpr std::uint32_t kResourceNodeType = 0xE519C933, kTransformType = 0x65246462, kDataListType = 0x6A836D56,
                        kBoneDataType = 0xE9075BC5, kShapeRefType = 0x65245517, kLightRefType = 0x253D2018,
                        kViewerType = 0xDCA76DBB, kRecursiveViewerType = 0x0C152B8E, kGeometryNodeType = 0x7BA3838C,
                        kShapeType = 0xFC6EB1F7, kMeshType = 0xAC4F8687, kMaterialType = 0x49596978;

// Reads values into a record. Each layout below is written once for reading and writing.
class In {
public:
    static constexpr bool reading = true;
    explicit In(std::span<const std::uint8_t> data) : r_(data) {}

    void u8(std::uint8_t& v) { v = r_.u8(); }
    void u16(std::uint16_t& v) { v = r_.u16(); }
    void u32(std::uint32_t& v) { v = r_.u32(); }
    void f32(float& v) { v = r_.f32(); }
    // A text's length in 7-bit groups. A longer form than the shortest is refused, as it couldn't be
    // written back the same.
    void str(std::string& v) {
        const std::size_t start = r_.position();
        v = r_.varString();
        std::size_t groups = 1;
        for (std::size_t n = v.size() >> 7; n != 0; n >>= 7) {
            ++groups;
        }
        if (r_.position() - start != groups + v.size()) {
            fail("a text length stored in a longer form than the game writes");
        }
    }
    void raw(std::vector<std::uint8_t>& v, std::size_t n) {
        const auto b = r_.bytes(n);
        v.assign(b.begin(), b.end());
    }
    void sized(std::vector<std::uint8_t>& v) { raw(v, r_.u32Count(1)); }
    // A u32 count and that many items, each taking at least `minBytes`.
    template <class T, class F>
    void list(std::vector<T>& v, std::size_t minBytes, F&& each) {
        const std::size_t n = r_.u32Count(minBytes);
        listMemory_ += n * sizeof(T);
        if (listMemory_ > kMaxListMemory) {
            fail("lists that would take more memory than a record can hold");
        }
        v.resize(n);
        for (auto& item : v) {
            each(item);
        }
    }
    // `lastVersion` 0 means only `version`.
    void header(Header& h, std::string_view name, std::uint32_t version, std::uint32_t lastVersion = 0) {
        str(h.name);
        h.id = r_.u32();
        h.version = r_.u32();
        if (h.name != name) {
            fail("expected " + std::string(name) + ", found " + h.name);
        }
        if (h.version < version || h.version > std::max(version, lastVersion)) {
            fail(h.name + " version " + std::to_string(h.version) + " not read");
        }
    }
    std::size_t remaining() const { return r_.remaining(); }
    std::size_t count(std::uint32_t n, std::size_t minBytes) const { return r_.checkedCount(n, minBytes); }

    int depth = 0;
    std::size_t values = 0;

private:
    BinaryReader r_;
    std::size_t listMemory_ = 0;
};

// Writes a record's values back.
class Out {
public:
    static constexpr bool reading = false;

    void u8(std::uint8_t& v) { w_.u8(v); }
    void u16(std::uint16_t& v) { w_.u16(v); }
    void u32(std::uint32_t& v) { w_.u32(v); }
    void f32(float& v) { w_.f32(v); }
    void str(std::string& v) { w_.text(v); }
    void raw(std::vector<std::uint8_t>& v, std::size_t n) {
        if (v.size() != n) {
            fail("a fixed-size part has " + std::to_string(v.size()) + " bytes, not " + std::to_string(n));
        }
        w_.bytes(v);
    }
    void sized(std::vector<std::uint8_t>& v) {
        w_.u32(checkedSize(v.size()));
        w_.bytes(v);
    }
    template <class T, class F>
    void list(std::vector<T>& v, std::size_t, F&& each) {
        w_.u32(checkedSize(v.size()));
        for (auto& item : v) {
            each(item);
        }
    }
    void header(Header& h, std::string_view, std::uint32_t, std::uint32_t = 0) {
        w_.text(h.name);
        w_.u32(h.id);
        w_.u32(h.version);
    }
    std::vector<std::uint8_t> take() { return w_.take(); }

    int depth = 0;
    std::size_t values = 0;

private:
    static std::uint32_t checkedSize(std::size_t n) {
        if (n > std::numeric_limits<std::uint32_t>::max()) {
            fail("too many items to write");
        }
        return static_cast<std::uint32_t>(n);
    }
    BinaryWriter w_;
};

template <class IO>
void io(IO& a, Ref& v) {
    a.u8(v.enabled);
    a.u8(v.kind);
    a.u32(v.index);
}

template <class IO>
void refs(IO& a, std::vector<Ref>& v) {
    a.list(v, 6, [&](Ref& r) { io(a, r); });
}

template <class IO>
void strings(IO& a, std::vector<std::string>& v) {
    a.list(v, 1, [&](std::string& s) { a.str(s); });
}

template <class IO>
void words(IO& a, std::vector<std::uint32_t>& v) {
    a.list(v, 4, [&](std::uint32_t& x) { a.u32(x); });
}

template <class IO>
void shorts(IO& a, std::vector<std::uint16_t>& v) {
    a.list(v, 2, [&](std::uint16_t& x) { a.u16(x); });
}

template <class IO, std::size_t N>
void floats(IO& a, std::array<float, N>& v) {
    for (auto& x : v) {
        a.f32(x);
    }
}

template <class IO>
void io(IO& a, ResourceName& v) {
    a.header(v.header, "cSGResource", 2);
    a.str(v.name);
}

template <class IO>
void io(IO& a, GraphNode& v) {
    a.header(v.header, "cObjectGraphNode", 4);
    refs(a, v.extensions);
    a.str(v.name);
}

template <class IO>
void io(IO& a, TreeNode& v) {
    a.header(v.header, "cCompositionTreeNode", 11);
    io(a, v.graph);
    refs(a, v.children);
}

template <class IO>
void io(IO& a, Transform& v) {
    a.header(v.header, "cTransformNode", 7);
    io(a, v.tree);
    floats(a, v.translation);
    floats(a, v.rotation);
    a.u32(v.joint);
}

template <class IO>
void io(IO& a, Renderable& v, std::string_view name, std::uint32_t version, std::uint32_t lastVersion = 0) {
    a.header(v.header, name, version, lastVersion);
    a.header(v.renderable, "cRenderableNode", 5);
    a.header(v.bounded, "cBoundedNode", 5);
    io(a, v.transform);
    a.u16(v.flags);
    strings(a, v.groups);
    a.u32(v.value);
    a.u8(v.byte);
}

template <class IO>
void io(IO& a, Value& v) {
    if (a.depth >= kMaxDepth) {
        fail("data list too deep");
    }
    if (++a.values > kMaxValues) {
        fail("too many data values");
    }
    a.u8(v.type);
    a.str(v.name);
    std::size_t floatCount = 0;
    switch (v.type) {
    case 1: {
        std::uint8_t b = static_cast<std::uint8_t>(v.word);
        a.u8(b);
        v.word = b;
        break;
    }
    case 2: a.u32(v.word); break;
    case 3: floatCount = 1; break;
    case 4: floatCount = 2; break;
    case 5: floatCount = 3; break;
    case 8: floatCount = 4; break;
    case 6: a.str(v.text); break;
    case 9: a.sized(v.bytes); break;
    case 7:
        ++a.depth;
        // The smallest value (an empty name and a bool) takes 3 bytes.
        a.list(v.items, 3, [&](Value& item) { io(a, item); });
        --a.depth;
        break;
    default: fail("unknown data value type " + std::to_string(v.type));
    }
    if (floatCount != 0) {
        if constexpr (IO::reading) {
            v.floats.resize(floatCount);
        } else if (v.floats.size() != floatCount) {
            fail("a data value has the wrong number of floats");
        }
        for (auto& f : v.floats) {
            a.f32(f);
        }
    }
}

template <class IO>
void io(IO& a, ResourceNode& v) {
    a.header(v.header, "cResourceNode", 7);
    a.u8(v.root);
    if (v.root != 0) {
        io(a, v.name);
        io(a, v.tree);
    } else {
        io(a, v.graph);
    }
    a.u8(v.byte);
    a.u32(v.value);
}

template <class IO>
void io(IO& a, DataList& v) {
    a.header(v.header, "cDataListExtension", 1);
    a.header(v.extension, "cExtension", 3);
    io(a, v.value);
}

template <class IO>
void io(IO& a, BoneData& v) {
    // Characters' models store version 5, with the same layout.
    a.header(v.header, "cBoneDataExtension", 4, 5);
    a.header(v.extension, "cExtension", 3);
    for (auto& w : v.words) {
        a.u32(w);
    }
    floats(a, v.floats);
}

template <class IO>
void io(IO& a, ShapeRef& v) {
    io(a, v.node, "cShapeRefNode", 20, 21);
    refs(a, v.shapes);
    a.u32(v.a);
    words(a, v.values);
    if (v.node.header.version >= 21) {
        if constexpr (IO::reading) {
            v.names.resize(v.values.size());
        } else if (v.names.size() != v.values.size()) {
            fail("a shape reference needs one name per value");
        }
        for (auto& name : v.names) {
            a.str(name);
        }
    }
    a.sized(v.data);
    a.u32(v.c);
}

template <class IO>
void io(IO& a, LightRef& v) {
    io(a, v.node, "cLightRefNode", 10);
    io(a, v.light);
    a.u16(v.tail);
}

template <class IO>
void io(IO& a, ViewerRef& v, bool recursive) {
    if (recursive) {
        // The renderable part's own header is the recursive node's, before the base's.
        a.header(v.node.header, "cViewerRefNodeRecursive", 1);
    } else {
        a.header(v.node.header, "cViewerRefNode", 13);
    }
    a.header(v.base, "cViewerRefNodeBase", 5);
    a.header(v.node.renderable, "cRenderableNode", 5);
    a.header(v.node.bounded, "cBoundedNode", 5);
    io(a, v.node.transform);
    a.u16(v.node.flags);
    strings(a, v.node.groups);
    a.u32(v.node.value);
    a.u8(v.node.byte);
    if (recursive) {
        a.u8(v.byte);
        a.str(v.name);
    }
    a.raw(v.settings, recursive ? kRecursiveViewerSettings : kViewerSettings);
}

template <class IO>
void io(IO& a, GeometryNode& v) {
    a.header(v.header, "cGeometryNode", 12);
    io(a, v.graph);
    io(a, v.name);
    a.u16(v.word);
    a.u32(v.value);
    a.u8(v.byte);
}

template <class IO>
void io(IO& a, Shape& v) {
    a.header(v.header, "cShape", 8);
    io(a, v.name);
    a.header(v.referent, "cReferentNode", 1);
    io(a, v.graph);
    words(a, v.detailLevels);
    a.list(v.items, 6, [&](Shape::Item& item) {
        a.u32(item.detailLevel);
        a.u8(item.named);
        if (item.named != 0) {
            a.str(item.node);
        } else {
            a.u8(item.kind);
            a.u32(item.link);
        }
    });
    a.list(v.parts, 11, [&](Shape::Part& part) {
        a.str(part.group);
        a.str(part.material);
        a.u8(part.byte);
        strings(a, part.names);
        a.u32(part.value);
    });
}

// A bounding mesh or hull: its point count, and only when it has points, its corner count, the
// points and the corners.
template <class IO>
void io(IO& a, HullMesh& v) {
    std::uint32_t points = static_cast<std::uint32_t>(v.points.size() / 3);
    std::uint32_t corners = static_cast<std::uint32_t>(v.indices.size());
    if constexpr (!IO::reading) {
        if (v.points.size() % 3 != 0 || v.points.size() / 3 > std::numeric_limits<std::uint32_t>::max() ||
            v.indices.size() > std::numeric_limits<std::uint32_t>::max()) {
            fail("a bounding mesh that can't be stored");
        }
        if (points == 0 && corners != 0) {
            fail("a bounding mesh with corners but no points can't be stored");
        }
    }
    a.u32(points);
    if (points == 0) {
        return;
    }
    a.u32(corners);
    if constexpr (IO::reading) {
        v.points.resize(a.count(points, 12) * 3);
        v.indices.resize(a.count(corners, 2));
    }
    for (auto& p : v.points) {
        a.f32(p);
    }
    for (auto& i : v.indices) {
        a.u16(i);
    }
}

template <class IO>
void io(IO& a, Mesh& v) {
    a.header(v.header, "cGeometryDataContainer", 4);
    io(a, v.name);
    a.list(v.elements, 28, [&](Mesh::Element& e) {
        a.u32(e.count);
        a.u32(e.identity);
        a.u32(e.stream);
        a.u32(e.blockFormat);
        a.u32(e.setFormat);
        a.sized(e.data);
        shorts(a, e.indices);
    });
    a.list(v.links, 24, [&](Mesh::Link& l) {
        shorts(a, l.elements);
        a.u32(l.vertexCount);
        a.u32(l.activeElements);
        for (auto& alias : l.aliases) {
            shorts(a, alias);
        }
    });
    a.list(v.groups, 17, [&](Mesh::Group& g) {
        a.u32(g.primitive);
        a.u32(g.link);
        a.str(g.name);
        shorts(a, g.indices);
        a.u32(g.opacity);
        shorts(a, g.bones);
    });
    a.list(v.joints, 28, [&](Mesh::Joint& j) {
        floats(a, j.rotation);
        floats(a, j.translation);
    });
    a.list(v.morphs, 2, [&](Mesh::Morph& m) {
        a.str(m.group);
        a.str(m.name);
    });
    io(a, v.bounds);
    a.list(v.hulls, 4, [&](HullMesh& h) { io(a, h); });
}

template <class IO>
void io(IO& a, Material& v) {
    a.header(v.header, "cMaterialDefinition", 11);
    io(a, v.name);
    a.str(v.description);
    a.str(v.type);
    a.list(v.properties, 2, [&](std::pair<std::string, std::string>& p) {
        a.str(p.first);
        a.str(p.second);
    });
    strings(a, v.textures);
}

template <class T>
Block readBlock(In& in) {
    T block;
    io(in, block);
    return block;
}

Block readBlock(In& in, std::uint32_t type) {
    switch (type) {
    case kResourceNodeType: return readBlock<ResourceNode>(in);
    case kTransformType: {
        TransformBlock block;
        io(in, block.transform);
        return block;
    }
    case kDataListType: return readBlock<DataList>(in);
    case kBoneDataType: return readBlock<BoneData>(in);
    case kShapeRefType: return readBlock<ShapeRef>(in);
    case kLightRefType: return readBlock<LightRef>(in);
    case kViewerType:
    case kRecursiveViewerType: {
        ViewerRef block;
        io(in, block, type == kRecursiveViewerType);
        return block;
    }
    case kGeometryNodeType: return readBlock<GeometryNode>(in);
    case kShapeType: return readBlock<Shape>(in);
    case kMeshType: return readBlock<Mesh>(in);
    case kMaterialType: return readBlock<Material>(in);
    default: fail("block type " + std::to_string(type) + " not read");
    }
}

void writeBlock(Out& out, Block& block) {
    std::visit(
        [&](auto& b) {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, TransformBlock>) {
                io(out, b.transform);
            } else if constexpr (std::is_same_v<T, ViewerRef>) {
                io(out, b, b.node.header.name == "cViewerRefNodeRecursive");
            } else {
                io(out, b);
            }
        },
        block);
}

} // namespace

Resource read(std::span<const std::uint8_t> data) {
    BinaryReader header(data);
    Resource resource;
    std::uint32_t links = header.u32();
    resource.highInstances = links == kHighInstances;
    if (resource.highInstances) {
        links = header.u32();
    }
    resource.links.resize(header.checkedCount(links, resource.highInstances ? 16 : 12));
    for (auto& l : resource.links) {
        l.group = header.u32();
        l.instance = header.u32();
        l.instanceHigh = resource.highInstances ? header.u32() : 0;
        l.type = header.u32();
    }
    resource.blockTypes.resize(header.u32Count(4));
    for (auto& t : resource.blockTypes) {
        t = header.u32();
    }
    In in(data.subspan(header.position()));
    for (const std::uint32_t type : resource.blockTypes) {
        resource.blocks.push_back(readBlock(in, type));
    }
    if (in.remaining() != 0) {
        fail(std::to_string(in.remaining()) + " bytes after the last block");
    }
    return resource;
}

std::vector<std::uint8_t> write(const Resource& resource) {
    if (resource.blocks.size() != resource.blockTypes.size()) {
        fail("blocks and block types don't match");
    }
    if (!resource.highInstances && resource.links.size() >= kHighInstances) {
        fail("too many links to write");
    }
    BinaryWriter header;
    if (resource.highInstances) {
        header.u32(kHighInstances);
    }
    header.u32(static_cast<std::uint32_t>(resource.links.size()));
    for (const auto& l : resource.links) {
        header.u32(l.group);
        header.u32(l.instance);
        if (resource.highInstances) {
            header.u32(l.instanceHigh);
        }
        header.u32(l.type);
    }
    header.u32(static_cast<std::uint32_t>(resource.blockTypes.size()));
    for (const std::uint32_t t : resource.blockTypes) {
        header.u32(t);
    }
    Out out;
    // The layouts take their values by reference for reading, so writing works on a copy.
    Resource copy = resource;
    for (auto& block : copy.blocks) {
        writeBlock(out, block);
    }
    auto bytes = header.take();
    const auto body = out.take();
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

} // namespace ts2::rcol
