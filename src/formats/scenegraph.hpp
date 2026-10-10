#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The game's 3D scenegraph resources. Each is an RCOL container: links to other resources, the
// type ids of its blocks, then the blocks, each starting with its class name, id and version.
// Blocks don't store their length, so a reader has to understand every block before the one it
// wants; the readers here take the first block, which is the one that names the resource.
namespace ts2::scenegraph {

constexpr std::uint32_t kScenegraphGroup = 0x1C0532FA;

struct Link {
    std::uint32_t type = 0;
    std::uint32_t group = 0;
    std::uint32_t instance = 0;
    std::uint32_t instanceHigh = 0;
};

struct Container {
    std::vector<Link> links;
    std::vector<std::uint32_t> blockTypes;
};

// Mesh data (GMDC, cGeometryDataContainer).
struct Element {
    std::uint32_t identity = 0;     // what it holds: positions, normals, texture coordinates...
    std::uint32_t blockFormat = 0;  // 0 one float, 1 two, 2 three, 4 four bytes
    std::uint32_t setFormat = 0;
    std::uint32_t count = 0;
    std::vector<std::uint8_t> data;
};
constexpr std::uint32_t kPositions = 0x5B830781;
constexpr std::uint32_t kNormals = 0x3B83078B;
constexpr std::uint32_t kTextureCoordinates = 0xBB8307AB;
constexpr std::uint32_t kBlendIndices = 0xFBD70111;
constexpr std::uint32_t kBlendWeights = 0x3BD70105;

// The elements one part of the mesh draws from.
struct ElementLink {
    std::vector<std::uint16_t> elements;  // indices into Mesh::elements
    std::uint32_t vertexCount = 0;
    // By kind (positions, normals, texture coordinates): when set, vertex i takes item aliases[k][i]
    // of the link's elements of that kind.
    std::array<std::vector<std::uint16_t>, 3> aliases;
};

struct Group {
    std::string name;
    std::uint32_t primitive = 0;  // 2: triangle list
    std::uint32_t link = 0;       // index into Mesh::links
    std::vector<std::uint16_t> indices;
    std::uint32_t opacity = 0;
};
constexpr std::uint32_t kTriangleList = 2;

struct Mesh {
    Container container;
    std::uint32_t version = 0;
    std::string name;
    std::vector<Element> elements;
    std::vector<ElementLink> links;
    std::vector<Group> groups;

    // An element of the link with this identity, or null.
    const Element* find(const ElementLink& link, std::uint32_t identity) const;
};
Mesh parseMesh(std::span<const std::uint8_t> data);

// Floats of an element (one, two or three per item).
std::vector<float> floats(const Element& element);

// Floats of the link's element with this identity, one item per vertex of the link (aliases
// applied); empty when the link has no such element or an alias points past its items. `nth` picks
// among elements of the same identity (a second set of texture coordinates).
std::vector<float> vertexFloats(const Mesh& mesh, const ElementLink& link, std::uint32_t identity, std::size_t nth = 0);

// Shape (SHPE, cShape): the geometry nodes it draws and the material of each mesh group.
struct ShapePart {
    std::string group;
    std::string material;
};
struct Shape {
    Container container;
    std::uint32_t version = 0;
    std::string name;
    std::vector<std::string> geometryNodes;  // per item, by name
    std::vector<ShapePart> parts;
};
Shape parseShape(std::span<const std::uint8_t> data);

// Resource node (CRES) and geometry node (GMND): only their links are read.
Container parseContainer(std::span<const std::uint8_t> data);

// The node tree of a resource node (CRES): transform nodes (joints, slots, routing points) and
// shape references, each with a local transform; shapes are drawn at their reference's place in
// the tree. Nodes are indexed by block; other blocks are kept as empty nodes.
struct SceneNode {
    enum class Kind { Other, Root, Transform, ShapeRef };
    Kind kind = Kind::Other;
    std::string name;
    std::vector<std::uint32_t> children;  // block indices
    std::array<float, 3> translation{};
    std::array<float, 4> rotation{0, 0, 0, 1};  // quaternion x, y, z, w
    std::vector<std::uint32_t> shapeLinks;     // indices into the container's links
};
struct ResourceNode {
    Container container;
    std::vector<SceneNode> nodes;
};
ResourceNode parseResourceNode(std::span<const std::uint8_t> data);

// Material definition (TXMT, cMaterialDefinition): a type and name-value properties.
struct Material {
    std::uint32_t version = 0;
    std::string name;
    std::string description;
    std::string type;  // e.g. "StandardMaterial"
    std::map<std::string, std::string> properties;
    std::vector<std::string> textures;
};
Material parseMaterial(std::span<const std::uint8_t> data);

// Texture (TXTR, cImageData): mip levels from the smallest, each either inline or the name of a
// large mip level resource (LIFO).
struct MipLevel {
    std::vector<std::uint8_t> data;
    std::string largeMipName;  // set instead of data
};
struct Image {
    std::uint32_t version = 0;
    std::string name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0;  // 4 DXT1, 5 DXT3, 8 DXT5, 1 raw 32-bit, 2 raw 24-bit
    std::vector<MipLevel> mips;               // the first texture's
    std::vector<std::vector<MipLevel>> faces;  // every texture's: one normally, six for a cube map
};
Image parseImage(std::span<const std::uint8_t> data);

// A large mip level (LIFO, cLevelInfo): its size and data.
struct LevelInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> data;
};
LevelInfo parseLevelInfo(std::span<const std::uint8_t> data);

// A placed object's scene record in its lot (cObject, instance = object id): the model it shows and
// the materials replacing a mesh group's default one (recolors). Records may wrap cObject in a
// cAnimatable block; the model name is empty for objects without one.
struct MaterialOverride {
    std::string material;
    std::string group;
};
struct SceneObject {
    std::string model;
    std::vector<MaterialOverride> overrides;
};
SceneObject parseSceneObject(std::span<const std::uint8_t> data);

// Decodes a mip level of the given size and format to 8-bit RGBA.
std::vector<std::uint8_t> decodeRgba(std::span<const std::uint8_t> data, std::uint32_t width, std::uint32_t height,
                                     std::uint32_t format);

} // namespace ts2::scenegraph
