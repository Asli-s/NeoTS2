#include "scene/neighborhood_scene.hpp"

#include "content/neighborhood.hpp"
#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "formats/lot_description.hpp"
#include "formats/nhood_objects.hpp"
#include "formats/nhood_terrain.hpp"
#include "scene/imposters.hpp"
#include "scene/neighborhood_paint.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>

namespace ts2::scene {

namespace {

// A tile is 10 world units (the game's terrain keeps it at 10).
constexpr float kTile = 10.0f;
constexpr float kEffectAboveLot = 2.0f;  // the effect over a lot (its plumbob) starts this far above it

// The smallest power of two above n, as the game sizes its alpha maps (256 for 128 tiles).
std::uint32_t powerAbove(std::uint32_t n) {
    std::uint32_t v = n;
    v |= v >> 16;
    v |= v >> 8;
    v |= v >> 4;
    v |= v >> 2;
    return (v >> 1 | v) + 1;
}

// The game's random numbers: state * 0x41C64E6D + 12345, each draw bits 16 to 47 of that sum.
struct GameRandom {
    std::uint32_t state;
    std::uint32_t next() {
        const std::uint64_t sum = static_cast<std::uint64_t>(state) * 0x41C64E6Du + 12345u;
        state = static_cast<std::uint32_t>(sum);
        return static_cast<std::uint32_t>(sum >> 16);
    }
    std::uint32_t below(std::uint32_t n) { return static_cast<std::uint32_t>((static_cast<std::uint64_t>(next()) * n) >> 32); }
};

// A copy of an image at another size, nearest texel.
Image resized(const Image& image, std::uint32_t width, std::uint32_t height) {
    Image out{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t from = (static_cast<std::size_t>(y * image.height / height) * image.width + x * image.width / width) * 4;
            std::copy_n(&image.rgba[from], 4, &out.rgba[(static_cast<std::size_t>(y) * width + x) * 4]);
        }
    }
    return out;
}

} // namespace

std::array<std::int64_t, 4> effectTiles(const nhood::LotDescription& lot) {
    const unsigned k = (static_cast<unsigned>(lot.facing) - lot.ownFacing + 4) & 3;
    const bool turned = (k & 1) != 0;
    const std::int64_t x = lot.x, y = lot.y;
    std::array<std::int64_t, 4> rect{x, y, x + (turned ? lot.depth : lot.width), y + (turned ? lot.width : lot.depth)};
    for (unsigned side = 0; side < 4; ++side) {
        if (lot.roadSides & (1u << side)) {
            switch ((side + k) & 3) {
            case 0: ++rect[0]; break;
            case 1: --rect[3]; break;
            case 2: --rect[2]; break;
            default: ++rect[1]; break;
            }
        }
    }
    return rect;
}

NeighborhoodSceneStats addNeighborhood(SceneBuilder& builder, const std::filesystem::path& neighborhood,
                                       const nhood::PaintRules& rules, const NeighborhoodLight& light,
                                       const std::map<std::string, std::string>& roadTextures, float imposterZOffset,
                                       const std::map<std::uint32_t, content::NeighborhoodObjectType>& objectTypes,
                                       std::optional<std::uint32_t> variantSeed) {
    dbpf::Package package{content::neighborhoodPackage(neighborhood)};
    std::optional<nhood::Terrain> terrain;
    std::optional<nhood::SavedCamera> camera;
    nhood::Objects objects;
    std::vector<nhood::LotDescription> lots;
    for (const auto& e : package.entries()) {
        if (e.key.type == nhood::kTerrainType && !terrain) {
            terrain = nhood::parseTerrain(package.read(e));
        } else if (e.key.type == nhood::kSavedCameraType && !camera) {
            camera = nhood::parseSavedCamera(package.read(e));
        } else if (e.key.type == nhood::kObjectsType) {
            objects = nhood::parseObjects(package.read(e));
        } else if (e.key.type == nhood::kLotDescriptionType) {
            try {
                lots.push_back(nhood::parseLotDescription(package.read(e)));
            } catch (const std::exception& ex) {
                builder.assets().warnings.push_back("lot description: " + std::string(ex.what()));
            }
        }
    }
    if (!terrain || terrain->columns < 2 || terrain->rows < 2) {
        throw std::runtime_error("the neighborhood has no terrain");
    }
    // The game's own neighborhoods are 128 tiles across. Anything over 1024 is a broken file.
    constexpr std::uint32_t kMaxCorners = 1025;
    if (terrain->columns > kMaxCorners || terrain->rows > kMaxCorners) {
        throw std::runtime_error("the neighborhood's terrain is too large");
    }
    const auto& t = *terrain;
    NeighborhoodSceneStats stats;
    stats.width = t.width;
    stats.height = t.height;
    stats.seaLevel = t.seaLevel;
    stats.terrainType = t.type;
    stats.camera = camera;

    // The tiles under roads, which the paints keep their distance from.
    const std::uint32_t tilesX = t.columns - 1, tilesY = t.rows - 1;
    std::vector<std::uint8_t> roadTiles(static_cast<std::size_t>(tilesX) * tilesY, 0);
    for (const auto& road : objects.roads) {
        const auto x = static_cast<std::int64_t>(std::lround(road.bounds[0] / kTile));
        const auto y = static_cast<std::int64_t>(std::lround(road.bounds[1] / kTile));
        if (x >= 0 && y >= 0 && x < tilesX && y < tilesY) {
            roadTiles[static_cast<std::size_t>(y) * tilesX + static_cast<std::size_t>(x)] = 1;
        }
    }

    // The paints, then one alpha map per paint the terrain uses.
    const auto painted = paintTerrain(t, rules, kTile, roadTiles);
    PaintedTerrain out;
    out.columns = t.columns;
    out.rows = t.rows;
    out.tileSize = kTile;
    out.heights = t.heights;
    out.normals = painted.normals;
    // Square maps covering both sides (the game's terrains are square, a broken file needn't be).
    out.alphaSize = powerAbove(std::max(t.columns, t.rows) - 1);
    std::map<std::uint8_t, std::size_t> layerOf;
    for (const auto& v : painted.vertices) {
        for (int i = 0; i < 3 && v.id[i] != 0; ++i) {
            layerOf.emplace(v.id[i], 0);
        }
    }
    std::size_t layer = 0;
    for (auto& [id, index] : layerOf) {
        index = layer++;
    }
    const std::size_t mapBytes = static_cast<std::size_t>(out.alphaSize) * out.alphaSize;
    out.alphas.assign(layerOf.size() * mapBytes, 0);
    for (std::uint32_t y = 0; y < t.rows; ++y) {
        for (std::uint32_t x = 0; x < t.columns; ++x) {
            const auto& v = painted.vertices[static_cast<std::size_t>(y) * t.columns + x];
            for (int i = 0; i < 3 && v.id[i] != 0; ++i) {
                out.alphas[layerOf[v.id[i]] * mapBytes + static_cast<std::size_t>(y) * out.alphaSize + x] =
                    static_cast<std::uint8_t>(v.weight[i]);
            }
        }
    }

    // The paint textures: a paint with two or more variants names them "<texture>-NN".
    std::vector<std::vector<std::uint8_t>> variantTextures(layerOf.size());
    for (const auto& [id, index] : layerOf) {
        const auto info = rules.paints.find(id);
        if (info == rules.paints.end()) {
            continue;
        }
        const int variants = std::max(1, info->second.variants);
        for (int v = 0; v < variants; ++v) {
            std::string name = info->second.texture;
            if (variants >= 2) {
                char suffix[8];
                std::snprintf(suffix, sizeof suffix, "-%02d", v);
                name += suffix;
            }
            // Layers are picked by a byte, so there are at most 256.
            if (out.textures.size() > UINT8_MAX) {
                builder.assets().warnings.push_back("too many neighborhood paint textures, skipped: " + name);
                break;
            }
            auto image = builder.assets().texture(name);
            if (!image) {
                builder.assets().warnings.push_back("neighborhood paint texture missing: " + name);
                continue;
            }
            if (!out.textures.empty() && (image->width != out.textures[0].width || image->height != out.textures[0].height)) {
                *image = resized(*image, out.textures[0].width, out.textures[0].height);
            }
            variantTextures[index].push_back(static_cast<std::uint8_t>(out.textures.size()));
            out.textures.push_back(std::move(*image));
        }
    }

    // The variant grid: a random byte per cell, new on every load as in the game. Each paint shows
    // the variant that byte picks.
    out.cellTiles = static_cast<std::uint32_t>(std::max(1, rules.textureVariantGridSize));
    out.cellsX = (t.columns - 1 + out.cellTiles - 1) / out.cellTiles;
    out.cellsY = (t.rows - 1 + out.cellTiles - 1) / out.cellTiles;
    GameRandom random{variantSeed ? *variantSeed : std::random_device{}()};
    std::vector<std::uint8_t> cellBytes(static_cast<std::size_t>(out.cellsX) * out.cellsY);
    for (auto& b : cellBytes) {
        b = static_cast<std::uint8_t>(random.below(256));
    }
    out.cellTextures.assign(layerOf.size() * cellBytes.size(), 0);
    for (std::size_t l = 0; l < layerOf.size(); ++l) {
        const auto& choices = variantTextures[l];
        for (std::size_t c = 0; c < cellBytes.size(); ++c) {
            out.cellTextures[l * cellBytes.size() + c] = choices.empty() ? 0 : choices[cellBytes[c] % choices.size()];
        }
    }
    // The light map (no trees or lots yet to shade it).
    out.lightMap = neighborhoodLightMap(t.columns, t.rows, t.heights, painted.normals, kTile, light);

    // The water: a tile gets the water sheet when a corner of it is under the water level.
    out.waterLevel = t.seaLevel;
    out.waterTiles.assign(static_cast<std::size_t>(t.columns - 1) * (t.rows - 1), 0);
    for (std::uint32_t y = 0; y + 1 < t.rows; ++y) {
        for (std::uint32_t x = 0; x + 1 < t.columns; ++x) {
            bool wet = false;
            for (std::uint32_t dy = 0; dy < 2; ++dy) {
                for (std::uint32_t dx = 0; dx < 2; ++dx) {
                    wet = wet || t.heights[static_cast<std::size_t>(y + dy) * t.columns + x + dx] < t.seaLevel;
                }
            }
            out.waterTiles[static_cast<std::size_t>(y) * (t.columns - 1) + x] = wet ? 1 : 0;
        }
    }
    auto& assets = builder.assets();
    if (auto ripples = assets.texture("neighborhood-water-1")) {
        out.ripples = std::move(*ripples);
    }
    if (auto gradient = assets.texture("neighborhood-underwater-gradient")) {
        out.underwaterGradient = std::move(*gradient);
    }
    out.sky = assets.textureFaces("neighborhood-sky2-envcube");

    // The roads, each piece showing the texture its material names.
    std::map<std::uint32_t, std::uint32_t> textureOf;  // by piece
    for (const auto& road : objects.roads) {
        auto known = textureOf.find(road.piece);
        if (known == textureOf.end()) {
            char name[96];
            std::snprintf(name, sizeof name, "neighborhood-roads-%s-%08x", t.type.c_str(), road.piece);
            const auto texture = roadTextures.find(name);
            auto image = texture == roadTextures.end() ? std::nullopt : assets.texture(texture->second);
            if (!image) {
                assets.warnings.push_back(std::string("neighborhood road texture missing for ") + name);
                textureOf[road.piece] = UINT32_MAX;
                continue;
            }
            if (!out.roadTextures.empty() && (image->width != out.roadTextures[0].width || image->height != out.roadTextures[0].height)) {
                *image = resized(*image, out.roadTextures[0].width, out.roadTextures[0].height);
            }
            known = textureOf.emplace(road.piece, static_cast<std::uint32_t>(out.roadTextures.size())).first;
            out.roadTextures.push_back(std::move(*image));
        }
        if (known->second != UINT32_MAX) {
            out.roads.push_back({road.corners, known->second, road.piece});
        }
    }

    // The lots, as their imposters.
    out.imposterLift = imposterZOffset;
    const std::string hood = neighborhood.filename().string();
    stats.lots = lots.size();
    for (const auto& lot : lots) {
        // The tiles it covers: its width along x unless placed a quarter turn from how it was built.
        const bool turned = ((static_cast<unsigned>(lot.facing) - lot.ownFacing) & 1) != 0;
        const float w = static_cast<float>(turned ? lot.depth : lot.width), d = static_cast<float>(turned ? lot.width : lot.depth);
        // The lot itself goes where its imposter does (without the imposter's lift): turned by its
        // placed facing less the one it was built with, then moved onto its tiles.
        const unsigned k = (static_cast<unsigned>(lot.facing) - lot.ownFacing + 4) & 3;
        const float lw = static_cast<float>(lot.width), ld = static_cast<float>(lot.depth);
        const float offsets[4][2] = {{0, 0}, {0, lw}, {lw, ld}, {ld, 0}};
        const Transform place = Transform::at((static_cast<float>(lot.x) + offsets[k][0]) * kTile, (static_cast<float>(lot.y) + offsets[k][1]) * kTile,
                                              lot.elevation, -static_cast<float>(k) * 1.5707963267948966f);
        stats.lotFootprints.push_back({lot.id, static_cast<float>(lot.x) * kTile, static_cast<float>(lot.y) * kTile,
                                       (static_cast<float>(lot.x) + w) * kTile, (static_cast<float>(lot.y) + d) * kTile, lot.elevation,
                                       lot.elevation, place, lot.texture});
        const auto rect = effectTiles(lot);
        stats.lotFootprints.back().effectAnchor = {static_cast<float>(rect[0] + rect[2]) * kTile / 2,
                                                   static_cast<float>(rect[1] + rect[3]) * kTile / 2, lot.elevation + kEffectAboveLot};
        const auto file = neighborhood / "Lots" / (hood + "_Lot" + std::to_string(lot.id) + ".package");
        if (!std::filesystem::is_regular_file(file)) {
            continue;
        }
        try {
            float top = -std::numeric_limits<float>::infinity();
            if (addImposter(out, file, lot, imposterZOffset, assets.warnings, &top)) {
                ++stats.imposters;
                stats.lotFootprints.back().hasImposter = true;
                // An imposter with no usable vertex keeps the lot's own height.
                if (std::isfinite(top)) {
                    stats.lotFootprints.back().top = top;
                    stats.lotFootprints.back().effectAnchor[2] = top + kEffectAboveLot;
                }
            }
        } catch (const std::exception& ex) {
            assets.warnings.push_back(file.filename().string() + ": " + ex.what());
        }
    }
    builder.scene().terrain = std::move(out);

    // The plants and decorations: their catalog entry's model, standing at their position, turned by
    // their angle. Entries of the "effects" section (surf, birds, wild flowers...) are kept for their
    // effects instead. Their model is only a test-textured stand-in with slots the effects play at.
    builder.setDrawShadows(true);
    auto placeAll = [&](const std::vector<nhood::PlacedObject>& list) {
        std::size_t shown = 0;
        for (const auto& item : list) {
            const auto type = objectTypes.find(item.type);
            if (type == objectTypes.end() || type->second.model.empty()) {
                continue;
            }
            if (!std::isfinite(item.angle) || !std::all_of(item.position.begin(), item.position.end(), [](float v) { return std::isfinite(v); })) {
                diag::warnOnce("neighborhood: a plant or decoration with a broken place is left out");
                continue;
            }
            if (type->second.sort == "effects") {
                stats.effectDecorations.push_back({type->second.model, item.position, item.angle});
                continue;
            }
            const int mesh = builder.model(type->second.model, {});
            const float angle = item.angle * 3.14159265358979f / 180.0f;
            if (builder.place(mesh, type->second.model, Transform::at(item.position[0], item.position[1], item.position[2], angle))) {
                ++shown;
            }
        }
        return shown;
    };
    stats.plants = objects.flora.size();
    stats.plantsShown = placeAll(objects.flora);
    stats.decorations = objects.decorations.size();
    stats.decorationsShown = placeAll(objects.decorations);

    // The bridges: each piece's model at its tile's centre, turned by its rotation.
    for (const auto& bridge : objects.bridges) {
        const auto& q = bridge.rotation;
        const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (!std::isfinite(length) || std::abs(length - 1) > 0.01f ||
            !std::all_of(bridge.road.center.begin(), bridge.road.center.end(), [](float v) { return std::isfinite(v); })) {
            diag::warnOnce("neighborhood: a bridge piece with a broken place is left out");
            continue;
        }
        char name[32];
        std::snprintf(name, sizeof name, "%04x-bridge", bridge.road.piece >> 16);
        const int mesh = builder.model(name, {});
        if (mesh < 0) {
            diag::warnOnce(std::string("neighborhood: bridge piece ") + name + " is missing");
            continue;
        }
        if (builder.place(mesh, name, Transform::of(bridge.road.center, q))) {
            ++stats.bridgesShown;
        }
    }

    return stats;
}

} // namespace ts2::scene
