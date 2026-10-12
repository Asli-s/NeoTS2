#include "sim/lot_world.hpp"

#include "sim/type_signature.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>

namespace ts2::sim {

namespace {

std::string hex(std::uint32_t value) {
    char buffer[11];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
    return buffer;
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
        object.x = static_cast<std::int32_t>(state.header.location[0] * 65536.0f);
        object.y = static_cast<std::int32_t>(state.header.location[1] * 65536.0f);
        object.level = state.header.value1;
        if (const auto& h = state.header;
            h.location[0] >= 0 && h.location[0] < 256 && h.location[1] >= 0 && h.location[1] < 256) {
            object.tile = Tile{static_cast<std::uint8_t>(h.location[0]), static_cast<std::uint8_t>(h.location[1]),
                               static_cast<std::uint8_t>(h.value1)};
        }
        object.thread.temps = state.temps;
        object.thread.codeGroup = type->group;
        if (state.tail) {
            object.arrays = state.tail->arrays;
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
        if (state.person) {
            object.motives = state.person->motives;
            copyInto(object.personData, state.person->personData);
        }
        if (auto problem = restoreStack(lot, saved, object, code)) {
            report.problems.push_back({saved.id, *problem});
        }
        // An object whose type changed since the save (a patch, expansion or mod) will start over. Sims
        // are left as saved for now (their reset also refills their person data).
        if (!state.person && saved.type && typeChangedSinceSave(code, *type, *saved.type)) {
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
    return report;
}

std::shared_ptr<const SavedNeighborhood> savedNeighborhood(const content::Neighborhood& hood) {
    auto saved = std::make_shared<SavedNeighborhood>();
    saved->neighborIds = hood.neighborIds;
    saved->lotIds = hood.lotIds;
    if (hood.record) {
        saved->record = std::make_shared<const ngbh::Neighborhood>(*hood.record);
    }
    for (const auto& [id, tables] : hood.dataTables) {
        saved->dataTables[id] = toDataTables(tables);
    }
    for (const auto& [id, n] : hood.neighbors) {
        saved->neighbors[id] = SavedNeighbor{n.guid, n.personData};
    }
    for (const auto& [id, w] : hood.wants) {
        saved->wants[id] = w.wants;
        saved->fears[id] = w.fears;
    }
    for (const auto& [pair, values] : hood.relationships) {
        // The game stores 32-bit values but the Relationship primitive moves 16-bit ones.
        auto& stored = saved->relationships[pair];
        stored.resize(values.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            stored[i] = static_cast<std::int16_t>(values[i]);
        }
    }
    return saved;
}

} // namespace ts2::sim
