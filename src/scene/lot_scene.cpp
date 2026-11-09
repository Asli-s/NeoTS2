#include "scene/lot_scene.hpp"
#include "scene/lot_scene_builder.hpp"

#include "content/lot.hpp"
#include "content/neighborhood.hpp"
#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "formats/objd.hpp"
#include "formats/resource_types.hpp"
#include "scene/lot_parts.hpp"
#include "scene/pool_water.hpp"
#include "scene/terrain_water.hpp"
#include "scene/rig.hpp"
#include "scene/sim_builder.hpp"
#include "scene/sims.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>

namespace sg = ts2::scenegraph;

namespace ts2::scene {

Heights::Heights(const content::Lot& lot) {
    if (const auto g = lot.grids.find(content::kFloorHeightGridInstance); g != lot.grids.end() && g->second.elementSize == 4) {
        grid_ = &g->second;
    }
}

Heights::Heights(std::shared_ptr<const array3d::Array3D> grid) : kept_(std::move(grid)), grid_(kept_.get()) {}

float Heights::at(int level, int x, int y, bool& found) const {
    found = grid_ != nullptr && grid_->contains(level, x, y);
    if (!found) {
        return static_cast<float>(level) * kLevelHeight;
    }
    return grid_->f32(level, x, y, 0);
}

float Heights::corner(int level, float x, float y) const {
    bool found = false;
    return at(level, static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)), found);
}

std::vector<float> Heights::drawnGrid(int level, std::uint32_t steps, std::optional<float> sea) const {
    if (grid_ == nullptr || grid_->width < 2 || grid_->height < 2 || steps == 0) {
        return {};
    }
    const std::uint32_t columns = (grid_->width - 1) * steps + 1, rows = (grid_->height - 1) * steps + 1;
    std::vector<float> h(static_cast<std::size_t>(columns) * rows);
    for (std::uint32_t i = 0; i < columns; ++i) {
        for (std::uint32_t j = 0; j < rows; ++j) {
            const std::uint32_t x = i / steps, y = j / steps;
            const float fx = static_cast<float>(i % steps) / static_cast<float>(steps), fy = static_cast<float>(j % steps) / static_cast<float>(steps);
            bool found = false;
            const auto at = [&](std::uint32_t cx, std::uint32_t cy) {
                return this->at(level, static_cast<int>(std::min(cx, grid_->width - 1)), static_cast<int>(std::min(cy, grid_->height - 1)), found);
            };
            const float top = at(x, y) * (1 - fx) + at(x + 1, y) * fx;
            const float bottom = at(x, y + 1) * (1 - fx) + at(x + 1, y + 1) * fx;
            h[static_cast<std::size_t>(i) * rows + j] = top * (1 - fy) + bottom * fy;
        }
    }
    if (!sea || columns < 3 || rows < 3) {
        return h;
    }
    // A few passes of each point taking its neighbours' average, about a tile and a half across.
    constexpr int kPasses = 24;
    constexpr float kAboveSea = 0.6f;
    std::vector<char> moves(h.size(), 0);
    for (std::uint32_t i = 1; i + 1 < columns; ++i) {
        for (std::uint32_t j = 1; j + 1 < rows; ++j) {
            moves[static_cast<std::size_t>(i) * rows + j] = h[static_cast<std::size_t>(i) * rows + j] < *sea + kAboveSea;
        }
    }
    std::vector<float> next = h;
    for (int pass = 0; pass < kPasses; ++pass) {
        for (std::uint32_t i = 1; i + 1 < columns; ++i) {
            for (std::uint32_t j = 1; j + 1 < rows; ++j) {
                const std::size_t k = static_cast<std::size_t>(i) * rows + j;
                if (!moves[k]) {
                    continue;
                }
                float sum = 0;
                for (int di = -1; di <= 1; ++di) {
                    for (int dj = -1; dj <= 1; ++dj) {
                        sum += h[static_cast<std::size_t>(static_cast<int>(i) + di) * rows + static_cast<std::size_t>(static_cast<int>(j) + dj)];
                    }
                }
                next[k] = sum / 9;
            }
        }
        h.swap(next);
        next = h;
    }
    return h;
}

float Heights::tile(int level, float x, float y) const {
    float sum = 0;
    int n = 0;
    for (const int dx : {0, 1}) {
        for (const int dy : {0, 1}) {
            bool found = false;
            const float v = at(level, static_cast<int>(x) + dx, static_cast<int>(y) + dy, found);
            if (found) {
                sum += v;
                ++n;
            }
        }
    }
    return n ? sum / static_cast<float>(n) : static_cast<float>(level) * kLevelHeight;
}

bool isExtraTile(Assets& assets, const content::LotObject& object) {
    const auto* d = object.type ? assets.objectDefinition(object.type->guid) : nullptr;
    return d && d->field(objd::field::kMultiTileMasterId) != 0 && d->field(objd::field::kMultiTileSubIndex) != 0xFFFF &&
           object.state.data[lotscene::kLeadTileData] > 0 && object.state.data[lotscene::kLeadTileData] != static_cast<std::int32_t>(object.id);
}

bool surfaceOpen(const array3d::Array3D& surface, int level, float x, float y) {
    if (surface.elementSize != 4 || !(x >= 0) || !(y >= 0) || x >= static_cast<float>(surface.width) || y >= static_cast<float>(surface.height)) {
        return false;
    }
    const auto tileX = static_cast<std::int32_t>(x), tileY = static_cast<std::int32_t>(y);
    const auto quarters = surface.at(level, tileX, tileY);
    if (quarters.size() != 4) {
        return false;
    }
    // The quarter by the nearest tile edge: x low, y high, x high, y low.
    const float u = x - static_cast<float>(tileX), v = y - static_cast<float>(tileY);
    const std::array<float, 4> distance{u, 1 - v, 1 - u, v};
    const auto q = static_cast<std::size_t>(std::min_element(distance.begin(), distance.end()) - distance.begin());
    return quarters[q] == 0;
}

bool surfaceQuarterOpen(const array3d::Array3D& surface, int level, int x, int y, std::size_t quarter) {
    const auto quarters = surface.at(level, x, y);
    return surface.elementSize == 4 && quarters.size() == 4 && quarter < 4 && quarters[quarter] == 0;
}

std::string withSavedCutout(const std::string& material, const std::string& group,
                            const std::vector<scenegraph::MaterialOverride>& saved) {
    static constexpr std::string_view kCutout = "-cutout-";
    if (group.empty()) {
        return material;
    }
    const std::string low = lower(group);
    const auto named = std::find_if(saved.rbegin(), saved.rend(),
                                    [&](const scenegraph::MaterialOverride& o) { return lower(o.group) == low; });
    if (named == saved.rend()) {
        return material;
    }
    const auto mask = lower(named->material).find(kCutout);
    if (mask == std::string::npos) {
        return material;
    }
    return material.substr(0, lower(material).find(kCutout)) + named->material.substr(mask);
}

namespace lotscene {

LotSceneStats LotSceneBuilder::build() {
    edge_ = b_.colorMaterial("wall edge", {0.36f, 0.25f, 0.18f});
    // Wall tops: the game's wall_top material, its texture tiled and unlit (no light map or lights).
    wallTop_ = b_.textureMaterial("wall-top");
    if (wallTop_ >= 0) {
        b_.scene().materials[static_cast<std::size_t>(wallTop_)].unlit = true;
    } else {
        wallTop_ = edge_;
    }
    collectWalls();
    placeFencePosts();
    // Where something the simulation moves stands: the floor of its storey (1 = ground), kept past the lot.
    const auto kept = std::make_shared<const Heights>(heights_.grid() ? std::make_shared<const array3d::Array3D>(*heights_.grid()) : nullptr);
    const int groundLevel = content::gridLevel(lot_, 1);
    b_.scene().floorHeight = [kept, groundLevel](int storey, float x, float y) { return kept->tile(storey - 1 + groundLevel, x, y); };
    b_.scene().groundLevel = groundLevel;
    b_.scene().boundsTopLevel = content::topGridLevel(lot_);
    if (const auto floors = lot_.grids.find(content::kFloorGridInstance); floors != lot_.grids.end() && floors->second.elementSize == 8) {
        const auto grid = std::make_shared<const std::remove_cvref_t<decltype(floors->second)>>(floors->second);
        b_.scene().floorCorner = [kept, grid](int level, int x, int y) -> std::optional<float> {
            if (level < 0 || x < 0 || y < 0 || level >= static_cast<int>(grid->levels) || x >= static_cast<int>(grid->width) ||
                y >= static_cast<int>(grid->height)) {
                return std::nullopt;
            }
            for (int q = 0; q < 4; ++q) {
                if (grid->i16(level, x, y, q) != 0) {
                    return kept->corner(level, static_cast<float>(x), static_cast<float>(y));
                }
            }
            return std::nullopt;
        };
    }
    placeObjects();
    placeSims();
    cutWalls();
    shapeRoofs();
    drawWalls();
    drawTerrain();
    drawFloors();
    drawPoolWater();
    drawPondWater();
    placeRoofs();
    // The sky behind a lot by day, as the game's lot sky effect draws it (its plain-weather
    // skybox, seasons and weather pick others). Lighting the lot puts in its state's sky.
    b_.scene().skybox = assets_.textureFaces("skymap-generic-day-highres-envcube");
    return stats_;
}

void LotSceneBuilder::collectWalls() {
    if (!lot_.roomGraph) {
        return;
    }
    std::map<std::int32_t, const walllayer::Entry*> coverings;
    for (const auto& c : lot_.wallCoverings) {
        coverings[static_cast<std::int32_t>(c.wall)] = &c;
    }
    const auto& rooms = *lot_.roomGraph;
    const auto& styles = assets_.buildScripts().wallStyles;
    // The graph also traces the lot's border, where there is no wall.
    auto onBorder = [&](const wallgraph::Point* p) {
        return p->x <= 0 || p->y <= 0 || p->x >= static_cast<float>(rooms.width) || p->y >= static_cast<float>(rooms.height);
    };
    // Points off the lot (or not numbers, in a broken file) make no wall.
    auto onLot = [](const wallgraph::Point* p) { return p->x >= -1 && p->x <= 257 && p->y >= -1 && p->y <= 257; };
    const wallgraph::PointIndex pointById(rooms);
    for (const auto& w : rooms.walls) {
        const auto* a = pointById(w.from);
        const auto* b = pointById(w.to);
        if (a == nullptr || b == nullptr || !onLot(a) || !onLot(b)) {
            continue;
        }
        const int level = content::gridIndex(lot_, a->level);
        const auto covering = coverings.find(w.id);
        // A segment with no covering only divides rooms: there is no wall there.
        if (covering == coverings.end()) {
            continue;
        }
        // A fence rail is a wall segment whose type is the fence's GUID: one tile of rail from a to b.
        if (const auto* fence = assets_.fence(covering->second->type)) {
            const float dx = b->x - a->x, dy = b->y - a->y;
            const std::string& model = dx != 0 && dy != 0 ? fence->diagonalRail : fence->rail;
            if (const int mesh = model.empty() ? -1 : b_.model(model, {}); mesh >= 0) {
                if (auto* rail = b_.place(mesh, "rail " + model, Transform::at(a->x, a->y, heights_.corner(level, a->x, a->y), std::atan2(dy, dx)),
                                          Part::Object, level)) {
                    rail->sunTest = SunTest::AboveBox;
                }
                ++stats_.fenceRails;
            }
            continue;
        }
        // The game adds attic walls under gable ends, often with the outdoors on both sides.
        const auto style = styles.find(covering->second->type);
        const bool toRoof = style != styles.end() && style->second.toRoof;
        if ((w.leftRoom == w.rightRoom && !toRoof) || (onBorder(a) && onBorder(b))) {
            continue;
        }
        ThickWall t;
        t.q = {a->x, a->y, b->x, b->y, heights_.corner(level, a->x, a->y), heights_.corner(level + 1, a->x, a->y),
               heights_.corner(level, b->x, b->y), heights_.corner(level + 1, b->x, b->y)};
        t.points = {w.from, w.to};
        t.level = level;
        t.toRoof = toRoof;
        // Room 0 is the outdoors, on every level.
        t.roomLeft = w.leftRoom != 0;
        t.roomRight = w.rightRoom != 0;
        if (style != styles.end()) {
            if (!style->second.rendered) {
                continue;
            }
            t.thick = style->second.thick;
            t.cuttable = style->second.cutAway;
            t.clear = !style->second.blocksLight && style->first != kPoolEdgeStyle;
            if (style->second.belowFloor) {
                t.q.top0 -= kFloorThickness;
                t.q.top1 -= kFloorThickness;
            }
        }
        // Each side shows its own covering. A side whose covering has no material (the inside
        // of a foundation) is not drawn.
        auto side = [&](std::uint16_t id) {
            const auto pattern = lot_.wallPatterns.find(id);
            if (pattern == lot_.wallPatterns.end()) {
                return std::string();
            }
            const std::string material = assets_.patternMaterial(pattern->second.name, true);
            if (b_.material(material) < 0) {
                return std::string();
            }
            // The pool's own walls show its caustics (the game names their material "PoolWall").
            return style != styles.end() && style->first == kPoolEdgeStyle ? b_.poolWallMaterial(material) : material;
        };
        t.left = t.leftCovering = side(covering->second->left);
        t.right = t.rightCovering = side(covering->second->right);
        ++stats_.coveredWalls;
        walls_.push_back(std::move(t));
    }
    stats_.walls = walls_.size();
    wallBase_ = static_cast<int>(b_.scene().walls.size());
    for (std::size_t i = 0; i < walls_.size(); ++i) {
        const auto& t = walls_[i];
        b_.scene().walls.push_back({t.q.x0, t.q.y0, t.q.x1, t.q.y1, t.level, t.roomLeft, t.roomRight, t.cuttable, t.toRoof});
        wallAt_[edgeKey(t.q.x0, t.q.y0, t.q.x1, t.q.y1, t.level)] = i;
    }
}

void LotSceneBuilder::placeFencePosts() {
    for (const auto& post : lot_.fencePosts) {
        const auto* fence = assets_.fence(post.fence);
        if (fence == nullptr || fence->post.empty()) {
            continue;
        }
        if (const int mesh = b_.model(fence->post, {}); mesh >= 0) {
            const int level = content::gridIndex(lot_, post.level);
            if (auto* placed = b_.place(mesh, "post " + fence->post, Transform::at(post.x, post.y, heights_.corner(level, post.x, post.y), 0),
                                        Part::Object, level)) {
                placed->sunTest = SunTest::AboveBox;
            }
            ++stats_.fencePosts;
        }
    }
}

LightMaps& LotSceneBuilder::lightMaps() {
    if (!b_.scene().lightMaps) {
        b_.scene().lightMaps.emplace();
    }
    return *b_.scene().lightMaps;
}

// An incidence map for a light map (see incidenceMap): straight out until the light is filled
// in. Added after the light map's own image.
int LotSceneBuilder::addIncidence(const Image& lightMap) {
    Image incidence;
    incidence.width = std::max(1u, lightMap.width / 2);
    incidence.height = std::max(1u, lightMap.height / 2);
    incidence.rgba.reserve(static_cast<std::size_t>(incidence.width) * incidence.height * 4);
    for (std::size_t i = 0; i < static_cast<std::size_t>(incidence.width) * incidence.height; ++i) {
        incidence.rgba.insert(incidence.rgba.end(), {128, 128, 255, 255});
    }
    b_.scene().images.push_back(std::move(incidence));
    return static_cast<int>(b_.scene().images.size()) - 1;
}

// Each level's floor light map, 8 texels a tile, grey (no light worked out yet) until it is filled.
void LotSceneBuilder::makeFloorLightMaps() {
    auto& maps = lightMaps();
    const auto floors = lot_.grids.find(content::kFloorGridInstance);
    if (!maps.floors.empty() || floors == lot_.grids.end()) {
        return;
    }
    // Each map has 8 texels per tile side, and each level a map and an incidence map. A lot larger
    // than the lighting accepts gets none, so a broken grid size can't ask for gigabytes. In all
    // the maps stay under 256 x 256 tiles over 4 levels, about 128 MB.
    constexpr std::uint64_t kMaxTiles = 256, kMaxLevels = 64, kMaxTileLevels = 256 * 256 * 4;
    const auto& grid = floors->second;
    if (grid.width > kMaxTiles || grid.height > kMaxTiles || grid.levels > kMaxLevels ||
        std::uint64_t{grid.width} * grid.height * grid.levels > kMaxTileLevels) {
        diag::warnOnce("lot: a floor grid larger than lighting accepts is left unlit");
        return;
    }
    // Every level, from the lowest (a pool's storey under the ground one), as the lot is lit.
    maps.lowest = -content::groundGridLevel(lot_);
    for (std::uint32_t level = 0; level < floors->second.levels; ++level) {
        Image map;
        map.width = 8 * floors->second.width;
        map.height = 8 * floors->second.height;
        map.rgba.assign(static_cast<std::size_t>(map.width) * map.height * 4, 128);
        maps.floors.push_back(static_cast<int>(b_.scene().images.size()));
        b_.scene().images.push_back(std::move(map));
        maps.floorIncidence.push_back(addIncidence(b_.scene().images.back()));
    }
}

void LotSceneBuilder::log(const content::LotObject& o, const std::string& what, const std::string& model) {
    if (!log_) {
        return;
    }
    const auto& h = o.state.header;
    std::ostringstream line;
    line << o.id << " " << (o.type ? o.type->name : std::string("?")) << " (" << model << ") at " << h.location[1] << " "
         << h.location[0] << " level " << h.value1 << " dir " << o.state.data[kFacingData] << ": " << what;
    stats_.objectLog.push_back(line.str());
}

// Doors and windows cut their wall masks out of the wall faces they sit in.
void LotSceneBuilder::cutWalls() {
    if (walls_.empty()) {
        return;
    }
    const auto masks = wallMasks(assets_.materialNames());
    for (const auto& o : shown_) {
        const auto list = masks.find(lower(o.model));
        if (list == masks.end()) {
            continue;
        }
        for (const auto& m : list->second) {
            // The face's direction: n is the object's front (+y), e its +x. Ne and the like are diagonal.
            const float fx = m.face.ends_with('e') ? 1.0f : m.face.ends_with('w') ? -1.0f : 0.0f;
            const float fy = m.face.starts_with('n') ? 1.0f : m.face.starts_with('s') ? -1.0f : 0.0f;
            if (fx == 0 && fy == 0) {
                continue;
            }
            // A straight face's wall runs along the tile edge behind it, a diagonal one's through the
            // tile's middle.
            const bool diagonal = fx != 0 && fy != 0;
            float mid[3] = {static_cast<float>(m.east) - (diagonal ? 0 : fx / 2), static_cast<float>(-m.south) - (diagonal ? 0 : fy / 2), 0};
            float facing[3] = {fx, fy, 0};
            o.world.apply(mid, true);
            o.world.apply(facing, false);
            const int level = content::gridLevel(lot_, o.object->state.header.value1) + m.level;
            const auto wall = wallAt_.find(edgeKey(mid[0], mid[1], mid[0], mid[1], level));
            if (wall == wallAt_.end()) {
                continue;
            }
            ThickWall& w = walls_[wall->second];
            const float dx = w.q.x1 - w.q.x0, dy = w.q.y1 - w.q.y0;
            // The wall graph's left side faces (dy, -dx). A straight face's mask runs along the
            // object's own -x: left to right as seen from n and e faces, right to left from s and w.
            // A diagonal one runs right to left as seen from ne and nw faces, left to right from se and sw.
            const bool left = facing[0] * dy - facing[1] * dx > 0;
            (left ? w.leftMask : w.rightMask) = m.material;
            (left ? w.leftMaskMirrored : w.rightMaskMirrored) = diagonal ? m.face.starts_with('n') : m.face == "s" || m.face == "w";
            b_.scene().instances[o.instance].walls.push_back(wallBase_ + static_cast<int>(wall->second));
            ++stats_.wallCuts;
        }
    }
    for (auto& w : walls_) {
        for (auto [side, mask, mirrored] : {std::tuple{&w.left, &w.leftMask, w.leftMaskMirrored},
                                            std::tuple{&w.right, &w.rightMask, w.rightMaskMirrored}}) {
            if (!side->empty() && !mask->empty()) {
                if (const int cut = b_.maskedMaterial(*side, *mask, mirrored); cut >= 0) {
                    *side = b_.scene().materials[static_cast<std::size_t>(cut)].name;
                }
            }
        }
    }
}

// Each wall and each hub on its own, in full and cut down, so that views can choose.
void LotSceneBuilder::drawWalls() {
    if (walls_.empty()) {
        return;
    }
    const auto hubs = joinWalls(walls_);
    // Each face its own light-map rect, 8 texels along a tile by 24 up a storey (as the game's
    // ordinary walls), 64 to a row of the wall light map. Grey until the light is worked out.
    auto& maps = lightMaps();
    constexpr int kRectWidth = 8, kRectHeight = 24, kAtlasWidth = 512, kPerRow = kAtlasWidth / kRectWidth;
    const int ground = content::groundGridLevel(lot_);
    auto faceKey = [ground](const ThickWall& w, bool left) {
        const int x0 = static_cast<int>(std::lround(w.q.x0)), y0 = static_cast<int>(std::lround(w.q.y0));
        const int x1 = static_cast<int>(std::lround(w.q.x1)), y1 = static_cast<int>(std::lround(w.q.y1));
        const int level = w.level - ground;
        return left ? std::array<int, 5>{level, x0, y0, x1, y1} : std::array<int, 5>{level, x1, y1, x0, y0};
    };
    for (const auto& w : walls_) {
        for (const bool left : {true, false}) {
            const auto key = faceKey(w, left);
            if (!maps.wallRects.contains(key)) {
                const int i = static_cast<int>(maps.wallRects.size());
                const int x0 = (i % kPerRow) * kRectWidth, y0 = (i / kPerRow) * kRectHeight;
                maps.wallRects[key] = {x0, y0, x0 + kRectWidth, y0 + kRectHeight};
            }
        }
    }
    Image atlas;
    atlas.width = kAtlasWidth;
    atlas.height = static_cast<std::uint32_t>(std::max<std::size_t>(1, (maps.wallRects.size() + kPerRow - 1) / kPerRow) * kRectHeight);
    atlas.rgba.assign(static_cast<std::size_t>(atlas.width) * atlas.height * 4, 128);
    const float atlasW = static_cast<float>(atlas.width), atlasH = static_cast<float>(atlas.height);
    maps.walls = static_cast<int>(b_.scene().images.size());
    b_.scene().images.push_back(std::move(atlas));
    maps.wallIncidence = addIncidence(b_.scene().images.back());
    // As texture coordinates, one texel in from each side (the game's inset for these rects).
    auto lightRect = [&](const ThickWall& w, bool left) -> std::optional<LightRect> {
        const auto it = maps.wallRects.find(faceKey(w, left));
        if (it == maps.wallRects.end()) return std::nullopt;
        const auto& r = it->second;
        return LightRect{(r.x0 + 1) / atlasW, (r.y0 + 1) / atlasH, (r.x1 - 1) / atlasW, (r.y1 - 1) / atlasH};
    };
    for (std::size_t i = 0; i < walls_.size(); ++i) {
        const auto& w = walls_[i];
        const std::array<std::optional<LightRect>, 2> rects{lightRect(w, true), lightRect(w, false)};
        for (const bool cut : {false, true}) {
            if (cut && !w.cuttable) {
                continue;
            }
            Surfaces surfaces;
            addThickWall(b_, surfaces, w, wallTop_, cut ? kCutWallHeight : 0, &rects);
            if (auto* instance = b_.place(b_.addMesh("wall", std::move(surfaces)), "wall", Transform{}, Part::Wall, w.level)) {
                instance->cut = cut;
                instance->walls = {wallBase_ + static_cast<int>(i)};
                if (w.clear) {
                    instance->litOutdoors = true;
                    continue;
                }
                instance->lightMap = maps.walls;
                instance->incidenceMap = maps.wallIncidence;
                instance->lightMapOnly = true;  // the game's wall shader leaves the covering's diffuse out
            }
        }
    }
    // Fan from each hub's point to each pair of neighboring corners (counterclockwise, so facing up).
    for (const auto& hub : hubs) {
        for (const bool cut : {false, true}) {
            const float top = cut ? std::min(hub.top, hub.bottom + kCutWallHeight) : hub.top;
            Surfaces surfaces;
            for (std::size_t i = 0; i < hub.corners.size(); ++i) {
                const auto& c0 = hub.corners[i];
                const auto& c1 = hub.corners[(i + 1) % hub.corners.size()];
                for (const auto& p : {std::array<float, 3>{hub.x, hub.y, top}, std::array<float, 3>{c0[0], c0[1], top},
                                      std::array<float, 3>{c1[0], c1[1], top}}) {
                    addVertex(surfaces[wallTop_], p, {0, 0, 1}, {p[0], p[1]});
                }
            }
            if (auto* instance = b_.place(b_.addMesh("wall hub", std::move(surfaces)), "wall hub", Transform{}, Part::WallHub,
                                          walls_[hub.walls.front()].level)) {
                instance->cut = cut;
                for (const std::size_t i : hub.walls) {
                    instance->walls.push_back(wallBase_ + static_cast<int>(i));
                }
            }
        }
    }
}

} // namespace lotscene

ObjectLighting outdoorLighting(const content::Lighting::State& state) {
    ObjectLighting out;
    out.ambient = state.skylight;
    for (const char* name : {"sun", "fill1", "sky", "simFill1", "simFill2"}) {
        const auto it = state.lights.find(name);
        if (it == state.lights.end()) {
            continue;
        }
        const auto& l = it->second;
        const float len = std::sqrt(l.direction[0] * l.direction[0] + l.direction[1] * l.direction[1] + l.direction[2] * l.direction[2]);
        if (len <= 0) {
            continue;
        }
        ObjectLighting::Light light;
        light.sun = std::string_view(name) == "sun";
        light.direction = {l.direction[0] / len, l.direction[1] / len, l.direction[2] / len};
        for (int i = 0; i < 3; ++i) {
            light.color[static_cast<std::size_t>(i)] = l.strength * (l.color[static_cast<std::size_t>(i)] - state.skylight[static_cast<std::size_t>(i)]);
        }
        (std::string_view(name).starts_with("sim") ? out.simLights : out.lights).push_back(light);
    }
    return out;
}

std::optional<ObjectLighting> objectDaylight(const content::Lighting& lighting) {
    const auto day = lighting.states.find("day");
    if (day == lighting.states.end()) {
        return std::nullopt;
    }
    return outdoorLighting(day->second);
}

LotSceneStats addLot(SceneBuilder& builder, const std::filesystem::path& lot, bool logObjects) {
    return addLot(builder, content::loadLot(lot), lot, logObjects);
}

LotSceneStats addLot(SceneBuilder& builder, const content::Lot& loaded, const std::filesystem::path& lot, bool logObjects) {
    builder.scene().objectLighting = objectDaylight(builder.assets().lighting());
    return lotscene::LotSceneBuilder(builder, loaded, lot, logObjects).build();
}

} // namespace ts2::scene
