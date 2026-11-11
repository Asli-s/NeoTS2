#include "scene/imposter_bake.hpp"

#include "formats/dxt_encode.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace ts2::scene {

namespace {

using Vec = std::array<float, 3>;

Vec operator+(Vec a, Vec b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec operator-(Vec a, Vec b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec operator*(Vec a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
float dot(Vec a, Vec b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec cross(Vec a, Vec b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float length(Vec a) { return std::sqrt(dot(a, a)); }

// The game's top camera for the ground, roofs and slices: it looks down on a 64 by 64 tile square
// from 100 tiles up.
constexpr float kTopSide = 64;
constexpr float kTopHeight = 100;
// Walls are drawn from 5 tiles in front, seeing only what is within half a tile of their plane.
constexpr float kWallDistance = 5;
constexpr float kWallReach = 0.5f;
// The most a slice's slab reaches above or below its height.
constexpr float kMostSlabReach = 0.5f;
// Texture coordinates closer than this are the same.
constexpr float kSame = 1e-4f;

// A flat triangle's texture laid on it: the point at (u, v) is origin + u * alongU + v * alongV.
struct Mapping {
    Vec origin, alongU, alongV;
};

// The mapping of a textured triangle, nullopt when its texture coordinates are degenerate.
std::optional<Mapping> mapping(const std::array<Vec, 3>& p, const std::array<std::array<float, 2>, 3>& t) {
    const float du1 = t[1][0] - t[0][0], dv1 = t[1][1] - t[0][1];
    const float du2 = t[2][0] - t[0][0], dv2 = t[2][1] - t[0][1];
    const float det = du1 * dv2 - du2 * dv1;
    if (std::abs(det) < 1e-9f) {
        return std::nullopt;
    }
    const Vec e1 = p[1] - p[0], e2 = p[2] - p[0];
    Mapping m;
    m.alongU = (e1 * dv2 - e2 * dv1) * (1 / det);
    m.alongV = (e2 * du1 - e1 * du2) * (1 / det);
    m.origin = p[0] - m.alongU * t[0][0] - m.alongV * t[0][1];
    return m;
}

bool near(Vec a, Vec b, float tolerance) { return length(a - b) <= tolerance; }

// One rectangle of a texture and how it lies in the lot.
struct Piece {
    PaintedTerrain::ImposterPart::Kind kind;
    std::string texture;
    Mapping map;
    std::array<float, 4> rect;  // u0, v0, u1, v1
};

// The triangles of the walls and slices as pieces, a triangle joining a piece of the same texture and
// mapping whose rectangle it touches.
std::vector<Piece> pieces(const LotImposter& imposter) {
    std::vector<Piece> out;
    for (const auto& part : imposter.parts) {
        if (part.kind != PaintedTerrain::ImposterPart::Kind::Wall && part.kind != PaintedTerrain::ImposterPart::Kind::Slice) {
            continue;
        }
        for (std::size_t i = 0; i + 2 < part.indices.size(); i += 3) {
            std::array<Vec, 3> p{};
            std::array<std::array<float, 2>, 3> t{};
            for (int k = 0; k < 3; ++k) {
                const std::size_t v = std::size_t{part.indices[i + static_cast<std::size_t>(k)]} * 7;
                p[static_cast<std::size_t>(k)] = {part.vertices[v], part.vertices[v + 1], part.vertices[v + 2]};
                t[static_cast<std::size_t>(k)] = {part.vertices[v + 3], part.vertices[v + 4]};
            }
            const auto map = mapping(p, t);
            if (!map) {
                continue;
            }
            const std::array<float, 4> rect{std::min({t[0][0], t[1][0], t[2][0]}), std::min({t[0][1], t[1][1], t[2][1]}),
                                            std::max({t[0][0], t[1][0], t[2][0]}), std::max({t[0][1], t[1][1], t[2][1]})};
            const auto same = std::find_if(out.begin(), out.end(), [&](const Piece& piece) {
                return piece.texture == part.texture && rect[0] <= piece.rect[2] + kSame && piece.rect[0] <= rect[2] + kSame &&
                       rect[1] <= piece.rect[3] + kSame && piece.rect[1] <= rect[3] + kSame && near(piece.map.alongU, map->alongU, 1e-2f) &&
                       near(piece.map.alongV, map->alongV, 1e-2f) && near(piece.map.origin, map->origin, 1e-2f);
            });
            if (same == out.end()) {
                out.push_back({part.kind, part.texture, *map, rect});
            } else {
                same->rect = {std::min(same->rect[0], rect[0]), std::min(same->rect[1], rect[1]), std::max(same->rect[2], rect[2]),
                              std::max(same->rect[3], rect[3])};
            }
        }
    }
    return out;
}

// A shot square on to a piece: it fills the piece's rectangle, looking at the side its texture faces.
ImposterShot squareOn(const Piece& piece) {
    const auto& m = piece.map;
    const float u = (piece.rect[0] + piece.rect[2]) / 2, v = (piece.rect[1] + piece.rect[3]) / 2;
    const Vec centre = m.origin + m.alongU * u + m.alongV * v;
    const Vec right = m.alongU * (1 / length(m.alongU));
    const Vec up = m.alongV * (-1 / length(m.alongV));
    const Vec facing = cross(right, up);
    const float halfWidth = length(m.alongU) * (piece.rect[2] - piece.rect[0]) / 2;
    const float halfHeight = length(m.alongV) * (piece.rect[3] - piece.rect[1]) / 2;
    ImposterShot shot;
    shot.texture = piece.texture;
    shot.rect = piece.rect;
    shot.up = up;
    shot.orthographic = {-halfWidth, halfWidth, -halfHeight, halfHeight};
    shot.target = centre;
    shot.eye = centre + facing * kWallDistance;
    shot.nearPlane = kWallDistance - kWallReach;
    shot.farPlane = kWallDistance + kWallReach;
    return shot;
}

// The top camera over the whole 64 tile square.
ImposterShot topShot(ImposterShot::Layer layer, const std::string& texture) {
    ImposterShot shot;
    shot.layer = layer;
    shot.texture = texture;
    shot.rect = {0, 0, 1, 1};
    shot.eye = {kTopSide / 2, kTopSide / 2, kTopHeight};
    shot.target = {kTopSide / 2, kTopSide / 2, 0};
    shot.up = {0, 1, 0};
    shot.orthographic = {-kTopSide / 2, kTopSide / 2, -kTopSide / 2, kTopSide / 2};
    shot.nearPlane = kTopHeight - 50;
    shot.farPlane = kTopHeight + 50;
    return shot;
}

} // namespace

std::vector<float> imposterSliceHeights(const LotImposter& imposter) {
    std::vector<float> heights;
    for (const auto& part : imposter.parts) {
        if (part.kind != PaintedTerrain::ImposterPart::Kind::Slice) {
            continue;
        }
        for (const auto i : part.indices) {
            const float z = part.vertices[std::size_t{i} * 7 + 2];
            if (std::none_of(heights.begin(), heights.end(), [&](float h) { return std::abs(h - z) < 1e-3f; })) {
                heights.push_back(z);
            }
        }
    }
    std::sort(heights.begin(), heights.end());
    return heights;
}

std::vector<ImposterShot> imposterShots(const LotImposter& imposter, const std::vector<float>& sliceHeights) {
    std::vector<ImposterShot> out;
    bool terrain = false, roofs = false;
    for (const auto& part : imposter.parts) {
        terrain = terrain || part.kind == PaintedTerrain::ImposterPart::Kind::Terrain;
        roofs = roofs || part.kind == PaintedTerrain::ImposterPart::Kind::Roof;
    }
    if (terrain) {
        out.push_back(topShot(ImposterShot::Layer::Ground, "terrain"));
    }
    if (roofs) {
        out.push_back(topShot(ImposterShot::Layer::Roof, "roofs"));
    }
    for (const auto& piece : pieces(imposter)) {
        ImposterShot shot = squareOn(piece);
        if (piece.kind == PaintedTerrain::ImposterPart::Kind::Wall) {
            shot.layer = ImposterShot::Layer::Walls;
            out.push_back(std::move(shot));
            continue;
        }
        // A slice looks down from the top camera's height onto its slab only.
        shot.layer = ImposterShot::Layer::Floors;
        if (shot.eye[2] < shot.target[2]) {
            continue;
        }
        const float height = shot.target[2];
        const auto at = std::min_element(sliceHeights.begin(), sliceHeights.end(),
                                         [&](float a, float b) { return std::abs(a - height) < std::abs(b - height); });
        float below = kMostSlabReach, above = kMostSlabReach;
        if (at != sliceHeights.end()) {
            if (at != sliceHeights.begin()) {
                below = std::min(kMostSlabReach, (*at - *(at - 1)) / 2);
            }
            if (at + 1 != sliceHeights.end()) {
                above = std::min(kMostSlabReach, (*(at + 1) - *at) / 2);
            }
        }
        shot.eye = {shot.target[0], shot.target[1], kTopHeight};
        shot.target[2] = 0;
        shot.nearPlane = kTopHeight - (height + above);
        shot.farPlane = kTopHeight - (height - below);
        out.push_back(std::move(shot));
    }
    return out;
}

bool inImposterLayer(const Instance& instance, ImposterShot::Layer layer, int groundLevel) {
    if (instance.hidden) {
        return false;
    }
    const bool floor = instance.part == Part::Floor && instance.underside == Underside::None && !instance.poolWater;
    const bool groundFloor = floor && instance.level == groundLevel && !instance.onStage;
    switch (layer) {
    case ImposterShot::Layer::Ground:
        return instance.part == Part::Other || groundFloor || instance.poolWater;
    case ImposterShot::Layer::Floors:
        return floor && !groundFloor;
    case ImposterShot::Layer::Roof:
        return instance.part == Part::Roof;
    case ImposterShot::Layer::Walls:
        return instance.part == Part::Wall || instance.part == Part::WallHub ||
               (instance.part == Part::Object && instance.lightGroup != LightGroup::Sims && (instance.objectType < 10 || instance.objectType > 12));
    }
    return false;
}

void prepareImposterScene(Scene& scene) {
    for (const auto& instance : scene.instances) {
        if (instance.part != Part::Other || instance.mesh < 0 || static_cast<std::size_t>(instance.mesh) >= scene.meshes.size()) {
            continue;
        }
        auto& primitives = scene.meshes[static_cast<std::size_t>(instance.mesh)].primitives;
        std::erase_if(primitives, [&](const Primitive& p) {
            return p.material >= 0 && static_cast<std::size_t>(p.material) < scene.materials.size() &&
                   scene.materials[static_cast<std::size_t>(p.material)].terrainCanvas;
        });
    }
}

void keepImposterCanvas(Scene& scene, const std::function<bool(int, int)>& kept) {
    for (const auto& instance : scene.instances) {
        if (instance.part != Part::Other || instance.mesh < 0 || static_cast<std::size_t>(instance.mesh) >= scene.meshes.size()) {
            continue;
        }
        auto& primitives = scene.meshes[static_cast<std::size_t>(instance.mesh)].primitives;
        const std::size_t count = primitives.size();
        for (std::size_t n = 0; n < count; ++n) {
            const int material = primitives[n].material;
            if (material < 0 || static_cast<std::size_t>(material) >= scene.materials.size() || !scene.materials[static_cast<std::size_t>(material)].terrainCanvas) {
                continue;
            }
            Primitive bed = primitives[n];
            bed.indices.clear();
            const auto& p = primitives[n];
            for (std::size_t i = 0; i + 2 < p.indices.size(); i += 3) {
                float x = 0, y = 0;
                bool inside = true;
                for (std::size_t k = 0; k < 3; ++k) {
                    const std::size_t v = static_cast<std::size_t>(p.indices[i + k]) * 3;
                    inside = inside && v + 1 < p.positions.size();
                    if (inside) {
                        x += p.positions[v] / 3;
                        y += p.positions[v + 1] / 3;
                    }
                }
                if (inside && kept(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)))) {
                    bed.indices.insert(bed.indices.end(), p.indices.begin() + static_cast<std::ptrdiff_t>(i), p.indices.begin() + static_cast<std::ptrdiff_t>(i) + 3);
                }
            }
            if (bed.indices.empty()) {
                continue;
            }
            // A copy of the canvas's material that prepareImposterScene keeps.
            Material copy = scene.materials[static_cast<std::size_t>(material)];
            copy.terrainCanvas = false;
            scene.materials.push_back(std::move(copy));
            bed.material = static_cast<int>(scene.materials.size() - 1);
            // Drawn first, under the paints, as the canvas is.
            primitives.insert(primitives.begin(), std::move(bed));
            return;
        }
    }
}

TexelRect texelRect(const ImposterShot& shot, unsigned side) {
    const float s = static_cast<float>(side);
    const auto texel = [&](float f) { return static_cast<int>(std::lround(std::clamp(f, 0.0f, 1.0f) * s)); };
    return {texel(shot.rect[0]), texel(shot.rect[1]), texel(shot.rect[2]), texel(shot.rect[3])};
}

namespace {

std::uint8_t* texelAt(Image& image, int x, int y) {
    return image.rgba.data() + (static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x)) * 4;
}

bool inside(const Image& image, int x, int y) {
    return x >= 0 && y >= 0 && x < static_cast<int>(image.width) && y < static_cast<int>(image.height);
}

// Repeats a rectangle's edge texels outward, a ring at a time, as the game pads its wall and slice
// rectangles so that filtering at their edges never reads a neighbour.
void repeatEdges(Image& page, TexelRect r, int rings) {
    const auto copy = [&](int fromX, int fromY, int toX, int toY) {
        if (inside(page, fromX, fromY) && inside(page, toX, toY)) {
            std::copy_n(texelAt(page, fromX, fromY), 4, texelAt(page, toX, toY));
        }
    };
    for (int ring = 0; ring < rings && r.width() > 0 && r.height() > 0; ++ring) {
        for (int x = r.x0; x < r.x1; ++x) {
            copy(x, r.y0, x, r.y0 - 1);
            copy(x, r.y1 - 1, x, r.y1);
        }
        --r.y0;
        ++r.y1;
        for (int y = r.y0; y < r.y1; ++y) {
            copy(r.x0, y, r.x0 - 1, y);
            copy(r.x1 - 1, y, r.x1, y);
        }
        --r.x0;
        ++r.x1;
    }
}

// Multiplies the rings at the edge of a texture's bottom-left `width` by `height` texels (where a
// lot lies in the 64 tile square) by 0, 1/rings, 2/rings and so on inward, colour and alpha alike.
void fadeEdge(Image& image, int width, int height, int rings) {
    const int top = static_cast<int>(image.height) - height;
    for (int k = 0; k < rings; ++k) {
        const float factor = static_cast<float>(k) / static_cast<float>(rings);
        const int x0 = k, x1 = width - 1 - k, y0 = top + k, y1 = static_cast<int>(image.height) - 1 - k;
        if (x0 > x1 || y0 > y1) {
            break;
        }
        const auto scale = [&](int x, int y) {
            if (inside(image, x, y)) {
                for (auto* c = texelAt(image, x, y); c != texelAt(image, x, y) + 4; ++c) {
                    *c = static_cast<std::uint8_t>(std::lround(*c * factor));
                }
            }
        };
        for (int x = x0; x <= x1; ++x) {
            scale(x, y0);
            if (y1 != y0) {
                scale(x, y1);
            }
        }
        for (int y = y0 + 1; y < y1; ++y) {
            scale(x0, y);
            if (x1 != x0) {
                scale(x1, y);
            }
        }
    }
}

// The terrain light map in the imposter's layout: the lot's ground storey light map, its colour
// doubled as the game copies it, opaque, and clear off the lot.
Image terrainLightMap(const Image& ground, unsigned side) {
    Image out{side, side, std::vector<std::uint8_t>(std::size_t{side} * side * 4, 0)};
    if (ground.width == 0 || ground.height == 0 || ground.rgba.size() < std::size_t{ground.width} * ground.height * 4) {
        return out;
    }
    const float tilesPerTexel = kTopSide / static_cast<float>(side);
    for (unsigned j = 0; j < side; ++j) {
        for (unsigned i = 0; i < side; ++i) {
            // The light map's texel under this one's centre, 8 to a tile, with rows along +y.
            const float x = (static_cast<float>(i) + 0.5f) * tilesPerTexel * 8;
            const float y = (kTopSide - (static_cast<float>(j) + 0.5f) * tilesPerTexel) * 8;
            const int tx = static_cast<int>(std::floor(x)), ty = static_cast<int>(std::floor(y));
            if (tx < 0 || ty < 0 || tx >= static_cast<int>(ground.width) || ty >= static_cast<int>(ground.height)) {
                continue;
            }
            const std::uint8_t* from = ground.rgba.data() + (static_cast<std::size_t>(ty) * ground.width + static_cast<std::size_t>(tx)) * 4;
            std::uint8_t* to = texelAt(out, static_cast<int>(i), static_cast<int>(j));
            for (int c = 0; c < 3; ++c) {
                to[c] = static_cast<std::uint8_t>(std::min(255, from[c] * 2));
            }
            to[3] = 255;
        }
    }
    return out;
}

} // namespace

void ImposterPages::put(const ImposterShot& shot, const Image& picture, unsigned supersample) {
    const TexelRect r = texelRect(shot, side_);
    if (r.width() <= 0 || r.height() <= 0 || supersample == 0 || picture.width != static_cast<unsigned>(r.width()) * supersample ||
        picture.height != static_cast<unsigned>(r.height()) * supersample || picture.rgba.size() < std::size_t{picture.width} * picture.height * 4) {
        return;
    }
    auto& page = pages_[shot.texture];
    if (page.width == 0) {
        page = Image{side_, side_, std::vector<std::uint8_t>(std::size_t{side_} * side_ * 4, 0)};
    }
    const unsigned n = supersample * supersample;
    for (int y = 0; y < r.height(); ++y) {
        for (int x = 0; x < r.width(); ++x) {
            unsigned sum[4] = {0, 0, 0, 0};
            for (unsigned sy = 0; sy < supersample; ++sy) {
                const std::uint8_t* row = picture.rgba.data() +
                                          ((static_cast<std::size_t>(y) * supersample + sy) * picture.width + static_cast<std::size_t>(x) * supersample) * 4;
                for (unsigned sx = 0; sx < supersample * 4; ++sx) {
                    sum[sx % 4] += row[sx];
                }
            }
            std::uint8_t* to = texelAt(page, r.x0 + x, r.y0 + y);
            for (int c = 0; c < 4; ++c) {
                to[c] = static_cast<std::uint8_t>((sum[c] + n / 2) / n);
            }
        }
    }
    if (shot.layer == ImposterShot::Layer::Walls || shot.layer == ImposterShot::Layer::Floors) {
        padded_.emplace_back(shot.texture, r);
    }
}

void ImposterPages::makeTerrainOpaque(const std::function<bool(int, int)>& opaque) {
    const auto terrain = pages_.find("terrain");
    if (terrain == pages_.end()) {
        return;
    }
    // As the terrain light map lies: rows from the top, along -y.
    const float tilesPerTexel = kTopSide / static_cast<float>(side_);
    for (unsigned j = 0; j < side_; ++j) {
        for (unsigned i = 0; i < side_; ++i) {
            const float x = (static_cast<float>(i) + 0.5f) * tilesPerTexel;
            const float y = kTopSide - (static_cast<float>(j) + 0.5f) * tilesPerTexel;
            if (opaque(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)))) {
                texelAt(terrain->second, static_cast<int>(i), static_cast<int>(j))[3] = 255;
            }
        }
    }
}

std::map<std::string, Image> ImposterPages::finish(const Image* groundLightMap, int lotWidth, int lotDepth) {
    // The game pads by a texel and fades over 4 rings and clears 2 more around the light map at its
    // 128, so High does the same over the same part of the lot.
    const int scale = static_cast<int>(side_ / kImposterSide);
    for (const auto& [texture, rect] : padded_) {
        repeatEdges(pages_[texture], rect, scale);
    }
    padded_.clear();
    const int texelsPerTile = static_cast<int>(side_) / static_cast<int>(kTopSide);
    const int width = std::clamp(lotWidth, 0, static_cast<int>(kTopSide)) * texelsPerTile;
    const int height = std::clamp(lotDepth, 0, static_cast<int>(kTopSide)) * texelsPerTile;
    if (const auto terrain = pages_.find("terrain"); terrain != pages_.end()) {
        fadeEdge(terrain->second, width, height, 4 * scale);
    }
    if (groundLightMap != nullptr) {
        Image light = terrainLightMap(*groundLightMap, side_);
        fadeEdge(light, width, height, 4 * scale);
        // Clear around the lot.
        const int top = static_cast<int>(side_) - height, border = 2 * scale;
        for (int y = top - border; y < static_cast<int>(side_); ++y) {
            for (int x = 0; x < width + border; ++x) {
                if ((x >= width || y < top) && inside(light, x, y)) {
                    std::fill_n(texelAt(light, x, y), 4, std::uint8_t{0});
                }
            }
        }
        pages_["terrainLmap"] = std::move(light);
    }
    return std::move(pages_);
}

std::map<std::string, Image> compressedImposterPictures(std::map<std::string, Image> pages) {
    for (auto& [name, image] : pages) {
        auto blocks = dxt::compressDxt5(image.rgba, image.width, image.height);
        if (blocks.empty()) {
            continue;
        }
        image.blockFormat = kDxt5;
        image.blockLevels = {std::move(blocks)};
        image.rgba = {};
    }
    return pages;
}

} // namespace ts2::scene
