#include "sim/catalog.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace ts2::sim::catalog {

namespace {

constexpr std::uint16_t kNormalObject = 4;
constexpr std::uint16_t kRewards = function::kAspirationReward | function::kCareerReward;

} // namespace

bool isBuyItem(const objd::ObjectDefinition& d) {
    if (d.field(objd::field::kType) != kNormalObject || d.field(objd::field::kPrice) == 0) {
        return false;
    }
    if (d.field(field::kRoomSort) == 0 && d.field(field::kFunctionSort) == 0) {
        return false;
    }
    if ((d.field(field::kFunctionSort) & kRewards) != 0 || d.field(field::kBuildModeType) != 0) {
        return false;
    }
    // A multi-tile object is bought as its master; its parts aren't listed.
    return d.field(objd::field::kMultiTileMasterId) == 0 || d.isMultiTileMaster();
}

std::vector<Item> buyItems(CodeSource& code) {
    std::vector<Item> items;
    std::set<std::uint32_t> seen;
    for (const std::uint32_t guid : code.objectTypeGuids()) {
        const ObjectType* type = code.objectType(guid);
        if (type == nullptr || !isBuyItem(type->definition) || !seen.insert(guid).second) {
            continue;
        }
        const auto& d = type->definition;
        Item item;
        item.guid = guid;
        item.group = type->group;
        item.catalogStrings = d.field(objd::field::kCatalogStringsId);
        item.price = d.field(objd::field::kPrice);
        item.rooms = d.field(field::kRoomSort);
        item.functions = d.field(field::kFunctionSort);
        item.subsorts = d.field(field::kFunctionSubsort);
        item.multiTile = d.isMultiTileMaster();
        items.push_back(item);
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.price < b.price; });
    return items;
}

std::vector<Item> inFunction(const std::vector<Item>& items, std::uint16_t function, std::uint16_t subsort) {
    std::vector<Item> out;
    for (const Item& item : items) {
        if ((item.functions & function) != 0 && (subsort == 0 || (item.subsorts & subsort) != 0)) {
            out.push_back(item);
        }
    }
    return out;
}

std::vector<Item> inRoom(const std::vector<Item>& items, std::uint16_t room) {
    std::vector<Item> out;
    for (const Item& item : items) {
        if ((item.rooms & room) != 0) {
            out.push_back(item);
        }
    }
    return out;
}

std::uint16_t subsortOfButton(std::uint32_t id) {
    // 0xBnk0: n the category (1-9), k the button (2 and up; 1 is Back).
    if ((id & 0xF00F) != 0xB000) {
        return 0;
    }
    const std::uint32_t category = (id >> 8) & 0xF, k = (id >> 4) & 0xF;
    if (category < 1 || category > 9 || k < 2) {
        return 0;
    }
    return static_cast<std::uint16_t>(1u << (k - 2));
}

std::int32_t placedValue(const objd::ObjectDefinition& d) {
    const std::int32_t price = d.field(objd::field::kPrice);
    const std::int32_t limit = d.field(field::kDepreciationLimit);
    const std::int32_t value = price - d.field(field::kInitialDepreciation);
    return std::clamp(value, std::min(limit, price), price);
}

bool fitsSlotHeight(std::uint16_t allowedHeights, std::int32_t slotHeight) {
    return slotHeight >= 1 && slotHeight <= 16 && ((allowedHeights >> (slotHeight - 1)) & 1) != 0;
}

bool wallsWhereNeeded(std::int16_t wallFlags, std::uint8_t sides, std::int16_t facing) {
    // The tile side each facing points to: 0 low y, 2 high x, 4 high y, 6 low x.
    constexpr std::uint8_t kLowY = 0x01, kLowX = 0x02, kHighY = 0x04, kHighX = 0x08;
    const auto sideTowards = [](int direction) -> std::uint8_t {
        switch (((direction % 8) + 8) % 8) {
        case 0: return kLowY;
        case 2: return kHighX;
        case 4: return kHighY;
        case 6: return kLowX;
        default: return 0;  // diagonal facings aren't known
        }
    };
    // In front, on the right, behind and on the left, a quarter turn apart.
    constexpr std::array<std::pair<int, int>, 4> kNeeds = {{{0x1, 0}, {0x2, 2}, {0x4, 4}, {0x8, 6}}};
    for (const auto& [bit, turn] : kNeeds) {
        if ((wallFlags & bit) != 0) {
            const std::uint8_t side = sideTowards(facing + turn);
            if (side == 0 || (sides & side) == 0) {
                return false;
            }
        }
    }
    return true;
}

} // namespace ts2::sim::catalog
