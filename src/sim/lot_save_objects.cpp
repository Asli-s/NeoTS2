#include "sim/lot_save_detail.hpp"
#include "formats/lot_records.hpp"
#include "formats/objd.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ts2::sim {

using namespace lot_save_detail;

namespace {

// `values` without the zeros a load added past `kept`, so a frame nothing wrote to is saved with
// the shape it was read with. Values a script wrote are never dropped.
// `values` cut or padded with zeros to `count`, keeping any value past it that isn't 0.
std::vector<std::int16_t> sized(const std::vector<std::int16_t>& values, std::size_t count) {
    std::size_t end = values.size();
    while (end > count && values[end - 1] == 0) {
        --end;
    }
    std::vector<std::int16_t> out(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(end));
    if (out.size() < count) {
        out.resize(count, 0);
    }
    return out;
}

// Copies as many values as the record held, so a record keeps the shape the lot was saved with.
template <typename T, std::size_t N>
void copyAsMany(std::vector<std::int16_t>& out, const std::array<T, N>& values) {
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = i < values.size() ? values[i] : 0;
    }
}

// How many attributes the game gives an object of a type: the definition's count, at least 8.
std::size_t attributeCount(const objd::ObjectDefinition& definition) {
    constexpr std::size_t kLeast = 8;
    return std::max<std::size_t>(definition.field(objd::field::kNumAttributes), kLeast);
}

// How many semi-global attributes the game gives an object of a type, which its type table entry
// keeps as its fifth value.
std::size_t semiAttributeCount(const objt::Entry& entry) {
    return static_cast<std::size_t>(entry.unknown[8]) | static_cast<std::size_t>(entry.unknown[9]) << 16;
}

// The record of an object made since the lot loaded, for a type no object of the lot has a record
// of to start from, as the game writes one for an object just made: these values, then the
// object's own state over them.
xobj::SavedObject freshRecord(const objt::Entry& entry, const objd::ObjectDefinition& definition) {
    xobj::SavedObject record;
    record.version = xobj::kWrittenVersion;
    record.header.value2 = 1;
    record.header.value4 = -1;
    record.attributes.assign(attributeCount(definition), 0);
    constexpr std::size_t kMaxSemiAttributes = 0x7FFF;
    record.semiAttributes.assign(std::min(semiAttributeCount(entry), kMaxSemiAttributes), 0);
    record.tail.emplace();
    record.tail->relationshipVersion = 3;
    record.tail->slots.marker = -101;
    return record;
}

// The types of the objects the lot was saved with that are not on it now. A lot makes its own
// controllers as it loads, so an object it made then of one of those types stands in for a saved
// object that is gone: the lot making it again, not a new object. Saving those would add one more
// at every save. Objects made later in play are new whatever their type.
std::unordered_set<std::int32_t> remadeTypes(const SaveState& save) {
    std::unordered_set<std::int32_t> remade;
    for (const auto& [id, type] : save.typeByObject) {
        if (continuingObject(save.world, *save.types, id, type) == nullptr) {
            remade.insert(type);
        }
    }
    return remade;
}

// Stores `body` under `key`, over the resource there or after the others.
void putResource(dbpf::PackageImage& package, const dbpf::ResourceKey& key, std::span<const std::uint8_t> body) {
    const auto at = std::find_if(package.resources.begin(), package.resources.end(), [&](const dbpf::StoredResource& r) { return r.key == key; });
    if (at != package.resources.end()) {
        *at = dbpf::storeResource(key, body);
    } else {
        package.resources.push_back(dbpf::storeResource(key, body));
        package.dataOrder.push_back(package.resources.size() - 1);
    }
}

// The scene record of the Sim with the lowest id the lot was loaded with. Empty when none reads.
std::optional<cobject::Record> anotherSimScene(const SaveState& save) {
    std::optional<cobject::Record> found;
    std::uint32_t lowest = UINT32_MAX;
    for (const dbpf::StoredResource& resource : save.package.resources) {
        if (resource.key.type != types::kSceneObject || resource.key.instance >= lowest ||
            listedObject(save, static_cast<std::int32_t>(resource.key.instance)).object == nullptr) {
            continue;
        }
        if (auto record = cobject::readRecord(bodyOf(resource)); record && record->chain == cobject::Chain::Person) {
            found = std::move(record);
            lowest = resource.key.instance;
        }
    }
    return found;
}

// The animatable part of the record of the object with the lowest id the lot was loaded with whose
// record is a cAnimatable one, not a Sim's. Empty when none reads.
std::optional<cobject::AnimatablePart> anotherObjectAnimatable(const SaveState& save) {
    std::optional<cobject::AnimatablePart> found;
    std::uint32_t lowest = UINT32_MAX;
    for (const dbpf::StoredResource& resource : save.package.resources) {
        if (resource.key.type != types::kSceneObject || resource.key.instance >= lowest ||
            listedObject(save, static_cast<std::int32_t>(resource.key.instance)).object == nullptr) {
            continue;
        }
        if (auto record = cobject::readRecord(bodyOf(resource)); record && record->chain == cobject::Chain::Animatable) {
            found = std::move(record->animatable);
            lowest = resource.key.instance;
        }
    }
    return found;
}

// The record of a Sim of another type with the counts a Sim of `entry`'s type gets. Empty when the
// lot has none.
std::optional<xobj::SavedObject> anotherSimRecord(SaveState& save, const SimObject& object, const objt::Entry& entry) {
    std::vector<std::pair<std::uint16_t, std::size_t>> sims;
    for (const auto& [index, at] : save.templates) {
        if (const objt::Entry* e = save.types->find(index); e != nullptr && e->kind == objt::kPersonKind) {
            sims.emplace_back(index, at);
        }
    }
    std::sort(sims.begin(), sims.end());
    for (const auto& [index, at] : sims) {
        try {
            xobj::SavedObject record = readSaved(save.package.resources[at], save.version, xobj::RecordKind::Person);
            if (record.person && record.attributes.size() == attributeCount(object.definition) &&
                record.semiAttributes.size() == semiAttributeCount(entry)) {
                return record;
            }
        } catch (const std::exception&) {
        }
    }
    return std::nullopt;
}

// The record to start a new object's record from. A record left at its id by an object of the same type,
// from a session where it was made the same way, is the closest thing to the record the game
// would write. Failing that, the record of another object of its type says what the game writes
// for the type. Empty when neither reads and the object is not an ordinary one.
std::optional<xobj::SavedObject> recordToStartFrom(SaveState& save, const SimObject& object, const objt::Entry& entry,
                                                   std::vector<dbpf::StoredResource>::iterator at, xobj::RecordKind kind) {
    std::optional<xobj::SavedObject> from;
    if (at != save.package.resources.end()) {
        try {
            xobj::SavedObject left = readSaved(*at, save.version, kind);
            // A record left by an object of another type rarely has the counts this type gets.
            if (left.attributes.size() == attributeCount(object.definition) &&
                left.semiAttributes.size() == semiAttributeCount(entry)) {
                from = std::move(left);
            }
        } catch (const std::exception&) {
        }
    }
    if (!from) {
        const auto sibling = save.templates.find(entry.index);
        if (sibling != save.templates.end()) {
            try {
                from = readSaved(save.package.resources[sibling->second], save.version, kind);
            } catch (const std::exception&) {
            }
        }
        // With no record of its type to start from, an ordinary object gets the record the game
        // writes for an object just made. One with a material block (XMTO) does not, as what the
        // game puts there for a new one is not known yet.
        if (!from && kind == xobj::RecordKind::Object) {
            from = freshRecord(entry, object.definition);
            ++save.counts.objectsFresh;
        }
        // A new Sim's type is its own, so it starts from another Sim's record, whose layout every
        // Sim's record shares, the one of the lowest type entry with the same counts.
        if (!from && kind == xobj::RecordKind::Person) {
            from = anotherSimRecord(save, object, entry);
        }
    }
    return from;
}

} // namespace

namespace lot_save_detail {

xobj::SavedObject asTemplate(const xobj::SavedObject& record) {
    xobj::SavedObject out = record;
    out.attributes.assign(out.attributes.size(), 0);
    out.semiAttributes.assign(out.semiAttributes.size(), 0);
    out.data = {};
    out.temps = {};
    out.tokenSource = {};
    if (out.tail) {
        out.tail->stack.clear();
        out.tail->effects.clear();
        out.tail->materials.clear();
        out.tail->timers.clear();
        out.tail->relationshipIds.clear();
        out.tail->token = {};
    }
    if (out.person) {
        out.person->motives = {};
        out.person->motives2 = {};
        out.person->personData.assign(out.person->personData.size(), 0);
        // A Sim just made has done nothing yet: its tail's first value is 0, both its running and
        // interrupted interactions are blank with id 0, and the queues, runs, motive changes and
        // autonomy's last pick are empty.
        // The engine fills its running interaction and queues from the Sim as it is now.
        if (out.tail) {
            out.tail->value1 = 0;
        }
        xobj::PersonState& state = out.person->state;
        state.running = blankInteraction(0);
        state.interrupted = blankInteraction(0);
        state.queue.clear();
        state.runs.clear();
        state.entries.clear();
        state.runningSub.reset();
        state.subQueue.clear();
        state.text.clear();
        state.pickedIconIndex = 0;
        state.pickedIconGuid = 0;
        state.pickedIconObject = 0;
        state.values = {};
        state.list.clear();
    }
    return out;
}

xobj::SavedObject readSaved(const dbpf::StoredResource& resource, std::uint32_t version, xobj::RecordKind kind) {
    xobj::SavedObject saved = xobj::parse(bodyOf(resource), version, kind);
    if (version == xobj::kWrittenVersion) {
        return saved;
    }
    auto moved = xobj::upgraded(std::move(saved));
    if (!moved) {
        throw std::runtime_error("XOBJ: no upgrade from this version");
    }
    return std::move(*moved);
}

std::optional<std::vector<xobj::StackFrame>> stackOf(const xobj::SavedObject& loaded, const SimObject& object,
                                                     const TypeOwners& owners) {
    std::vector<xobj::StackFrame> frames;
    const auto& was = loaded.tail ? loaded.tail->stack : std::vector<xobj::StackFrame>{};
    for (std::size_t at = 0; at < object.thread.frames.size(); ++at) {
        const Frame& frame = object.thread.frames[at];
        const auto owner = owners.indexByGroup.find(frame.codeOwner);
        if (owner == owners.indexByGroup.end()) {
            return std::nullopt;
        }
        // The fields the lot's loader does not read keep what the frame at this depth was saved
        // with, since nothing in the running lot says what they should be.
        xobj::StackFrame out = at < was.size() ? was[at] : xobj::StackFrame{};
        const auto was_ = owners.groupByIndex.find(out.codeOwner);
        const bool keepsOwner = at < was.size() && was_ != owners.groupByIndex.end() && was_->second == frame.codeOwner;
        // Only the frame saved at this depth running the same tree is this one, carried on.
        const bool continues = keepsOwner && static_cast<std::uint16_t>(out.treeId) == frame.instance;
        out.codeOwner = keepsOwner ? out.codeOwner : owner->second;
        out.treeId = static_cast<std::int16_t>(frame.instance);
        out.node = frame.pc;
        // A frame saves as many parameters and locals as its tree declares, as the game writes nearly
        // every frame, and a new one the object's own id first. One carried on keeps the counts it
        // was saved with. A tree reads a local past those it has as an error.
        const std::size_t takes = frame.code != nullptr ? frame.code->argCount : frame.params.size();
        const std::size_t has = frame.code != nullptr ? frame.code->localCount : frame.locals.size();
        out.params = sized(frame.params, continues ? was[at].params.size() : takes);
        out.locals = sized(frame.locals, continues ? was[at].locals.size() : has);
        if (!continues) {
            out.value1 = object.id;
        }
        out.objectId = frame.stackObjectId;
        out.value2 = frame.runningOnId;
        out.value3 = frame.idleStarted ? 1 : 0;
        frames.push_back(std::move(out));
    }
    return frames;
}

} // namespace lot_save_detail

xobj::SavedObject objectRecord(const xobj::SavedObject& loaded, const SimObject& object, const TypeOwners& owners,
                               SaveCounters* counters) {
    xobj::SavedObject record = loaded;
    bool stackFromState = false;
    copyAsMany(record.attributes, object.attributes);
    copyAsMany(record.semiAttributes, object.semiAttributes);
    std::copy_n(object.data.begin(), std::min(object.data.size(), record.data.size()), record.data.begin());
    record.temps = object.thread.temps;
    record.header.location = {static_cast<float>(object.x) / 65536.0f, static_cast<float>(object.y) / 65536.0f};
    record.header.value1 = object.level;
    const auto& source = object.tokenSource;
    record.tokenSource = {source.kind,     source.id,       source.counted, source.index,
                          source.guidLow,  source.guidHigh, source.flags,   source.propertyCount};
    if (record.tail) {
        xobj::Tail& tail = *record.tail;
        tail.arrays = object.arrays;
        tail.shorts = object.arrayIterators;
        tail.strings = object.savedMaterialNames ? *object.savedMaterialNames : object.materialNames;
        tail.token.guid = object.token.guid;
        tail.token.flags = object.token.flags;
        tail.token.value1 = object.token.value1;
        tail.token.value2 = object.token.value2;
        tail.token.value3 = object.token.category;
        tail.relationshipVersion = 3;
        tail.relationshipIds = owners.relationshipList ? owners.relationshipList(object) : object.relationshipTable;
        tail.token.properties = object.token.properties;
        // A record whose slot marker is not negative keeps the lot's own slots, so writing the
        // object's there would not be read back.
        if (tail.slots.marker < 0) {
            tail.slots.values = object.slots;
        }
        // The effects it plays now, in the list's second version as the game writes it. One that had
        // effects keeps its list, empty when they have all ended.
        if (owners.runningEffects) {
            tail.effects = owners.runningEffects(object.id);
            if (!tail.effects.empty() || tail.effectFlag != 0) {
                constexpr std::uint16_t kEffectListVersion = 2;
                tail.effectFlag = 1;
                tail.effectVersion = kEffectListVersion;
            }
        }
        if (const auto frames = stackOf(loaded, object, owners)) {
            tail.stack = *frames;
            stackFromState = true;
        }
    }
    if (record.person) {
        SaveCounters none;
        saveQueue(record.person->state, object, counters != nullptr ? *counters : none, owners.tunings);
        record.person->state.collectiblesLow = object.vacationCollectibles[0];
        record.person->state.collectiblesHigh = object.vacationCollectibles[1];
        // The runs go with the stack: each is the depth it started at and the object the frame there
        // runs on. With the stack kept as loaded they stay as loaded too.
        if (stackFromState) {
            std::vector<xobj::SavedRun>& runs = record.person->state.runs;
            runs.clear();
            for (const std::size_t depth : object.runDepths) {
                if (depth >= 1 && depth <= record.tail->stack.size()) {
                    runs.push_back({record.tail->stack[depth - 1].value2, static_cast<std::int16_t>(depth), false});
                }
            }
        }
        record.person->motives = object.motives;
        record.person->motives2 = object.motivesAtDecay;
        for (std::size_t i = 0; i < record.person->personData.size(); ++i) {
            record.person->personData[i] = object.personData[i];
        }
    }
    return record;
}

namespace lot_save_detail {

void addNewObjects(SaveState& save) {
    const objt::ObjectTypeTable& types = *save.types;
    std::unordered_map<std::uint32_t, const objt::Entry*> entryByGuid;
    for (const objt::Entry& entry : types.entries) {
        entryByGuid.emplace(entry.guid, &entry);
    }
    const std::unordered_set<std::int32_t> remade = remadeTypes(save);
    // Read once, when the first new cAnimatable object needs it. New objects are not listed, so the
    // records written here never change it.
    std::optional<std::optional<cobject::AnimatablePart>> animatable;
    save.world.nextAfter(0, [&](const SimObject& object) {
        const auto id = static_cast<std::int32_t>(object.id);
        if (save.done.contains(id) || listedObject(save, id).object != nullptr) {
            return false;
        }
        const auto entry = entryByGuid.find(object.definition.guid());
        if (entry == entryByGuid.end()) {
            ++save.counts.objectsUnnamed;
            return false;
        }
        if (save.owners.madeAsLoaded.contains(object.id) && remade.contains(static_cast<std::int32_t>(entry->second->index))) {
            ++save.counts.objectsRemade;
            return false;
        }
        const std::uint32_t type = wantedType(*entry->second);
        const auto kind = resourceKind(type);
        const dbpf::ResourceKey key{type, 0xFFFFFFFF, static_cast<std::uint32_t>(id), 0};
        const auto at = std::find_if(save.package.resources.begin(), save.package.resources.end(),
                                     [&](const dbpf::StoredResource& r) { return r.key == key; });
        const std::optional<xobj::SavedObject> from = recordToStartFrom(save, object, *entry->second, at, *kind);
        if (!from) {
            ++save.counts.objectsUnnamed;
            return false;
        }
        std::vector<std::uint8_t> written;
        try {
            written = xobj::write(objectRecord(asTemplate(*from), object, save.owners, &save.counters), *kind);
        } catch (const std::exception&) {
            ++save.counts.objectsUnnamed;
            return false;
        }
        putResource(save.package, key, written);
        ++save.counts.objectsAdded;
        save.done.insert(id);
        // A new Sim gets the routing aggregate every idle Sim has, with nothing in it, and a scene record
        // placing it where it stands, which a load needs to put it there.
        if (*kind == xobj::RecordKind::Person) {
            putResource(save.package, {lotrecords::kRoutingAggregateType, 0xFFFFFFFF, static_cast<std::uint32_t>(id), 0},
                        lotrecords::writeRoutingAggregate({}));
            if (const auto another = anotherSimScene(save)) {
                const float x = static_cast<float>(object.y) / 65536.0f, y = static_cast<float>(object.x) / 65536.0f;
                const float floor = save.owners.floorHeight ? save.owners.floorHeight(object.level, x, y) : 0.0f;
                putResource(save.package, {types::kSceneObject, 0xFFFFFFFF, static_cast<std::uint32_t>(id), 0},
                            cobject::writeRecord(newSimSceneRecord(*another, object, save.world, save.world.clockMs(), save.anyAnimation, floor,
                                                                  save.owners, &save.clipHandles)));
                ++save.counts.scenes;
            }
        } else if (const auto chain = sceneChainOf(object); chain && save.owners.modelName &&
                                                             (object.data[kLeadTileField] <= 0 || object.data[kLeadTileField] == object.id)) {
            // A new object gets a record of its own, over any a removed object left at its id. The parts of a
            // multi-tile object other than its lead are left as they are, as what the game writes for them
            // isn't known yet.
            if (*chain == cobject::Chain::Animatable && !animatable) {
                animatable = anotherObjectAnimatable(save);
            }
            // A cPerson record of an object that isn't saved as a Sim (a Sim template) starts from the
            // settings the game makes. What else the game writes for one is not known yet.
            cobject::AnimatablePart made;
            made.settings = newAnimatableSettings();
            const auto another = *chain == cobject::Chain::Animatable ? *animatable : std::optional(made);
            if (another) {
                putResource(save.package, {types::kSceneObject, 0xFFFFFFFF, static_cast<std::uint32_t>(id), 0},
                            cobject::writeRecord(newObjectSceneRecord(*chain, save.owners.modelName(object), *another, object, save.world,
                                                                     save.world.clockMs(), save.anyAnimation, save.owners, &save.clipHandles)));
                ++save.counts.scenes;
            }
        }
        return false;
    });
}

} // namespace lot_save_detail

} // namespace ts2::sim
