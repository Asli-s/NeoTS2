#pragma once

#include "formats/bhav.hpp"
#include "formats/objd.hpp"
#include "formats/ttab.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ts2::sim {

constexpr std::uint32_t kGlobalGroup = 0x7FD46CD0;

struct ObjectType {
    std::uint32_t group = 0;
    objd::ObjectDefinition definition;
    // Container slots a new object of this type has: the type-0 entries of its SLOT resource
    // (OBJD "slots id"); 0 without one.
    std::size_t containerSlots = 0;
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
    virtual std::optional<std::string> string(std::uint32_t group, std::uint16_t instance,
                                              std::size_t index) = 0;
    // Instance of the BHAV named `name` in `group` (lowest instance if several), if any.
    virtual std::optional<std::uint16_t> behaviorByName(std::uint32_t group,
                                                        const std::string& name) = 0;
    // Interaction table (TTAB) `instance` of `group`, if any.
    virtual std::shared_ptr<const ttab::InteractionTable> interactionTable(std::uint32_t group,
                                                                          std::uint16_t instance) = 0;
};

} // namespace ts2::sim
