// Buy mode's catalog: which objects are sold, how they sort, and what a placed one is worth.
#include "test_support.hpp"

#include "sim/catalog.hpp"

namespace {

namespace catalog = ts2::sim::catalog;
using ts2::objd::ObjectDefinition;

constexpr std::uint32_t kGroup = 0x7F000001;

ObjectDefinition object(std::uint32_t guid, std::uint16_t price, std::uint16_t rooms, std::uint16_t functions, std::uint16_t subsorts = 0) {
    ObjectDefinition d;
    d.fields.assign(ts2::objd::kMaxFields, 0);
    d.fields[ts2::objd::field::kType] = 4;  // a normal object
    d.fields[ts2::objd::field::kGuid1] = static_cast<std::uint16_t>(guid);
    d.fields[ts2::objd::field::kGuid2] = static_cast<std::uint16_t>(guid >> 16);
    d.fields[ts2::objd::field::kPrice] = price;
    d.fields[catalog::field::kRoomSort] = rooms;
    d.fields[catalog::field::kFunctionSort] = functions;
    d.fields[catalog::field::kFunctionSubsort] = subsorts;
    return d;
}

// The game sells normal objects with a price and a room or function sort, but not rewards, Build mode
// objects or the parts of multi-tile objects (their master is sold).
void buyModeSellsWhatTheCatalogShows() {
    CHECK(catalog::isBuyItem(object(1, 80, catalog::room::kOutside, catalog::function::kSeating)));
    CHECK(!catalog::isBuyItem(object(1, 0, catalog::room::kOutside, catalog::function::kSeating)));  // no price
    CHECK(!catalog::isBuyItem(object(1, 80, 0, 0)));                                                 // in no sort
    CHECK(!catalog::isBuyItem(object(1, 80, 0, catalog::function::kAspirationReward | catalog::function::kDecorative)));
    auto build = object(1, 80, catalog::room::kMisc, catalog::function::kMiscellaneous);
    build.fields[catalog::field::kBuildModeType] = 1;
    CHECK(!catalog::isBuyItem(build));
    auto person = object(1, 80, catalog::room::kMisc, catalog::function::kMiscellaneous);
    person.fields[ts2::objd::field::kType] = 2;
    CHECK(!catalog::isBuyItem(person));
    auto master = object(1, 450, catalog::room::kBedroom, catalog::function::kSeating);
    master.fields[ts2::objd::field::kMultiTileMasterId] = 5;
    master.fields[ts2::objd::field::kMultiTileSubIndex] = 0xFFFF;
    auto part = master;
    part.fields[ts2::objd::field::kMultiTileSubIndex] = 0x0001;
    CHECK(catalog::isBuyItem(master) && !catalog::isBuyItem(part));
}

// The catalog lists each object once, cheapest first, and sorts by function, sub-category and room.
void theCatalogSortsByFunctionAndRoom() {
    MemoryCodeSource code;
    code.addObjectType(kGroup, object(0x100, 900, catalog::room::kKitchen, catalog::function::kAppliances, 0x02));
    code.addObjectType(kGroup, object(0x200, 80, catalog::room::kOutside | catalog::room::kDining, catalog::function::kSeating, 0x01));
    code.addObjectType(kGroup, object(0x300, 300, catalog::room::kBathroom, catalog::function::kPlumbing, 0x01));
    code.addObjectType(kGroup, object(0x400, 0, catalog::room::kBathroom, catalog::function::kPlumbing));  // not for sale
    const auto items = catalog::buyItems(code);
    CHECK(items.size() == 3);
    CHECK(items.size() == 3 && items[0].guid == 0x200 && items[1].guid == 0x300 && items[2].guid == 0x100);
    CHECK(items.size() == 3 && items[0].price == 80 && items[0].group == kGroup);
    CHECK(catalog::inFunction(items, catalog::function::kAppliances).size() == 1);
    CHECK(catalog::inFunction(items, catalog::function::kAppliances, 0x02).size() == 1);
    CHECK(catalog::inFunction(items, catalog::function::kAppliances, 0x01).empty());
    CHECK(catalog::inRoom(items, catalog::room::kDining).size() == 1 && catalog::inRoom(items, catalog::room::kDining)[0].guid == 0x200);
    CHECK(catalog::inRoom(items, catalog::room::kKids).empty());
}

// A sub-category panel numbers its buttons 0xBn20, 0xBn30, ...: the n-th is sub-sort bit n - 2.
// 0xBn10 is Back.
void subCategoryButtonsPickSubsortBits() {
    CHECK(catalog::subsortOfButton(0xB420) == 0x01);  // Electronics: Entertainment
    CHECK(catalog::subsortOfButton(0xB430) == 0x02);  // TVs and Computers
    CHECK(catalog::subsortOfButton(0xB440) == 0x04);  // Audio
    CHECK(catalog::subsortOfButton(0xB410) == 0);     // Back
    CHECK(catalog::subsortOfButton(0xA100) == 0 && catalog::subsortOfButton(0xC100) == 0);
}

// Once placed, a bought object is worth its price less its initial depreciation, but not less than
// its depreciation limit.
void aPlacedObjectLosesItsInitialDepreciation() {
    auto stereo = object(1, 375, catalog::room::kLivingRoom, catalog::function::kElectronics);
    stereo.fields[catalog::field::kInitialDepreciation] = 56;
    stereo.fields[catalog::field::kDepreciationLimit] = 150;
    CHECK(catalog::placedValue(stereo) == 319);
    stereo.fields[catalog::field::kInitialDepreciation] = 300;
    CHECK(catalog::placedValue(stereo) == 150);
    auto painting = object(1, 35, catalog::room::kKids, catalog::function::kDecorative);  // no depreciation
    CHECK(catalog::placedValue(painting) == 35);
}

} // namespace

std::vector<Test> catalogTests() {
    return {
        {"Buy mode sells what the catalog shows", buyModeSellsWhatTheCatalogShows},
        {"the catalog sorts by function and room", theCatalogSortsByFunctionAndRoom},
        {"sub-category buttons pick sub-sort bits", subCategoryButtonsPickSubsortBits},
        {"a placed object loses its initial depreciation", aPlacedObjectLosesItsInitialDepreciation},
    };
}
