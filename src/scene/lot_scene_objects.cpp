// The lot's objects and Sims, as the lot saved them: placed, in their containers' slots and in
// their Sims' hands.
#include "scene/lot_scene_builder.hpp"

#include "content/neighborhood.hpp"
#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "formats/objd.hpp"
#include "formats/resource_types.hpp"
#include "scene/sim_builder.hpp"
#include "scene/sims.hpp"
#include "scene/terrain_water.hpp"

#include <algorithm>
#include <optional>
#include <sstream>

namespace ts2::scene::lotscene {

namespace {

// Whether a saved position is on the lot. Written so that one that isn't a number is off it.
bool onLot(float x, float y) {
    return x >= 0 && y >= 0 && x <= 256 && y <= 256;
}

// The turn of a saved facing: direction d faces 180 - 45d degrees (0 along -X, 6 along -Y), and
// models face +Y.
float facingAngle(std::int32_t direction) {
    return kPi / 2 - static_cast<float>(direction) * kPi / 4;
}

} // namespace

void LotSceneBuilder::placeObjects() {
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
        if (!onLot(x, y)) {
            log(object, "off the lot", "");
            continue;
        }
        if (isExtraTile(assets_, object)) {
            log(object, "part of " + std::to_string(object.state.data[kLeadTileData]), "");
            continue;
        }
        const auto record = sceneRecords.find(object.id);
        if (record == sceneRecords.end()) {
            ++stats_.withoutRecord;
            log(object, "no scene record", "");
            continue;
        }
        scenegraph::SceneObject so;
        try {
            so = scenegraph::parseSceneObject(package.read(*record->second));
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
        // Its joints as the lot saved them: a dining chair pushed under a table is drawn that way.
        const auto saved = lot_.savedJoints.find(object.id);
        std::string boneModel;
        const int mesh = b_.savedPoseModel(so.model, so.overrides,
                                           saved != lot_.savedJoints.end() ? std::span<const cobject::SavedJoint>(saved->second)
                                                                           : std::span<const cobject::SavedJoint>{},
                                           boneModel);
        if (mesh < 0) {
            ++stats_.withoutModel;
            log(object, "model not found", so.model);
            continue;
        }
        // Stair pieces (header value 4 names their staircase) stand at the height in value 3, even
        // when it is 0 and the floor under them is lower. Other objects stand on their storey's
        // floor, raised by value 3 (the pieces of a stacked chimney).
        const int level = content::gridLevel(lot_, h.value1);
        const float floorZ = heights_.tile(level, x, y);
        float z = h.value4 >= 0 ? h.value3 : floorZ + h.value3;
        // Another piece reaching down into the ground (a chimney's base) ends its full width at the
        // floor above, so that its tapered top tucks into the piece standing there.
        if (const auto range = heightRange(mesh); h.value4 < 0 && range[0] < -kLevelHeight) {
            z = heights_.tile(level + 1, x, y) - range[2];
        }
        // Objects with placement flag 15 (object data 0x2A, as lily pads) float: on the ground, where
        // the water stands above the floor, they sit on its surface with no offset.
        if ((object.state.data[kPlacementData] & 0x4000) != 0 && level == content::groundGridLevel(lot_)) {
            if (const auto water = lot_.flatGrids.find(content::kWaterHeightGridInstance); water != lot_.flatGrids.end()) {
                if (const auto surface = terrainwater::heightAt(water->second, x, y); surface && floorZ < *surface) {
                    z = *surface;
                }
            }
        }
        const float angle = facingAngle(object.state.data[kFacingData]);
        shown_.push_back({&object, mesh, so.model, Transform::at(x, y, z, angle), angle, 0, boneModel, so.overrides});
        byId[object.id] = shown_.size() - 1;
        log(object, "placed", so.model);
    }
    // Objects held in a container slot (object data 2: container id, 3: slot number) sit at that
    // slot's bone in the container's model, so each is placed after its container.
    auto containerOf = [&](std::size_t i) -> std::optional<std::size_t> {
        const Shown& s = shown_[i];
        const auto container = byId.find(static_cast<std::uint32_t>(s.object->state.data[kContainerData]));
        if (s.object->state.data[kContainerData] > 0 && container != byId.end() && container->second != i) {
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
            name = assets_.slotBone(c.object->type->guid, static_cast<std::size_t>(s.object->state.data[kSlotData]));
        }
        const Transform* bone = name.empty() ? nullptr : b_.bone(c.boneModel, name);
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
            record.model = shown_[i].model;
            record.overrides = shown_[i].overrides;
            if (const auto saved = lot_.savedJoints.find(shown_[i].object->id); saved != lot_.savedJoints.end()) {
                record.savedJoints = saved->second;
            }
            if (shown_[i].object->type) {
                for (std::size_t slot = 0; slot < kMaxShownSlots; ++slot) {
                    const std::string bone = assets_.slotBone(shown_[i].object->type->guid, slot);
                    const Transform* at = bone.empty() ? nullptr : b_.bone(shown_[i].boneModel, bone);
                    record.slots.push_back(at ? std::optional<Transform>(*at) : std::nullopt);
                }
                while (!record.slots.empty() && !record.slots.back()) {
                    record.slots.pop_back();
                }
            }
            if (const std::string* joint = b_.followJoint(shown_[i].boneModel)) {
                if (const Transform* at = b_.bone(shown_[i].boneModel, *joint)) {
                    record.followJoint = *joint;
                    record.followRest = *at;
                }
            }
            b_.scene().placedObjects.push_back(std::move(record));
            instance->sunTest = sunTestOf(*shown_[i].object);
            if (instance->sunTest == SunTest::NodePosition) {
                instance->lightGroup = LightGroup::Portals;
            }
            instance->shadowFacts = shadowFactsOf(*shown_[i].object, shown_[i].model, shown_[i].mesh);
            instance->objectType = static_cast<std::uint16_t>(instance->shadowFacts.type);
            instance->shadow = objectshadow::requested(instance->shadowFacts);
            if (instance->shadow == objectshadow::Kind::Sun) {
                instance->shadowMesh = b_.model(shown_[i].model, {});
            }
            instance->shadowFacing = static_cast<std::uint8_t>((2 - shown_[i].object->state.data[kFacingData]) & 7);
        }
        ++stats_.objects;
    }
}

// What the game reads from an object to choose its shadow (see objectshadow).
objectshadow::Facts LotSceneBuilder::shadowFactsOf(const content::LotObject& object, const std::string& model, int mesh) {
    objectshadow::Facts f;
    const auto* d = object.type ? assets_.objectDefinition(object.type->guid) : nullptr;
    f.type = d ? d->field(objd::field::kType) : 0;
    f.data8 = object.state.data[8];
    f.data13 = object.state.data[0x0D];
    f.data68 = object.state.data[0x44];
    const auto& meshes = b_.scene().meshes;
    if (mesh >= 0 && static_cast<std::size_t>(mesh) < meshes.size()) {
        const auto& primitives = meshes[static_cast<std::size_t>(mesh)].primitives;
        f.drawable = !primitives.empty() && lower(model) != "nullmodel";
        f.hasShadowGroups = std::ranges::any_of(primitives, [](const Primitive& p) { return objectshadow::isShadowGroup(p.group); });
    }
    return f;
}

// Doors and windows are tested where their parts stand in their wall. Stairs never are.
SunTest LotSceneBuilder::sunTestOf(const content::LotObject& object) {
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
void LotSceneBuilder::placeSims() {
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
        if (!onLot(x, y) || file == files.end()) {
            log(*sim, file == files.end() ? "no character file" : "off the lot", "");
            continue;
        }
        const Assets::CharacterInUse inUse(assets_, file->second);
        const SimLook look = simLook(assets_, file->second);
        const float angle = facingAngle(sim->state.data[kFacingData]);
        const int level = content::gridLevel(lot_, h.value1);
        const Transform at = Transform::at(x, y, heights_.tile(level, x, y), angle);
        // Standing, as the game shows an idle Sim: its age's idle loop, played from its start.
        const std::string idle = standingIdle(assets_, look.age);
        std::vector<Transform> world;
        Actor actor = buildSim(b_, rigs_, look, at, level, std::to_string(sim->id), idle, world);
        actor.object = sim->id;
        actor.basePose = standingPose(assets_, look.age);
        actor.floorHeight = floorHeight;
        for (const auto& bone : simSlotBones(assets_, file->second)) {
            actor.slotNodes.push_back(actor.rig && !bone.empty() ? actor.rig->find(bone) : -1);
        }
        for (const auto& bone : simSlotBones(assets_, file->second, true)) {
            actor.targetNodes.push_back(actor.rig && !bone.empty() ? actor.rig->find(bone) : -1);
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
void LotSceneBuilder::holdBySim(const content::LotObject& sim, const std::filesystem::path& characterFile, Actor& actor,
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
        if (o == objects.end() || depth > 8 || o->second->state.data[kContainerData] <= 0) {
            return std::nullopt;
        }
        const auto container = static_cast<std::uint32_t>(o->second->state.data[kContainerData]);
        if (const auto it = held.find(id); it != held.end()) {
            return it->second;
        }
        std::optional<int> result;
        if (container == sim.id) {
            const std::string bone = simSlotBone(assets_, characterFile, static_cast<std::size_t>(o->second->state.data[kSlotData]));
            result = actor.rig && !bone.empty() ? actor.rig->find(bone) : -1;
        } else {
            result = self(self, container, depth + 1);
        }
        held[id] = result;
        return result;
    };
    for (auto& s : shown_) {
        if (s.object->state.data[kContainerData] <= 0) {
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
std::array<float, 3> LotSceneBuilder::heightRange(int mesh) const {
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

} // namespace ts2::scene::lotscene
