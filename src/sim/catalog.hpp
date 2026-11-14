#pragma once

#include "formats/objd.hpp"
#include "sim/code_source.hpp"

#include <cstdint>
#include <vector>

namespace ts2::sim::catalog {

// The OBJD fields Buy mode reads, by the names the game's own field table gives them (STR# 0xCC of
// the global objects).
namespace field {
constexpr std::size_t kInitialDepreciation = 0x23;
constexpr std::size_t kDailyDepreciation = 0x24;
constexpr std::size_t kSelfDepreciating = 0x25;
constexpr std::size_t kDepreciationLimit = 0x26;
constexpr std::size_t kRoomSort = 0x27;
constexpr std::size_t kFunctionSort = 0x28;
constexpr std::size_t kBuildModeType = 0x45;  // the Build catalog category, one bit (see buildSort)
constexpr std::size_t kBuildModeSubsort = 0x4A;
constexpr std::size_t kFunctionSubsort = 0x5E;
} // namespace field

// Object data 0x29, "current value" in the game's object data table (STR# 0x8D): what the object is
// worth now, which selling it gives back.
constexpr std::size_t kCurrentValueData = 0x29;

// The function sort flags (OBJD 0x28), one bit per category, in the order of the Buy panel's
// buttons 0xA100 to 0xA900 (Comfort, Surfaces, Appliances, Electronics, Plumbing, Decorative,
// Miscellaneous, Lighting, Hobbies). 0x400 and 0x800 mark aspiration and career rewards.
namespace function {
constexpr std::uint16_t kSeating = 0x001, kSurfaces = 0x002, kAppliances = 0x004, kElectronics = 0x008, kPlumbing = 0x010,
                        kDecorative = 0x020, kMiscellaneous = 0x040, kLighting = 0x080, kHobbies = 0x100;
constexpr std::uint16_t kAspirationReward = 0x400, kCareerReward = 0x800;
} // namespace function

// The room sort flags (OBJD 0x27), one bit per room.
namespace room {
constexpr std::uint16_t kKitchen = 0x001, kBedroom = 0x002, kBathroom = 0x004, kLivingRoom = 0x008, kOutside = 0x010,
                        kDining = 0x020, kMisc = 0x040, kStudy = 0x080, kKids = 0x100;
} // namespace room

// An object Buy mode can sell.
struct Item {
    std::uint32_t guid = 0;
    std::uint32_t group = 0;
    std::uint16_t catalogStrings = 0;  // OBJD 0x29, the instance of its catalog strings (name, description)
    std::int32_t price = 0;
    std::uint16_t rooms = 0, functions = 0, subsorts = 0;
    bool multiTile = false;  // a multi-tile object's master (a bed, a sofa)
    bool operator==(const Item&) const = default;
};

// Whether an object type is one Buy mode shows: a normal object (OBJD type 4) with a price, sorted
// into a room or a function, not a reward (those come from the rewards panel), not a part of a
// multi-tile object (its master is shown) and not a Build mode object (OBJD 0x45 set).
bool isBuyItem(const objd::ObjectDefinition& definition);

// Every object Buy mode shows, the cheapest first (the game's order), each type once.
std::vector<Item> buyItems(CodeSource& code);

// The items in a function category (one bit of the function sort flags), and of those in one
// sub-category when `subsort` isn't 0 (one bit of OBJD 0x5E).
std::vector<Item> inFunction(const std::vector<Item>& items, std::uint16_t function, std::uint16_t subsort = 0);
// The items for a room (one bit of the room sort flags).
std::vector<Item> inRoom(const std::vector<Item>& items, std::uint16_t room);


// The Build catalog's categories of objects (OBJD 0x45) and, for doors and windows, their
// sub-categories (OBJD 0x4A), as the game's own objects have them.
namespace buildSort {
constexpr std::uint16_t kDoorsAndWindows = 0x08;
constexpr std::uint16_t kDoors = 0x01, kTallWindows = 0x02, kWindows = 0x04, kGates = 0x08, kArches = 0x10, kMultiStory = 0x20;
} // namespace buildSort

// Whether an object type is one Build mode's Doors & Windows catalog shows: a door (OBJD type 8) or
// window (type 9) with a price in that category, but not a gate (the fence tools' own) nor a part
// of a multi-tile one.
bool isDoorOrWindow(const objd::ObjectDefinition& definition);

// Every door, window and arch Build mode sells, the cheapest first, each type once: Item's
// `functions` holds its category bits and `subsorts` its sub-category bits.
std::vector<Item> doorsAndWindows(CodeSource& code);
// The sub-category bit of a Buy panel sub-category button: the panels number them 0xBn20, 0xBn30
// and so on (0xBn10 is Back), the n-th of which is bit n - 2. 0 for other ids.
std::uint16_t subsortOfButton(std::uint32_t id);

// What a newly bought object is worth once placed: its price less its initial depreciation (OBJD
// 0x23), but not below its depreciation limit (OBJD 0x26).
std::int32_t placedValue(const objd::ObjectDefinition& definition);

// Whether an object fits a container slot by height: a slot of height n (its SLOT entry's
// containerKind: 2 coffee tables, 4 counters, 8 end tables, 9 sinks, 10 under counters) takes an
// object whose allowed heights (object data 4) have bit n - 1. Bit 0 is the floor.
bool fitsSlotHeight(std::uint16_t allowedHeights, std::int32_t slotHeight);

// Whether a tile's walls are where an object's wall placement flags (object data 0x0D) need them,
// by its facing (0, 2, 4 or 6): bit 0x1 a wall in front, 0x2 on its right, 0x4 behind it (posters,
// paintings, mirrors, sconces), 0x8 on its left. `sides` are the tile's wall sides as WallTiles
// gives them (0x01 low y, 0x02 low x, 0x04 high y, 0x08 high x). Facing 0 points to low y, 2 to
// high x, 4 to high y and 6 to low x.
bool wallsWhereNeeded(std::int16_t wallFlags, std::uint8_t sides, std::int16_t facing);

} // namespace ts2::sim::catalog
