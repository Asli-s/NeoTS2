#include "sim/index_code_source.hpp"

#include <algorithm>

#include "diag/log.hpp"
#include "formats/bcon.hpp"
#include "formats/glob.hpp"
#include "formats/hash.hpp"
#include "formats/image.hpp"
#include "formats/objd.hpp"
#include "formats/propset.hpp"
#include "formats/resource_types.hpp"
#include "formats/slot.hpp"
#include "formats/str.hpp"
#include "sim/world.hpp"

#include <exception>
#include <format>

namespace ts2::sim {

namespace {

constexpr std::uint32_t kBehaviorConstants = 0x42434F4E;
constexpr std::uint32_t kSemiGlobalReference = 0x474C4F42;
constexpr std::uint8_t kUsEnglish = 1;
// The type new Sims are made from. Its own trees serve Sims whose files carry none.
constexpr std::uint32_t kTemplatePersonGuid = 0x7FD96B54;

std::uint64_t groupInstance(std::uint32_t group, std::uint16_t instance) {
    return static_cast<std::uint64_t>(group) << 32 | instance;
}

} // namespace

IndexCodeSource::IndexCodeSource(content::ResourceIndex& index) : index_(index) {
    for (const auto& r : index_.resources()) {
        if (r.entry.key.type == types::kBehavior) {
            groupsWithTrees_.insert(r.entry.key.group);
        }
        if (r.entry.key.type != kSemiGlobalReference) {
            continue;
        }
        try {
            const auto glob = glob::parse(index_.read(r));
            semiGlobals_.emplace(r.entry.key.group, hash::groupId(glob.semiGlobalName));
        } catch (const std::exception& ex) {
            diag::warnOnce(std::string("objects: a semi-global reference that can't be read is left out: ") + ex.what());
        }
    }
}

std::uint32_t IndexCodeSource::privateCodeGroup(std::uint32_t group) {
    if (groupsWithTrees_.contains(group)) {
        return group;
    }
    objectType(0);  // finds the Sims' groups the first time
    const auto sim = simGroups_.find(group);
    if (sim == simGroups_.end()) {
        return group;
    }
    const ObjectType* original = sim->second != 0 ? objectType(sim->second) : nullptr;
    if (original != nullptr && groupsWithTrees_.contains(original->group)) {
        return original->group;
    }
    const ObjectType* templatePerson = objectType(kTemplatePersonGuid);
    return templatePerson != nullptr ? templatePerson->group : group;
}

std::shared_ptr<const bhav::Behavior> IndexCodeSource::behavior(std::uint32_t group,
                                                                std::uint16_t instance) {
    group = privateCodeGroup(group);
    const std::uint64_t key = groupInstance(group, instance);
    if (const auto it = behaviors_.find(key); it != behaviors_.end()) {
        return it->second;
    }

    std::shared_ptr<const bhav::Behavior> parsed;
    // A broken tree counts as missing, once, so it can't throw out of whatever needed it.
    if (const auto* r = index_.find({types::kBehavior, group, instance, 0})) {
        try {
            parsed = std::make_shared<const bhav::Behavior>(bhav::parse(index_.read(*r)));
        } catch (const std::exception& ex) {
            diag::warning(std::format("tree 0x{:08X} 0x{:04X} can't be read: {}", group, instance, ex.what()));
        }
    }
    behaviors_.emplace(key, parsed);
    return parsed;
}

std::shared_ptr<const ttab::InteractionTable> IndexCodeSource::interactionTable(std::uint32_t group,
                                                                               std::uint16_t instance) {
    const std::uint64_t key = groupInstance(group, instance);
    if (const auto it = interactionTables_.find(key); it != interactionTables_.end()) {
        return it->second;
    }
    std::shared_ptr<const ttab::InteractionTable> parsed;
    if (const auto* r = index_.find({types::kInteractionTable, group, instance, 0})) {
        try {
            parsed = std::make_shared<const ttab::InteractionTable>(ttab::parse(index_.read(*r)));
        } catch (const std::exception& ex) {
            diag::warning(std::format("interaction table 0x{:08X} 0x{:04X} can't be read: {}", group, instance, ex.what()));
        }
    }
    interactionTables_.emplace(key, parsed);
    return parsed;
}

const std::vector<std::int16_t>* IndexCodeSource::constants(std::uint32_t group,
                                                           std::uint16_t instance) {
    group = privateCodeGroup(group);
    const std::uint64_t key = groupInstance(group, instance);
    auto it = constants_.find(key);
    if (it == constants_.end()) {
        std::optional<std::vector<std::int16_t>> values;
        if (const auto* r = index_.find({kBehaviorConstants, group, instance, 0})) {
            try {
                values = bcon::parse(index_.read(*r)).values;
            } catch (const std::exception& ex) {
                diag::warning(std::format("constants 0x{:08X} 0x{:04X} can't be read: {}", group, instance, ex.what()));
            }
        }
        it = constants_.emplace(key, std::move(values)).first;
    }
    return it->second ? &*it->second : nullptr;
}

const std::vector<objf::Function>* IndexCodeSource::objectFunctions(const ObjectType& type) {
    const std::uint32_t group = privateCodeGroup(type.group);
    const std::uint64_t key = groupInstance(group, type.instance);
    auto it = objectFunctions_.find(key);
    if (it == objectFunctions_.end()) {
        std::optional<std::vector<objf::Function>> functions;
        if (const auto* r = index_.find({objf::kType, group, type.instance, 0})) {
            try {
                functions = objf::parse(index_.read(*r));
            } catch (const std::exception& ex) {
                diag::warning(std::format("object functions 0x{:08X} 0x{:04X} can't be read: {}",
                                          group, type.instance, ex.what()));
            }
        }
        it = objectFunctions_.emplace(key, std::move(functions)).first;
    }
    return it->second ? &*it->second : nullptr;
}

const std::vector<std::uint32_t>& IndexCodeSource::objectTypeGuids() {
    objectType(0);  // reads the object types the first time
    return objectTypeGuids_;
}

const ObjectType* IndexCodeSource::addMadeObjectType(ObjectType type) {
    objectType(0);  // the saved types first, so a made one never hides one of them
    const auto [it, added] = objectTypes_.try_emplace(type.definition.guid(), std::move(type));
    return added ? &it->second : nullptr;
}

const ObjectType* IndexCodeSource::objectType(std::uint32_t guid) {
    if (!objectTypesLoaded_) {
        objectTypesLoaded_ = true;
        for (const auto& r : index_.resources()) {
            if (r.entry.key.type != types::kObjectDefinition) {
                continue;
            }
            try {
                objd::ObjectDefinition definition = objd::parse(index_.read(r));
                const std::uint32_t id = definition.guid();
                std::size_t containerSlots = 0;
                std::vector<slot::Entry> routingSlots;
                std::vector<slot::Entry> containerEntries;
                std::vector<slot::Entry> targetSlots;
                const auto slotsId = definition.field(objd::field::kSlotsId);
                if (slotsId != 0) {
                    if (const auto* s = index_.find({types::kSlots, r.entry.key.group, slotsId, 0})) {
                        try {
                            const slot::Slots slots = slot::parse(index_.read(*s));
                            containerSlots = slots.containerCount();
                            for (const auto& entry : slots.entries) {
                                if (entry.type == slot::kRoutingSlot) {
                                    routingSlots.push_back(entry);
                                } else if (entry.type == slot::kContainerSlot) {
                                    containerEntries.push_back(entry);
                                } else if (entry.type == slot::kTargetSlot) {
                                    targetSlots.push_back(entry);
                                }
                            }
                        } catch (const std::exception& ex) {
                            diag::warnOnce(std::string("objects: slots that can't be read are left out: ") + ex.what());
                        }
                    }
                }
                const std::uint16_t multiTileId = definition.field(objd::field::kMultiTileMasterId);
                if (multiTileId != 0 && definition.field(objd::field::kMultiTileSubIndex) == 0xFFFF) {
                    multiTileMasters_.try_emplace(static_cast<std::uint64_t>(r.entry.key.group) << 16 | multiTileId, id);
                } else if (multiTileId != 0 && !objectTypes_.contains(id)) {
                    multiTilePartGuids_[static_cast<std::uint64_t>(r.entry.key.group) << 16 | multiTileId].push_back(id);
                }
                ObjectType type{r.entry.key.group, std::move(definition), static_cast<std::uint16_t>(r.entry.key.instance),
                                containerSlots, std::move(routingSlots), std::move(containerEntries),
                                std::move(targetSlots)};
                if (type.definition.field(objd::field::kType) == kPersonType) {
                    constexpr std::size_t kOriginalGuidLow = 0x46, kOriginalGuidHigh = 0x47;
                    simGroups_.try_emplace(r.entry.key.group,
                                           type.definition.field(kOriginalGuidLow) |
                                               static_cast<std::uint32_t>(type.definition.field(kOriginalGuidHigh)) << 16);
                }
                if (objectTypes_.try_emplace(id, std::move(type)).second) {
                    objectTypeGuids_.push_back(id);
                }
            } catch (const std::exception& ex) {
                diag::warnOnce(std::string("objects: an object definition that can't be read is left out: ") + ex.what());
            }
        }
    }
    const auto it = objectTypes_.find(guid);
    return it != objectTypes_.end() ? &it->second : nullptr;
}

const std::map<std::string, std::string>* IndexCodeSource::ageData(std::uint32_t group) {
    constexpr std::uint32_t kAgeData = 0xAC598EAC;
    auto it = ageData_.find(group);
    if (it == ageData_.end()) {
        std::optional<std::map<std::string, std::string>> properties;
        if (const auto* r = index_.find({kAgeData, group, 1, 0})) {
            try {
                properties = propset::parse(index_.read(*r));
            } catch (const std::exception& ex) {
                diag::warning(std::format("age data 0x{:08X} can't be read: {}", group, ex.what()));
            }
        }
        it = ageData_.emplace(group, std::move(properties)).first;
    }
    return it->second ? &*it->second : nullptr;
}

std::optional<std::vector<std::uint8_t>> IndexCodeSource::characterResource(std::uint32_t type, std::uint32_t group) {
    const auto* r = index_.find({type, group, 1, 0});
    if (r == nullptr) {
        return std::nullopt;
    }
    try {
        return index_.read(*r);
    } catch (const std::exception& ex) {
        diag::warning(std::format("resource 0x{:08X} of 0x{:08X} can't be read: {}", type, group, ex.what()));
        return std::nullopt;
    }
}

std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> IndexCodeSource::groupResources(std::uint32_t type,
                                                                                                    std::uint32_t group) {
    std::vector<std::pair<std::uint32_t, const content::ResourceIndex::Resource*>> found;
    for (const auto& r : index_.resources()) {
        if (r.entry.key.type == type && r.entry.key.group == group) {
            found.emplace_back(r.entry.key.instance, &r);
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> out;
    for (const auto& [instance, r] : found) {
        try {
            out.emplace_back(instance, index_.read(*r));
        } catch (const std::exception& ex) {
            diag::warning(std::format("resource 0x{:08X} of 0x{:08X} can't be read: {}", type, group, ex.what()));
        }
    }
    return out;
}

const fcns::Tunings* IndexCodeSource::tunings() {
    constexpr std::uint32_t kTunings = 0x46434E53;
    constexpr std::uint32_t kAutonomyTunings = 2;
    if (!tunings_) {
        std::optional<fcns::Tunings> found;
        if (const auto* r = index_.find({kTunings, kGlobalGroup, kAutonomyTunings, 0})) {
            try {
                found = fcns::parse(index_.read(*r));
            } catch (const std::exception& ex) {
                diag::warnOnce(std::string("autonomy: the tuning file can't be read: ") + ex.what());
            }
        }
        tunings_ = std::move(found);
    }
    return *tunings_ ? &**tunings_ : nullptr;
}

const std::vector<slot::Entry>& IndexCodeSource::globalSlots() {
    constexpr std::uint32_t kGlobalSlots = 0x64;
    if (!globalSlots_) {
        std::vector<slot::Entry> found;
        if (const auto* r = index_.find({types::kSlots, kGlobalGroup, kGlobalSlots, 0})) {
            try {
                for (const auto& entry : slot::parse(index_.read(*r)).entries) {
                    if (entry.type == slot::kRoutingSlot) {
                        found.push_back(entry);
                    }
                }
            } catch (const std::exception& ex) {
                diag::warnOnce(std::string("objects: the global slots can't be read: ") + ex.what());
            }
        }
        globalSlots_ = std::move(found);
    }
    return *globalSlots_;
}

const ObjectType* IndexCodeSource::multiTileMaster(const ObjectType& part) {
    const std::uint16_t multiTileId = part.definition.field(objd::field::kMultiTileMasterId);
    if (multiTileId == 0) {
        return nullptr;
    }
    objectType(0);  // loads the types (and masters) on first use
    const auto it = multiTileMasters_.find(static_cast<std::uint64_t>(part.group) << 16 | multiTileId);
    return it != multiTileMasters_.end() ? objectType(it->second) : nullptr;
}

std::vector<const ObjectType*> IndexCodeSource::multiTileParts(const ObjectType& master) {
    constexpr std::size_t kLeadPartField = 0x3F;
    const std::uint16_t multiTileId = master.definition.field(objd::field::kMultiTileMasterId);
    if (multiTileId == 0) {
        return {};
    }
    objectType(0);  // loads the types (and parts) on first use
    const auto it = multiTilePartGuids_.find(static_cast<std::uint64_t>(master.group) << 16 | multiTileId);
    if (it == multiTilePartGuids_.end()) {
        return {};
    }
    std::vector<const ObjectType*> parts;
    for (const std::uint32_t guid : it->second) {
        if (const ObjectType* part = objectType(guid)) {
            parts.push_back(part);
        }
    }
    const auto lead = std::ranges::find_if(parts, [](const ObjectType* p) { return p->definition.field(kLeadPartField) != 0; });
    if (lead == parts.end()) {
        return {};
    }
    std::rotate(parts.begin(), lead, lead + 1);
    return parts;
}

const std::vector<std::string>& IndexCodeSource::strings(std::uint32_t type, std::uint32_t group, std::uint16_t instance,
                                                         bool descriptions) {
    const auto key = std::tuple{type, group, instance};
    auto& cache = descriptions ? descriptions_ : strings_;
    auto it = cache.find(key);
    if (it == cache.end()) {
        std::vector<std::string> values;
        if (const auto* r = index_.find({type, group, instance, 0})) {
            try {
                const str::StringTable table = str::parse(index_.read(*r));
                for (const str::Entry* entry : table.forLanguage(kUsEnglish)) {
                    values.push_back(descriptions ? entry->description : entry->value);
                }
            } catch (const std::exception& ex) {
                diag::warning(std::format("strings 0x{:08X} 0x{:08X} 0x{:04X} can't be read: {}",
                                          type, group, instance, ex.what()));
            }
        } else if (type == types::kStringTable && !descriptions && textTables_) {
            values = textTables_(group, instance);
        }
        it = cache.emplace(key, std::move(values)).first;
    }
    return it->second;
}

std::optional<std::string> IndexCodeSource::string(std::uint32_t group, std::uint16_t instance, std::size_t index) {
    const auto& values = strings(types::kStringTable, group, instance);
    return index < values.size() ? std::optional<std::string>(values[index]) : std::nullopt;
}

std::optional<std::string> IndexCodeSource::stringDescription(std::uint32_t group, std::uint16_t instance, std::size_t index) {
    const auto& values = strings(types::kStringTable, group, instance, true);
    return index < values.size() ? std::optional<std::string>(values[index]) : std::nullopt;
}

std::optional<std::string> IndexCodeSource::catalogString(std::uint32_t group, std::uint16_t instance, std::size_t index) {
    const auto& values = strings(types::kCatalogString, group, instance);
    return index < values.size() ? std::optional<std::string>(values[index]) : std::nullopt;
}

std::optional<tga::Pixels> IndexCodeSource::image(std::uint32_t group, std::uint32_t instance, std::uint32_t maxSide) {
    const auto* r = index_.find({types::kImage, group, instance, 0});
    if (r == nullptr) {
        return std::nullopt;
    }
    try {
        return image::decode(index_.read(*r), maxSide);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<std::string> IndexCodeSource::interactionString(std::uint32_t group, std::uint16_t instance, std::size_t index) {
    const auto& values = strings(types::kInteractionStrings, group, instance);
    return index < values.size() ? std::optional<std::string>(values[index]) : std::nullopt;
}

std::optional<std::uint16_t> IndexCodeSource::behaviorByName(std::uint32_t group,
                                                             const std::string& name) {
    group = privateCodeGroup(group);
    if (!behaviorNamesLoaded_) {
        behaviorNamesLoaded_ = true;
        for (const auto& r : index_.resources()) {
            if (r.entry.key.type != types::kBehavior) {
                continue;
            }
            const auto bhavInstance = static_cast<std::uint16_t>(r.entry.key.instance);
            auto code = behavior(r.entry.key.group, bhavInstance);
            if (!code) {
                continue;
            }
            auto& names = behaviorNames_[r.entry.key.group];
            const auto [it, inserted] = names.try_emplace(code->name, bhavInstance);
            if (!inserted && bhavInstance < it->second) {
                it->second = bhavInstance;
            }
        }
    }
    const auto group_it = behaviorNames_.find(group);
    if (group_it == behaviorNames_.end()) {
        return std::nullopt;
    }
    const auto it = group_it->second.find(name);
    if (it == group_it->second.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::uint32_t IndexCodeSource::semiGlobalGroup(std::uint32_t objectGroup) {
    const auto it = semiGlobals_.find(objectGroup);
    return it != semiGlobals_.end() ? it->second : objectGroup;
}

} // namespace ts2::sim
