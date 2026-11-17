#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

// The game's scenegraph resources (RCOL) as whole records, read and written back byte for byte:
// models (CRES), shapes (SHPE), geometry nodes (GMND), meshes (GMDC) and materials (TXMT). Each is a
// list of links to other resources, the type ids of its blocks, then the blocks. A block starts
// with its class name, id and version, and holds blocks of its base classes the same way. Every
// value is kept, those whose meaning is not known yet too, so a record changed in one place writes
// the rest as the game wrote it.
//
// Only the block versions the game's own files use are read: one per class, and also the bone data
// and shape reference versions of characters' models. Others throw, as their layout is not known.
namespace ts2::rcol {

// A block's class name, id (the type id for a resource's blocks, 0 for base class blocks) and version.
struct Header {
    std::string name;
    std::uint32_t id = 0;
    std::uint32_t version = 0;
};

// A reference to another block of the resource (or a link), with two flag bytes before it.
struct Ref {
    std::uint8_t enabled = 0, kind = 0;
    std::uint32_t index = 0;
};

// cSGResource: the resource's own name.
struct ResourceName {
    Header header;
    std::string name;
};

// cObjectGraphNode: the node's extensions (data list and bone data blocks) and its name.
struct GraphNode {
    Header header;
    std::vector<Ref> extensions;
    std::string name;
};

// cCompositionTreeNode: a graph node and its children.
struct TreeNode {
    Header header;
    GraphNode graph;
    std::vector<Ref> children;
};

// cTransformNode: a tree node placed by a translation and a rotation, and its joint (-1 for none).
struct Transform {
    Header header;
    TreeNode tree;
    std::array<float, 3> translation{};
    std::array<float, 4> rotation{};  // quaternion x, y, z, w
    std::uint32_t joint = 0;
};

// What every renderable node (shape, light and viewer references) starts with: its own class header,
// the renderable and bounded node headers, its transform, then its render settings.
struct Renderable {
    Header header, renderable, bounded;
    Transform transform;
    std::uint16_t flags = 0;
    std::vector<std::string> groups;  // render groups, e.g. "Practical"
    std::uint32_t value = 0;
    std::uint8_t byte = 0;
};

// A data list value (as scenegraph::DataValue, with its stored bytes kept).
struct Value {
    std::uint8_t type = 0;  // 1 bool, 2 int, 3 float, 4 two floats, 5 three, 6 text, 7 list, 8 four floats, 9 bytes
    std::string name;
    std::uint32_t word = 0;  // bool (one byte) and int
    std::vector<float> floats;
    std::string text;
    std::vector<std::uint8_t> bytes;
    std::vector<Value> items;
};

// cResourceNode: a model's root, holding its name and tree, or a model reference inside a tree.
struct ResourceNode {
    Header header;
    std::uint8_t root = 0;
    ResourceName name;  // root only
    TreeNode tree;      // root only
    GraphNode graph;    // reference only
    std::uint8_t byte = 0;
    std::uint32_t value = 0;
};

struct TransformBlock {
    Transform transform;
};

// cDataListExtension: a named list of values.
struct DataList {
    Header header, extension;
    Value value;
};

// cBoneDataExtension: three words, a weight and a rotation.
struct BoneData {
    Header header, extension;
    std::array<std::uint32_t, 3> words{};
    std::array<float, 5> floats{};  // weight, then a quaternion
};

// cShapeRefNode: a shape drawn at the node's place. From version 21 each value has a name, the
// morph it is for ("pufaceblends::cry").
struct ShapeRef {
    Renderable node;
    std::vector<Ref> shapes;
    std::uint32_t a = 0;
    std::vector<std::uint32_t> values;
    std::vector<std::string> names;
    std::vector<std::uint8_t> data;
    std::uint32_t c = 0;
};

// cLightRefNode: a light at the node's place, by link.
struct LightRef {
    Renderable node;
    Ref light;
    std::uint16_t tail = 0;
};

// cViewerRefNode and cViewerRefNodeRecursive: a camera's node. The camera settings after the
// renderable part are kept as stored, as their layout is not read yet.
struct ViewerRef {
    Header base;  // cViewerRefNodeBase
    Renderable node;
    std::uint8_t byte = 0;     // the recursive node only
    std::string name;          // the recursive node only, e.g. "screenreflection"
    std::vector<std::uint8_t> settings;
};

// cGeometryNode: names the mesh it shows.
struct GeometryNode {
    Header header;
    GraphNode graph;
    ResourceName name;
    std::uint16_t word = 0;
    std::uint32_t value = 0;
    std::uint8_t byte = 0;
};

// cShape: the geometry nodes it draws per level of detail and each mesh group's material.
struct Shape {
    Header header;
    ResourceName name;
    Header referent;  // cReferentNode
    GraphNode graph;
    std::vector<std::uint32_t> detailLevels;
    struct Item {
        std::uint32_t detailLevel = 0;
        std::uint8_t named = 0;
        std::string node;  // when named
        std::uint8_t kind = 0;
        std::uint32_t link = 0;  // when not named
    };
    std::vector<Item> items;
    struct Part {
        std::string group, material;
        std::uint8_t byte = 0;
        std::vector<std::string> names;
        std::uint32_t value = 0;
    };
    std::vector<Part> parts;
};

// A list of points and the triangle corners that use them: a mesh's bounding mesh, or a joint's hull.
struct HullMesh {
    std::vector<float> points;  // three per point
    std::vector<std::uint16_t> indices;
};

// cGeometryDataContainer: a mesh (see scenegraph::Mesh for what its parts mean).
struct Mesh {
    Header header;
    ResourceName name;
    struct Element {
        std::uint32_t count = 0, identity = 0, stream = 0, blockFormat = 0, setFormat = 0;
        std::vector<std::uint8_t> data;
        std::vector<std::uint16_t> indices;
    };
    std::vector<Element> elements;
    struct Link {
        std::vector<std::uint16_t> elements;
        std::uint32_t vertexCount = 0, activeElements = 0;
        std::array<std::vector<std::uint16_t>, 3> aliases;
    };
    std::vector<Link> links;
    struct Group {
        std::uint32_t primitive = 0, link = 0;
        std::string name;
        std::vector<std::uint16_t> indices;
        std::uint32_t opacity = 0;
        std::vector<std::uint16_t> bones;
    };
    std::vector<Group> groups;
    struct Joint {
        std::array<float, 4> rotation{};
        std::array<float, 3> translation{};
    };
    std::vector<Joint> joints;
    struct Morph {
        std::string group, name;
    };
    std::vector<Morph> morphs;
    HullMesh bounds;
    std::vector<HullMesh> hulls;
};

// cMaterialDefinition: a material type and its properties, in the order stored.
struct Material {
    Header header;
    ResourceName name;
    std::string description;
    std::string type;
    std::vector<std::pair<std::string, std::string>> properties;
    std::vector<std::string> textures;
};

using Block = std::variant<ResourceNode, TransformBlock, DataList, BoneData, ShapeRef, LightRef, ViewerRef, GeometryNode, Shape, Mesh, Material>;

struct Link {
    std::uint32_t type = 0, group = 0, instance = 0, instanceHigh = 0;
};

struct Resource {
    // Whether the links carry a high instance (every file of the game's does).
    bool highInstances = true;
    std::vector<Link> links;
    std::vector<Block> blocks;  // with their type ids, in order
    std::vector<std::uint32_t> blockTypes;
};

// Throws std::runtime_error for a block type or version not read, and for broken data.
Resource read(std::span<const std::uint8_t> data);

// Throws std::runtime_error when a value can't be stored, such as a bounding mesh with corners but
// no points, or more items than a count holds.
std::vector<std::uint8_t> write(const Resource& resource);

} // namespace ts2::rcol
