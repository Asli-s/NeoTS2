#include "sim/world.hpp"

#include <algorithm>
#include <climits>
#include <stdexcept>
#include <string>

namespace ts2::sim {

SimObject& World::create(std::uint32_t group, objd::ObjectDefinition definition, std::size_t containerSlots) {
    if (nextId_ <= 0) {
        throw std::runtime_error("World: out of object ids");
    }
    auto object = std::make_unique<SimObject>();
    object->id = nextId_++;
    object->group = group;
    object->definition = std::move(definition);
    object->data[kObjectIdField] = object->id;
    object->data[kLeadTileField] = object->id;
    object->arrays.resize(object->definition.field(kObjectArrayCountField));
    object->arrayIterators.resize(object->arrays.size(), 0);
    object->slots.resize(containerSlots, 0);

    SimObject& ref = *object;
    objects_.emplace(ref.id, std::move(object));
    byId_[static_cast<std::size_t>(ref.id)] = &ref;
    objectsChanged();
    footprintChanges_.insert(ref.id);
    return ref;
}

SimObject& World::restore(std::int16_t id, std::uint32_t group, objd::ObjectDefinition definition) {
    if (id <= 0 || objects_.contains(id)) {
        throw std::runtime_error("World: object id " + std::to_string(id) + " is not free");
    }
    auto object = std::make_unique<SimObject>();
    object->id = id;
    object->group = group;
    object->definition = std::move(definition);
    object->arrays.resize(object->definition.field(kObjectArrayCountField));
    object->arrayIterators.resize(object->arrays.size(), 0);

    SimObject& ref = *object;
    objects_.emplace(id, std::move(object));
    byId_[static_cast<std::size_t>(id)] = &ref;
    objectsChanged();
    footprintChanges_.insert(id);
    if (id >= nextId_) {
        nextId_ = static_cast<std::int16_t>(id + 1);
    }
    return ref;
}

SimObject* World::find(std::int16_t id) {
    SimObject* o = findIncludingRemoved(id);
    return o != nullptr && !o->removed ? o : nullptr;
}

const SimObject* World::find(std::int16_t id) const {
    const SimObject* o = id > 0 ? byId_[static_cast<std::size_t>(id)] : nullptr;
    return o != nullptr && !o->removed ? o : nullptr;
}

SimObject* World::findIncludingRemoved(std::int16_t id) {
    return id > 0 ? byId_[static_cast<std::size_t>(id)] : nullptr;
}

void World::remove(SimObject& object) {
    if (!object.removed) {
        object.removed = true;
        objectsChanged();
        ++removedCount_;
        footprintChanges_.insert(object.id);
    }
}

void World::updateRoom(SimObject& object) const {
    constexpr std::size_t kRoomField = 0x1D;
    const std::int32_t room = rooms_.roomAtPosition(object.level - 1, static_cast<float>(object.x) / 65536.0f,
                                            static_cast<float>(object.y) / 65536.0f);
    if (room != RoomMap::kNoRoom) {
        object.data[kRoomField] = static_cast<std::int16_t>(room);
    }
}

void World::markFootprintChanged(std::int16_t id) {
    constexpr std::size_t kLeadField = 0x13;
    footprintChanges_.insert(id);
    const SimObject* object = find(id);
    if (object == nullptr || object->definition.field(objd::field::kMultiTileMasterId) == 0) {
        return;
    }
    const std::int16_t lead = object->data[kLeadField];
    if (lead <= 0) {
        return;
    }
    if (find(lead) != nullptr) {
        footprintChanges_.insert(lead);
    }
    for (const auto& [otherId, other] : objects_) {
        if (!other->removed && other->definition.field(objd::field::kMultiTileMasterId) != 0 &&
            other->data[kLeadField] == lead) {
            footprintChanges_.insert(otherId);
        }
    }
}

void World::countUse(const Frame& interaction) {
    constexpr std::size_t kUseCountField = 0x3E;
    if (SimObject* object = findIncludingRemoved(interaction.runningOnId)) {
        ++object->data[kUseCountField];
    }
}

void World::releaseUse(const Frame& frame) {
    constexpr std::size_t kUseCountField = 0x3E;
    if (!frame.runsInteraction) {
        return;
    }
    if (SimObject* object = findIncludingRemoved(frame.runningOnId)) {
        --object->data[kUseCountField];
    }
}

void World::releaseUses(const std::vector<Frame>& frames) {
    for (const Frame& f : frames) {
        releaseUse(f);
    }
}

std::vector<std::int16_t> World::removedIds() const {
    std::vector<std::int16_t> ids;
    if (removedCount_ == 0) {
        return ids;
    }
    for (const auto& [id, object] : objects_) {
        if (object->removed) {
            ids.push_back(id);
        }
    }
    return ids;
}

void World::purge() {
    // A relationship can only lose one of its objects when an object was removed or when it is new.
    if (removedCount_ == 0 && !relationshipAdded_) {
        return;
    }
    relationshipAdded_ = false;
    // A deleted object leaves the slot it was in (object data 2 and 3 name the container and slot).
    constexpr std::size_t kContainerField = 2, kSlotField = 3;
    for (const auto& [id, object] : objects_) {
        if (!object->removed || object->data[kContainerField] <= 0) {
            continue;
        }
        if (SimObject* container = findIncludingRemoved(object->data[kContainerField])) {
            const auto slot = static_cast<std::size_t>(object->data[kSlotField]);
            if (object->data[kSlotField] >= 0 && slot < container->slots.size() && container->slots[slot] == id) {
                container->slots[slot] = 0;
            }
        }
        object->data[kContainerField] = 0;
        object->data[kSlotField] = -1;
    }
    std::erase_if(objects_, [this](const auto& entry) {
        if (entry.second->removed) {
            byId_[static_cast<std::size_t>(entry.first)] = nullptr;
            return true;
        }
        return false;
    });
    std::erase_if(relationships_, [this](const auto& entry) {
        return findIncludingRemoved(entry.first.first) == nullptr || findIncludingRemoved(entry.first.second) == nullptr;
    });
    removedCount_ = 0;
}

// How many other neighbors are friends with neighbor `id` both ways and not its family: in a family
// (person data 0x3D), without person data 0x44 bit 0, no family tie (relationship 3) either way, and
// each holding the other as a friend, best friend (relationship 1 bits 4, 5) or BFF (9 bit 0). The
// game also skips neighbors of one kind not identified yet.
// A neighbor's person data as changed on this lot, else as saved, null for an unknown neighbor.
const std::vector<std::int16_t>* World::livePersonData(std::int16_t id) const {
    if (const auto it = neighborPersonData_.find(id); it != neighborPersonData_.end()) {
        return &it->second;
    }
    const SavedNeighbor* saved = neighbor(id);
    return saved ? &saved->personData : nullptr;
}

// Friends as the neighbor fields count them: no relationship value 3 either way, and each a friend
// of the other.
bool World::neighborsAreFriends(std::int16_t a, std::int16_t b) const {
    const auto friendOf = [&](std::int16_t from, std::int16_t to) {
        return (neighborRelationshipValue(from, to, 1) & 0x30) != 0 || (neighborRelationshipValue(from, to, 9) & 0x01) != 0;
    };
    if (neighborRelationshipValue(a, b, 3) != 0 || neighborRelationshipValue(b, a, 3) != 0) {
        return false;
    }
    return friendOf(a, b) && friendOf(b, a);
}

// Neighbors outside the family, with a family of their own, who are friends of any of its members.
std::int16_t World::familyFriends(std::uint32_t family) const {
    constexpr std::size_t kFamilyPersonData = 0x3D;
    if (!hood_) {
        return 0;
    }
    const auto members = hood_->familyMembers.find(family);
    if (members == hood_->familyMembers.end()) {
        return 0;
    }
    const auto isMember = [&](std::uint32_t guid) {
        return std::find(members->second.begin(), members->second.end(), guid) != members->second.end();
    };
    // Each member as a neighbor: the first neighbor of that GUID.
    std::vector<std::int16_t> memberIds;
    for (const std::uint32_t guid : members->second) {
        for (const auto& [id, n] : hood_->neighbors) {
            if (n.guid == guid) {
                memberIds.push_back(id);
                break;
            }
        }
    }
    std::int16_t count = 0;
    for (const auto& [id, n] : hood_->neighbors) {
        const auto* data = livePersonData(id);
        if (isMember(n.guid) || data == nullptr || data->size() <= kFamilyPersonData || (*data)[kFamilyPersonData] == 0) {
            continue;
        }
        if (std::any_of(memberIds.begin(), memberIds.end(), [&](std::int16_t m) { return neighborsAreFriends(id, m); })) {
            ++count;
        }
    }
    return count;
}

std::int16_t World::mutualFriends(std::int16_t id) const {
    constexpr std::size_t kFamilyPersonData = 0x3D, kFlagsPersonData = 0x44;
    const auto* ids = neighbors();
    if (ids == nullptr) {
        return 0;
    }
    std::int16_t count = 0;
    for (const std::int16_t other : *ids) {
        const auto* data = livePersonData(other);
        if (other == id || data == nullptr || data->size() <= kFlagsPersonData) {
            continue;
        }
        if ((*data)[kFamilyPersonData] == 0 || ((*data)[kFlagsPersonData] & 1) != 0) {
            continue;
        }
        if (neighborsAreFriends(other, id)) {
            ++count;
        }
    }
    return count;
}

// The game's rule: the first free id from the counter on, or, when that is past the number of
// neighbors, the first free id from 1. The counter then moves past the new id.
std::optional<std::int16_t> World::addNeighbor(SavedNeighbor neighbor) {
    if (!hood_) {
        return std::nullopt;
    }
    const std::set<std::int16_t>& ids = hood_->neighborIds;
    auto firstFree = [&](int from) {
        int id = std::max(from, 1);
        while (id <= INT16_MAX && ids.contains(static_cast<std::int16_t>(id))) {
            ++id;
        }
        return id;
    };
    int id = firstFree(nextNeighborId_);
    if (id > static_cast<int>(ids.size()) && id != nextNeighborId_) {
        id = firstFree(1);
        if (id >= nextNeighborId_) {
            nextNeighborId_ = static_cast<std::int16_t>(std::min(id + 1, int{INT16_MAX}));
        }
    } else {
        nextNeighborId_ = static_cast<std::int16_t>(std::min(id + 1, int{INT16_MAX}));
    }
    if (id > INT16_MAX) {
        return std::nullopt;
    }
    const auto newId = static_cast<std::int16_t>(id);
    SavedNeighborhood& hood = ownNeighborhood();
    hood.neighborIds.insert(newId);
    hood.neighbors[newId] = std::move(neighbor);
    return newId;
}

void World::addFamily(std::uint32_t family) {
    if (hood_ && !hood_->familyLots.contains(family)) {
        ownNeighborhood().familyLots[family] = 0;
    }
}

// The saved neighborhood is shared, so the first change makes this world its own copy.
SavedNeighborhood& World::ownNeighborhood() {
    if (!ownHood_) {
        ownHood_ = std::make_shared<SavedNeighborhood>(*hood_);
        hood_ = ownHood_;
    }
    return *ownHood_;
}

std::optional<std::int16_t> World::neighborData(std::int16_t neighborId, std::uint16_t field) const {
    const SavedNeighbor* n = neighbor(neighborId);
    if (n == nullptr) {
        return std::int16_t{0};
    }
    switch (field) {
    case 0: {
        const auto [cached, added] = simOfNeighbor_.try_emplace(n->guid, std::int16_t{0});
        if (added) {
            for (const auto& [id, o] : objects_) {
                if (!o->removed && o->isPerson() && o->definition.guid() == n->guid) {
                    cached->second = id;
                    break;
                }
            }
        }
        return cached->second;
    }
    case 1: {
        // Whether the neighbor belongs to the family living on this lot.
        const auto number = lotNumber();
        if (!number || !hood_) {
            return std::int16_t{0};
        }
        for (const auto& [family, lot] : hood_->familyLots) {
            if (lot == *number) {
                const auto members = hood_->familyMembers.find(family);
                return static_cast<std::int16_t>(members != hood_->familyMembers.end() &&
                                                 std::find(members->second.begin(), members->second.end(), n->guid) !=
                                                     members->second.end());
            }
        }
        return std::int16_t{0};
    }
    case 2:
        return n->personData.size() > kLifeStagePersonData ? n->personData[kLifeStagePersonData] : std::int16_t{0};
    case 3:
    case 4:
        return std::int16_t{0};
    case 5:
        return mutualFriends(neighborId);
    case 6:
    case 7:
    case 8:
    case 9:
    case 10: {
        // From the neighbor's family (person data 0x3D): 6 its lot ("house number"). 9 its friends.
        // 7, 8 and 10 bits 0, 1 and 3 of its saved flags, only while it has a lot. 0 without a family.
        constexpr std::size_t kFamilyPersonData = 0x3D;
        const auto live = neighborPersonData_.find(neighborId);
        const std::vector<std::int16_t>& data = live != neighborPersonData_.end() ? live->second : n->personData;
        if (data.size() <= kFamilyPersonData || !hood_) {
            return std::int16_t{0};
        }
        const auto familyId = static_cast<std::uint16_t>(data[kFamilyPersonData]);
        const auto lot = hood_->familyLots.find(familyId);
        if (lot == hood_->familyLots.end()) {
            return std::int16_t{0};
        }
        if (field == 6) {
            return static_cast<std::int16_t>(lot->second);
        }
        if (field == 9) {
            return familyId == 0 ? std::int16_t{0} : familyFriends(familyId);
        }
        if (familyId == 0 || lot->second == 0) {
            return std::int16_t{0};
        }
        const auto flags = hood_->familyFlags.find(familyId);
        const int bit = field == 7 ? 0 : field == 8 ? 1 : 3;
        return static_cast<std::int16_t>(flags != hood_->familyFlags.end() ? (flags->second >> bit) & 1 : 0);
    }
    default:
        return std::nullopt;
    }
}

} // namespace ts2::sim
