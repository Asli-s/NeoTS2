#include "scene/imposters.hpp"

#include "formats/dbpf.hpp"
#include "formats/hash.hpp"
#include "formats/scenegraph.hpp"
#include "content/lot.hpp"
#include "scene/terrain_water.hpp"

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
constexpr std::uint32_t kCres = 0xE519C933;

// Limits for an imposter's object tree, which comes from a saved file: how deep transforms may
// nest (each level is a call) and how many objects one lot may bring.
constexpr int kMostObjectDepth = 16;
constexpr std::size_t kMostImposterObjects = 4096;

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

// The lot package's textures, by lowercase name, put in the terrain's imposter textures as they are used.
struct LotTextures {
    dbpf::Package& package;
    std::vector<std::string>& warnings;
    std::map<std::string, const dbpf::IndexEntry*> textures;  // found on first use
    bool found = false;
    std::map<std::string, std::uint32_t> loaded;  // texture name -> index into the terrain's imposter textures

    void find() {
        found = true;
        for (const auto& e : package.entries()) {
            if (e.key.type != kTxtr) {
                continue;
            }
            try {
                textures.emplace(lower(sg::parseImage(package.read(e)).name), &e);
            } catch (const std::exception& ex) {
                warnings.push_back(std::string("imposter texture: ") + ex.what());
            }
        }
    }

    // Index of a picture in `out` by its key, added on first use.
    std::uint32_t add(PaintedTerrain& out, const std::string& key, const Image& picture) {
        if (const auto it = loaded.find(key); it != loaded.end()) {
            return it->second;
        }
        const auto index = static_cast<std::uint32_t>(out.imposterTextures.size());
        out.imposterTextures.push_back(picture);
        loaded.emplace(key, index);
        return index;
    }

    // Index of a texture in `out`, loaded on first use (its largest level). Nullopt when missing.
    std::optional<std::uint32_t> texture(PaintedTerrain& out, const std::string& name) {
        const std::string key = lower(name);
        if (const auto it = loaded.find(key); it != loaded.end()) {
            return it->second;
        }
        if (!found) {
            find();
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
            return add(out, key, Image{img.width, img.height, sg::decodeRgba(bytes, img.width, img.height, img.format)});
        } catch (const std::exception& ex) {
            warnings.push_back("imposter texture " + name + ": " + ex.what());
            return std::nullopt;
        }
    }
};

} // namespace

std::optional<LotImposter> readImposter(dbpf::Package& package, std::vector<std::string>& warnings) {
    std::optional<sg::Mesh> mesh;
    std::optional<sg::Shape> shape;
    std::map<std::string, const dbpf::IndexEntry*> materials;
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
                materials.emplace(lower(sg::parseMaterial(package.read(e)).name), &e);
            }
        } catch (const std::exception& ex) {
            warnings.push_back(std::string("imposter: ") + ex.what());
        }
    }
    if (!mesh || !shape) {
        return std::nullopt;
    }
    LotImposter out;
    // Texture names in the imposter's materials are its own: "<lot>!<name>_txtr".
    out.prefix = mesh->name.substr(0, mesh->name.find('!') + 1);
    for (const auto& part : shape->parts) {
        const auto group = std::find_if(mesh->groups.begin(), mesh->groups.end(), [&](const sg::Group& g) { return g.name == part.group; });
        const auto materialEntry = materials.find(lower(part.material) + "_txmt");
        if (group == mesh->groups.end() || materialEntry == materials.end() || group->primitive != sg::kTriangleList ||
            group->link >= mesh->links.size()) {
            continue;
        }
        const sg::Material material = sg::parseMaterial(package.read(*materialEntry->second));
        const auto property = [&](const std::string& name) {
            const auto it = material.properties.find(name);
            return it == material.properties.end() ? std::string() : it->second;
        };
        LotImposter::Part p;
        if (material.type == "ImposterWallMaterial") {
            p.kind = PaintedTerrain::ImposterPart::Kind::Wall;
            p.texture = "walls_" + property("page");
        } else if (material.type == "ImposterDualPackedSliceMaterial") {
            p.kind = PaintedTerrain::ImposterPart::Kind::Slice;
            p.texture = "slices_" + property("page");
        } else if (material.type == "ImposterRoofMaterial") {
            p.kind = PaintedTerrain::ImposterPart::Kind::Roof;
            p.texture = "roofs";
        } else if (material.type == "ImposterTerrainMaterial") {
            p.kind = PaintedTerrain::ImposterPart::Kind::Terrain;
            p.texture = "terrain";
            const std::string include = property("includeLightMap");
            if (!include.empty() && include != "0" && lower(include) != "false") {
                p.lightMap = "terrainLmap";
                p.lightMapTint = color(property("lightMapTint"), p.lightMapTint);
            }
        } else {
            warnings.push_back("imposter material type " + material.type + " not handled");
            continue;
        }

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
            p.vertices.insert(p.vertices.end(), {x, y, z, u, vv, u1, v1});
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
            out.parts.push_back(std::move(p));
        }
    }
    return out;
}

namespace {

// Walks the imposter's node tree from `index`, collecting each model node with the transforms above it.
void collectObjects(const sg::ResourceNode& tree, std::uint32_t index, const Transform& above, int depth, std::vector<bool>& seen,
                    std::vector<ImposterObject>& out) {
    if (index >= tree.nodes.size() || seen[index] || depth > kMostObjectDepth || out.size() >= kMostImposterObjects) {
        return;
    }
    seen[index] = true;
    const sg::SceneNode& node = tree.nodes[index];
    if (node.kind == sg::SceneNode::Kind::ModelRef) {
        std::string model = node.name;
        if (lower(model).ends_with("_cres")) {
            model.resize(model.size() - 5);
        }
        if (model.empty()) {
            return;
        }
        ImposterObject object{std::move(model), {}, above};
        for (const auto e : node.extensions) {
            if (e >= tree.nodes.size() || !tree.nodes[e].dataList || tree.nodes[e].dataList->name != "DesignModes") {
                continue;
            }
            // Each entry names a part of the model (its subset) and the material it shows.
            for (const auto& item : tree.nodes[e].dataList->items) {
                if (item.type == sg::DataValue::Type::String) {
                    object.overrides.push_back({item.text, item.name});
                }
            }
        }
        out.push_back(std::move(object));
        return;
    }
    const Transform here = node.kind == sg::SceneNode::Kind::Transform ? above * Transform::of(node.translation, node.rotation) : above;
    for (const auto child : node.children) {
        collectObjects(tree, child, here, depth + 1, seen, out);
    }
}

} // namespace

std::vector<ImposterObject> readImposterObjects(dbpf::Package& package, std::vector<std::string>& warnings) {
    std::vector<ImposterObject> out;
    for (const auto& e : package.entries()) {
        if (e.key.type != kCres) {
            continue;
        }
        try {
            const auto tree = sg::parseResourceNode(package.read(e));
            if (tree.nodes.empty() || tree.nodes.front().kind != sg::SceneNode::Kind::Root ||
                !lower(tree.nodes.front().name).ends_with("!imposter_cres")) {
                continue;
            }
            std::vector<bool> seen(tree.nodes.size(), false);
            collectObjects(tree, 0, Transform{}, 0, seen, out);
            if (out.size() >= kMostImposterObjects) {
                warnings.push_back("imposter: more than " + std::to_string(kMostImposterObjects) + " objects, the rest left out");
            }
            break;
        } catch (const std::exception& ex) {
            warnings.push_back(std::string("imposter objects: ") + ex.what());
        }
    }
    return out;
}

namespace {

// Not in the original game: a lot imposter's ground drawn through every corner of the lot's tiles at
// the lot's own heights. The game saves it through a corner every 10 tiles, each at that one corner's
// height, so a dip at one of those corners slopes the ground for 10 tiles around it and leaves what
// stands there floating. Where the lot has water the ground lies just under the water's surface, as
// its picture shows the water there and not the bed under it. The picture keeps its place: each
// corner takes the texture coordinates the saved ground has there. `vertices` (x, y, z, u, v, u, v)
// and `indices` are the saved ground's, in the lot's own coordinates. Left as they are when the
// heights don't cover it.
void followLotGround(std::vector<float>& vertices, std::vector<std::uint32_t>& indices, const content::Lot& lot) {
    const auto grid = lot.grids.find(content::kFloorHeightGridInstance);
    // The game saves a few dozen triangles. More than this comes from a broken file and is left as it is.
    constexpr std::size_t kMostTriangles = 4096;
    if (grid == lot.grids.end() || grid->second.elementSize != 4 || vertices.size() < 21 || indices.size() < 3 ||
        indices.size() / 3 > kMostTriangles) {
        return;
    }
    const int level = content::groundGridLevel(lot);
    const auto water = lot.flatGrids.find(content::kWaterHeightGridInstance);
    // Under the water, this far below its surface, so the water drawn over it shows on top.
    constexpr float kUnderWater = 0.1f;
    float x0 = vertices[0], y0 = vertices[1], x1 = x0, y1 = y0;
    for (std::size_t v = 0; v + 6 < vertices.size(); v += 7) {
        if (!std::all_of(vertices.begin() + static_cast<std::ptrdiff_t>(v), vertices.begin() + static_cast<std::ptrdiff_t>(v + 7),
                         [](float f) { return std::isfinite(f); })) {
            return;
        }
        x0 = std::min(x0, vertices[v]);
        y0 = std::min(y0, vertices[v + 1]);
        x1 = std::max(x1, vertices[v]);
        y1 = std::max(y1, vertices[v + 1]);
    }
    // A lot is at most 64 tiles a side. Saved ground wider than that, off the lot or not at a number in
    // a broken file is left as it is.
    constexpr float kMostTiles = 64;
    const auto onLot = [](float v) { return v >= -kMostTiles && v <= 2 * kMostTiles; };
    if (!(onLot(x0) && onLot(y0) && onLot(x1) && onLot(y1) && x1 - x0 <= kMostTiles && y1 - y0 <= kMostTiles)) {
        return;
    }
    const int ix0 = static_cast<int>(std::ceil(x0)), iy0 = static_cast<int>(std::ceil(y0));
    const int ix1 = static_cast<int>(std::floor(x1)), iy1 = static_cast<int>(std::floor(y1));
    const int columns = ix1 - ix0 + 1, rows = iy1 - iy0 + 1;
    if (columns < 2 || rows < 2) {
        return;
    }
    // Each corner's texture coordinates, from the saved triangle it lies in. Empty off the saved ground.
    std::vector<std::optional<std::array<float, 5>>> corners(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows));
    for (int iy = iy0; iy <= iy1; ++iy) {
        for (int ix = ix0; ix <= ix1; ++ix) {
            const float px = static_cast<float>(ix), py = static_cast<float>(iy);
            for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
                const float* a = &vertices[static_cast<std::size_t>(indices[i]) * 7];
                const float* b = &vertices[static_cast<std::size_t>(indices[i + 1]) * 7];
                const float* c = &vertices[static_cast<std::size_t>(indices[i + 2]) * 7];
                const float det = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
                if (std::abs(det) < 1e-6f) {
                    continue;
                }
                const float wa = ((b[1] - c[1]) * (px - c[0]) + (c[0] - b[0]) * (py - c[1])) / det;
                const float wb = ((c[1] - a[1]) * (px - c[0]) + (a[0] - c[0]) * (py - c[1])) / det;
                const float wc = 1 - wa - wb;
                constexpr float kEdge = -1e-4f;
                if (wa < kEdge || wb < kEdge || wc < kEdge) {
                    continue;
                }
                std::array<float, 5> out{};
                out[0] = a[2] * wa + b[2] * wb + c[2] * wc;
                for (std::size_t k = 0; k < 4; ++k) {
                    out[k + 1] = a[3 + k] * wa + b[3 + k] * wb + c[3 + k] * wc;
                }
                if (grid->second.contains(level, ix, iy)) {
                    out[0] = grid->second.f32(level, ix, iy, 0);
                    if (water != lot.flatGrids.end()) {
                        if (const auto w = terrainwater::cornerHeight(water->second, ix, iy); w && out[0] < *w) {
                            out[0] = *w - kUnderWater;
                        }
                    }
                }
                corners[static_cast<std::size_t>(iy - iy0) * static_cast<std::size_t>(columns) + static_cast<std::size_t>(ix - ix0)] = out;
                break;
            }
        }
    }
    std::vector<float> fine;
    std::vector<std::uint32_t> fineIndices;
    std::vector<std::uint32_t> at(corners.size(), UINT32_MAX);
    auto corner = [&](int cx, int cy) {
        const std::size_t n = static_cast<std::size_t>(cy) * static_cast<std::size_t>(columns) + static_cast<std::size_t>(cx);
        if (at[n] == UINT32_MAX) {
            const auto& c = *corners[n];
            at[n] = static_cast<std::uint32_t>(fine.size() / 7);
            fine.insert(fine.end(), {static_cast<float>(ix0 + cx), static_cast<float>(iy0 + cy), c[0], c[1], c[2], c[3], c[4]});
        }
        return at[n];
    };
    for (int cy = 0; cy + 1 < rows; ++cy) {
        for (int cx = 0; cx + 1 < columns; ++cx) {
            const auto has = [&](int x, int y) { return corners[static_cast<std::size_t>(y) * static_cast<std::size_t>(columns) + static_cast<std::size_t>(x)].has_value(); };
            if (!has(cx, cy) || !has(cx + 1, cy) || !has(cx, cy + 1) || !has(cx + 1, cy + 1)) {
                continue;
            }
            // Split from the tile's low x and y corner to its high one, as the lot's terrain is.
            const std::uint32_t a = corner(cx, cy), b = corner(cx + 1, cy), d = corner(cx, cy + 1), e = corner(cx + 1, cy + 1);
            fineIndices.insert(fineIndices.end(), {a, b, e, a, e, d});
        }
    }
    if (!fineIndices.empty()) {
        vertices = std::move(fine);
        indices = std::move(fineIndices);
    }
}

} // namespace

bool addImposter(PaintedTerrain& out, const std::filesystem::path& lotPackage, const nhood::LotDescription& lot,
                 float zOffset, std::vector<std::string>& warnings, float* top, const std::map<std::string, Image>* pictures,
                 std::vector<ImposterObject>* objects) {
    dbpf::Package package{lotPackage};
    const auto imposter = readImposter(package, warnings);
    if (!imposter) {
        return false;
    }
    LotTextures res{package, warnings, {}, false, {}};
    // A bake replaces every texture of the lot's imposter, as the game's deletes the old ones before
    // it writes its own, so a picture the bake didn't make is not taken from the package.
    const auto texture = [&](const std::string& name) -> std::optional<std::uint32_t> {
        if (pictures != nullptr) {
            const auto it = pictures->find(name);
            return it != pictures->end() ? std::optional(res.add(out, "picture " + name, it->second)) : std::nullopt;
        }
        return res.texture(out, imposter->prefix + name + "_txtr");
    };

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

    // The lot's own heights, for its ground (see followLotGround).
    std::optional<content::Lot> ground;
    if (std::any_of(imposter->parts.begin(), imposter->parts.end(), [](const auto& part) { return part.kind == PaintedTerrain::ImposterPart::Kind::Terrain; })) {
        try {
            ground = content::loadLotGround(lotPackage);
        } catch (const std::exception&) {
            ground.reset();
        }
    }
    for (const auto& part : imposter->parts) {
        const auto picture = texture(part.texture);
        if (!picture) {
            continue;
        }
        PaintedTerrain::ImposterPart p;
        p.lot = lot.id;
        p.kind = part.kind;
        p.texture = *picture;
        if (!part.lightMap.empty()) {
            if (const auto lightMap = texture(part.lightMap)) {
                p.lightMap = static_cast<int>(*lightMap);
                p.lightMapTint = part.lightMapTint;
            }
        }
        p.vertices = part.vertices;
        p.indices = part.indices;
        if (part.kind == PaintedTerrain::ImposterPart::Kind::Terrain && ground) {
            followLotGround(p.vertices, p.indices, *ground);
        }
        for (std::size_t v = 0; v + 6 < p.vertices.size(); v += 7) {
            const float x = p.vertices[v], y = p.vertices[v + 1], z = p.vertices[v + 2];
            p.vertices[v] = x * c - y * s + tx;
            p.vertices[v + 1] = x * s + y * c + ty;
            p.vertices[v + 2] = z + tz;
            if (top) {
                *top = std::max(*top, z + tz);
            }
        }
        out.imposters.push_back(std::move(p));
    }
    if (objects != nullptr) {
        // The objects hang under the imposter's root, so they move with it.
        const Transform place = Transform::at(tx, ty, tz, angle);
        for (auto& object : readImposterObjects(package, warnings)) {
            object.place = place * object.place;
            objects->push_back(std::move(object));
        }
    }
    return true;
}

bool replaceImposter(PaintedTerrain& terrain, std::uint32_t lot, PaintedTerrain&& with) {
    std::erase_if(terrain.imposters, [&](const PaintedTerrain::ImposterPart& p) { return p.lot == lot; });
    const auto first = static_cast<std::uint32_t>(terrain.imposterTextures.size());
    for (const auto& image : with.imposterTextures) {
        terrain.imposterTextures.push_back(image);
    }
    for (auto& part : with.imposters) {
        if (part.lot != lot) {
            continue;
        }
        part.texture += first;
        if (part.lightMap >= 0) {
            part.lightMap += static_cast<int>(first);
        }
        terrain.imposters.push_back(std::move(part));
    }
    // What the textures no part uses take.
    std::vector<bool> used(terrain.imposterTextures.size(), false);
    for (const auto& part : terrain.imposters) {
        if (part.texture >= used.size() || (part.lightMap >= 0 && static_cast<std::size_t>(part.lightMap) >= used.size())) {
            return false;
        }
        used[part.texture] = true;
        if (part.lightMap >= 0) {
            used[static_cast<std::size_t>(part.lightMap)] = true;
        }
    }
    const auto bytes = [](const Image& image) {
        std::size_t n = image.rgba.size();
        for (const auto& level : image.blockLevels) {
            n += level.size();
        }
        return n;
    };
    std::size_t unused = 0;
    for (std::size_t i = 0; i < used.size(); ++i) {
        unused += used[i] ? 0 : bytes(terrain.imposterTextures[i]);
    }
    if (unused <= kMostUnusedImposterBytes) {
        return false;
    }
    std::vector<int> moved(used.size(), -1);
    SharedImages kept;
    for (std::size_t i = 0; i < used.size(); ++i) {
        if (used[i]) {
            moved[i] = static_cast<int>(kept.size());
            kept.push_back(terrain.imposterTextures[i]);
        }
    }
    terrain.imposterTextures = std::move(kept);
    for (auto& part : terrain.imposters) {
        part.texture = static_cast<std::uint32_t>(moved[part.texture]);
        if (part.lightMap >= 0) {
            part.lightMap = moved[static_cast<std::size_t>(part.lightMap)];
        }
    }
    return true;
}

} // namespace ts2::scene
