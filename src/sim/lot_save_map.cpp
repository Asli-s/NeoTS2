#include "sim/lot_save_detail.hpp"
#include "sim/lot_simulation.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ts2::sim {

using namespace lot_save_detail;

const SimObject* continuingObject(const World& world, const objt::ObjectTypeTable& types, std::int32_t id,
                                  std::int32_t typeIndex) {
    constexpr std::int32_t kLastId = 0x7FFF;
    if (id <= 0 || id > kLastId) {
        return nullptr;
    }
    const SimObject* object = world.find(static_cast<std::int16_t>(id));
    const objt::Entry* entry = types.find(static_cast<std::uint16_t>(typeIndex));
    return object != nullptr && entry != nullptr && object->definition.guid() == entry->guid ? object : nullptr;
}

objm::MapRecord objectMapRecord(const objm::MapRecord& loaded, const objt::ObjectTypeTable& types, const World& world,
                                const std::unordered_set<std::int32_t>* recorded) {
    objm::MapRecord record = loaded;
    std::unordered_set<std::int32_t> listed;
    std::vector<std::pair<std::int32_t, std::int32_t>> kept;
    for (const auto& [id, type] : loaded.objects) {
        if (continuingObject(world, types, id, type) != nullptr) {
            kept.emplace_back(id, type);
            listed.insert(id);
        }
    }
    // An object the record doesn't list yet is one made since the lot was saved. The game puts each
    // new object at the front of its list as it is made, so the newest stands first.
    std::vector<std::pair<std::int32_t, std::int32_t>> added;
    // The wait table runs by object id from 1. Each entry keeps in its low half the ticks the
    // object's script still sleeps, and in its high half the object's run flags: 1 its script runs,
    // 2 it runs while the game is paused, 4 its script hit an error, 8 it advertises interactions.
    // An id with no object holds 0, so the table starts empty and each object fills its own entry.
    std::vector<std::uint32_t> waits(loaded.waits.size(), 0);
    std::int32_t waitsWithObject = 0;
    world.nextAfter(0, [&](const SimObject& object) {
        const auto id = static_cast<std::int32_t>(object.id);
        if (!listed.contains(id)) {
            const auto type = typeIndexOf(types, object);
            // An object whose type the lot's table does not hold is not listed or saved, so like an
            // id with no object it has no entry in the wait table.
            if (!type || (recorded != nullptr && !recorded->contains(id))) {
                return false;
            }
            added.emplace_back(id, *type);
        }
        // Every listed object has an entry, so the table grows to the highest id. Ids are 16-bit
        // and positive, which bounds it.
        const auto at = static_cast<std::size_t>(object.id) - 1;
        if (object.id <= 0) {
            return false;
        }
        if (at >= waits.size()) {
            waits.resize(at + 1, 0);
        }
        // An object new at this id has nothing loaded of its own here, an old object's entry being
        // another object's. It gets what the game gives an object it makes: an empty count (0xFFFF)
        // and run flag 1, without which the game never runs the object's script after a load. Flag 2
        // (object data 0x3C, runs while paused) is set as the game sets it. The game works out flags
        // 2 and 8 again for every object after a load, so flag 8 is left to it.
        constexpr std::uint32_t kEmptyCount = 0xFFFF, kRunFlag = 0x10000, kPausedRunFlag = 0x20000;
        constexpr std::size_t kRunsWhilePaused = 0x3C;
        const std::uint32_t made =
            kEmptyCount | kRunFlag | (object.data.size() > kRunsWhilePaused && object.data[kRunsWhilePaused] != 0 ? kPausedRunFlag : 0);
        const std::uint32_t was = listed.contains(id) && at < loaded.waits.size() ? loaded.waits[at] : made;
        // Only a script counting down a Sleep gets its ticks written. What an object that isn't
        // sleeping keeps here isn't known yet, so its loaded value stays.
        waits[at] = object.thread.sleepTicks > 0
                        ? (was & 0xFFFF0000U) | static_cast<std::uint16_t>(object.thread.sleepTicks - 1)
                        : was;
        ++waitsWithObject;
        return false;
    });
    std::sort(added.begin(), added.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    added.insert(added.end(), kept.begin(), kept.end());
    record.objects = std::move(added);
    record.waits = std::move(waits);
    record.waitsWithObject = waitsWithObject;
    // A map read at the shipped version saved no subscriptions, so every one the lot holds now was
    // made by its scripts since it loaded. Only an object the map lists keeps its subscriptions, as
    // one left out of the save would leave a message naming an id with no object. The order the
    // game lists them in is not known yet.
    if (loaded.upgraded) {
        std::unordered_set<std::int32_t> saved;
        for (const auto& [id, type] : record.objects) {
            saved.insert(id);
        }
        record.subscriptions.clear();
        for (const auto& [message, subscribers] : world.messageSubscriptions().all()) {
            objm::Subscription out;
            out.message = message;
            for (const auto& [object, tree] : subscribers) {
                if (saved.contains(object)) {
                    out.subscribers.push_back({object, tree});
                }
            }
            if (!out.subscribers.empty()) {
                record.subscriptions.push_back(std::move(out));
            }
        }
        record.upgraded = false;
    }
    return record;
}

objm::MapRecord objectMapRecord(const objm::MapRecord& loaded, const objt::ObjectTypeTable& types, LotSimulation& simulation) {
    return objectMapRecord(loaded, types, simulation.world());
}

namespace lot_save_detail {

void checkListedRecords(const dbpf::PackageImage& package, const objm::MapRecord& map, const objt::ObjectTypeTable& types,
                        const std::unordered_map<std::int32_t, std::int32_t>& loadedTypes, bool upgrading) {
    std::unordered_map<std::uint64_t, const dbpf::StoredResource*> byKey;
    for (const dbpf::StoredResource& resource : package.resources) {
        byKey.emplace(static_cast<std::uint64_t>(resource.key.type) << 32 | resource.key.instance, &resource);
    }
    for (const auto& [id, typeIndex] : map.objects) {
        if (!upgrading && loadedTypes.contains(id)) {
            continue;
        }
        const objt::Entry* entry = types.find(static_cast<std::uint16_t>(typeIndex));
        if (entry == nullptr) {
            throw std::runtime_error("the save would list object " + std::to_string(id) + " with a type the lot does not hold");
        }
        const std::uint32_t type = wantedType(*entry);
        const auto found = byKey.find(static_cast<std::uint64_t>(type) << 32 | static_cast<std::uint32_t>(id));
        if (found == byKey.end()) {
            throw std::runtime_error("the save would list object " + std::to_string(id) + " without a record");
        }
        try {
            (void)xobj::parse(bodyOf(*found->second), xobj::kWrittenVersion, *resourceKind(type));
        } catch (const std::exception& ex) {
            throw std::runtime_error("the save would list object " + std::to_string(id) +
                                     " with a record that does not read: " + ex.what());
        }
    }
}

} // namespace lot_save_detail

} // namespace ts2::sim
