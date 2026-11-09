// The water in the lot's pools and in the ponds dug in its terrain.
#include "scene/lot_scene_builder.hpp"

#include "scene/pool_water.hpp"
#include "scene/terrain_water.hpp"

#include <algorithm>
#include <limits>
#include <optional>

namespace ts2::scene::lotscene {

namespace {

// A box that takes in nothing yet: x, y and z low, then high.
std::array<float, 6> emptyBox() {
    constexpr float kMax = std::numeric_limits<float>::max(), kLow = std::numeric_limits<float>::lowest();
    return {kMax, kMax, kMax, kLow, kLow, kLow};
}

// Gives a flat water sheet its normals, straight up, and its texture coordinates, all 0.
void faceUp(Primitive& p) {
    p.normals.resize(p.positions.size());
    for (std::size_t i = 2; i < p.normals.size(); i += 3) {
        p.normals[i] = 1;
    }
    p.uvs.assign(p.positions.size() / 3 * 2, 0.0f);
}

} // namespace

// The pools' box: their tiles out to their walls' outer faces, from the level below's lowest corner
// (their floor) to the ground storey's highest (their walls' top).
std::array<float, 6> LotSceneBuilder::poolBox(const std::vector<poolwater::Quarter>& quarters, int ground) {
    constexpr float kHalfWall = kWallThickness / 2;
    std::array<float, 6> box = emptyBox();
    for (const auto& q : quarters) {
        const float x = static_cast<float>(q.x), y = static_cast<float>(q.y);
        box[0] = std::min(box[0], x - kHalfWall);
        box[1] = std::min(box[1], y - kHalfWall);
        box[3] = std::max(box[3], x + 1 + kHalfWall);
        box[4] = std::max(box[4], y + 1 + kHalfWall);
        for (const float cx : {x, x + 1}) {
            for (const float cy : {y, y + 1}) {
                box[2] = std::min(box[2], heights_.corner(ground - 1, cx, cy));
                box[5] = std::max(box[5], heights_.corner(ground, cx, cy));
            }
        }
    }
    return box;
}

// The pools' water: its surface, and the tinted sheets under it that darken deeper water.
void LotSceneBuilder::drawPoolWater() {
    const auto quarters = poolwater::waterQuarters(lot_);
    if (quarters.empty()) {
        return;
    }
    const int ground = content::groundGridLevel(lot_);
    auto height = [&](float x, float y) {
        const float below = x == std::floor(x) && y == std::floor(y) ? heights_.corner(ground - 1, x, y) : heights_.tile(ground - 1, x, y);
        return poolwater::surfaceHeight(below);
    };
    Mesh mesh;
    mesh.name = "pool water";
    if (auto s = poolwater::surface(quarters, height); !s.indices.empty()) {
        Primitive p;
        // The game's poolWater-1 material, on poolWaterLayer (5).
        p.material = b_.poolSurfaceMaterial("poolWater-1", "poolShape-body-bump", "causticsTiled", 5);
        if (p.material >= 0) {
            p.positions = std::move(s.positions);
            faceUp(p);
            p.indices = std::move(s.indices);
            mesh.primitives.push_back(std::move(p));
        }
    }
    int n = 0;
    for (auto& layer : poolwater::depthLayers(quarters, height)) {
        Primitive p;
        // The game's PoolDepthLayerMaterial draws on poolWaterLayer (5).
        p.material = b_.blendedColorMaterial("poolWaterLayer-" + std::to_string(++n), {0, 0.5f, 1, layer.opacity}, 5);
        p.positions = std::move(layer.positions);
        faceUp(p);
        p.alphas = std::move(layer.alphas);
        p.indices = std::move(layer.indices);
        mesh.primitives.push_back(std::move(p));
    }
    b_.scene().meshes.push_back(std::move(mesh));
    if (auto* water = b_.place(static_cast<int>(b_.scene().meshes.size() - 1), "pool water", Transform{}, Part::Floor, ground)) {
        water->poolWater = true;
        water->poolBox = poolBox(quarters, ground);
    }
}

// The water standing in the lot's dug terrain, as the game's waterOnTerrain material draws it on
// its layer 1.
void LotSceneBuilder::drawPondWater() {
    const auto water = lot_.flatGrids.find(content::kWaterHeightGridInstance);
    if (water == lot_.flatGrids.end()) {
        return;
    }
    const int ground = content::groundGridLevel(lot_);
    auto terrain = [&](int x, int y) { return heights_.corner(ground, static_cast<float>(x), static_cast<float>(y)); };
    auto built = terrainwater::build(water->second, terrain, assets_.waterAlphaCurve());
    if (built.indices.empty()) {
        return;
    }
    if (b_.poolStylePonds()) {
        drawPondsAsPools(water->second, terrain, ground);
        return;
    }
    Primitive p;
    p.material = b_.pondWaterMaterial("waterOnTerrain", "reflectionoutdoorwater-envcube", {"water-1-bump", "water-2-bump"}, 1);
    if (p.material < 0) {
        return;
    }
    p.positions = std::move(built.positions);
    faceUp(p);
    p.uvs = std::move(built.uvs);
    p.alphas = std::move(built.alphas);
    p.indices = std::move(built.indices);
    Mesh mesh;
    mesh.name = "pond water";
    mesh.primitives.push_back(std::move(p));
    b_.scene().meshes.push_back(std::move(mesh));
    b_.place(static_cast<int>(b_.scene().meshes.size() - 1), "pond water", Transform{}, Part::Floor, ground);
}

// Not in the original game: the ponds' water drawn as the pools' is, the pools' surface and depth
// layers over every tile with water. It shares the pools' reflection and caustics, their box
// widened to take the ponds in.
void LotSceneBuilder::drawPondsAsPools(const terrain::Array2D& water, const std::function<float(int, int)>& terrain, int ground) {
    std::vector<poolwater::Quarter> quarters;
    std::array<float, 6> box = emptyBox();
    for (int y = 0; y + 1 < static_cast<int>(water.height); ++y) {
        for (int x = 0; x + 1 < static_cast<int>(water.width); ++x) {
            bool wet = false;
            for (const auto [dx, dy] : {std::pair{0, 0}, {1, 0}, {0, 1}, {1, 1}}) {
                const auto w = terrainwater::cornerHeight(water, x + dx, y + dy);
                const float t = terrain(x + dx, y + dy);
                if (w && t < *w) {
                    wet = true;
                    box[2] = std::min(box[2], t);
                    box[5] = std::max(box[5], *w);
                }
            }
            if (!wet) {
                continue;
            }
            for (int q = 0; q < 4; ++q) {
                quarters.push_back({x, y, q});
            }
            box[0] = std::min(box[0], static_cast<float>(x));
            box[1] = std::min(box[1], static_cast<float>(y));
            box[3] = std::max(box[3], static_cast<float>(x + 1));
            box[4] = std::max(box[4], static_cast<float>(y + 1));
        }
    }
    if (quarters.empty()) {
        return;
    }
    Mesh mesh;
    mesh.name = "pond water";
    // The water's level at a point: the highest of the corners under water around it. Dry corners
    // hold a lower height of their own, which would bend the surface down at the shore and drop the
    // mirror the pools' reflection is drawn about below the water.
    auto height = [&](float x, float y) {
        std::optional<float> level;
        for (int cx = static_cast<int>(std::ceil(x)) - 1; cx <= static_cast<int>(std::floor(x)) + 1; ++cx) {
            for (int cy = static_cast<int>(std::ceil(y)) - 1; cy <= static_cast<int>(std::floor(y)) + 1; ++cy) {
                if (const auto w = terrainwater::cornerHeight(water, cx, cy); w && terrain(cx, cy) < *w && (!level || *w > *level)) {
                    level = *w;
                }
            }
        }
        return level ? *level : terrainwater::heightAt(water, x, y).value_or(0.0f);
    };
    // The pools' finer surface mesh: the shader waves and lights it per vertex. Ponds take a darker,
    // teal than a pool's blue and murk on the surface, and a murky teal in the depth layers.
    constexpr std::array<float, 3> kPondTint{0.04f, 0.07f, 0.07f};
    constexpr float kPondMurk = 0.55f;
    constexpr std::array<float, 3> kPondDepth{0.06f, 0.1f, 0.1f};
    Primitive p;
    p.material = b_.poolSurfaceMaterial("pondWater-pool", "poolShape-body-bump", "causticsTiled", 5, kPondTint, kPondMurk);
    if (auto s = poolwater::surface(quarters, height); p.material >= 0 && !s.indices.empty()) {
        p.positions = std::move(s.positions);
        faceUp(p);
        p.indices = std::move(s.indices);
        mesh.primitives.push_back(std::move(p));
    }
    // The ground under a point, across its tile's corners.
    auto groundAt = [&](float x, float y) {
        const float fx = x - std::floor(x), fy = y - std::floor(y);
        const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
        const float low = terrain(ix, iy) * (1 - fx) + terrain(ix + 1, iy) * fx;
        const float high = terrain(ix, iy + 1) * (1 - fx) + terrain(ix + 1, iy + 1) * fx;
        return low * (1 - fy) + high * fy;
    };
    // A pool's sheets end on its walls. A pond's run into its banks, so each fades in over 0.75
    // above the ground under it. Below the ground the value stays negative, so the fade reaches
    // 0 about where the sheet meets the bank.
    constexpr float kBankFade = 0.75f;
    // Ponds are murky: their sheets are kMurk times as opaque as a pool's (at most 0.9), and a
    // second three hang below the first (3, 3.75 and 4.5 down), so deep water clouds over further.
    constexpr float kMurk = 6.0f, kMostOpaque = 0.9f, kDeeper = 2.25f;
    std::vector<poolwater::Layer> layers;
    for (const float shift : {0.0f, kDeeper}) {
        for (auto& layer : poolwater::depthLayers(quarters, [&](float x, float y) { return height(x, y) - shift; })) {
            layers.push_back(std::move(layer));
        }
    }
    int n = 0;
    for (auto& layer : layers) {
        for (std::size_t i = 0; i < layer.alphas.size(); ++i) {
            const float* at = &layer.positions[i * 3];
            const float bank = std::min((at[2] - groundAt(at[0], at[1])) / kBankFade, 1.0f);
            layer.alphas[i] = std::min(layer.alphas[i] * bank * layer.opacity * kMurk, kMostOpaque);
        }
        Primitive sheet;
        sheet.material = b_.blendedColorMaterial("pondWaterLayer-" + std::to_string(++n), {kPondDepth[0], kPondDepth[1], kPondDepth[2], 1}, 5);
        sheet.positions = std::move(layer.positions);
        faceUp(sheet);
        sheet.alphas = std::move(layer.alphas);
        sheet.indices = std::move(layer.indices);
        mesh.primitives.push_back(std::move(sheet));
    }
    b_.scene().meshes.push_back(std::move(mesh));
    auto* pond = b_.place(static_cast<int>(b_.scene().meshes.size() - 1), "pond water", Transform{}, Part::Floor, ground);
    if (pond == nullptr) {
        return;
    }
    pond->poolWater = true;
    pond->poolBox = box;
    pond->poolMirrorKeep = 0.05f;
    for (auto& instance : b_.scene().instances) {
        if (instance.poolWater && &instance != pond) {
            for (std::size_t i = 0; i < 3; ++i) {
                instance.poolBox[i] = pond->poolBox[i] = std::min(instance.poolBox[i], pond->poolBox[i]);
                instance.poolBox[i + 3] = pond->poolBox[i + 3] = std::max(instance.poolBox[i + 3], pond->poolBox[i + 3]);
            }
        }
    }
}

} // namespace ts2::scene::lotscene
