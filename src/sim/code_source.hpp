#pragma once

#include "formats/bhav.hpp"
#include "formats/dbpf.hpp"
#include "formats/fcns.hpp"
#include "formats/objd.hpp"
#include "formats/objf.hpp"
#include "formats/slot.hpp"
#include "formats/tga.hpp"
#include "formats/ttab.hpp"


#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ts2::sim {

constexpr std::uint32_t kGlobalGroup = 0x7FD46CD0;

struct ObjectType {
    std::uint32_t group = 0;
    objd::ObjectDefinition definition;
    std::uint16_t instance = 0;  // of its OBJD, which its OBJf shares
    // Container slots a new object of this type has: the type-0 entries of its SLOT resource
    // (OBJD "slots id"), 0 without one.
    std::size_t containerSlots = 0;
    // Its routing slots (type-3 entries of the SLOT resource) in their order, which Snap numbers from 0.
    std::vector<slot::Entry> routingSlots;
    // Its container slots (type-0 entries of the SLOT resource) in their order.
    std::vector<slot::Entry> containerSlotEntries;
    // Its target slots (type-4 entries of the SLOT resource) in their order.
    std::vector<slot::Entry> targetSlots;
};

class CodeSource {
public:
    virtual ~CodeSource() = default;

    virtual std::shared_ptr<const bhav::Behavior> behavior(std::uint32_t group,
                                                           std::uint16_t instance) = 0;
    virtual const std::vector<std::int16_t>* constants(std::uint32_t group,
                                                       std::uint16_t instance) = 0;
    virtual std::uint32_t semiGlobalGroup(std::uint32_t objectGroup) = 0;
    virtual const ObjectType* objectType(std::uint32_t guid) = 0;

    // US English entry `index` of string table (STR#) `instance` in `group`, if any.
    [[nodiscard]] virtual std::optional<std::string> string(std::uint32_t group, std::uint16_t instance,
                                              std::size_t index) = 0;
    // The description of that entry, if any.
    [[nodiscard]] virtual std::optional<std::string> stringDescription(std::uint32_t, std::uint16_t, std::size_t) { return std::nullopt; }
    // Picture `instance` (an image resource) in `group`, as the game's thumbnail service shows an
    // object (a career's icon is instance 1), nullopt when there is none or it can't be read. At most
    // `maxSide` pixels a side.
    [[nodiscard]] virtual std::optional<tga::Pixels> image(std::uint32_t, std::uint32_t, std::uint32_t) { return std::nullopt; }
    // US English entry `index` of catalog string table (CTSS) `instance` in `group`, if any.
    [[nodiscard]] virtual std::optional<std::string> catalogString(std::uint32_t, std::uint16_t, std::size_t) { return std::nullopt; }
    // US English entry `index` of pie-menu string table (TTAs) `instance` in `group`, if any.
    [[nodiscard]] virtual std::optional<std::string> interactionString(std::uint32_t, std::uint16_t, std::size_t) { return std::nullopt; }
    // Instance of the BHAV named `name` in `group` (lowest instance if several), if any. A Sim's group
    // without trees is searched as behavior() reads it: in the template it was made from.
    [[nodiscard]] virtual std::optional<std::uint16_t> behaviorByName(std::uint32_t group,
                                                        const std::string& name) = 0;
    // Interaction table (TTAB) `instance` of `group`, if any.
    virtual std::shared_ptr<const ttab::InteractionTable> interactionTable(std::uint32_t group,
                                                                          std::uint16_t instance) = 0;
    // An object type's functions (OBJf), if it has them.
    virtual const std::vector<objf::Function>* objectFunctions(const ObjectType&) { return nullptr; }
    // Adds the type of an object made during play (a new character's Sim), found by its GUID from
    // then on. Null when this source can't hold one.
    virtual const ObjectType* addMadeObjectType(ObjectType) { return nullptr; }
    // The master of a multi-tile object's part: the type in the same group with the same multi-tile
    // id whose sub-index (OBJD field 0x0B) is 0xFFFF. Null for other types or when it is missing.
    virtual const ObjectType* multiTileMaster(const ObjectType&) { return nullptr; }
    // The parts of a multi-tile master in the order the game makes them: the lead (OBJD field 0x3F
    // set) first, then the others in the order the types were read. Empty when there is no lead.
    virtual std::vector<const ObjectType*> multiTileParts(const ObjectType&) { return {}; }
    // Every object type's GUID, once each, in the order they were read (Set to Next 0x26 walks them).
    virtual const std::vector<std::uint32_t>& objectTypeGuids() {
        static const std::vector<std::uint32_t> none;
        return none;
    }
    // The properties of a Sim's age data (AGED, instance 1) in its character group, if any.
    virtual const std::map<std::string, std::string>* ageData(std::uint32_t) { return nullptr; }
    // Another resource of that type (instance 1) in a Sim's character group (an NPC's face shape), if any.
    virtual std::optional<std::vector<std::uint8_t>> characterResource(std::uint32_t, std::uint32_t) { return std::nullopt; }
    // Every resource of a type in an object's group (an outfit object's age data), with its instance,
    // in instance order.
    virtual std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> groupResources(std::uint32_t, std::uint32_t) {
        return {};
    }
    // Every resource of a group, of any type, with its key (the template a new character is made
    // from), the highest pack's version of each key.
    virtual std::vector<std::pair<dbpf::ResourceKey, std::vector<std::uint8_t>>> everyGroupResource(std::uint32_t) { return {}; }
    // The game's named tuning values (FCNS instance 2 of the global group), if any.
    virtual const fcns::Tunings* tunings() { return nullptr; }
    // The global routing slots (the routing entries of SLOT 0x64 in the global group, in order), which
    // Go To Routing Slot and Snap can use with any object, empty without them.
    virtual const std::vector<slot::Entry>& globalSlots() {
        static const std::vector<slot::Entry> none;
        return none;
    }
};

// The container slots an object of `type` has when made or loaded. A Sim's follow the person it was
// made from (OBJD 0x46-0x47, its original GUID) when that has more, as newer packs give the person
// more slots than a Sim's own copy has.
inline std::size_t containerSlotsOf(CodeSource& code, const ObjectType& type) {
    constexpr std::uint16_t kPerson = 2;
    constexpr std::size_t kOriginalGuidLow = 0x46, kOriginalGuidHigh = 0x47;
    if (type.definition.field(objd::field::kType) != kPerson) {
        return type.containerSlots;
    }
    const std::uint32_t guid = type.definition.field(kOriginalGuidLow) |
                               static_cast<std::uint32_t>(type.definition.field(kOriginalGuidHigh)) << 16;
    const ObjectType* original = guid != 0 ? code.objectType(guid) : nullptr;
    return original != nullptr ? std::max(type.containerSlots, original->containerSlots) : type.containerSlots;
}

// A slot entry's bone names: the first is the bone, as entry `bone` of the "Model - Bones" string
// table 0x90 of `type`. Sims' character files mostly carry no such table and read the template
// person's, as the Sim's model does. None for a bone past the table or below 0.
inline std::optional<std::string> slotBoneNames(CodeSource& code, const ObjectType& type, std::int32_t bone) {
    constexpr std::uint16_t kBoneNames = 0x90;
    constexpr std::uint16_t kPerson = 2;
    constexpr std::uint32_t kTemplatePersonGuid = 0x7FD96B54;
    if (bone < 0) {
        return std::nullopt;
    }
    std::uint32_t group = type.group;
    if (type.definition.field(objd::field::kType) == kPerson && !code.string(group, kBoneNames, 0)) {
        const ObjectType* templatePerson = code.objectType(kTemplatePersonGuid);
        group = templatePerson != nullptr ? templatePerson->group : group;
    }
    return code.string(group, kBoneNames, static_cast<std::size_t>(bone));
}

// Object data 0x37 of an object of `type` made during play, as every saved object has it: the
// type's number of target slots.
constexpr std::size_t kTargetSlotCountField = 0x37;
inline std::int16_t targetSlotCount(const ObjectType& type) {
    return static_cast<std::int16_t>(std::min<std::size_t>(type.targetSlots.size(), 0x7FFF));
}

// How many semi-global attributes an object of `type` gets: the labels in its semi-global's string
// table 0x100, or 0 without a semi-global. An object made during play keeps it in object data 0x4F.
// Objects saved under older packs hold smaller counts there, which a load keeps.
constexpr std::size_t kSemiAttributeCountField = 0x4F;
inline std::int16_t semiAttributeCount(CodeSource& code, const ObjectType& type) {
    const std::uint32_t semiGlobal = code.semiGlobalGroup(type.group);
    if (semiGlobal == type.group) {
        return 0;
    }
    constexpr std::uint16_t kAttributeLabels = 0x100;
    constexpr std::int16_t kMaxLabels = 0x7FFF;
    std::int16_t count = 0;
    while (count < kMaxLabels && code.string(semiGlobal, kAttributeLabels, static_cast<std::size_t>(count))) {
        ++count;
    }
    return count;
}

} // namespace ts2::sim
