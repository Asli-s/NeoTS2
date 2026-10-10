#include "scene/lot_scene.hpp"

#include "content/lot.hpp"
#include "formats/dbpf.hpp"
#include "formats/resource_types.hpp"
#include "scene/lot_parts.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <sstream>
#include <tuple>

namespace sg = ts2::scenegraph;

namespace ts2::scene {

Heights::Heights(const content::Lot& lot) {
    if (const auto g = lot.grids.find(content::kFloorHeightGridInstance); g != lot.grids.end() && g->second.elementSize == 4) {
        grid_ = &g->second;
    }
}

float Heights::at(int level, int x, int y, bool& found) const {
    found = grid_ != nullptr && grid_->contains(level, x, y);
    if (!found) {
        return static_cast<float>(level) * kLevelHeight;
    }
    float v = 0;
    std::memcpy(&v, grid_->at(level, x, y).data(), 4);
    return v;
}

float Heights::corner(int level, float x, float y) const {
    bool found = false;
    return at(level, static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)), found);
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

namespace {

// A placed object, before objects held in slots are moved onto their container's slot.
struct Shown {
    const content::LotObject* object;
    int mesh;
    std::string model;
    Transform world;
    float yaw;
    std::size_t instance = 0;  // index into the scene's instances once placed
};

// Middles of wall segments (doubled, so they are whole numbers) by level.
using EdgeKey = std::tuple<int, int, int>;
EdgeKey edgeKey(float x0, float y0, float x1, float y1, int level) {
    return {static_cast<int>(std::lround(x0 + x1)), static_cast<int>(std::lround(y0 + y1)), level};
}

class LotSceneBuilder {
public:
    LotSceneBuilder(SceneBuilder& builder, const content::Lot& lot, const std::filesystem::path& path, bool log)
        : b_(builder), assets_(builder.assets()), lot_(lot), path_(path), heights_(lot), log_(log) {}

    LotSceneStats build() {
        edge_ = b_.colorMaterial("wall edge", {0.36f, 0.25f, 0.18f});
        plain_ = "wall plain";
        b_.colorMaterial(plain_, {0.86f, 0.83f, 0.76f});
        collectWalls();
        placeFencePosts();
        placeObjects();
        cutWalls();
        drawWalls();
        drawTerrain();
        drawFloors();
        drawRoofs();
        return stats_;
    }

private:
    void collectWalls() {
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
        for (const auto& w : rooms.walls) {
            const auto* a = rooms.point(w.from);
            const auto* b = rooms.point(w.to);
            if (a == nullptr || b == nullptr) {
                continue;
            }
            const int level = a->level - rooms.firstLevel;
            const auto covering = coverings.find(w.id);
            // A fence rail is a wall segment whose type is the fence's GUID: one tile of rail from a to b.
            if (covering != coverings.end()) {
                if (const auto* fence = assets_.fence(covering->second->type)) {
                    const float dx = b->x - a->x, dy = b->y - a->y;
                    const std::string& model = dx != 0 && dy != 0 ? fence->diagonalRail : fence->rail;
                    if (const int mesh = model.empty() ? -1 : b_.model(model, {}); mesh >= 0) {
                        b_.place(mesh, "rail " + model,
                                 Transform::at(a->x, a->y, heights_.corner(level, a->x, a->y), std::atan2(dy, dx)),
                                 Part::Object, level);
                        ++stats_.fenceRails;
                    }
                    continue;
                }
            }
            if (w.leftRoom == w.rightRoom || (onBorder(a) && onBorder(b))) {
                continue;
            }
            ThickWall t;
            t.q = {a->x, a->y, b->x, b->y, heights_.corner(level, a->x, a->y), heights_.corner(level + 1, a->x, a->y),
                   heights_.corner(level, b->x, b->y), heights_.corner(level + 1, b->x, b->y)};
            t.points = {w.from, w.to};
            t.level = level;
            // Room 0 is the outdoors, on every level.
            t.roomLeft = w.leftRoom != 0;
            t.roomRight = w.rightRoom != 0;
            if (covering == coverings.end()) {
                t.left = t.right = plain_;
            } else {
                if (const auto style = styles.find(covering->second->type); style != styles.end()) {
                    if (!style->second.rendered) {
                        continue;
                    }
                    t.thick = style->second.thick;
                    t.cuttable = style->second.cutAway;
                    if (style->second.belowFloor) {
                        t.q.top0 -= kFloorThickness;
                        t.q.top1 -= kFloorThickness;
                    }
                }
                // Each side shows its own covering; a side whose covering has no material (the inside
                // of a foundation) is not drawn.
                auto side = [&](std::uint16_t id) {
                    const auto pattern = lot_.wallPatterns.find(id);
                    if (pattern == lot_.wallPatterns.end()) {
                        return std::string();
                    }
                    const std::string material = assets_.patternMaterial(pattern->second.name, true);
                    return b_.material(material) >= 0 ? material : std::string();
                };
                t.left = side(covering->second->left);
                t.right = side(covering->second->right);
                ++stats_.coveredWalls;
            }
            walls_.push_back(std::move(t));
        }
        stats_.walls = walls_.size();
        wallBase_ = static_cast<int>(b_.scene().walls.size());
        for (std::size_t i = 0; i < walls_.size(); ++i) {
            const auto& t = walls_[i];
            b_.scene().walls.push_back({t.q.x0, t.q.y0, t.q.x1, t.q.y1, t.level, t.roomLeft, t.roomRight, t.cuttable});
            wallAt_[edgeKey(t.q.x0, t.q.y0, t.q.x1, t.q.y1, t.level)] = i;
        }
    }

    void placeFencePosts() {
        for (const auto& post : lot_.fencePosts) {
            const auto* fence = assets_.fence(post.fence);
            if (fence == nullptr || fence->post.empty()) {
                continue;
            }
            if (const int mesh = b_.model(fence->post, {}); mesh >= 0) {
                b_.place(mesh, "post " + fence->post, Transform::at(post.x, post.y, heights_.corner(post.level, post.x, post.y), 0),
                         Part::Object, post.level);
                ++stats_.fencePosts;
            }
        }
    }

    void log(const content::LotObject& o, const std::string& what, const std::string& model) {
        if (!log_) {
            return;
        }
        const auto& h = o.state.header;
        std::ostringstream line;
        line << o.id << " (" << model << ") at " << h.location[1] << " " << h.location[0] << " level " << h.value1 << " dir "
             << o.state.data[1] << ": " << what;
        stats_.objectLog.push_back(line.str());
    }

    void placeObjects() {
        dbpf::Package package{path_};
        std::map<std::uint32_t, const dbpf::IndexEntry*> sceneRecords;
        for (const auto& e : package.entries()) {
            if (e.key.type == types::kSceneObject) {
                sceneRecords[e.key.instance] = &e;
            }
        }
        std::map<std::uint32_t, std::size_t> byId;
        for (const auto& object : lot_.objects) {
            // The lot's grids and wall graphs run along (location[1], location[0]); that is the world here.
            const auto& h = object.state.header;
            const float x = h.location[1], y = h.location[0];
            if (x < 0 || y < 0 || x > 256 || y > 256) {
                log(object, "off the lot", "");
                continue;
            }
            const auto record = sceneRecords.find(object.id);
            if (record == sceneRecords.end()) {
                ++stats_.withoutRecord;
                log(object, "no scene record", "");
                continue;
            }
            sg::SceneObject so;
            try {
                so = sg::parseSceneObject(package.read(*record->second));
            } catch (const std::exception& ex) {
                ++stats_.withoutRecord;
                log(object, ex.what(), "");
                continue;
            }
            const std::string low = lower(so.model);
            if (so.model.empty() || low == "nullmodel" || low == "nomodel") {
                log(object, "no model", so.model);
                continue;
            }
            const int mesh = b_.model(so.model, so.overrides);
            if (mesh < 0) {
                ++stats_.withoutModel;
                log(object, "model not found", so.model);
                continue;
            }
            // Stair pieces carry their own height (header value 3); other objects stand on the floor.
            const float z = h.value3 != 0 ? h.value3 : heights_.tile(h.value1 - 1, x, y);
            // Direction d faces 180 - 45d degrees (0 along -X, 6 along -Y); models face +Y.
            const float angle = kPi / 2 - static_cast<float>(object.state.data[1]) * kPi / 4;
            shown_.push_back({&object, mesh, so.model, Transform::at(x, y, z, angle), angle});
            byId[object.id] = shown_.size() - 1;
            log(object, "placed", so.model);
        }
        // Objects held in a container slot (object data 2: container id, 3: slot number) sit at that
        // slot's bone in the container's model.
        std::vector<int> resolved(shown_.size(), 0);  // 0 not yet, 1 in progress, 2 done
        auto resolve = [&](auto&& self, std::size_t i) -> const Transform& {
            Shown& s = shown_[i];
            if (resolved[i] != 0) {
                return s.world;
            }
            resolved[i] = 1;
            const auto container = byId.find(static_cast<std::uint32_t>(s.object->state.data[2]));
            if (s.object->state.data[2] > 0 && container != byId.end() && container->second != i) {
                const Shown& c = shown_[container->second];
                const Transform& base = self(self, container->second);
                std::string name;
                if (c.object->type) {
                    name = assets_.slotBone(c.object->type->guid, static_cast<std::size_t>(s.object->state.data[3]));
                }
                const Transform* bone = name.empty() ? nullptr : b_.bone(c.model, name);
                const Transform slot = bone ? base * *bone : base;
                // The saved direction is the slot's rounded to 45 degrees unless the player turned the
                // object on it: keep whole quarter turns of difference.
                const float slotYaw = std::atan2(slot.m[4], slot.m[0]);
                const float turn = std::round(std::remainder(s.yaw - slotYaw, 2 * kPi) / (kPi / 2)) * (kPi / 2);
                s.world = slot * Transform::at(0, 0, 0, turn);
                ++stats_.inSlots;
            }
            resolved[i] = 2;
            return s.world;
        };
        for (std::size_t i = 0; i < shown_.size(); ++i) {
            resolve(resolve, i);
            shown_[i].instance = b_.scene().instances.size();
            b_.place(shown_[i].mesh, std::to_string(shown_[i].object->id) + " " + shown_[i].model, shown_[i].world, Part::Object,
                     shown_[i].object->state.header.value1 - 1);
            ++stats_.objects;
        }
    }

    // Doors and windows cut their wall masks out of the wall faces they sit in.
    void cutWalls() {
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
                // The face's direction: n is the object's front (+y), e its +x; ne and the like are diagonal.
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
                const int level = o.object->state.header.value1 - 1 + m.level;
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
    void drawWalls() {
        if (walls_.empty()) {
            return;
        }
        const auto hubs = joinWalls(walls_);
        for (std::size_t i = 0; i < walls_.size(); ++i) {
            const auto& w = walls_[i];
            for (const bool cut : {false, true}) {
                if (cut && !w.cuttable) {
                    continue;
                }
                Surfaces surfaces;
                addThickWall(b_, surfaces, w, edge_, cut ? kCutWallHeight : 0);
                if (auto* instance = b_.place(b_.addMesh("wall", std::move(surfaces)), "wall", Transform{}, Part::Wall, w.level)) {
                    instance->cut = cut;
                    instance->walls = {wallBase_ + static_cast<int>(i)};
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
                        addVertex(surfaces[edge_], p, {0, 0, 1}, {0, 0});
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

    // The terrain: a grid of four points to a tile (as the paint strengths are kept), between the
    // height grid's corners. Like the game, the canvas texture repeats every 20 tiles and its detail
    // every 4, and each paint is laid over the canvas the same way, as strong as its strength there.
    void drawTerrain() {
        const auto* g = heights_.grid();
        if (g == nullptr || g->width < 2 || g->height < 2) {
            return;
        }
        constexpr std::uint32_t kSteps = 4;
        constexpr float kRepeat = 20, kShade = 0.9f;
        constexpr int kDetailRepeats = 5;  // detail every 4 tiles
        const std::uint32_t columns = (g->width - 1) * kSteps + 1, rows = (g->height - 1) * kSteps + 1;
        auto corner = [&](std::uint32_t x, std::uint32_t y) {
            float v = 0;
            std::memcpy(&v, g->at(0, static_cast<std::int32_t>(std::min(x, g->width - 1)), static_cast<std::int32_t>(std::min(y, g->height - 1))).data(), 4);
            return v;
        };
        auto height = [&](std::int64_t i, std::int64_t j) {
            const auto ci = static_cast<std::uint32_t>(std::clamp<std::int64_t>(i, 0, columns - 1));
            const auto cj = static_cast<std::uint32_t>(std::clamp<std::int64_t>(j, 0, rows - 1));
            const std::uint32_t x = ci / kSteps, y = cj / kSteps;
            const float fx = static_cast<float>(ci % kSteps) / kSteps, fy = static_cast<float>(cj % kSteps) / kSteps;
            const float top = corner(x, y) * (1 - fx) + corner(x + 1, y) * fx;
            const float bottom = corner(x, y + 1) * (1 - fx) + corner(x + 1, y + 1) * fx;
            return top * (1 - fy) + bottom * fy;
        };

        Primitive canvas;
        constexpr float step = 1.0f / kSteps;
        for (std::uint32_t i = 0; i < columns; ++i) {
            for (std::uint32_t j = 0; j < rows; ++j) {
                const float x = static_cast<float>(i) * step, y = static_cast<float>(j) * step;
                const float sx = (height(i + 1, j) - height(static_cast<std::int64_t>(i) - 1, j)) / (2 * step);
                const float sy = (height(i, j + 1) - height(i, static_cast<std::int64_t>(j) - 1)) / (2 * step);
                const float len = std::sqrt(sx * sx + sy * sy + 1);
                canvas.positions.insert(canvas.positions.end(), {x, y, height(i, j)});
                canvas.normals.insert(canvas.normals.end(), {-sx / len, -sy / len, 1 / len});
                canvas.uvs.insert(canvas.uvs.end(), {x / kRepeat, y / kRepeat});
            }
        }
        auto cell = [&](std::vector<std::uint32_t>& indices, std::uint32_t i, std::uint32_t j) {
            const std::uint32_t a = i * rows + j, b = (i + 1) * rows + j;
            indices.insert(indices.end(), {a, b, b + 1, a, b + 1, a + 1});
        };
        for (std::uint32_t i = 0; i + 1 < columns; ++i) {
            for (std::uint32_t j = 0; j + 1 < rows; ++j) {
                cell(canvas.indices, i, j);
            }
        }

        Mesh mesh{"terrain", {}};
        const auto* textures = lot_.terrainTextures ? &*lot_.terrainTextures : nullptr;
        canvas.material = textures ? b_.terrainMaterial(textures->canvas, kDetailRepeats, kShade, false) : -1;
        if (canvas.material < 0) {
            canvas.material = b_.colorMaterial("terrain", {0.32f, 0.45f, 0.25f}, true);
        }
        mesh.primitives.push_back(canvas);
        for (std::size_t n = 0; textures != nullptr && n < textures->paints.size(); ++n) {
            const auto map = lot_.flatGrids.find(content::kFirstPaintMapInstance + static_cast<std::uint32_t>(n));
            if (map == lot_.flatGrids.end() || map->second.width != columns || map->second.height != rows) {
                continue;
            }
            const auto& strength = map->second;
            Primitive paint;
            for (std::uint32_t i = 0; i + 1 < columns; ++i) {
                for (std::uint32_t j = 0; j + 1 < rows; ++j) {
                    const auto x = static_cast<std::int32_t>(i), y = static_cast<std::int32_t>(j);
                    // The game draws a paint where its strength is above 10 of 255.
                    if (std::max({strength.byte(x, y), strength.byte(x + 1, y), strength.byte(x, y + 1), strength.byte(x + 1, y + 1)}) > 10) {
                        cell(paint.indices, i, j);
                    }
                }
            }
            if (paint.indices.empty()) {
                continue;
            }
            paint.material = b_.terrainMaterial(textures->paints[n], kDetailRepeats, kShade, true);
            if (paint.material < 0) {
                continue;
            }
            paint.positions = canvas.positions;
            paint.normals = canvas.normals;
            paint.uvs = canvas.uvs;
            for (std::uint32_t i = 0; i < columns; ++i) {
                for (std::uint32_t j = 0; j < rows; ++j) {
                    paint.alphas.push_back(static_cast<float>(strength.byte(static_cast<std::int32_t>(i), static_cast<std::int32_t>(j))) / 255);
                }
            }
            mesh.primitives.push_back(std::move(paint));
            ++stats_.terrainPaints;
        }
        b_.scene().meshes.push_back(std::move(mesh));
        // Slightly under the floors standing on it.
        b_.place(static_cast<int>(b_.scene().meshes.size() - 1), "terrain", Transform::at(0, 0, -0.01f, 0));
    }

    // Floors: a pattern per quarter tile, each quarter the triangle from one tile edge to the tile's
    // center (stored in the order left, bottom, right, top, as the room grid is); then the slab under
    // floors above the ground.
    void drawFloors() {
        const auto floors = lot_.grids.find(content::kFloorGridInstance);
        if (floors == lot_.grids.end() || floors->second.elementSize != 8) {
            return;
        }
        const auto& g = floors->second;
        std::set<int> patterns;
        for (std::int32_t level = 0; level < static_cast<std::int32_t>(g.levels); ++level) {
            Surfaces surfaces;
            for (std::int32_t x = 0; x < static_cast<std::int32_t>(g.width); ++x) {
                for (std::int32_t y = 0; y < static_cast<std::int32_t>(g.height); ++y) {
                    const float fx = static_cast<float>(x), fy = static_cast<float>(y);
                    const std::array<std::array<float, 2>, 4> corners{{{fx, fy}, {fx + 1, fy}, {fx + 1, fy + 1}, {fx, fy + 1}}};
                    std::array<float, 4> h{};
                    float center = 0;
                    for (std::size_t c = 0; c < 4; ++c) {
                        h[c] = heights_.corner(level, corners[c][0], corners[c][1]) + 0.004f;
                        center += h[c] / 4;
                    }
                    for (std::size_t q = 0; q < 4; ++q) {
                        const auto id = static_cast<std::uint16_t>(g.i16(level, x, y, 3 - q));
                        const auto pattern = lot_.floorPatterns.find(id);
                        if (id == 0 || pattern == lot_.floorPatterns.end()) {
                            continue;
                        }
                        const int material = b_.material(assets_.patternMaterial(pattern->second.name, false));
                        patterns.insert(material);
                        auto& s = surfaces[material];
                        const std::size_t n = (q + 1) % 4;
                        // Textures run along the game's own tile axes (world y, x here).
                        addVertex(s, {corners[q][0], corners[q][1], h[q]}, {0, 0, 1}, {corners[q][1], corners[q][0]});
                        addVertex(s, {corners[n][0], corners[n][1], h[n]}, {0, 0, 1}, {corners[n][1], corners[n][0]});
                        addVertex(s, {fx + 0.5f, fy + 0.5f, center}, {0, 0, 1}, {fy + 0.5f, fx + 0.5f});
                    }
                }
            }
            b_.place(b_.addMesh("floors", std::move(surfaces)), "floors", Transform{}, Part::Floor, level);
        }
        stats_.floorPatterns = patterns.size();

        // The slab under each floored tile above the ground: a ceiling underneath and a band down each
        // edge where the floor stops, unless a wall runs along it (the slab ends inside the wall).
        const int ceiling = b_.colorMaterial("ceiling", {0.92f, 0.91f, 0.88f});
        std::set<EdgeKey> wallEdges;
        for (const auto& w : walls_) {
            if (w.thick) {
                wallEdges.insert(edgeKey(w.q.x0, w.q.y0, w.q.x1, w.q.y1, w.level));
            }
        }
        auto floored = [&](std::int32_t level, std::int32_t x, std::int32_t y) {
            if (!g.contains(level, x, y)) {
                return false;
            }
            for (std::size_t q = 0; q < 4; ++q) {
                if (g.i16(level, x, y, q) != 0) {
                    return true;
                }
            }
            return false;
        };
        const std::array<std::array<float, 2>, 4> noUv{};
        for (std::int32_t level = 1; level < static_cast<std::int32_t>(g.levels); ++level) {
            Surfaces slabs;
            for (std::int32_t x = 0; x < static_cast<std::int32_t>(g.width); ++x) {
                for (std::int32_t y = 0; y < static_cast<std::int32_t>(g.height); ++y) {
                    if (!floored(level, x, y)) {
                        continue;
                    }
                    const float fx = static_cast<float>(x), fy = static_cast<float>(y);
                    const std::array<std::array<float, 2>, 4> c{{{fx, fy}, {fx + 1, fy}, {fx + 1, fy + 1}, {fx, fy + 1}}};
                    std::array<float, 4> top{};
                    for (std::size_t k = 0; k < 4; ++k) {
                        top[k] = heights_.corner(level, c[k][0], c[k][1]);
                    }
                    addQuad(slabs[ceiling],
                            {{{c[0][0], c[0][1], top[0] - kFloorThickness}, {c[1][0], c[1][1], top[1] - kFloorThickness},
                              {c[2][0], c[2][1], top[2] - kFloorThickness}, {c[3][0], c[3][1], top[3] - kFloorThickness}}},
                            {0, 0, -1}, noUv);
                    const int neighbor[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};  // across edges c0-c1, c1-c2, ...
                    for (std::size_t e = 0; e < 4; ++e) {
                        if (floored(level, x + neighbor[e][0], y + neighbor[e][1])) {
                            continue;
                        }
                        const auto& a = c[e];
                        const auto& b = c[(e + 1) % 4];
                        if (wallEdges.contains(edgeKey(a[0], a[1], b[0], b[1], level)) ||
                            wallEdges.contains(edgeKey(a[0], a[1], b[0], b[1], level - 1))) {
                            continue;
                        }
                        const float ta = top[e], tb = top[(e + 1) % 4];
                        addQuad(slabs[edge_],
                                {{{a[0], a[1], ta - kFloorThickness}, {b[0], b[1], tb - kFloorThickness}, {b[0], b[1], tb}, {a[0], a[1], ta}}},
                                {static_cast<float>(neighbor[e][0]), static_cast<float>(neighbor[e][1]), 0}, noUv);
                    }
                }
            }
            b_.place(b_.addMesh("floor slabs", std::move(slabs)), "floor slabs", Transform{}, Part::Floor, level);
        }
    }

    void drawRoofs() {
        for (const auto& roof : lot_.roofs) {
            Surfaces surfaces;
            RoofLook look;
            if (const auto* pattern = assets_.roofPattern(roof.pattern)) {
                // The top texture is a 5 x 5 grid of roof pieces (plain courses, ridge and hip caps; each
                // cell one repeat of the courses), cell (4, 3) plain, the cap piece along the top. The
                // colors are those of the game's roof materials.
                const std::array<float, 3> slopeColor{0.4f, 0.385f, 0.38f};
                look.top = b_.textureCellMaterial(pattern->top, 5, 5, 4, 3, slopeColor);
                look.under = b_.textureMaterial(pattern->under, 0.8f);
                look.trim = b_.textureMaterial(pattern->trim, 0.8f);
                look.cap = b_.textureRegionMaterial(pattern->top, 0.4, 0.0, 0.6, 37.0 / 512, 128, 48, slopeColor);
            } else {
                look.top = look.under = look.trim = b_.colorMaterial("roof plain", {0.35f, 0.27f, 0.22f});
            }
            look.gable = b_.material(plain_);
            // A roof may keep its own slope (half the tangent of its angle); otherwise 45 degrees.
            const float slope = roof.slope > 0 ? 2 * roof.slope : 1.0f;
            addRoof(surfaces, roof, heights_.corner(roof.level0, roof.x0, roof.y0), slope, look);
            // A roof belongs to the storey under its eaves.
            b_.place(b_.addMesh("roof", std::move(surfaces)), "roof", Transform{}, Part::Roof, roof.level0 - 1);
        }
        stats_.roofs = lot_.roofs.size();
    }

    SceneBuilder& b_;
    Assets& assets_;
    const content::Lot& lot_;
    std::filesystem::path path_;
    Heights heights_;
    bool log_;
    LotSceneStats stats_;
    int edge_ = -1;
    std::string plain_;
    std::vector<ThickWall> walls_;
    int wallBase_ = 0;  // index of walls_[0] in the scene's walls
    std::map<EdgeKey, std::size_t> wallAt_;  // walls_ by their middle
    std::vector<Shown> shown_;
};

} // namespace

LotSceneStats addLot(SceneBuilder& builder, const std::filesystem::path& lot, bool logObjects) {
    const content::Lot loaded = content::loadLot(lot);
    return LotSceneBuilder(builder, loaded, lot, logObjects).build();
}

} // namespace ts2::scene
