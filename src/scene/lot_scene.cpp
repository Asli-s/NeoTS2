#include "scene/lot_scene.hpp"

#include "content/lot.hpp"
#include "content/neighborhood.hpp"
#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "formats/objd.hpp"
#include "formats/resource_types.hpp"
#include "scene/lot_parts.hpp"
#include "scene/pool_water.hpp"
#include "scene/rig.hpp"
#include "scene/sim_builder.hpp"
#include "scene/sims.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
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

// The container slots kept for each object, for placing what scripts put into them.
constexpr std::size_t kMaxShownSlots = 16;
// The pool's edge (wall style 0x1D) lets light through by walls.txt but the game treats it as stopping it.
constexpr std::uint32_t kPoolEdgeStyle = 0x1D;

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
        // Wall tops: the game's wall_top material, its texture tiled and unlit (no light map or lights).
        wallTop_ = b_.textureMaterial("wall-top");
        if (wallTop_ >= 0) {
            b_.scene().materials[static_cast<std::size_t>(wallTop_)].unlit = true;
        } else {
            wallTop_ = edge_;
        }
        plain_ = "wall plain";
        b_.colorMaterial(plain_, {0.86f, 0.83f, 0.76f});
        collectWalls();
        placeFencePosts();
        // Where something the simulation moves stands: the floor of its storey (1 = ground), kept past the lot.
        const auto kept = std::make_shared<const Heights>(heights_.grid() ? std::make_shared<const array3d::Array3D>(*heights_.grid()) : nullptr);
        const int groundLevel = content::gridLevel(lot_, 1);
        b_.scene().floorHeight = [kept, groundLevel](int storey, float x, float y) { return kept->tile(storey - 1 + groundLevel, x, y); };
        b_.scene().groundLevel = groundLevel;
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
        placeRoofs();
        // The sky behind a lot by day, as the game's lot sky effect draws it (its plain-weather
        // skybox, seasons and weather pick others). Lighting the lot puts in its state's sky.
        b_.scene().skybox = assets_.textureFaces("skymap-generic-day-highres-envcube");
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
        const wallgraph::PointIndex pointById(rooms);
        for (const auto& w : rooms.walls) {
            const auto* a = pointById(w.from);
            const auto* b = pointById(w.to);
            // Points off the lot (or not numbers, in a broken file) make no wall.
            auto onLot = [](const wallgraph::Point* p) { return p->x >= -1 && p->x <= 257 && p->y >= -1 && p->y <= 257; };
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
            if (covering != coverings.end()) {
                if (const auto* fence = assets_.fence(covering->second->type)) {
                    const float dx = b->x - a->x, dy = b->y - a->y;
                    const std::string& model = dx != 0 && dy != 0 ? fence->diagonalRail : fence->rail;
                    if (const int mesh = model.empty() ? -1 : b_.model(model, {}); mesh >= 0) {
                        if (auto* rail = b_.place(mesh, "rail " + model,
                                                              Transform::at(a->x, a->y, heights_.corner(level, a->x, a->y), std::atan2(dy, dx)),
                                                              Part::Object, level)) {
                            rail->sunTest = SunTest::AboveBox;
                        }
                        ++stats_.fenceRails;
                    }
                    continue;
                }
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
            if (covering == coverings.end()) {
                t.left = t.right = plain_;
                t.leftCovering = t.rightCovering = plain_;
            } else {
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
                    return b_.material(material) >= 0 ? material : std::string();
                };
                t.left = t.leftCovering = side(covering->second->left);
                t.right = t.rightCovering = side(covering->second->right);
                ++stats_.coveredWalls;
            }
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

    void placeFencePosts() {
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

    LightMaps& lightMaps() {
        if (!b_.scene().lightMaps) {
            b_.scene().lightMaps.emplace();
        }
        return *b_.scene().lightMaps;
    }

    // An incidence map for a light map (see incidenceMap): straight out until the light is filled
    // in. Added after the light map's own image.
    int addIncidence(const Image& lightMap) {
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
    void makeFloorLightMaps() {
        auto& maps = lightMaps();
        const auto floors = lot_.grids.find(content::kFloorGridInstance);
        if (!maps.floors.empty() || floors == lot_.grids.end()) {
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

    void log(const content::LotObject& o, const std::string& what, const std::string& model) {
        if (!log_) {
            return;
        }
        const auto& h = o.state.header;
        std::ostringstream line;
        line << o.id << " " << (o.type ? o.type->name : std::string("?")) << " (" << model << ") at " << h.location[1] << " "
             << h.location[0] << " level " << h.value1 << " dir " << o.state.data[1] << ": " << what;
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
            // The lot's grids and wall graphs run along (location[1], location[0]), which is the world here.
            const auto& h = object.state.header;
            const float x = h.location[1], y = h.location[0];
            // Written so a position that isn't a number counts as off the lot.
            if (!(x >= 0 && y >= 0 && x <= 256 && y <= 256)) {
                log(object, "off the lot", "");
                continue;
            }
            // The extra tiles of a multi-tile object show nothing: its lead tile draws the whole model.
            // A scene record filed under an extra tile's id is left over from an earlier object.
            if (const auto* d = object.type ? assets_.objectDefinition(object.type->guid) : nullptr;
                d && d->field(objd::field::kMultiTileMasterId) != 0 && d->field(objd::field::kMultiTileSubIndex) != 0xFFFF &&
                object.state.data[0x13] > 0 && object.state.data[0x13] != static_cast<std::int32_t>(object.id)) {
                log(object, "part of " + std::to_string(object.state.data[0x13]), "");
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
            // Stair pieces (header value 4 names their staircase) keep their own height in value 3. Other
            // objects stand on their storey's floor, raised by value 3 (the pieces of a stacked chimney).
            const int level = content::gridLevel(lot_, h.value1);
            const float floorZ = heights_.tile(level, x, y);
            float z = h.value4 >= 0 ? (h.value3 != 0 ? h.value3 : floorZ) : floorZ + h.value3;
            // Another piece reaching down into the ground (a chimney's base) ends its full width at the
            // floor above, so that its tapered top tucks into the piece standing there.
            if (const auto range = heightRange(mesh); h.value4 < 0 && range[0] < -kLevelHeight) {
                z = heights_.tile(level + 1, x, y) - range[2];
            }
            // Direction d faces 180 - 45d degrees (0 along -X, 6 along -Y). Models face +Y.
            const float angle = kPi / 2 - static_cast<float>(object.state.data[1]) * kPi / 4;
            shown_.push_back({&object, mesh, so.model, Transform::at(x, y, z, angle), angle});
            byId[object.id] = shown_.size() - 1;
            log(object, "placed", so.model);
        }
        // Objects held in a container slot (object data 2: container id, 3: slot number) sit at that
        // slot's bone in the container's model, so each is placed after its container.
        auto containerOf = [&](std::size_t i) -> std::optional<std::size_t> {
            const Shown& s = shown_[i];
            const auto container = byId.find(static_cast<std::uint32_t>(s.object->state.data[2]));
            if (s.object->state.data[2] > 0 && container != byId.end() && container->second != i) {
                return container->second;
            }
            return std::nullopt;
        };
        auto placeInSlot = [&](std::size_t i, std::size_t container) {
            Shown& s = shown_[i];
            const Shown& c = shown_[container];
            const Transform& base = c.world;
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
        };
        // A chain of containers is followed without recursion (a broken lot could chain thousands)
        // and placed from its outermost container in. A loop of containers is cut where it closes.
        std::vector<int> placed(shown_.size(), 0);  // 0 not yet, 1 in the chain being placed, 2 done
        std::vector<std::size_t> chain;
        for (std::size_t i = 0; i < shown_.size(); ++i) {
            chain.clear();
            for (std::optional<std::size_t> k = i; k && placed[*k] == 0; k = containerOf(*k)) {
                placed[*k] = 1;
                chain.push_back(*k);
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                const auto container = containerOf(*it);
                if (container && placed[*container] == 2) {
                    placeInSlot(*it, *container);
                }
                placed[*it] = 2;
            }
            shown_[i].instance = b_.scene().instances.size();
            if (auto* instance = b_.place(shown_[i].mesh, std::to_string(shown_[i].object->id) + " " + shown_[i].model, shown_[i].world,
                                          Part::Object, content::gridLevel(lot_, shown_[i].object->state.header.value1))) {
                instance->object = shown_[i].object->id;
                PlacedObject record{shown_[i].object->id, shown_[i].instance, Transform{}, {}};
                if (shown_[i].object->type) {
                    for (std::size_t slot = 0; slot < kMaxShownSlots; ++slot) {
                        const std::string bone = assets_.slotBone(shown_[i].object->type->guid, slot);
                        const Transform* at = bone.empty() ? nullptr : b_.bone(shown_[i].model, bone);
                        record.slots.push_back(at ? std::optional<Transform>(*at) : std::nullopt);
                    }
                    while (!record.slots.empty() && !record.slots.back()) {
                        record.slots.pop_back();
                    }
                }
                b_.scene().placedObjects.push_back(std::move(record));
                instance->sunTest = sunTestOf(*shown_[i].object);
                if (instance->sunTest == SunTest::NodePosition) {
                    instance->lightGroup = LightGroup::Portals;
                }
            }
            ++stats_.objects;
        }
    }

    // Doors and windows are tested where their parts stand in their wall. Stairs never are.
    SunTest sunTestOf(const content::LotObject& object) {
        constexpr std::uint16_t kDoor = 8, kWindow = 9, kStairs = 10, kModularStairs = 11, kModularStairsPortal = 12;
        const auto* d = object.type ? assets_.objectDefinition(object.type->guid) : nullptr;
        const auto kind = d ? d->field(objd::field::kType) : 0;
        if (kind == kDoor || kind == kWindow) {
            return SunTest::NodePosition;
        }
        return kind == kStairs || kind == kModularStairs || kind == kModularStairsPortal ? SunTest::Never : SunTest::BoxCentre;
    }

    // The Sims saved on the lot, each in what it wears: its parts from its character file, found by
    // the Sim's object GUID. They stand on their storey's floor.
    void placeSims() {
        std::vector<const content::LotObject*> sims;
        for (const auto& object : lot_.objects) {
            if (object.type && object.type->kind == objt::kPersonKind) {
                sims.push_back(&object);
            }
        }
        if (sims.empty()) {
            return;
        }
        const auto& floorHeight = b_.scene().floorHeight;
        std::map<std::uint32_t, std::filesystem::path> files;
        for (const auto& file : content::characterFiles(content::neighborhoodOfLot(path_))) {
            try {
                dbpf::Package package{file};
                for (const auto& e : package.entries()) {
                    if (e.key.type == types::kObjectDefinition) {
                        files.emplace(objd::parse(package.read(e)).guid(), file);
                        break;
                    }
                }
            } catch (const std::exception& ex) {
                diag::warnOnce(std::string("lot: an object file that can't be read is left out: ") + ex.what());
            }
        }
        for (const auto* sim : sims) {
            const auto& h = sim->state.header;
            const float x = h.location[1], y = h.location[0];
            const auto file = files.find(sim->type->guid);
            if (!(x >= 0 && y >= 0 && x <= 256 && y <= 256) || file == files.end()) {
                log(*sim, file == files.end() ? "no character file" : "off the lot", "");
                continue;
            }
            const Assets::CharacterInUse inUse(assets_, file->second);
            const SimLook look = simLook(assets_, file->second);
            const float angle = kPi / 2 - static_cast<float>(sim->state.data[1]) * kPi / 4;
            const int level = content::gridLevel(lot_, h.value1);
            const Transform at = Transform::at(x, y, heights_.tile(level, x, y), angle);
            // Standing, as the game shows an idle Sim: its age's idle loop, played from its start.
            const std::string idle = standingIdle(assets_, look.age);
            std::vector<Transform> world;
            Actor actor = buildSim(b_, rigs_, look, at, level, std::to_string(sim->id), idle, world);
            actor.object = sim->id;
            actor.basePose = standingPose(assets_, look.age);
            actor.floorHeight = floorHeight;
            for (const auto& bone : simSlotBones(file->second)) {
                actor.slotNodes.push_back(actor.rig && !bone.empty() ? actor.rig->find(bone) : -1);
            }
            holdBySim(*sim, file->second, actor, world);
            b_.scene().actors.push_back(std::move(actor));
            ++stats_.sims;
            log(*sim, "placed", file->second.filename().string());
        }
    }

    // Objects a Sim holds (a ring, the plumbob over the selected Sim) go to the bone of the Sim's slot
    // they are in, and follow it as the Sim moves. Through holders with no model of their own (the
    // head slots holder), whose own slot offsets aren't known, they go to the holder's place. `world` is
    // the Sim's rig as it stands now.
    void holdBySim(const content::LotObject& sim, const std::filesystem::path& characterFile, Actor& actor,
                   const std::vector<Transform>& world) {
        std::map<std::uint32_t, const content::LotObject*> objects;
        for (const auto& o : lot_.objects) {
            objects[o.id] = &o;
        }
        // The rig node an object is held at, through its containers: -1 at the Sim's own place,
        // nullopt when not held by the Sim.
        std::map<std::uint32_t, std::optional<int>> held;
        auto node = [&](auto&& self, std::uint32_t id, int depth) -> std::optional<int> {
            if (id == sim.id) {
                return -1;
            }
            const auto o = objects.find(id);
            if (o == objects.end() || depth > 8 || o->second->state.data[2] <= 0) {
                return std::nullopt;
            }
            const auto container = static_cast<std::uint32_t>(o->second->state.data[2]);
            if (const auto it = held.find(id); it != held.end()) {
                return it->second;
            }
            std::optional<int> result;
            if (container == sim.id) {
                const std::string bone = simSlotBone(characterFile, static_cast<std::size_t>(o->second->state.data[3]));
                result = actor.rig && !bone.empty() ? actor.rig->find(bone) : -1;
            } else {
                result = self(self, container, depth + 1);
            }
            held[id] = result;
            return result;
        };
        for (auto& s : shown_) {
            if (s.object->state.data[2] <= 0) {
                continue;
            }
            if (const auto n = node(node, s.object->id, 0)) {
                s.world = *n >= 0 && static_cast<std::size_t>(*n) < world.size() ? actor.place * world[static_cast<std::size_t>(*n)]
                                                                                   : actor.place;
                b_.scene().instances[s.instance].transform = s.world;
                actor.held.push_back({s.instance, *n});
            }
        }
    }

    // The lowest and highest points of a mesh, below and above its origin, and the highest point still
    // at the mesh's full width (on the edge of its footprint).
    std::array<float, 3> heightRange(int mesh) const {
        const auto& primitives = b_.scene().meshes[static_cast<std::size_t>(mesh)].primitives;
        std::array<float, 4> box{1e9f, 1e9f, -1e9f, -1e9f};
        std::array<float, 3> r{1e9f, -1e9f, -1e9f};
        for (const auto& p : primitives) {
            for (std::size_t i = 0; i + 2 < p.positions.size(); i += 3) {
                box = {std::min(box[0], p.positions[i]), std::min(box[1], p.positions[i + 1]), std::max(box[2], p.positions[i]),
                       std::max(box[3], p.positions[i + 1])};
                r[0] = std::min(r[0], p.positions[i + 2]);
                r[1] = std::max(r[1], p.positions[i + 2]);
            }
        }
        constexpr float eps = 1e-3f;
        for (const auto& p : primitives) {
            for (std::size_t i = 0; i + 2 < p.positions.size(); i += 3) {
                const float x = p.positions[i], y = p.positions[i + 1];
                if (std::abs(x - box[0]) < eps || std::abs(x - box[2]) < eps || std::abs(y - box[1]) < eps || std::abs(y - box[3]) < eps) {
                    r[2] = std::max(r[2], p.positions[i + 2]);
                }
            }
        }
        return r;
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
    void drawWalls() {
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
        const int ground = g->contains(content::groundGridLevel(lot_), 0, 0) ? content::groundGridLevel(lot_) : 0;
        auto corner = [&](std::uint32_t x, std::uint32_t y) {
            float v = 0;
            std::memcpy(&v, g->at(ground, static_cast<std::int32_t>(std::min(x, g->width - 1)), static_cast<std::int32_t>(std::min(y, g->height - 1))).data(), 4);
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

        // The ground is lit by the ground level's floor light map, as the game lights it.
        makeFloorLightMaps();
        const float lotWidth = static_cast<float>(g->width - 1), lotHeight = static_cast<float>(g->height - 1);
        b_.scene().lotSize = {lotWidth, lotHeight};
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
                canvas.lightUvs.insert(canvas.lightUvs.end(), {x / lotWidth, y / lotHeight});
            }
        }
        // Ground with no surface (over a pool or a sunken pond) is left open.
        const auto surface = lot_.grids.find(content::kSurfaceGridInstance);
        auto open = [&](std::uint32_t i, std::uint32_t j) {
            return surface != lot_.grids.end() &&
                   surfaceOpen(surface->second, ground, (static_cast<float>(i) + 0.5f) * step, (static_cast<float>(j) + 0.5f) * step);
        };
        auto cell = [&](std::vector<std::uint32_t>& indices, std::uint32_t i, std::uint32_t j) {
            if (open(i, j)) {
                return;
            }
            const std::uint32_t a = i * rows + j, b = (i + 1) * rows + j;
            indices.insert(indices.end(), {a, b, b + 1, a, b + 1, a + 1});
        };
        for (std::uint32_t i = 0; i + 1 < columns; ++i) {
            for (std::uint32_t j = 0; j + 1 < rows; ++j) {
                cell(canvas.indices, i, j);
            }
        }

        Mesh mesh;
        mesh.name = "terrain";
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
            paint.lightUvs = canvas.lightUvs;
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
        if (auto* instance = b_.place(static_cast<int>(b_.scene().meshes.size() - 1), "terrain", Transform::at(0, 0, -kGroundDrop, 0));
            instance && -lightMaps().lowest >= 0 && static_cast<std::size_t>(-lightMaps().lowest) < lightMaps().floors.size()) {
            // The game's lot terrain: its paint times twice its diffuse (0.9) times the light map, at most 1.
            instance->lightMap = lightMaps().floors[static_cast<std::size_t>(-lightMaps().lowest)];
            instance->lightMapGain = 1.8f;
            instance->lightMapOnly = true;
            instance->lightMapMultiply = true;
        }
    }

    // The tinted sheets under the pools' water that darken deeper water.
    void drawPoolWater() {
        const auto quarters = poolwater::waterQuarters(lot_);
        if (quarters.empty()) {
            return;
        }
        const int ground = content::groundGridLevel(lot_);
        auto height = [&](float x, float y) {
            return x == std::floor(x) && y == std::floor(y) ? heights_.corner(ground, x, y) : heights_.tile(ground, x, y);
        };
        Mesh mesh;
        mesh.name = "pool water";
        int n = 0;
        for (auto& layer : poolwater::depthLayers(quarters, height)) {
            Primitive p;
            // The game's PoolDepthLayerMaterial draws on poolWaterLayer (5).
            p.material = b_.blendedColorMaterial("poolWaterLayer-" + std::to_string(++n), {0, 0.5f, 1, layer.opacity}, 5);
            p.positions = std::move(layer.positions);
            p.normals.resize(p.positions.size());
            for (std::size_t i = 2; i < p.normals.size(); i += 3) {
                p.normals[i] = 1;
            }
            p.uvs.assign(p.positions.size() / 3 * 2, 0.0f);
            p.alphas = std::move(layer.alphas);
            p.indices = std::move(layer.indices);
            mesh.primitives.push_back(std::move(p));
        }
        b_.scene().meshes.push_back(std::move(mesh));
        b_.place(static_cast<int>(b_.scene().meshes.size() - 1), "pool water", Transform{}, Part::Floor, ground);
    }

    // Floors: a pattern per quarter tile, each quarter the triangle from one tile edge to the tile's
    // center (stored in the order left, bottom, right, top, as the room grid is). Then the slab under
    // floors above the ground.
    void drawFloors() {
        const auto floors = lot_.grids.find(content::kFloorGridInstance);
        if (floors == lot_.grids.end() || floors->second.elementSize != 8) {
            return;
        }
        const auto& g = floors->second;
        std::set<int> patterns;
        // Light-map coordinates: the ground level's tiles run on into each other. Above it each tile
        // keeps to its own texels, its edges on their centres, as the game's light-map slots do.
        makeFloorLightMaps();
        auto& maps = lightMaps();
        const float mapWidth = static_cast<float>(g.width), mapHeight = static_cast<float>(g.height);
        const int ground = content::groundGridLevel(lot_);
        auto lightUv = [&](std::int32_t level, float tileX, float tileY, float fx, float fy) {
            if (level == ground) {
                return std::array<float, 2>{(tileX + fx) / mapWidth, (tileY + fy) / mapHeight};
            }
            return std::array<float, 2>{(tileX + (0.5f + 7 * fx) / 8) / mapWidth, (tileY + (0.5f + 7 * fy) / 8) / mapHeight};
        };
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
                        // Textures run along the game's own tile axes (world y, x here), a repeat
                        // spanning the material's tiles.
                        std::array<float, 2> span{1, 1};
                        if (material >= 0) {
                            span = b_.scene().materials[static_cast<std::size_t>(material)].floorTiles;
                        }
                        addVertex(s, {corners[q][0], corners[q][1], h[q]}, {0, 0, 1}, {corners[q][1] / span[0], corners[q][0] / span[1]});
                        addVertex(s, {corners[n][0], corners[n][1], h[n]}, {0, 0, 1}, {corners[n][1] / span[0], corners[n][0] / span[1]});
                        addVertex(s, {fx + 0.5f, fy + 0.5f, center}, {0, 0, 1}, {(fy + 0.5f) / span[0], (fx + 0.5f) / span[1]});
                        for (const auto& c : {corners[q], corners[n], std::array<float, 2>{fx + 0.5f, fy + 0.5f}}) {
                            const auto uv = lightUv(level, fx, fy, c[0] - fx, c[1] - fy);
                            s.lightUvs.insert(s.lightUvs.end(), uv.begin(), uv.end());
                        }
                    }
                }
            }
            if (auto* instance = b_.place(b_.addMesh("floors", std::move(surfaces)), "floors", Transform{}, Part::Floor, level);
                instance && static_cast<std::size_t>(level) < maps.floors.size()) {
                instance->lightMap = maps.floors[static_cast<std::size_t>(level)];
                instance->incidenceMap = maps.floorIncidence[static_cast<std::size_t>(level)];
            }
        }
        stats_.floorPatterns = patterns.size();

        // The slab under each floored quarter tile above the ground: a ceiling underneath and a band down each
        // edge where the floor stops, unless a wall below runs along it (the slab ends inside the wall,
        // and a wall standing on the edge with none under it, as over a porch, still shows the slab).
        // The bands are the game's FloorThickness material (floor-edge at diffuse 0.5), the texture
        // once across each tile edge or half diagonal, left to right as seen from outside.
        const int ceiling = b_.colorMaterial("ceiling", {0.92f, 0.91f, 0.88f});
        int floorEdge = b_.textureMaterial("floor-edge", 0.5f);
        if (floorEdge < 0) {
            floorEdge = edge_;
        }
        std::set<EdgeKey> wallEdges;
        for (const auto& w : walls_) {
            if (w.thick) {
                wallEdges.insert(edgeKey(w.q.x0, w.q.y0, w.q.x1, w.q.y1, w.level));
            }
        }
        // Quarter e of a tile is the triangle on its edge e (corners e, e + 1 and the centre).
        auto floored = [&](std::int32_t level, std::int32_t x, std::int32_t y, std::size_t e) {
            return g.contains(level, x, y) && g.i16(level, x, y, 3 - e % 4) != 0;
        };
        const std::array<std::array<float, 2>, 4> noUv{};
        const std::array<std::array<float, 2>, 4> bandUv{{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
        constexpr float kHalf = 0.70710678f;  // a diagonal band's normal (1 / sqrt 2 each way)
        for (std::int32_t level = 1; level < static_cast<std::int32_t>(g.levels); ++level) {
            Surfaces slabs, edges;
            for (std::int32_t x = 0; x < static_cast<std::int32_t>(g.width); ++x) {
                for (std::int32_t y = 0; y < static_cast<std::int32_t>(g.height); ++y) {
                    const float fx = static_cast<float>(x), fy = static_cast<float>(y);
                    const std::array<std::array<float, 2>, 4> c{{{fx, fy}, {fx + 1, fy}, {fx + 1, fy + 1}, {fx, fy + 1}}};
                    std::array<float, 4> top{};
                    float middle = 0;
                    for (std::size_t k = 0; k < 4; ++k) {
                        top[k] = heights_.corner(level, c[k][0], c[k][1]);
                        middle += top[k] / 4;
                    }
                    const std::array<float, 2> centre{fx + 0.5f, fy + 0.5f};
                    // A band from `a` (left, seen from outside) to `b`, facing `out`.
                    auto band = [&](const std::array<float, 2>& a, float ta, const std::array<float, 2>& b, float tb, const std::array<float, 3>& out) {
                        addQuad(edges[floorEdge], {{{a[0], a[1], ta - kFloorThickness}, {b[0], b[1], tb - kFloorThickness}, {b[0], b[1], tb}, {a[0], a[1], ta}}},
                                out, bandUv);
                    };
                    const int neighbor[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};  // across edges c0-c1, c1-c2, ...
                    for (std::size_t e = 0; e < 4; ++e) {
                        if (!floored(level, x, y, e)) {
                            continue;
                        }
                        const auto& a = c[e];
                        const auto& b = c[(e + 1) % 4];
                        const float ta = top[e], tb = top[(e + 1) % 4];
                        addQuad(slabs[ceiling],
                                {{{a[0], a[1], ta - kFloorThickness}, {b[0], b[1], tb - kFloorThickness},
                                  {centre[0], centre[1], middle - kFloorThickness}, {centre[0], centre[1], middle - kFloorThickness}}},
                                {0, 0, -1}, noUv);
                        const std::array<float, 3> out{static_cast<float>(neighbor[e][0]), static_cast<float>(neighbor[e][1]), 0};
                        if (!floored(level, x + neighbor[e][0], y + neighbor[e][1], e + 2) &&
                            !wallEdges.contains(edgeKey(a[0], a[1], b[0], b[1], level - 1))) {
                            band(a, ta, b, tb, out);
                        }
                        // Where only half the tile is floored, along the diagonal (a diagonal wall below
                        // hides it as well). It faces the two unfloored quarters beyond it.
                        const bool diagonalWall = wallEdges.contains(edgeKey(fx, fy, fx + 1, fy + 1, level - 1));
                        auto facing = [&](std::size_t p, std::size_t q) {
                            return std::array<float, 3>{kHalf * static_cast<float>(neighbor[p % 4][0] + neighbor[q % 4][0]),
                                                        kHalf * static_cast<float>(neighbor[p % 4][1] + neighbor[q % 4][1]), 0};
                        };
                        if (!floored(level, x, y, e + 1) && !diagonalWall) {
                            band(b, tb, centre, middle, facing(e + 1, e + 2));
                        }
                        if (!floored(level, x, y, e + 3) && !diagonalWall) {
                            band(centre, middle, a, ta, facing(e + 2, e + 3));
                        }
                    }
                }
            }
            b_.place(b_.addMesh("floor slabs", std::move(slabs)), "floor slabs", Transform{}, Part::Floor, level);
            // Every floor edge on the lot is lit by the outdoor lights without the sun, even inside.
            if (auto* instance = b_.place(b_.addMesh("floor edges", std::move(edges)), "floor edges", Transform{}, Part::Floor, level)) {
                instance->litOutdoors = true;
                instance->sunTest = SunTest::NoSun;
            }
        }
    }

    // Roofs are shaped before the walls, as attic walls follow the slopes of the gable ends they close.
    void shapeRoofs() {
        // The most greenhouse pieces placed on one lot. A whole 64 x 64 lot under glass needs about
        // 8,300, and a broken file's many large roofs could otherwise ask for millions.
        constexpr std::size_t kMaxGreenhousePieces = 65536;
        std::size_t glassPieces = 0;
        roofs_.reserve(lot_.roofs.size());
        for (const auto& roof : lot_.roofs) {
            if (!sensibleRoof(roof)) {
                diag::warnOnce("roofs: a roof reaching past 256 tiles or with broken numbers is left out");
                continue;
            }
            RoofLook look;
            if (const auto* pattern = assets_.roofPattern(roof.pattern)) {
                // The top texture is a 5 x 5 grid of roof pieces (plain courses, ridge and hip caps, each
                // cell one repeat of the courses), cell (4, 3) plain, the cap piece along the top. The
                // colors are those of the game's roof materials.
                const std::array<float, 3> slopeColor{0.4f, 0.385f, 0.38f};
                look.top = b_.textureCellMaterial(pattern->top, 5, 5, 4, 3, slopeColor);
                look.under = b_.textureMaterial(pattern->under, 0.8f);
                look.trim = b_.textureMaterial(pattern->trim, 0.8f);
                look.edges = b_.textureMaterial(pattern->edges, 0.8f);
                look.atlas = b_.textureRegionMaterial(pattern->top, 0, 0, 1, 1, 512, 512, slopeColor);
                look.cap = b_.textureRegionMaterial(pattern->top, 0.4, 0.0, 0.6, 37.0 / 512, 128, 48, slopeColor);
                for (const int m : {look.top, look.atlas, look.cap}) {
                    if (m >= 0) {
                        b_.scene().materials[static_cast<std::size_t>(m)].roofLight = true;
                    }
                }
                // Valleys where roofs meet: the game's RoofEdges material (the edges texture at 0.8). Which
                // part of the texture the game shows there is not known yet.
                look.valley = b_.textureMaterial(pattern->edges, 0.8f);
            } else {
                look.top = look.under = look.trim = b_.colorMaterial("roof plain", {0.35f, 0.27f, 0.22f});
            }
            // A roof may keep its own slope (half the tangent of its angle), otherwise 45 degrees.
            const float slope = roof.slope > 0 ? 2 * roof.slope : 1.0f;
            ShapedRoof& r = roofs_.emplace_back();
            const int eaveLevel = content::gridIndex(lot_, roof.level0);
            r.level = eaveLevel - 1;  // a roof belongs to the storey under its eaves
            const float eave = heights_.corner(eaveLevel, roof.x0, roof.y0);
            r.look = look;
            RoofLook round = look;
            round.top = look.atlas >= 0 ? look.atlas : look.top;
            const auto* g = heights_.grid();
            const float lotWidth = g ? static_cast<float>(g->width - 1) : 1e9f, lotHeight = g ? static_cast<float>(g->height - 1) : 1e9f;
            if (auto grid = RoofGrid::of(roof, eave)) {
                r.shape = gridShape(roof, eave, std::move(*grid));
            } else if (addRoundRoof(r.surfaces, roof, eave, round, lotWidth, lotHeight)) {
                r.shape.eave = eave;
            } else if (roof.type == kGreenhouseRoof) {
                // Its glass pieces are the whole roof. The gable's shape still guides the walls.
                Surfaces none;
                r.shape = addRoof(none, roof, eave, slope, look);
                r.greenhouse = greenhousePieces(roof, eave);
                glassPieces += r.greenhouse.size();
                if (glassPieces > kMaxGreenhousePieces) {
                    diag::warnOnce("roofs: greenhouse roofs needing more than 65536 glass pieces are left out");
                    glassPieces -= r.greenhouse.size();
                    r.greenhouse.clear();
                }
            } else {
                r.shape = addRoof(r.surfaces, roof, eave, slope, look);
            }
            r.valley = look.valley;
        }
        // Grid roofs are built once all are known, as each is cut by and meets the others.
        std::vector<const RoofGrid*> grids;
        for (const auto& r : roofs_) {
            if (r.shape.grid) {
                grids.push_back(&*r.shape.grid);
            }
        }
        for (auto& r : roofs_) {
            if (!r.shape.grid) {
                continue;
            }
            // Only roofs whose grid overlaps this one's can cover or meet it: their heights are
            // none outside their own grid. Leaving the rest out keeps a lot of many roofs from
            // costing roofs x roofs x points.
            const RoofGrid& own = *r.shape.grid;
            auto overlaps = [&own](const RoofGrid& g) {
                constexpr float kSlack = 1e-3f;
                return g.minX <= own.maxX() + kSlack && own.minX <= g.maxX() + kSlack && g.minY <= own.maxY() + kSlack &&
                       own.minY <= g.maxY() + kSlack;
            };
            std::vector<const RoofGrid*> others;
            for (const auto* g : grids) {
                if (g != &own && overlaps(*g)) {
                    others.push_back(g);
                }
            }
            RoofLook look = r.look;
            look.top = look.atlas >= 0 ? look.atlas : look.top;
            addRoofGrid(r.surfaces, *r.shape.grid, others, look);
        }
        for (std::size_t i = 0; i < roofs_.size(); ++i) {
            for (std::size_t j = i + 1; j < roofs_.size(); ++j) {
                if (!roofs_[i].shape.grid && !roofs_[j].shape.grid) {
                    addValleys(roofs_[i].surfaces, roofs_[i].shape, roofs_[j].shape, roofs_[i].valley);
                }
            }
        }
        // An attic wall closes the gable ends it lies along, of roofs whose eaves are at its floor.
        for (auto& w : walls_) {
            if (!w.toRoof) {
                continue;
            }
            for (const auto& r : roofs_) {
                if (r.level + 1 != w.level) {
                    continue;
                }
                for (const auto& gable : r.shape.gables) {
                    const float gx = gable.b[0] - gable.a[0], gy = gable.b[1] - gable.a[1], glen = std::sqrt(gx * gx + gy * gy);
                    auto onLine = [&](float x, float y) {
                        const float off = ((x - gable.a[0]) * gy - (y - gable.a[1]) * gx) / glen;
                        const float t = ((x - gable.a[0]) * gx + (y - gable.a[1]) * gy) / glen;
                        return std::abs(off) < 1e-3f && t > -1e-3f && t < glen + 1e-3f;
                    };
                    if (glen > 0 && onLine(w.q.x0, w.q.y0) && onLine(w.q.x1, w.q.y1)) {
                        w.ceiling.gables.push_back(&r.shape);
                        break;
                    }
                }
            }
        }
    }

    void placeRoofs() {
        for (auto& r : roofs_) {
            b_.place(b_.addMesh("roof", std::move(r.surfaces)), "roof", Transform{}, Part::Roof, r.level);
            for (const auto& p : r.greenhouse) {
                if (const int mesh = b_.model(p.model, {}); mesh >= 0) {
                    b_.place(mesh, p.model, Transform::at(p.x, p.y, p.z, p.angle), Part::Roof, r.level);
                }
            }
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
    int wallTop_ = -1;
    std::string plain_;
    std::vector<ThickWall> walls_;
    int wallBase_ = 0;  // index of walls_[0] in the scene's walls
    std::map<EdgeKey, std::size_t> wallAt_;  // walls_ by their middle
    struct ShapedRoof {
        Surfaces surfaces;
        RoofShape shape;
        RoofLook look;
        std::vector<GreenhousePiece> greenhouse;
        int level = 0;
        int valley = -1;
    };

    // A grid roof's shape for the walls: its surface, slab depth and gable-end lines.
    static RoofShape gridShape(const roofs::Roof& roof, float eave, RoofGrid grid) {
        RoofShape s;
        s.eave = eave;
        s.slope = 2 * grid.scale;
        s.thickness = grid.z(0, 0, 0) - grid.z(0, 0, 2);
        const float lx = std::min(roof.x0, roof.x1), hx = std::max(roof.x0, roof.x1);
        const float ly = std::min(roof.y0, roof.y1), hy = std::max(roof.y0, roof.y1);
        // The high side of a shed or a slope rises like a gable end, and is closed the same way.
        if (grid.kind == RoofGrid::Kind::shed || grid.kind == RoofGrid::Kind::slope) {
            switch (grid.high) {
            case 0: s.gables.push_back({{lx, ly}, {lx, hy}, {-1, 0}}); break;
            case 1: s.gables.push_back({{hx, ly}, {hx, hy}, {1, 0}}); break;
            case 2: s.gables.push_back({{lx, ly}, {hx, ly}, {0, -1}}); break;
            case 3: s.gables.push_back({{lx, hy}, {hx, hy}, {0, 1}}); break;
            default: break;
            }
        }
        if (grid.kind == RoofGrid::Kind::slope) {
            // A slope's sides along its fall are gable ends.
            if (grid.high <= 1) {
                s.gables.push_back({{lx, ly}, {hx, ly}, {0, -1}});
                s.gables.push_back({{lx, hy}, {hx, hy}, {0, 1}});
            } else {
                s.gables.push_back({{lx, ly}, {lx, hy}, {-1, 0}});
                s.gables.push_back({{hx, ly}, {hx, hy}, {1, 0}});
            }
        } else if (grid.kind != RoofGrid::Kind::gable) {
            // Hips slope on every side.
        } else if (grid.ridgeAlongX) {
            s.gables.push_back({{lx, ly}, {lx, hy}, {-1, 0}});
            s.gables.push_back({{hx, ly}, {hx, hy}, {1, 0}});
        } else {
            s.gables.push_back({{lx, ly}, {hx, ly}, {0, -1}});
            s.gables.push_back({{lx, hy}, {hx, hy}, {0, 1}});
        }
        s.grid = std::move(grid);
        return s;
    }
    std::vector<ShapedRoof> roofs_;  // reserved up front: walls point at their shapes
    std::vector<Shown> shown_;
    RigLibrary rigs_{assets_};
};

} // namespace

ObjectLighting outdoorLighting(const content::Lighting::State& state) {
    ObjectLighting out;
    out.ambient = state.skylight;
    for (const char* name : {"sun", "fill1", "sky"}) {
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
        out.lights.push_back(light);
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
    return LotSceneBuilder(builder, loaded, lot, logObjects).build();
}

} // namespace ts2::scene
