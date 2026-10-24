#include "sim/lot_world.hpp"

#include "sim/type_signature.hpp"

#include "content/animation_lists.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>

namespace ts2::sim {

namespace {

// Whether a saved position fits the engine's 16.16 fixed point.
bool fitsFixedPoint(float v) { return std::isfinite(v) && std::fabs(v) < 32767.0f; }

std::string hex(std::uint32_t value) {
    char buffer[11];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
    return buffer;
}

const ObjectType* originalType(CodeSource& code, const ObjectType& type) {
    constexpr std::size_t kOriginalGuidLow = 0x46, kOriginalGuidHigh = 0x47;
    const std::uint32_t original = type.definition.field(kOriginalGuidLow) |
                                   static_cast<std::uint32_t>(type.definition.field(kOriginalGuidHigh)) << 16;
    return original != 0 ? code.objectType(original) : nullptr;
}

template <std::size_t N>
bool copyInto(std::array<std::int16_t, N>& to, const std::vector<std::int16_t>& from) {
    std::copy_n(from.begin(), std::min(N, from.size()), to.begin());
    return from.size() <= N;
}

std::optional<std::string> restoreStack(const content::Lot& lot, const content::LotObject& saved,
                                        SimObject& object, CodeSource& code) {
    if (!saved.state.tail) {
        return std::nullopt;
    }
    std::vector<Frame> frames;
    for (const auto& s : saved.state.tail->stack) {
        const auto* ownerEntry = lot.types.find(static_cast<std::uint16_t>(s.codeOwner));
        const auto* owner = ownerEntry ? code.objectType(ownerEntry->guid) : nullptr;
        if (!owner) {
            return "stack frame's code owner has no definition";
        }
        const auto tree = static_cast<std::uint16_t>(s.treeId);
        const std::uint32_t group = tree < 0x1000   ? kGlobalGroup
                                    : tree < 0x2000 ? owner->group
                                                    : code.semiGlobalGroup(owner->group);
        auto bhav = code.behavior(group, tree);
        if (!bhav) {
            return "stack frame's tree not found: " + hex(group) + " " + std::to_string(tree);
        }
        if (s.node >= bhav->instructions.size()) {
            return "stack frame's node is past the end of its tree";
        }
        Frame f;
        f.group = group;
        f.codeOwner = owner->group;
        f.instance = tree;
        f.pc = s.node;
        f.params = s.params;
        f.params.resize(std::max<std::size_t>({4, bhav->argCount, f.params.size()}), 0);
        f.locals = s.locals;
        f.locals.resize(std::max<std::size_t>(bhav->localCount, f.locals.size()), 0);
        f.stackObjectId = s.objectId;
        f.runningOnId = s.value2 != 0 ? s.value2 : static_cast<std::int16_t>(object.id);
        f.code = std::move(bhav);
        frames.push_back(std::move(f));
    }
    if (!frames.empty()) {
        object.thread.frames = std::move(frames);
        object.thread.stackObjectId = saved.state.tail->stack.front().objectId;
    }
    return std::nullopt;
}

// The overlays an object was playing when the lot was saved, carrying on from where they were. Only
// playing ("state 1") overlays come back. The game's other saved animations (an object's own
// animation, attached ones) are not restored yet.
void restoreOverlays(const content::Lot& lot, std::uint32_t id, SimObject& object, CodeSource& code, double nowMs) {
    const auto saved = lot.savedAnimations.find(id);
    if (saved == lot.savedAnimations.end()) {
        return;
    }
    constexpr std::array<int, 5> kPriorityLayers{1, 3, 5, 7, 0};
    for (const auto& a : saved->second) {
        const auto layer = std::find(kPriorityLayers.begin(), kPriorityLayers.end(), a.layer);
        if (a.kind != cobject::SavedAnimation::Kind::Clip || a.state != 1 || layer == kPriorityLayers.end() ||
            a.type.size() != 3 || a.type[0] != 'o') {
            continue;
        }
        PlayingOverlay o;
        o.name = a.name;
        // Saved values out of range are read as Animate Overlay reads its own (a weight outside 0-1 is 1).
        const double played = a.startedMsAgo;
        o.startMs = nowMs - played;
        o.lengthMs = a.lengthMs && !std::isnan(*a.lengthMs) ? std::max(*a.lengthMs, 0.0f) : 0.0f;
        o.plays = static_cast<std::int16_t>(std::clamp(a.plays, -1, 0x7FFF));
        o.weight = a.weight >= 0 && a.weight <= 1 ? a.weight : 1.0f;
        o.priority = static_cast<std::uint8_t>(layer - kPriorityLayers.begin());
        // The saved kind lets Animate Stop by kind stop it, so a saved idle stops when the Sim walks.
        if (a.category && *a.category >= 0 && *a.category <= content::animation_category::kReceptivity) {
            o.category = static_cast<std::uint8_t>(*a.category);
        }
        o.eventTree = a.eventTree;
        o.stackObject = a.eventObject;
        if (const ObjectType* owner = code.objectType(a.eventObjectGuid)) {
            o.codeOwner = owner->group;
        }
        o.blendingOut = o.plays >= 0 && played >= o.lengthMs - kOverlayBlendMs;
        object.overlays.push_back(std::move(o));
    }
}

} // namespace

LotWorldReport buildWorld(const content::Lot& lot, CodeSource& code, World& world) {
    LotWorldReport report;
    RoomMap rooms;
    if (const auto saved = lot.grids.find(content::kRoomGridInstance); saved != lot.grids.end()) {
        rooms = RoomMap(saved->second, lot.roomGraph ? lot.roomGraph->firstLevel : 0);
    } else if (lot.roomGraph) {
        rooms = RoomMap(*lot.roomGraph);
    }
    for (int layer = 0; layer < 2; ++layer) {
        if (const auto graph = lot.layerGraphs.find(content::kLayerGraphInstances[layer]);
            graph != lot.layerGraphs.end()) {
            rooms.assignLayer(layer, RoomMap(graph->second));
        }
    }
    world.setRooms(std::move(rooms));
    for (const auto& saved : lot.objects) {
        const ObjectType* type = saved.type ? code.objectType(saved.type->guid) : nullptr;
        if (!type) {
            report.problems.push_back(
                {saved.id, "no definition for " + (saved.type ? hex(saved.type->guid) + " " + saved.type->name
                                                               : std::string("an untyped object"))});
            continue;
        }
        if (saved.id == 0 || saved.id > 0x7FFF) {
            report.problems.push_back({saved.id, "object id out of range"});
            continue;
        }
        SimObject& object = world.restore(static_cast<std::int16_t>(saved.id), type->group, type->definition);
        const auto& state = saved.state;
        if (!copyInto(object.attributes, state.attributes)) {
            report.problems.push_back({saved.id, "more saved attributes than the engine keeps"});
        }
        if (!copyInto(object.semiAttributes, state.semiAttributes)) {
            report.problems.push_back({saved.id, "more saved semi-attributes than the engine keeps"});
        }
        std::copy(state.data.begin(), state.data.end(), object.data.begin());
        if (fitsFixedPoint(state.header.location[0]) && fitsFixedPoint(state.header.location[1])) {
            object.x = static_cast<std::int32_t>(state.header.location[0] * 65536.0f);
            object.y = static_cast<std::int32_t>(state.header.location[1] * 65536.0f);
        } else {
            report.problems.push_back({saved.id, "position out of range"});
        }
        object.level = state.header.value1;
        if (const auto& h = state.header;
            h.location[0] >= 0 && h.location[0] < 256 && h.location[1] >= 0 && h.location[1] < 256) {
            object.tile = Tile{static_cast<std::uint8_t>(h.location[0]), static_cast<std::uint8_t>(h.location[1]),
                               static_cast<std::uint8_t>(h.value1)};
        }
        object.thread.temps = state.temps;
        const auto& s = state.tokenSource;
        object.tokenSource = {s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]};
        object.thread.codeGroup = type->group;
        if (state.tail) {
            object.arrays = state.tail->arrays;
            object.materialNames = state.tail->strings;
            const auto& t = state.tail->token;
            object.token = InventoryToken{t.guid, t.flags, t.value1, t.value2, t.value3, t.properties};
            object.arrayIterators = state.tail->shorts;
            object.arrayIterators.resize(object.arrays.size(), 0);
            // The saved slots (one object id per container slot) win over the SLOT resource.
            object.slots = state.tail->slots.marker < 0 ? state.tail->slots.values
                                                        : std::vector<std::int16_t>(type->containerSlots, 0);
        } else {
            object.slots.assign(type->containerSlots, 0);
        }
        // A Sim's slots follow the person it was made from (its original GUID), which newer packs give
        // more slots than the Sim's own copy has.
        if (const ObjectType* original = object.isPerson() ? originalType(code, *type) : nullptr;
            original != nullptr && object.slots.size() < original->containerSlots) {
            object.slots.resize(original->containerSlots, 0);
        }
        if (state.person) {
            object.motives = state.person->motives;
            // A broken save can hold any float: levels that aren't numbers start at 0, others are kept
            // to the motive range, as every write to them is.
            for (float& level : object.motives) {
                level = std::isfinite(level) ? std::clamp(level, kMotiveMin, kMotiveMax) : 0.0f;
            }
            copyInto(object.personData, state.person->personData);
        }
        if (auto problem = restoreStack(lot, saved, object, code)) {
            report.problems.push_back({saved.id, *problem});
        }
        restoreOverlays(lot, saved.id, object, code, world.clockMs());
        // An object or Sim whose type changed since the save (a patch, expansion or mod) will start over.
        if (saved.type && typeChangedSinceSave(code, *type, *saved.type)) {
            report.changedType.push_back(object.id);
        }
        ++report.objects;
        report.threads += object.thread.isRunning();
    }
    for (const auto& [id, tables] : lot.dataTables) {
        SimObject* object = world.find(static_cast<std::int16_t>(id));
        if (object == nullptr) {
            ++report.staleDataTables;
            continue;
        }
        object->dataTables = toDataTables(tables);
    }
    for (const auto& [pair, values] : lot.relationships) {
        if (world.find(pair.first) == nullptr || world.find(pair.second) == nullptr) {
            continue;
        }
        // The game stores 32-bit values but the Relationship primitive moves 16-bit ones.
        auto& stored = world.relationship(pair.first, pair.second);
        stored.resize(values.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            stored[i] = static_cast<std::int16_t>(values[i]);
        }
    }
    return report;
}

std::shared_ptr<const SavedNeighborhood> savedNeighborhood(const content::Neighborhood& hood,
                                                          std::map<std::int32_t, nhoodid::NeighborhoodId> all) {
    auto saved = std::make_shared<SavedNeighborhood>();
    if (hood.identity) {
        saved->id = hood.identity->id;
        all.try_emplace(hood.identity->id, *hood.identity);
    }
    saved->allNeighborhoods = std::make_shared<const std::map<std::int32_t, nhoodid::NeighborhoodId>>(std::move(all));
    saved->neighborIds = hood.neighborIds;
    saved->lotIds = hood.lotIds;
    saved->lotTypes = hood.lotTypes;
    saved->lotOwners = hood.lotOwners;
    for (const auto& [instance, family] : hood.families) {
        saved->familyLots[instance] = family.lot;
        saved->familyFlags[instance] = family.flags;
        saved->familyMembers[instance] = family.members;
    }
    if (hood.record) {
        saved->record = std::make_shared<const ngbh::Neighborhood>(*hood.record);
    }
    for (const auto& [id, tables] : hood.dataTables) {
        saved->dataTables[id] = toDataTables(tables);
    }
    for (const auto& [id, n] : hood.neighbors) {
        saved->neighbors[id] = SavedNeighbor{n.guid, n.personData};
    }
    saved->familyTies = hood.familyTies;
    for (const auto& [id, w] : hood.wants) {
        const auto slots = swaf::slots(w);
        saved->wantSlots[id] = slots;
        saved->wants[id].assign(w.wants.begin(), w.wants.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(w.wants.size(), slots.wants)));
        saved->fears[id].assign(w.fears.begin(), w.fears.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(w.fears.size(), slots.fears)));
    }
    saved->relationships = hood.relationships;
    return saved;
}

} // namespace ts2::sim
