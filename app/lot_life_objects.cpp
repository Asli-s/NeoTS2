// The lot's objects as scripts move, create, remove, animate and light them.
#include "lot_life.hpp"
#include "lot_life_clips.hpp"

#include "diag/log.hpp"
#include "scene/object_lights.hpp"
#include "sim/object_model.hpp"

#include <algorithm>
#include <climits>
#include <format>

namespace app {

namespace {

// Object data 1, 2 and 3: an object's facing, the object it is held by and its slot there.
constexpr std::size_t kDirectionField = 1, kContainerField = 2, kSlotField = 3;

// What the game reads from an object to choose its shadow (see objectshadow), as the lot scene reads
// it for the lot's own objects.
ts2::scene::objectshadow::Facts shadowFacts(const ts2::sim::SimObject& o, const std::string& model, int mesh,
                                            const ts2::scene::Scene& scene) {
    ts2::scene::objectshadow::Facts f;
    f.type = o.definition.field(ts2::objd::field::kType);
    f.data8 = o.data[8];
    f.data13 = o.data[0x0D];
    f.data68 = o.data[0x44];
    if (mesh >= 0 && static_cast<std::size_t>(mesh) < scene.meshes.size()) {
        const auto& primitives = scene.meshes[static_cast<std::size_t>(mesh)].primitives;
        f.drawable = !primitives.empty() && ts2::scene::lower(model) != "nullmodel";
        f.hasShadowGroups = std::ranges::any_of(primitives, [](const ts2::scene::Primitive& p) {
            return ts2::scene::objectshadow::isShadowGroup(p.group);
        });
    }
    return f;
}

// The model a created object is drawn with (see createdModelName). An extra tile of a multi-tile
// object draws none, as on a saved lot: its lead draws the whole model.
std::string modelName(ts2::scene::Assets& assets, const ts2::sim::SimObject& object) {
    constexpr std::uint32_t kModelNames = 0x85;
    const std::int16_t lead = object.data[ts2::sim::kLeadTileField];
    if (object.definition.field(ts2::objd::field::kMultiTileMasterId) != 0 && lead > 0 && lead != object.id) {
        return {};
    }
    return ts2::sim::createdModelName(object, [&](std::size_t row) { return assets.objectString(object.group, kModelNames, row); });
}

} // namespace

// An object a script moved goes where the simulation has it: into a Sim's hand (where the Sim's pose
// carries it), onto another object's slot, or onto its storey's floor, facing its direction. One a
// script removed is no longer drawn. Objects that haven't moved keep the place the lot gave them.
// An object in a slot hangs under its container, as the game attaches it, so it moves whenever the
// container does: a bowl taken off the counter takes its food along.
void LotLife::placeObjects(ts2::scene::Scene& scene) {
    // Containers nest (food in a bowl on a counter), so what is in an object placed in one pass is
    // placed again in the next, from where its container is now.
    constexpr int kMaxNesting = 8;
    std::set<std::uint32_t> movedContainers;
    for (int pass = 0; pass <= kMaxNesting; ++pass) {
        std::set<std::uint32_t> placedNow;
        placeObjectsOnce(scene, movedContainers, pass == 0, placedNow);
        if (placedNow.empty()) {
            break;
        }
        movedContainers = std::move(placedNow);
    }
}

void LotLife::placeObjectsOnce(ts2::scene::Scene& scene, const std::set<std::uint32_t>& movedContainers, bool firstPass,
                               std::set<std::uint32_t>& placedNow) {
    for (const auto& placed : scene.placedObjects) {
        if (placed.object > static_cast<std::uint32_t>(INT16_MAX) || placed.instance >= scene.instances.size()) {
            continue;
        }
        // An object the lot keeps until it unloads is still drawn.
        const auto* object = sim_->world().findIncludingRemoved(static_cast<std::int16_t>(placed.object));
        ObjectPlace now;
        if (object == nullptr || (object->removed && !object->keptUntilUnload)) {
            now.gone = true;
        } else {
            now = {object->x, object->y, object->level, object->data[kDirectionField], object->data[kContainerField], object->data[kSlotField], false};
        }
        if (placed.object >= objectPlaces_.size()) {
            objectPlaces_.resize(std::size_t{placed.object} + 1);
        }
        KnownPlace& last = objectPlaces_[placed.object];
        const bool containerMoved = !now.gone && now.container > 0 && movedContainers.contains(static_cast<std::uint32_t>(now.container));
        const bool selfMoved = firstPass && last.known && !(last.place == now);
        if (!selfMoved && !containerMoved) {
            if (firstPass) {
                last = {now, true};
            }
            continue;
        }
        last.place = now;
        placedNow.insert(placed.object);
        for (auto& actor : scene.actors) {
            std::erase_if(actor.held, [&](const ts2::scene::Actor::Held& h) { return h.instance == placed.instance; });
        }
        ts2::scene::Transform place;
        if (now.gone) {
            place.m.fill(0.0f);
        } else if (now.container > 0) {
            // An object in something a Sim holds hangs at the bone of the Sim's slot the outermost holder
            // is in, from there at each holder's slot: food sits in the bowl in the hand. A holder with
            // no model (the head slots holder the plumbob sits in) or a slot with no bone adds nothing.
            constexpr int kMaxDepth = 8;
            std::int16_t holderId = now.container, holderSlot = now.slot;
            ts2::scene::Transform offset;
            auto holder = scene.actors.end();
            for (int depth = 0; depth <= kMaxDepth; ++depth) {
                holder = std::find_if(scene.actors.begin(), scene.actors.end(),
                                      [&](const ts2::scene::Actor& a) { return a.object == static_cast<std::uint32_t>(holderId); });
                const auto* outer = sim_->world().find(holderId);
                if (holder != scene.actors.end() || outer == nullptr || outer->removed || outer->isPerson() || outer->data[kContainerField] <= 0) {
                    break;
                }
                const auto drawn = std::find_if(scene.placedObjects.begin(), scene.placedObjects.end(),
                                                [&](const ts2::scene::PlacedObject& p) { return p.object == static_cast<std::uint32_t>(holderId); });
                const auto at = static_cast<std::size_t>(holderSlot);
                if (drawn != scene.placedObjects.end() && holderSlot >= 0 && at < drawn->slots.size() && drawn->slots[at]) {
                    offset = *drawn->slots[at] * offset;
                }
                holderSlot = outer->data[kSlotField];
                holderId = outer->data[kContainerField];
            }
            if (holder != scene.actors.end()) {
                const auto at = static_cast<std::size_t>(holderSlot);
                holder->held.push_back({placed.instance, at < holder->slotNodes.size() ? holder->slotNodes[at] : -1, offset});
                continue;
            }
            const auto container = std::find_if(scene.placedObjects.begin(), scene.placedObjects.end(),
                                                [&](const ts2::scene::PlacedObject& c) { return c.object == static_cast<std::uint32_t>(now.container); });
            if (container != scene.placedObjects.end() && container->instance < scene.instances.size()) {
                place = slotPlace(*container, static_cast<std::size_t>(now.slot), scene).value_or(standingPlace(*container, scene));
            } else if (const auto onPart = now.slot >= 0 ? partSlot(now.container, static_cast<std::size_t>(now.slot), scene) : std::nullopt) {
                // A multi-tile part has no model of its own: what is on the far end of a dining table
                // sits at that part's slot in the lead's model.
                place = *onPart;
            } else {
                continue;
            }
        } else if (now.x == ts2::sim::kOffLot || now.y == ts2::sim::kOffLot || !scene.floorHeight) {
            continue;
        } else {
            const float x = static_cast<float>(now.y) / 65536.0f;  // the lot's x and y are the position's y and x
            const float y = static_cast<float>(now.x) / 65536.0f;
            place = placed.lotFrame * ts2::scene::Transform::at(x, y, scene.floorHeight(now.level, x, y), ts2::sim::compassHeading(now.direction));
        }
        if (!now.gone) {
            // An object in a slot is on its container's storey, as the simulation keeps it.
            scene.instances[placed.instance].level = scene.groundLevel + now.level - 1;
        }
        scene.instances[placed.instance].transform = place;
        clipBases_.erase(placed.instance);
        moved_.emplace_back(placed.instance, place);
        relight(scene, placed.instance, placed.lotFrame.rigidInverse() * place, now.level - 1);
        // In a container not placed yet (a tray made in the same step as its food), it has no place to
        // choose its shadow from, so it chooses once the container is placed.
        const bool hasPlace = std::any_of(place.m.begin(), place.m.begin() + 11, [](float v) { return v != 0.0f; });
        if (!now.gone && hasPlace && unshaded_.erase(placed.instance) > 0) {
            shadeCreated(scene, placed.instance, placed.lotFrame.rigidInverse() * place, now.direction);
        }
    }
}

// Models go as scripts hide them and come back as scripts show them (see modelHidden). Whether what
// a hidden Sim holds goes with it is not known yet, so each object answers for its own model.
void LotLife::hideModels(ts2::scene::Scene& scene) {
    const auto& world = sim_->world();
    const auto hiddenId = [&](std::uint32_t id) {
        const auto* object = id <= static_cast<std::uint32_t>(INT16_MAX) ? world.find(static_cast<std::int16_t>(id)) : nullptr;
        return object != nullptr && ts2::sim::modelHidden(*object);
    };
    for (const auto& placed : scene.placedObjects) {
        if (placed.instance < scene.instances.size()) {
            scene.instances[placed.instance].hidden = hiddenId(placed.object);
        }
    }
    for (const auto& actor : scene.actors) {
        const bool hidden = hiddenId(actor.object);
        for (const auto& part : actor.skinned) {
            if (part.instance < scene.instances.size()) {
                scene.instances[part.instance].hidden = hidden;
            }
        }
    }
}

// A created object's shadow, chosen where it is first placed out of a Sim's hands, as a lot object's
// is where the lot has it: its shadow groups indoors or its sun picture outdoors, as it asks. `inLot`
// is its place in the lot's own frame, which the lot's rooms and floors are in.
void LotLife::shadeCreated(ts2::scene::Scene& scene, std::size_t index, const ts2::scene::Transform& inLot, std::int16_t direction) {
    using namespace ts2::scene;
    auto& instance = scene.instances[index];
    const auto asked = objectshadow::requested(instance.shadowFacts);
    const int storey = objectlights::lightStorey(scene.floorHeight, inLot, instance.level - scene.groundLevel);
    const bool outdoors = scene.roomAt && scene.roomAt(storey, inLot.m[3], inLot.m[7]) == 0;
    instance.shadow = objectshadow::resolved(asked, outdoors, instance.shadowFacts);
    if (asked == objectshadow::Kind::Sun) {
        instance.shadowMesh = instance.mesh;
        instance.shadowFacing = static_cast<std::uint8_t>((2 - direction) & 7);
    }
    // A shadow it had before a Sim took it gives way to this one.
    heldShadows_.erase(index);
}

// Where container slot `slot` of a multi-tile part without a model of its own holds what is in it:
// at the bone of the part's slot in its lead's model, as the lead draws the whole object. None for
// other objects, and when the lead's model has no such bone.
std::optional<ts2::scene::Transform> LotLife::partSlot(std::int16_t part, std::size_t slot, const ts2::scene::Scene& scene) {
    const auto* object = sim_->world().find(part);
    if (object == nullptr || object->data[ts2::sim::kLeadTileField] == part) {
        return std::nullopt;
    }
    const auto lead = std::find_if(scene.placedObjects.begin(), scene.placedObjects.end(), [&](const ts2::scene::PlacedObject& p) {
        return p.object == static_cast<std::uint32_t>(object->data[ts2::sim::kLeadTileField]);
    });
    if (lead == scene.placedObjects.end() || lead->model.empty() || lead->instance >= scene.instances.size()) {
        return std::nullopt;
    }
    const auto key = std::pair<std::int16_t, std::size_t>(part, slot);
    auto [it, first] = partSlots_.try_emplace(key);
    if (first) {
        const std::string bone = assets_.slotBone(object->definition.guid(), slot);
        const auto rig = bone.empty() ? nullptr : objectMaterials_.objectRig(lead->model, lead->savedJoints);
        const int node = rig ? rig->find(ts2::scene::lower(bone)) : -1;
        if (node >= 0) {
            std::vector<ts2::scene::Transform> rest;
            ts2::scene::poseRig(*rig, nullptr, 0, rest);
            it->second = rest[static_cast<std::size_t>(node)];
        }
    }
    if (!it->second) {
        return std::nullopt;
    }
    return standingPlace(*lead, scene) * *it->second;
}

// Where a lot object's container slot `slot` is in the world, none when its model has no such bone.
// A slot hangs under bones of the object's own, which its clip poses: a dining chair's getting-in
// clip holds the seat still as the chair slides out and back, and one that leaves the chair out
// takes the seat out with it.
std::optional<ts2::scene::Transform> LotLife::slotPlace(const ts2::scene::PlacedObject& placed, std::size_t slot, const ts2::scene::Scene& scene) {
    if (slot >= placed.slots.size() || !placed.slots[slot]) {
        return std::nullopt;
    }
    const auto& base = standingPlace(placed, scene);
    // An object playing a clip carries its slots with the bones they sit at: a carpool's passenger
    // seat drives off with the car.
    const auto* object = placed.object <= static_cast<std::uint32_t>(INT16_MAX) ? sim_->world().find(static_cast<std::int16_t>(placed.object))
                                                                               : nullptr;
    if (object != nullptr && !object->removed && object->animation) {
        const std::string bone = ts2::scene::lower(assets_.slotBone(object->definition.guid(), slot));
        const double nowMs = drawnMs();
        // The joints its clip leaves alone are where the object was last drawn with them.
        const std::vector<ts2::scene::HeldJoint>* heldJoints = nullptr;
        if (pose_ != nullptr) {
            if (const auto held = pose_->held.find(static_cast<std::uint32_t>(object->id)); held != pose_->held.end()) {
                heldJoints = &held->second;
            }
        }
        if (const auto posed = bone.empty() ? std::nullopt
                                            : objectMaterials_.jointPose(*object, nowMs, animations_, placed.model, bone, nullptr, heldJoints)) {
            return base * *posed;
        }
    }
    return base * *placed.slots[slot];
}

// Where a lot object stands without its clip, which moves its model from there (see followClips).
const ts2::scene::Transform& LotLife::standingPlace(const ts2::scene::PlacedObject& placed, const ts2::scene::Scene& scene) const {
    const auto base = clipBases_.find(placed.instance);
    return base != clipBases_.end() ? base->second : scene.instances[placed.instance].transform;
}

// An object playing a clip (Animate Object) whose whole drawn model hangs under one joint moves
// with that joint, from where it stands: a dining chair sliding in under its table or out again.
// It keeps the clip's last pose once the clip has played.
void LotLife::followClips(ts2::scene::Scene& scene) {
    const double nowMs = drawnMs();
    for (auto& placed : scene.placedObjects) {
        if (placed.followJoint.empty() || placed.object > static_cast<std::uint32_t>(INT16_MAX) ||
            placed.instance >= scene.instances.size() || heldNow_.contains(placed.instance)) {
            continue;
        }
        const auto* object = sim_->world().find(static_cast<std::int16_t>(placed.object));
        if (object == nullptr || object->removed || !object->animation) {
            continue;
        }
        // One drawn skinned for its morphs is posed by animateObjects instead.
        if (std::any_of(scene.objectActors.begin(), scene.objectActors.end(), [&](const ts2::scene::Actor& a) { return a.object == placed.object; })) {
            continue;
        }
        const auto joint = objectMaterials_.jointPose(*object, nowMs, animations_, placed.model, placed.followJoint, &placed.bones);
        if (!joint) {
            continue;
        }
        auto& instance = scene.instances[placed.instance];
        const auto base = clipBases_.try_emplace(placed.instance, instance.transform).first;
        for (auto& bone : placed.bones) {
            bone.second = base->second * bone.second;
        }
        const ts2::scene::Transform place = base->second * *joint * placed.followRest.rigidInverse();
        if (place.m != instance.transform.m) {
            instance.transform = place;
            moved_.emplace_back(placed.instance, place);
        }
    }
}

// Any other object whose clips move its model is drawn skinned to the model's joints from its first
// clip on, as a Sim is: its own clip (Animate Object) and the overlays laid on it. A bed's covers
// fold back as a Sim gets in and a fridge's doors swing open. One a Sim holds hangs from the Sim's
// hand (see poseActors) while its clip moves its joints: a snack bag tips as the Sim eats.
void LotLife::animateObjects(ts2::scene::Scene& scene) {
    const double nowMs = drawnMs();
    for (const auto& placed : scene.placedObjects) {
        if (placed.model.empty() || placed.object > static_cast<std::uint32_t>(INT16_MAX) || placed.instance >= scene.instances.size()) {
            continue;
        }
        const auto* object = sim_->world().find(static_cast<std::int16_t>(placed.object));
        auto actor = std::find_if(scene.objectActors.begin(), scene.objectActors.end(),
                                  [&](const ts2::scene::Actor& a) { return a.object == placed.object; });
        // A model that hangs under one joint moves whole with it (see followClips), unless it has
        // morphs, which only a skinned model draws: a living chair's cushions dent as a Sim sits.
        if (!placed.followJoint.empty() && !heldNow_.contains(placed.instance) && actor == scene.objectActors.end()) {
            const bool playing = object != nullptr && (object->animation || !object->overlays.empty());
            const int mesh = playing ? objectMaterials_.skinnedModel(placed.model, placed.overrides, scene) : -1;
            if (mesh < 0 || scene.meshes[static_cast<std::size_t>(mesh)].morphs.empty()) {
                continue;
            }
        }
        // One a script removed is no longer drawn (placeObjects gave its instance no place).
        if (object == nullptr || object->removed || object->isPerson()) {
            if (actor != scene.objectActors.end()) {
                actor->place = scene.instances[placed.instance].transform;
                actor->clips.clear();
            }
            continue;
        }
        if (actor == scene.objectActors.end()) {
            if ((!object->animation && object->overlays.empty()) || unskinned_.contains(placed.object)) {
                continue;
            }
            const int mesh = objectMaterials_.skinnedModel(placed.model, placed.overrides, scene);
            auto rig = mesh >= 0 ? objectMaterials_.objectRig(placed.model, placed.savedJoints) : nullptr;
            if (!rig || rig->nodes.empty()) {
                unskinned_.insert(placed.object);
                continue;
            }
            ts2::scene::Actor added;
            added.object = placed.object;
            added.skinned.push_back({placed.instance, ts2::scene::skinNodes(scene.meshes[static_cast<std::size_t>(mesh)], *rig)});
            added.rig = std::move(rig);
            for (std::size_t slot = 0; slot < placed.slots.size(); ++slot) {
                const std::string bone = ts2::scene::lower(assets_.slotBone(object->definition.guid(), slot));
                added.slotNodes.push_back(!bone.empty() ? added.rig->find(bone) : -1);
            }
            scene.instances[placed.instance].mesh = mesh;
            changedInstances_.push_back(placed.instance);
            objectMaterials_.showAgain(object->id);
            scene.objectActors.push_back(std::move(added));
            actor = scene.objectActors.end() - 1;
        }
        actor->place = standingPlace(placed, scene);
        // The clip its latest replaced plays on under it while the game keeps it: until the latest
        // has crossfaded it, else to its own end.
        const auto& own = object->animation;
        const auto& replaced = object->ending;
        const bool under = own && replaced && (crossfades(*replaced, *own, animations_) || stillPlaying(*replaced, *own));
        playClips(*actor, *object, own, under ? replaced : std::nullopt, 0, animations_);
        // A latest that doesn't move the object's root leaves it where the clip it replaced did: at
        // that clip's last frame once it has played out, however few of its frames were drawn (a bus's
        // drive clip, done between two frames at ultra speed, leaves the bus where it stopped before
        // its door opens). One that moves the root places the object itself.
        const auto movesRoot = [&](const ts2::sim::PlayingAnimation& a) {
            const auto bound = animations_.bound(actor->rig, a.name, a.flipped);
            const int root = actor->rig->find("root_trans");
            return bound && root >= 0 && static_cast<std::size_t>(root) < bound->translation.size() &&
                   bound->translation[static_cast<std::size_t>(root)] >= 0;
        };
        if (own && replaced && !under && replaced->startMs + replaced->lengthMs <= own->startMs && !movesRoot(*own)) {
            if (auto bound = animations_.bound(actor->rig, replaced->name, replaced->flipped); bound && bound->animation) {
                auto& held = actor->clips.emplace_back();
                held.animation = std::move(bound);
                held.layer = own->layer;
                held.start = replaced->startMs / 1000.0;
                held.end = held.start;
                held.fadeIn = held.fadeOut = false;
                held.atMs = clipMs(*replaced, held.animation->animation->duration, replaced->startMs + replaced->lengthMs);
            }
        }
        // A stopped clip holds the pose it was stopped at, in full, however few of its frames were
        // drawn (a car's stop clip, played out within a few frames at ultra speed).
        if (own && nowMs - own->startMs >= own->lengthMs) {
            const std::string ownName = ts2::scene::lower(own->name);
            for (auto& clip : actor->clips) {
                if (clip.layer == own->layer && clip.animation && clip.animation->animation &&
                    ts2::scene::lower(clip.animation->animation->name) == ownName) {
                    clip.atMs = clipMs(*own, clip.animation->animation->duration, nowMs);
                    clip.fadeOut = false;
                }
            }
        }
    }
}

// An object in a slot of one drawn skinned (one that has played a clip) hangs from that slot's bone
// as the clip poses it now, as the game attaches it under the bone: the food in a bowl a Sim lifts to
// eat. poseActors places it after whatever holds its container. A slot with no bone keeps the place
// placeObjects gave it.
void LotLife::hangInAnimated(ts2::scene::Scene& scene) {
    for (auto& actor : scene.objectActors) {
        actor.held.clear();
    }
    if (scene.objectActors.empty()) {
        return;
    }
    for (const auto& placed : scene.placedObjects) {
        if (placed.object >= objectPlaces_.size() || placed.instance >= scene.instances.size()) {
            continue;
        }
        const ObjectPlace& at = objectPlaces_[placed.object].place;
        if (!objectPlaces_[placed.object].known || at.gone || at.container <= 0 || at.slot < 0) {
            continue;
        }
        const auto container = std::find_if(scene.objectActors.begin(), scene.objectActors.end(),
                                            [&](const ts2::scene::Actor& a) { return a.object == static_cast<std::uint32_t>(at.container); });
        const auto slot = static_cast<std::size_t>(at.slot);
        if (container == scene.objectActors.end() || slot >= container->slotNodes.size() || container->slotNodes[slot] < 0) {
            continue;
        }
        container->held.push_back({placed.instance, container->slotNodes[slot], {}});
        for (auto& sim : scene.actors) {
            std::erase_if(sim.held, [&](const ts2::scene::Actor::Held& h) { return h.instance == placed.instance; });
        }
    }
}

// An object a Sim holds that asks for a sun picture draws no shadow indoors, as the game's do. Outdoors
// the game may give it another kind of shadow, not known yet, so none is drawn there either. Put
// down, it has its own again.
void LotLife::shadeHeld(ts2::scene::Scene& scene) {
    using ts2::scene::objectshadow::Kind;
    for (const auto& placed : scene.placedObjects) {
        if (placed.instance >= scene.instances.size()) {
            continue;
        }
        auto& instance = scene.instances[placed.instance];
        const auto was = heldShadows_.find(placed.instance);
        if (!heldNow_.contains(placed.instance)) {
            if (was != heldShadows_.end()) {
                instance.shadow = was->second;
                heldShadows_.erase(was);
            }
        } else if (was == heldShadows_.end() && ts2::scene::objectshadow::requested(instance.shadowFacts) == Kind::Sun) {
            heldShadows_.emplace(placed.instance, instance.shadow);
            instance.shadow = Kind::None;
        }
    }
}

// The game's shape colour on an object in its fading draw style (object data 0x47, the draw style, at
// 4, as a car leaving the lot is): red, green and blue from object data 0x49, 0x4A and 0x4B, alpha
// the lower of data 0x39 and 0x48, each the low byte in 255ths.
void LotLife::shapeColors(ts2::scene::Scene& scene) {
    constexpr std::size_t kDrawStyleField = 0x47, kAlphaField = 0x48, kRedField = 0x49, kGreenField = 0x4A, kBlueField = 0x4B,
                          kOtherAlphaField = 0x39;
    constexpr std::int16_t kFadingStyle = 4;
    for (const auto& placed : scene.placedObjects) {
        if (placed.object > static_cast<std::uint32_t>(INT16_MAX) || placed.instance >= scene.instances.size()) {
            continue;
        }
        const auto* object = sim_->world().find(static_cast<std::int16_t>(placed.object));
        std::optional<std::array<float, 4>> color;
        if (object != nullptr && !object->removed && object->data[kDrawStyleField] == kFadingStyle) {
            const auto byte = [](std::int16_t value) { return static_cast<float>(static_cast<std::uint8_t>(value)) / 255.0f; };
            const std::int16_t alpha = std::min(object->data[kOtherAlphaField], object->data[kAlphaField]);
            color = std::array<float, 4>{byte(object->data[kRedField]), byte(object->data[kGreenField]), byte(object->data[kBlueField]), byte(alpha)};
        }
        auto& instance = scene.instances[placed.instance];
        if (instance.shapeColor != color) {
            instance.shapeColor = color;
            shapeColored_.push_back(placed.instance);
        }
    }
}

void LotLife::relightAll(ts2::scene::Scene& scene) {
    for (const auto& placed : scene.placedObjects) {
        if (placed.instance >= scene.instances.size()) {
            continue;
        }
        const auto& instance = scene.instances[placed.instance];
        if (std::all_of(instance.transform.m.begin(), instance.transform.m.end(), [](float v) { return v == 0; })) {
            continue;  // removed by a script
        }
        relight(scene, placed.instance, placed.lotFrame.rigidInverse() * instance.transform, instance.level - scene.groundLevel);
    }
    for (const auto& actor : scene.actors) {
        const auto inLot = actor.lotFrame.rigidInverse() * actor.place;
        for (const auto& skinned : actor.skinned) {
            if (skinned.instance < scene.instances.size()) {
                relight(scene, skinned.instance, inLot, scene.instances[skinned.instance].level - scene.groundLevel);
            }
        }
    }
    std::sort(relit_.begin(), relit_.end());
    relit_.erase(std::unique(relit_.begin(), relit_.end()), relit_.end());
}

// An object takes the sun that reaches it under the buildings' shadow where it stands now, and a
// thing or a Sim the lights of the room it is in (doors and windows keep theirs).
void LotLife::relight(ts2::scene::Scene& scene, std::size_t index, const ts2::scene::Transform& inLot, int storey) {
    // Light sets only ever grow. Past this a thing keeps the light it has.
    constexpr std::size_t kMaxLightSets = 4096;
    if (index >= scene.instances.size()) {
        return;
    }
    auto& instance = scene.instances[index];
    if (scene.thingSun && instance.part == ts2::scene::Part::Object) {
        ts2::scene::Instance placed = instance;
        placed.transform = inLot;
        if (auto sun = scene.thingSun(scene, placed); sun != instance.sun) {
            instance.sun = std::move(sun);
            relit_.push_back(index);
        }
    }
    if (!scene.thingLights || instance.lightGroup == ts2::scene::LightGroup::Portals || instance.mesh < 0 ||
        static_cast<std::size_t>(instance.mesh) >= scene.meshes.size()) {
        return;
    }
    std::vector<int> parts;
    for (auto& set : scene.thingLights(scene.meshes[static_cast<std::size_t>(instance.mesh)], inLot,
                                       ts2::scene::objectlights::lightStorey(scene.floorHeight, inLot, storey), instance.lightGroup)) {
        const auto same = std::find(scene.lightSets.begin(), scene.lightSets.end(), set);
        if (same == scene.lightSets.end() && scene.lightSets.size() >= kMaxLightSets) {
            return;
        }
        parts.push_back(static_cast<int>(same - scene.lightSets.begin()));
        if (same == scene.lightSets.end()) {
            scene.lightSets.push_back(std::move(set));
        }
    }
    int whole = -1;
    if (!parts.empty() && std::all_of(parts.begin(), parts.end(), [&](int set) { return set == parts[0]; })) {
        whole = parts[0];
        parts.clear();
    }
    if (whole != instance.lightSet || parts != instance.partLightSets) {
        instance.lightSet = whole;
        instance.partLightSets = std::move(parts);
        relit_.push_back(index);
    }
}

// Draws objects scripts create after load as the lot draws its objects (placeObjects finds them moved
// from nowhere). One without a model gets an empty instance so RayTrace can start or end on it, at
// the object's origin, as the game aims at its model bounds' middle. A removed one's instance goes to
// the next object created. New objects take the lowest free id, so all ids are checked, and an id now
// held by another type is drawn anew. Removed Sims go only with `removeSims`, as the frame's pose finds
// Sims by actor list place and their instances by number, so a ray query between frames leaves both.
void LotLife::addCreatedObjects(ts2::scene::Scene& scene, bool removeSims) {
    // A lot that keeps creating objects can't grow the scene past this.
    constexpr std::size_t kMaxCreated = 4096;
    constexpr std::size_t kMaxSlots = 16;  // as the lot scene reads them
    auto& world = sim_->world();
    // What is drawn for `id` is taken out: a created object's instance goes to the next one created,
    // a lot object's draws nothing from now on.
    const auto undraw = [&](std::int16_t id) {
        const auto created = created_.find(id);
        std::vector<std::size_t> indices;
        for (const auto& p : scene.placedObjects) {
            if (p.object == static_cast<std::uint32_t>(id)) {
                indices.push_back(p.instance);
            }
        }
        for (const std::size_t index : indices) {
            if (created != created_.end()) {
                scene.instances[index] = ts2::scene::Instance{};
                freeInstances_.push_back(index);
            } else {
                scene.instances[index].transform.m.fill(0.0f);
            }
            for (auto& actor : scene.actors) {
                std::erase_if(actor.held, [&](const ts2::scene::Actor::Held& h) { return h.instance == index; });
            }
            std::erase_if(moved_, [&](const auto& m) { return m.first == index; });
            heldShadows_.erase(index);
            unshaded_.erase(index);
            clipBases_.erase(index);
            changedInstances_.push_back(index);
        }
        std::erase_if(scene.objectActors, [&](const ts2::scene::Actor& a) { return a.object == static_cast<std::uint32_t>(id); });
        std::erase_if(scene.placedObjects, [&](const ts2::scene::PlacedObject& p) { return p.object == static_cast<std::uint32_t>(id); });
        if (static_cast<std::size_t>(id) < objectPlaces_.size()) {
            objectPlaces_[static_cast<std::size_t>(id)] = {};
        }
        drawnTypes_.erase(id);
        drawnModels_.erase(id);
        // What was known of the old object's model goes with it, so the next one at this id starts afresh.
        unskinned_.erase(static_cast<std::uint32_t>(id));
        // A multi-tile part at this id, or the parts it leads, may be another object's now.
        partSlots_.clear();
        objectMaterials_.showAgain(id);
        if (created != created_.end()) {
            created_.erase(created);
        }
    };
    // Objects only change this way when the world's objects version does, and this runs every frame.
    const bool objectsSame = createdFor_ == world.objectsVersion();
    if (!objectsSame) {
        std::vector<std::int16_t> gone;
        for (const auto& [id, index] : created_) {
            const auto* object = world.find(id);
            if (object == nullptr || object->removed) {
                gone.push_back(id);
            }
        }
        for (const std::int16_t id : gone) {
            undraw(id);
        }
    }
    for (auto it = createdSims_.begin(); it != createdSims_.end();) {
        const auto* sim = world.find(it->first);
        if (!removeSims || (sim != nullptr && !sim->removed)) {
            ++it;
            continue;
        }
        for (const std::size_t index : it->second) {
            freeSimInstance(scene, index);
        }
        std::erase_if(scene.actors, [&](const ts2::scene::Actor& a) { return a.object == static_cast<std::uint32_t>(it->first); });
        triedSims_.erase(it->first);
        // A new Sim given its id starts without its clips.
        ownAnimations_.erase(static_cast<std::uint32_t>(it->first));
        it = createdSims_.erase(it);
    }
    // A Sim the lot was loaded with goes the same way once removed, so a new Sim given its id is drawn
    // as itself, without its clips.
    std::erase_if(scene.actors, [&](const ts2::scene::Actor& a) {
        if (!removeSims || a.object == 0 || a.object > static_cast<std::uint32_t>(INT16_MAX)) {
            return false;
        }
        const auto* sim = world.findIncludingRemoved(static_cast<std::int16_t>(a.object));
        if (sim != nullptr && (!sim->removed || sim->keptUntilUnload)) {
            return false;
        }
        for (const auto& s : a.skinned) {
            if (s.instance < scene.instances.size()) {
                freeSimInstance(scene, s.instance);
            }
        }
        triedSims_.erase(static_cast<std::int16_t>(a.object));
        ownAnimations_.erase(a.object);
        return true;
    });
    // New objects take the lowest free id, so Sims are looked for among all of them. Each is tried once.
    world.nextAfter(0, [&](const ts2::sim::SimObject& o) {
        // A Sim the loader is building is drawn once it is built.
        if (!o.isPerson() || o.removed || simLoading(o) || !triedSims_.insert(o.id).second ||
            std::any_of(scene.actors.begin(), scene.actors.end(), [&](const ts2::scene::Actor& a) { return a.object == static_cast<std::uint32_t>(o.id); })) {
            return false;
        }
        try {
            if (!addCreatedSim(scene, o)) {
                ts2::diag::warning(std::format("Sim {} (neighbor {}) is not drawn: it has no look yet", o.id, world.neighborIdOf(o)));
            } else {
                dressFromSource(o.id);
            }
        } catch (const std::exception& ex) {
            ts2::diag::warning(std::format("Sim {} is not drawn: its look can't be read: {}", o.id, ex.what()));
        }
        return false;
    });
    if (objectsSame) {
        return;
    }
    world.nextAfter(0, [&](const ts2::sim::SimObject& o) {
        if (o.isPerson() || o.removed) {
            return false;
        }
        if (const auto drawn = drawnTypes_.find(o.id); drawn != drawnTypes_.end()) {
            const auto drawnModel = drawnModels_.find(o.id);
            const bool sameModel = drawnModel != drawnModels_.end() ? drawnModel->second == o.model : o.model.empty();
            if (drawn->second == o.definition.guid() && sameModel) {
                return false;
            }
            undraw(o.id);
        }
        if (created_.size() >= kMaxCreated) {
            ts2::diag::warning(std::format("object {} is not drawn: {} created objects already are", o.id, kMaxCreated));
            return false;
        }
        const std::string model = modelName(assets_, o);
        ts2::scene::Instance instance;
        instance.mesh = model.empty() ? -1 : objectMaterials_.model(model, scene);
        instance.name = std::format("{} {}", o.id, model);
        instance.part = ts2::scene::Part::Object;
        instance.object = static_cast<std::uint32_t>(o.id);
        instance.transform.m.fill(0.0f);
        // Its shadow is what it asks for until it is first placed (see shadeCreated).
        instance.shadowFacts = shadowFacts(o, model, instance.mesh, scene);
        instance.shadow = ts2::scene::objectshadow::requested(instance.shadowFacts);
        std::size_t index = scene.instances.size();
        if (!freeInstances_.empty()) {
            index = freeInstances_.back();
            freeInstances_.pop_back();
            scene.instances[index] = std::move(instance);
        } else {
            scene.instances.push_back(std::move(instance));
        }
        ts2::scene::PlacedObject placed{static_cast<std::uint32_t>(o.id), index, lotFrame_, {}};
        placed.model = model;
        for (std::size_t slot = 0; slot < kMaxSlots && !model.empty(); ++slot) {
            const std::string bone = assets_.slotBone(o.definition.guid(), slot);
            const ts2::scene::Transform* at = bone.empty() ? nullptr : objectMaterials_.bone(model, bone);
            placed.slots.push_back(at ? std::optional<ts2::scene::Transform>(*at) : std::nullopt);
        }
        while (!placed.slots.empty() && !placed.slots.back()) {
            placed.slots.pop_back();
        }
        scene.placedObjects.push_back(std::move(placed));
        created_[o.id] = index;
        unshaded_.insert(index);
        drawnTypes_[o.id] = o.definition.guid();
        drawnModels_[o.id] = o.model;
        // Never placed yet, so placeObjects places it.
        const auto slot = static_cast<std::size_t>(o.id);
        if (slot >= objectPlaces_.size()) {
            objectPlaces_.resize(slot + 1);
        }
        objectPlaces_[slot] = {ObjectPlace{.gone = true}, true};
        changedInstances_.push_back(index);
        return false;
    });
    createdFor_ = world.objectsVersion();
}

} // namespace app
