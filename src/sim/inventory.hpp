#pragma once

#include "formats/ngbh.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace ts2::sim {

// A token in an inventory: an object type's GUID, flags (1 visible, 2 memory, 4 marked for
// deletion, 8 shopping), a category and a list of values.
struct InventoryToken {
    std::uint32_t guid = 0;
    std::uint16_t flags = 0;
    std::uint16_t value1 = 0;
    std::uint32_t value2 = 0;
    std::uint16_t category = 0;
    std::vector<std::int16_t> properties;
};

// Counted tokens keep their count in their first property; singular tokens are one each. Tokens
// are shared so that a script holding one keeps it after it leaves the inventory.
struct Inventory {
    std::vector<std::shared_ptr<InventoryToken>> counted;
    std::vector<std::shared_ptr<InventoryToken>> singular;
};

enum class InventoryKind { Global = 0, Lot = 1, Family = 2, Neighbor = 3, GameWide = 4 };

inline std::vector<std::shared_ptr<InventoryToken>> toTokens(const std::vector<xobj::Token>& saved) {
    std::vector<std::shared_ptr<InventoryToken>> tokens;
    for (const auto& t : saved) {
        tokens.push_back(std::make_shared<InventoryToken>(
            InventoryToken{t.guid, t.flags, t.value1, t.value2, t.value3, t.properties}));
    }
    return tokens;
}

inline Inventory toInventory(const ngbh::Inventory& saved) {
    return Inventory{toTokens(saved.counted), toTokens(saved.singular)};
}

} // namespace ts2::sim
