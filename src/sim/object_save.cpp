#include "sim/object_save.hpp"

#include "formats/cobject.hpp"
#include "formats/objm.hpp"
#include "formats/resource_types.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>
#include <set>
#include <stdexcept>

namespace ts2::sim {

namespace {

constexpr std::size_t kInUse = 1, kData3CFlag = 2;
constexpr std::size_t kData3C = 0x3C, kFacingData = 1, kContainerData = 2;
constexpr std::uint32_t kGroup = 0xFFFFFFFF;

struct Saved {
    dbpf::ResourceKey key;
    std::vector<std::uint8_t> bytes;
};

void putFloats(std::vector<std::uint8_t>& bytes, std::size_t at, const std::array<float, 7>& floats) {
    if (at == 0 || at + 7 * 4 > bytes.size()) {
        throw std::runtime_error("scene record too short for its place");
    }
    for (std::size_t i = 0; i < 7; ++i) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &floats[i], 4);
        for (std::size_t k = 0; k < 4; ++k) {
            bytes[at + 4 * i + k] = static_cast<std::uint8_t>(bits >> (8 * k));
        }
    }
}

// The scene record with the object's place in it, both cObject's and cAnimatable's.
std::vector<std::uint8_t> placed(std::vector<std::uint8_t> bytes, const cobject::SavedObject& parsed, const std::array<float, 7>& own,
                                 const std::array<float, 7>& world) {
    if (parsed.transform) {
        putFloats(bytes, parsed.transformOffset, own);
    }
    if (parsed.animatableTransform) {
        putFloats(bytes, parsed.animatableTransformOffset, world);
    }
    return bytes;
}

std::uint16_t waitFlags(std::uint16_t flags, const xobj::SavedObject& state) {
    flags = static_cast<std::uint16_t>((flags & ~kData3CFlag) | kInUse);
    if ((state.data[kData3C] & 1) != 0) {
        flags = static_cast<std::uint16_t>(flags | kData3CFlag);
    }
    return flags;
}

xobj::RecordKind kindOf(std::uint32_t type) {
    return type == types::kSavedObjectWithMaterial ? xobj::RecordKind::ObjectWithMaterial
           : type == types::kSavedPerson           ? xobj::RecordKind::Person
                                                   : xobj::RecordKind::Object;
}

} // namespace

std::array<float, 4> facingRotation(std::int16_t direction) {
    // Facing 2 is the model's own; each step of 1 turns it 45 degrees the other way.
    const double angle = (2 - direction) * std::numbers::pi / 4;
    return {0, 0, static_cast<float>(std::sin(angle / 2)), static_cast<float>(std::cos(angle / 2))};
}

ObjectSave objectChanges(dbpf::Package& original, const ObjectChanges& changes) {
    ObjectSave out;
    std::optional<Saved> mapRecord, typeRecord, groupRecord;
    // Every state record by id (the game leaves old ones of the other kind behind), and scene records.
    std::map<std::int16_t, std::vector<Saved>> records;
    std::map<std::int16_t, Saved> scenes;
    std::vector<dbpf::ResourceKey> tables, relationships;
    std::set<std::int16_t> sold(changes.sold.begin(), changes.sold.end());
    for (const auto& e : original.entries()) {
        const auto id = static_cast<std::int16_t>(e.key.instance);
        if (e.key.type == types::kObjectMap && e.key.instance == 1) {
            mapRecord = Saved{e.key, original.read(e)};
        } else if (e.key.type == types::kObjectMap && e.key.instance == 2) {
            groupRecord = Saved{e.key, original.read(e)};
        } else if (e.key.type == types::kObjectTypeTable) {
            typeRecord = Saved{e.key, original.read(e)};
        } else if ((e.key.type == types::kSavedObject || e.key.type == types::kSavedObjectWithMaterial || e.key.type == types::kSavedPerson) &&
                   e.key.instance <= 0x7FFF) {
            records[id].push_back(Saved{e.key, original.read(e)});
        } else if (e.key.type == types::kSceneObject && e.key.instance <= 0x7FFF) {
            scenes[id] = Saved{e.key, original.read(e)};
        } else if (e.key.type == types::kDataTables && sold.contains(id)) {
            tables.push_back(e.key);
        } else if (e.key.type == types::kSimRelationship &&
                   (sold.contains(static_cast<std::int16_t>(e.key.instance >> 16)) || sold.contains(static_cast<std::int16_t>(e.key.instance & 0xFFFF)))) {
            relationships.push_back(e.key);
        }
    }
    if (!mapRecord || !typeRecord) {
        out.problems.push_back("objects: the lot has no object map or type table");
        return out;
    }
    auto map = objm::parse(mapRecord->bytes);
    auto typeTable = objt::parse(typeRecord->bytes);
    // Each listed object's state: the record of the kind its type is saved as (XMTO for flag 1).
    std::map<std::int16_t, Saved> states;
    for (const auto& [id, list] : records) {
        const auto listed = map.typeIndexByObject.find(static_cast<std::uint32_t>(id));
        const auto* type = listed != map.typeIndexByObject.end() ? typeTable.find(static_cast<std::uint16_t>(listed->second)) : nullptr;
        if (type == nullptr) {
            continue;
        }
        const std::uint32_t wanted = type->kind == objt::kPersonKind ? types::kSavedPerson
                                     : (type->flag & 1)              ? types::kSavedObjectWithMaterial
                                                                     : types::kSavedObject;
        for (const auto& s : list) {
            if (s.key.type == wanted) {
                states[id] = s;
            }
        }
    }
    const std::size_t objectsBefore = map.order.size();
    const std::uint32_t version = map.version;
    std::span<const std::uint8_t> blankName;
    for (const auto& [id, list] : records) {
        blankName = std::span(list.front().bytes).first(64);
        break;
    }
    bool mapChanged = false, typesChanged = false;
    auto setWait = [&](std::int16_t id, std::int16_t ticks, std::uint16_t flags) {
        const auto i = static_cast<std::size_t>(id - 1);
        if (!map.hasWaits) {
            return;
        }
        if (map.waitTicks.size() <= i) {
            map.waitTicks.resize(i + 1, 0);
            map.waitSecond.resize(i + 1, 0);
        }
        map.waitTicks[i] = ticks;
        map.waitSecond[i] = flags;
        mapChanged = true;
    };

    // Sold: every record of the object goes, and its places in the map and lists.
    if (!sold.empty()) {
        for (const auto id : sold) {
            if (const auto r = records.find(id); r != records.end()) {
                for (const auto& s : r->second) {
                    out.removed.push_back(s.key);
                }
            }
            if (const auto s = scenes.find(id); s != scenes.end()) {
                out.removed.push_back(s->second.key);
            }
            std::erase_if(map.order, [&](const auto& entry) { return entry.first == static_cast<std::uint32_t>(id); });
            map.typeIndexByObject.erase(static_cast<std::uint32_t>(id));
            setWait(id, 0, 0);
            ++out.sold;
        }
        out.removed.insert(out.removed.end(), tables.begin(), tables.end());
        out.removed.insert(out.removed.end(), relationships.begin(), relationships.end());
        if (groupRecord) {
            try {
                auto groups = objm::parseGroups(groupRecord->bytes);
                bool listed = false;
                for (auto& [key, objects] : groups.objectsByKey) {
                    listed = std::erase_if(objects, [&](std::uint16_t o) { return sold.contains(static_cast<std::int16_t>(o)); }) > 0 || listed;
                }
                if (listed) {
                    out.resources.push_back({groupRecord->key, objm::writeGroups(groups, std::span(groupRecord->bytes).first(64))});
                }
            } catch (const std::exception& ex) {
                out.problems.push_back(std::format("object lists: {}", ex.what()));
            }
        }
    }

    // Scene places. An object held in a slot has none of its own; its world place is its container's.
    using Place = std::array<float, 7>;
    constexpr Place kNoPlace{0, 0, 0, 0, 0, 0, 1};
    std::map<std::int16_t, Place> worldNow;  // of objects placed here, for what they hold
    auto savedWorld = [&](std::int16_t id) -> std::optional<Place> {
        if (const auto w = worldNow.find(id); w != worldNow.end()) {
            return w->second;
        }
        const auto scene = scenes.find(id);
        if (scene == scenes.end()) {
            return std::nullopt;
        }
        const auto parsed = cobject::parse(scene->second.bytes);
        return parsed.animatableTransform ? parsed.animatableTransform : parsed.transform;
    };
    auto floorAt = [&](const xobj::SavedObject& s) {
        return changes.floorHeight ? changes.floorHeight(s.header.value1, s.header.location[0], s.header.location[1]) : 0.0f;
    };
    // The object's own place and its world place, `rise` above the floor.
    auto placeOf = [&](const xobj::SavedObject& s, float rise) -> std::pair<Place, Place> {
        const std::int16_t container = s.data[kContainerData];
        if (container != 0) {
            return {kNoPlace, savedWorld(container).value_or(kNoPlace)};
        }
        const auto turn = facingRotation(s.data[kFacingData]);
        const Place own{s.header.location[1], s.header.location[0], floorAt(s) + rise, turn[0], turn[1], turn[2], turn[3]};
        return {own, own};
    };
    // Containers first, so what they hold goes where they are.
    const auto heldLast = [](const xobj::SavedObject& a, const xobj::SavedObject& b) {
        return (a.data[kContainerData] == 0) > (b.data[kContainerData] == 0);
    };

    // Moved: the saved state with the object's place, data and slots now; the scene record placed, as
    // high above the floor as it was.
    std::vector<const ObjectNow*> moved;
    for (const auto& now : changes.moved) {
        moved.push_back(&now);
    }
    std::stable_sort(moved.begin(), moved.end(), [&](const ObjectNow* a, const ObjectNow* b) { return heldLast(a->state, b->state); });
    for (const auto* nowPtr : moved) {
        const auto& now = *nowPtr;
        try {
            const auto s = states.find(now.id);
            if (s == states.end()) {
                throw std::runtime_error("it has no saved state");
            }
            const auto kind = kindOf(s->second.key.type);
            if (kind == xobj::RecordKind::Person) {
                throw std::runtime_error("it is a Sim");
            }
            auto state = xobj::parse(s->second.bytes, version, kind);
            const auto scene = scenes.find(now.id);
            std::optional<cobject::SavedObject> parsed;
            if (scene != scenes.end()) {
                parsed = cobject::parse(scene->second.bytes);
            }
            float rise = 0;
            if (parsed && parsed->transform && state.data[kContainerData] == 0) {
                rise = (*parsed->transform)[2] - floorAt(state);
            }
            state.header.location = now.state.header.location;
            state.header.value1 = now.state.header.value1;
            state.data = now.state.data;
            if (state.tail && now.state.tail && state.tail->slots.marker < 0) {
                state.tail->slots.values = now.state.tail->slots.values;
            }
            out.resources.push_back({s->second.key, xobj::write(state, std::span(s->second.bytes).first(64), kind)});
            const auto [own, world] = placeOf(state, rise);
            worldNow[now.id] = world;
            if (parsed) {
                out.resources.push_back({scene->second.key, placed(scene->second.bytes, *parsed, own, world)});
            }
            const auto i = static_cast<std::size_t>(now.id - 1);
            if (map.hasWaits && i < map.waitSecond.size()) {
                setWait(now.id, map.waitTicks[i], waitFlags(map.waitSecond[i], state));
            }
            ++out.moved;
        } catch (const std::exception& ex) {
            out.problems.push_back(std::format("object {} moved: {}", now.id, ex.what()));
            out.failed.insert(now.id);
        }
    }

    // Bought: new records, the scene record from one of the type's in a saved lot.
    std::vector<const BoughtObject*> bought;
    for (const auto& b : changes.bought) {
        bought.push_back(&b);
    }
    std::stable_sort(bought.begin(), bought.end(), [&](const BoughtObject* a, const BoughtObject* b) { return heldLast(a->now.state, b->now.state); });
    for (const auto* b : bought) {
        const auto& now = b->now;
        const auto& scene = b->scene;
        // An object that can't be saved adds no types.
        const auto typesBefore = typeTable;
        const bool typesChangedBefore = typesChanged;
        try {
            if (map.typeIndexByObject.contains(static_cast<std::uint32_t>(now.id))) {
                throw std::runtime_error("its id is taken in the saved lot");
            }
            const auto parsed = cobject::parse(scene.bytes);
            if (parsed.classes.size() > 2 || !parsed.transform) {
                throw std::runtime_error("its type's scene record is a Sim's");
            }
            // Its type's entry and those its stack runs code of, added when the table has none.
            const auto indexOf = [&](std::uint32_t guid) -> std::uint16_t {
                if (const auto e = std::ranges::find(typeTable.entries, guid, &objt::Entry::guid); e != typeTable.entries.end()) {
                    return e->index;
                }
                auto entry = changes.newType ? changes.newType(guid) : std::nullopt;
                if (!entry) {
                    throw std::runtime_error(std::format("type {:08x} couldn't be added to the lot's type table", guid));
                }
                std::uint16_t highest = 0;
                for (const auto& t : typeTable.entries) {
                    highest = std::max(highest, t.index);
                }
                entry->index = static_cast<std::uint16_t>(highest + 1);
                typeTable.entries.push_back(std::move(*entry));
                typesChanged = true;
                return static_cast<std::uint16_t>(highest + 1);
            };
            auto state = now.state;
            state.version = version;
            if (!state.tail || b->frameOwners.size() != state.tail->stack.size()) {
                throw std::runtime_error("its state is incomplete");
            }
            const std::uint16_t index = indexOf(now.guid);
            for (std::size_t k = 0; k < b->frameOwners.size(); ++k) {
                state.tail->stack[k].codeOwner = static_cast<std::int16_t>(indexOf(b->frameOwners[k]));
            }
            const auto kind = scene.kind == xobj::RecordKind::ObjectWithMaterial && state.tail->materialBlock.size() == 26 ? scene.kind : xobj::RecordKind::Object;
            if (blankName.empty()) {
                throw std::runtime_error("the lot has no saved objects to follow");
            }
            const dbpf::ResourceKey stateKey{kind == xobj::RecordKind::ObjectWithMaterial ? types::kSavedObjectWithMaterial : types::kSavedObject, kGroup,
                                             static_cast<std::uint32_t>(now.id), 0};
            const dbpf::ResourceKey sceneKey{types::kSceneObject, kGroup, static_cast<std::uint32_t>(now.id), 0};
            auto stateBytes = xobj::write(state, blankName, kind);
            // The template placed, without the animations it was playing (they come last).
            const auto [own, world] = placeOf(state, 0);
            worldNow[now.id] = world;
            auto placedScene = placed(scene.bytes, parsed, own, world);
            if (parsed.animatableTransform) {
                placedScene.resize(parsed.animationsOffset);
                placedScene.insert(placedScene.end(), 4, 0);
            }
            // Records the game left behind for the id when it last let it go are replaced.
            if (const auto stale = records.find(now.id); stale != records.end()) {
                for (const auto& s : stale->second) {
                    if (!(s.key == stateKey)) {
                        out.removed.push_back(s.key);
                    }
                }
            }
            out.resources.push_back({stateKey, std::move(stateBytes)});
            out.resources.push_back({sceneKey, std::move(placedScene)});
            map.order.emplace_back(static_cast<std::uint32_t>(now.id), index);
            map.typeIndexByObject[static_cast<std::uint32_t>(now.id)] = index;
            setWait(now.id, now.waitTicks, waitFlags(scene.waitFlags, state));
            mapChanged = true;
            ++out.bought;
        } catch (const std::exception& ex) {
            typeTable = typesBefore;
            typesChanged = typesChangedBefore;
            out.problems.push_back(std::format("object {} bought: {}", now.id, ex.what()));
            out.failed.insert(now.id);
        }
    }

    if (mapChanged || out.sold > 0) {
        // What follows the wait table starts with the object count.
        if (map.rest.size() >= 4) {
            std::uint32_t count = 0;
            std::memcpy(&count, map.rest.data(), 4);
            if (count == objectsBefore) {
                const auto now = static_cast<std::uint32_t>(map.order.size());
                std::memcpy(map.rest.data(), &now, 4);
            }
        }
        out.resources.push_back({mapRecord->key, objm::write(map, std::span(mapRecord->bytes).first(64))});
    }
    if (typesChanged) {
        out.resources.push_back({typeRecord->key, objt::write(typeTable, std::span(typeRecord->bytes).first(64))});
    }
    // An id sold and bought again keeps its records' keys, written anew.
    std::erase_if(out.removed, [&](const dbpf::ResourceKey& key) {
        return std::ranges::any_of(out.resources, [&](const content::ReplacedResource& r) { return r.key == key; });
    });
    // Each once.
    std::vector<dbpf::ResourceKey> once;
    for (const auto& key : out.removed) {
        if (std::ranges::find(once, key) == once.end()) {
            once.push_back(key);
        }
    }
    out.removed = std::move(once);
    return out;
}

std::optional<SceneTemplate> findSceneTemplate(const std::vector<std::filesystem::path>& lotPackages, std::uint32_t guid) {
    for (const auto& file : lotPackages) {
        try {
            dbpf::Package package(file);
            std::optional<objt::ObjectTypeTable> table;
            std::optional<objm::ObjectMap> map;
            for (const auto& e : package.entries()) {
                if (e.key.type == types::kObjectTypeTable) {
                    table = objt::parse(package.read(e));
                } else if (e.key.type == types::kObjectMap && e.key.instance == 1) {
                    map = objm::parse(package.read(e));
                }
            }
            if (!table || !map) {
                continue;
            }
            const auto type = std::ranges::find(table->entries, guid, &objt::Entry::guid);
            if (type == table->entries.end()) {
                continue;
            }
            for (const auto& [object, index] : map->order) {
                if (index != type->index) {
                    continue;
                }
                SceneTemplate found;
                const dbpf::IndexEntry *scene = nullptr, *state = nullptr;
                for (const auto& e : package.entries()) {
                    if (e.key.instance != object) {
                        continue;
                    }
                    if (e.key.type == types::kSceneObject) {
                        scene = &e;
                    } else if (e.key.type == ((type->flag & 1) ? types::kSavedObjectWithMaterial : types::kSavedObject)) {
                        state = &e;  // of the kind the type is saved as
                    }
                }
                if (scene == nullptr || state == nullptr) {
                    continue;
                }
                found.bytes = package.read(*scene);
                const auto parsed = cobject::parse(found.bytes);
                if (parsed.classes.size() > 2 || !parsed.transform) {
                    continue;
                }
                found.kind = kindOf(state->key.type);
                found.state = xobj::parse(package.read(*state), map->version, found.kind);
                if (!found.state.tail) {
                    continue;
                }
                const auto i = static_cast<std::size_t>(object - 1);
                if (i < map->waitSecond.size()) {
                    found.waitFlags = map->waitSecond[i];
                }
                return found;
            }
        } catch (const std::exception&) {
            continue;  // a lot NeoTS2 can't read gives none
        }
    }
    return std::nullopt;
}

} // namespace ts2::sim
