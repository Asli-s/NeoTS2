#include "scene/imposters.hpp"

#include "formats/dbpf.hpp"
#include "formats/hash.hpp"
#include "formats/scenegraph.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <sstream>

namespace ts2::scene {

namespace {

namespace sg = scenegraph;

constexpr std::uint32_t kTxmt = 0x49596978;
constexpr std::uint32_t kTxtr = 0x1C4A276C;
constexpr std::uint32_t kLifo = 0xED534136;
constexpr std::uint32_t kShpe = 0xFC6EB1F7;
constexpr std::uint32_t kGmdc = 0xAC4F8687;

// A tile is 10 world units.
constexpr float kTile = 10.0f;
// The roof shader's texture coordinates: (x, -y) / 64 + (0, 1), from the lot-local position.
constexpr float kRoofTextureTiles = 64.0f;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// A material property that is a color "(r, g, b)".
std::array<float, 3> color(const std::string& text, std::array<float, 3> fallback) {
    std::string s = text;
    std::replace_if(s.begin(), s.end(), [](char c) { return c == '(' || c == ')' || c == ','; }, ' ');
    std::istringstream in(s);
    std::array<float, 3> out{};
    return in >> out[0] >> out[1] >> out[2] ? out : fallback;
}

// The lot package's resources of the imposter, by lowercase name.
struct LotResources {
    dbpf::Package& package;
    std::map<std::string, const dbpf::IndexEntry*> textures, materials;
    std::vector<std::string>& warnings;
    std::map<std::string, std::uint32_t> loaded;  // texture name -> index into the terrain's imposter textures

    // Index of a texture in `out`, loaded on first use (its largest level); nullopt when missing.
    std::optional<std::uint32_t> texture(PaintedTerrain& out, const std::string& name) {
        const std::string key = lower(name);
        if (const auto it = loaded.find(key); it != loaded.end()) {
            return it->second;
        }
        const auto entry = textures.find(key);
        if (entry == textures.end()) {
            warnings.push_back("imposter texture " + name + " not found");
            return std::nullopt;
        }
        try {
            const sg::Image img = sg::parseImage(package.read(*entry->second));
            if (img.mips.empty()) {
                return std::nullopt;
            }
            std::vector<std::uint8_t> bytes = img.mips.back().data;
            if (!img.mips.back().largeMipName.empty()) {
                const auto k = hash::scenegraphKey(img.mips.back().largeMipName);
                const auto lifo = std::find_if(package.entries().begin(), package.entries().end(), [&](const dbpf::IndexEntry& e) {
                    return e.key.type == kLifo && e.key.instance == k.instance && e.key.instanceHigh == k.instanceHigh;
                });
                if (lifo == package.entries().end()) {
                    warnings.push_back("imposter large mip " + img.mips.back().largeMipName + " not found");
                    return std::nullopt;
                }
                bytes = sg::parseLevelInfo(package.read(*lifo)).data;
            }
            const auto index = static_cast<std::uint32_t>(out.imposterTextures.size());
            out.imposterTextures.push_back(Image{img.width, img.height, sg::decodeRgba(bytes, img.width, img.height, img.format)});
            loaded.emplace(key, index);
            return index;
        } catch (const std::exception& ex) {
            warnings.push_back("imposter texture " + name + ": " + ex.what());
            return std::nullopt;
        }
    }
};

} // namespace

bool addImposter(PaintedTerrain& out, const std::filesystem::path& lotPackage, const nhood::LotDescription& lot,
                 float zOffset, std::vector<std::string>& warnings, float* top) {
    dbpf::Package package{lotPackage};
    LotResources res{package, {}, {}, warnings, {}};
    std::optional<sg::Mesh> mesh;
    std::optional<sg::Shape> shape;
    for (const auto& e : package.entries()) {
        try {
            if (e.key.type == kGmdc && !mesh) {
                auto m = sg::parseMesh(package.read(e));
                if (lower(m.name).ends_with("!imposter_gmdc")) {
                    mesh = std::move(m);
                }
            } else if (e.key.type == kShpe && !shape) {
                auto s = sg::parseShape(package.read(e));
                if (lower(s.name).ends_with("!imposter_shpe")) {
                    shape = std::move(s);
                }
            } else if (e.key.type == kTxmt) {
                res.materials.emplace(lower(sg::parseMaterial(package.read(e)).name), &e);
            } else if (e.key.type == kTxtr) {
                res.textures.emplace(lower(sg::parseImage(package.read(e)).name), &e);
            }
        } catch (const std::exception& ex) {
            warnings.push_back(lotPackage.filename().string() + ": " + ex.what());
        }
    }
    if (!mesh || !shape) {
        return false;
    }
    // Texture names in the imposter's materials are its own: "<lot>!<name>_txtr".
    const std::string prefix = mesh->name.substr(0, mesh->name.find('!') + 1);

    // Placed turned by (facing - ownFacing) quarter turns clockwise, then moved so that the turned
    // lot covers its tiles from (x, y).
    const unsigned k = (static_cast<unsigned>(lot.facing) - lot.ownFacing + 4) & 3;
    const float w = static_cast<float>(lot.width), d = static_cast<float>(lot.depth);
    const float offsets[4][2] = {{0, 0}, {0, w}, {w, d}, {d, 0}};
    const float angle = -static_cast<float>(k) * 1.5707963267948966f;
    const float c = std::round(std::cos(angle)), s = std::round(std::sin(angle));
    const float tx = (static_cast<float>(lot.x) + offsets[k][0]) * kTile;
    const float ty = (static_cast<float>(lot.y) + offsets[k][1]) * kTile;
    const float tz = lot.elevation + zOffset;

    for (const auto& part : shape->parts) {
        const auto group = std::find_if(mesh->groups.begin(), mesh->groups.end(), [&](const sg::Group& g) { return g.name == part.group; });
        const auto materialEntry = res.materials.find(lower(part.material) + "_txmt");
        if (group == mesh->groups.end() || materialEntry == res.materials.end() || group->primitive != sg::kTriangleList ||
            group->link >= mesh->links.size()) {
            continue;
        }
        const sg::Material material = sg::parseMaterial(package.read(*materialEntry->second));
        const auto property = [&](const std::string& name) {
            const auto it = material.properties.find(name);
            return it == material.properties.end() ? std::string() : it->second;
        };
        PaintedTerrain::ImposterPart p;
        p.lot = lot.id;
        std::optional<std::uint32_t> texture;
        if (material.type == "ImposterWallMaterial") {
            p.kind = PaintedTerrain::ImposterPart::Kind::Wall;
            texture = res.texture(out, prefix + "walls_" + property("page") + "_txtr");
        } else if (material.type == "ImposterDualPackedSliceMaterial") {
            p.kind = PaintedTerrain::ImposterPart::Kind::Slice;
            texture = res.texture(out, prefix + "slices_" + property("page") + "_txtr");
        } else if (material.type == "ImposterRoofMaterial") {
            p.kind = PaintedTerrain::ImposterPart::Kind::Roof;
            texture = res.texture(out, prefix + "roofs_txtr");
        } else if (material.type == "ImposterTerrainMaterial") {
            p.kind = PaintedTerrain::ImposterPart::Kind::Terrain;
            texture = res.texture(out, prefix + "terrain_txtr");
            const std::string include = property("includeLightMap");
            if (!include.empty() && include != "0" && lower(include) != "false") {
                if (const auto lightMap = res.texture(out, prefix + "terrainLmap_txtr")) {
                    p.lightMap = static_cast<int>(*lightMap);
                    p.lightMapTint = color(property("lightMapTint"), p.lightMapTint);
                }
            }
        } else {
            warnings.push_back("imposter material type " + material.type + " not handled");
            continue;
        }
        if (!texture) {
            continue;
        }
        p.texture = *texture;

        const auto& link = mesh->links[group->link];
        const auto positions = sg::vertexFloats(*mesh, link, sg::kPositions);
        const auto uvs = sg::vertexFloats(*mesh, link, sg::kTextureCoordinates);
        auto uvs1 = sg::vertexFloats(*mesh, link, sg::kTextureCoordinates, 1);
        const std::size_t count = positions.size() / 3;
        const bool roof = p.kind == PaintedTerrain::ImposterPart::Kind::Roof;
        if (!roof && uvs.size() < count * 2) {
            warnings.push_back("imposter group " + group->name + " has no texture coordinates");
            continue;
        }
        if (uvs1.size() < count * 2) {
            uvs1 = uvs;
        }
        p.vertices.reserve(count * 7);
        for (std::size_t v = 0; v < count; ++v) {
            const float x = positions[v * 3], y = positions[v * 3 + 1], z = positions[v * 3 + 2];
            float u = 0, vv = 0;
            if (roof) {
                u = x / kRoofTextureTiles;
                vv = 1 - y / kRoofTextureTiles;
            } else {
                u = uvs[v * 2];
                vv = uvs[v * 2 + 1];
            }
            const float u1 = uvs1.size() >= (v + 1) * 2 ? uvs1[v * 2] : u;
            const float v1 = uvs1.size() >= (v + 1) * 2 ? uvs1[v * 2 + 1] : vv;
            p.vertices.insert(p.vertices.end(), {x * c - y * s + tx, x * s + y * c + ty, z + tz, u, vv, u1, v1});
            if (top) {
                *top = std::max(*top, z + tz);
            }
        }
        p.indices.reserve(group->indices.size());
        for (const auto i : group->indices) {
            if (i >= count) {
                p.indices.clear();
                break;
            }
            p.indices.push_back(i);
        }
        if (!p.indices.empty()) {
            out.imposters.push_back(std::move(p));
        }
    }
    return true;
}

} // namespace ts2::scene
