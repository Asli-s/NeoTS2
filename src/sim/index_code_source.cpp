#include "sim/index_code_source.hpp"

#include "formats/bcon.hpp"
#include "formats/glob.hpp"
#include "formats/hash.hpp"
#include "formats/objd.hpp"
#include "formats/resource_types.hpp"
#include "formats/slot.hpp"
#include "formats/str.hpp"

#include <exception>

namespace ts2::sim {

namespace {

constexpr std::uint32_t kBehaviorConstants = 0x42434F4E;
constexpr std::uint32_t kSemiGlobalReference = 0x474C4F42;
constexpr std::uint8_t kUsEnglish = 1;

std::uint64_t groupInstance(std::uint32_t group, std::uint16_t instance) {
    return static_cast<std::uint64_t>(group) << 32 | instance;
}

} // namespace

IndexCodeSource::IndexCodeSource(content::ResourceIndex& index) : index_(index) {
    for (const auto& r : index_.resources()) {
        if (r.entry.key.type != kSemiGlobalReference) {
            continue;
        }
        try {
            const auto glob = glob::parse(index_.read(r));
            semiGlobals_.emplace(r.entry.key.group, hash::groupId(glob.semiGlobalName));
        } catch (const std::exception&) {
        }
    }
}

std::shared_ptr<const bhav::Behavior> IndexCodeSource::behavior(std::uint32_t group,
                                                                std::uint16_t instance) {
    const std::uint64_t key = groupInstance(group, instance);
    if (const auto it = behaviors_.find(key); it != behaviors_.end()) {
        return it->second;
    }

    std::shared_ptr<const bhav::Behavior> parsed;
    if (const auto* r = index_.find({types::kBehavior, group, instance, 0})) {
        parsed = std::make_shared<const bhav::Behavior>(bhav::parse(index_.read(*r)));
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
        } catch (const std::exception&) {
        }
    }
    interactionTables_.emplace(key, parsed);
    return parsed;
}

const std::vector<std::int16_t>* IndexCodeSource::constants(std::uint32_t group,
                                                           std::uint16_t instance) {
    const std::uint64_t key = groupInstance(group, instance);
    auto it = constants_.find(key);
    if (it == constants_.end()) {
        std::optional<std::vector<std::int16_t>> values;
        if (const auto* r = index_.find({kBehaviorConstants, group, instance, 0})) {
            values = bcon::parse(index_.read(*r)).values;
        }
        it = constants_.emplace(key, std::move(values)).first;
    }
    return it->second ? &*it->second : nullptr;
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
                const auto slotsId = definition.field(objd::field::kSlotsId);
                if (slotsId != 0) {
                    if (const auto* s = index_.find({types::kSlots, r.entry.key.group, slotsId, 0})) {
                        try {
                            containerSlots = slot::parse(index_.read(*s)).containerCount();
                        } catch (const std::exception&) {
                        }
                    }
                }
                objectTypes_.try_emplace(id, ObjectType{r.entry.key.group, std::move(definition), containerSlots});
            } catch (const std::exception&) {
            }
        }
    }
    const auto it = objectTypes_.find(guid);
    return it != objectTypes_.end() ? &it->second : nullptr;
}

std::optional<std::string> IndexCodeSource::string(std::uint32_t group, std::uint16_t instance,
                                                   std::size_t index) {
    const std::uint64_t key = groupInstance(group, instance);
    auto it = strings_.find(key);
    if (it == strings_.end()) {
        std::vector<std::string> values;
        if (const auto* r = index_.find({types::kStringTable, group, instance, 0})) {
            try {
                const str::StringTable table = str::parse(index_.read(*r));
                for (const str::Entry* entry : table.forLanguage(kUsEnglish)) {
                    values.push_back(entry->value);
                }
            } catch (const std::exception&) {
            }
        }
        it = strings_.emplace(key, std::move(values)).first;
    }
    if (index >= it->second.size()) {
        return std::nullopt;
    }
    return it->second[index];
}

std::optional<std::uint16_t> IndexCodeSource::behaviorByName(std::uint32_t group,
                                                             const std::string& name) {
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
