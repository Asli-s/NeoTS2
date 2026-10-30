#pragma once

#include "formats/bhav.hpp"
#include "formats/fcns.hpp"
#include "formats/objd.hpp"
#include "formats/objf.hpp"
#include "formats/slot.hpp"
#include "formats/tga.hpp"
#include "formats/ttab.hpp"


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
    // How high each container slot is above the object's floor (its SLOT entry's z), in tiles.
    std::vector<float> containerSlotHeights;
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
    // Instance of the BHAV named `name` in `group` (lowest instance if several), if any.
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
    // Every object type's GUID, once each, in the order they were read (Set to Next 0x26 walks them).
    virtual const std::vector<std::uint32_t>& objectTypeGuids() {
        static const std::vector<std::uint32_t> none;
        return none;
    }
    // The properties of a Sim's age data (AGED, instance 1) in its character group, if any.
    virtual const std::map<std::string, std::string>* ageData(std::uint32_t) { return nullptr; }
    // Another resource of that type (instance 1) in a Sim's character group (an NPC's face shape), if any.
    virtual std::optional<std::vector<std::uint8_t>> characterResource(std::uint32_t, std::uint32_t) { return std::nullopt; }
    // The game's named tuning values (FCNS instance 2 of the global group), if any.
    virtual const fcns::Tunings* tunings() { return nullptr; }
    // The global routing slots (the routing entries of SLOT 0x64 in the global group, in order), which
    // Go To Routing Slot and Snap can use with any object, empty without them.
    virtual const std::vector<slot::Entry>& globalSlots() {
        static const std::vector<slot::Entry> none;
        return none;
    }
};

} // namespace ts2::sim
