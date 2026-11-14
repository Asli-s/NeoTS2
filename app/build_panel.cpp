#include "build_panel.hpp"

#include "content/ui_meters.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11, kImages = 0x499DB772;
// The tools: the hand (lit), the Build Mode tab (lit), and the others (not done).
constexpr std::uint32_t kHand = 0x000000A5, kBuildTab = 0x000000A6;
constexpr std::array<std::uint32_t, 8> kToolsNotDone = {0xA1, 0xA2, 0xA3, 0xA4, 0xAA, 0xAB, 0xAE, 0xAF};
// The category buttons' group, the Floors and Wall Coverings buttons, and their sub-category
// panels, each with Back.
constexpr std::uint32_t kCategories = 0x0000A000, kFloors = 0x0000A400, kFloorSorts = 0x0000B400, kFloorBack = 0x0000B410;
constexpr std::uint32_t kWalls = 0x0000A500, kWallSorts = 0x0000B500, kWallBack = 0x0000B510;
constexpr std::uint32_t kDoorsWindows = 0x0000A300, kDoorSorts = 0x0000B300, kDoorBack = 0x0000B310;
// Terrain: its panel with Back, Elevation, Paint (done), Water and Flatten Lot (not done), and the
// brush sizes, small to large.
constexpr std::uint32_t kTerrain = 0x0000A700, kTerrainSorts = 0x0000B700, kTerrainBack = 0x0000B710, kElevation = 0x0000B720,
                        kPaintTool = 0x0000B730, kWater = 0x0000B740, kFlattenLot = 0x000000AD;
constexpr std::array<std::uint32_t, 3> kBrushes = {0xB751, 0xB752, 0xB753};
// The Wall Tool category: its panel with Back, Walls & Rooms (done) and Half Walls (not done).
constexpr std::uint32_t kWallTool = 0x0000A100, kWallToolSorts = 0x0000B100, kWallToolBack = 0x0000B110, kWallsAndRooms = 0x0000B120,
                        kHalfWalls = 0x0000B130;
// The sub-category buttons and the catalog's sub-sorts.
struct SortButton {
    std::uint32_t id;
    const char* subsort;
};
constexpr std::array<SortButton, 22> kSortButtons = {{{0xB320, "doors"},
                                                      {0xB330, "tallwindows"},
                                                      {0xB340, "windows"},
                                                      {0xB350, "arches"},
                                                      {0xB370, "multistory"},
                                                      {0xB420, "stone"},
                                                      {0xB430, "brick"},
                                                      {0xB440, "wood"},
                                                      {0xB450, "carpet"},
                                                      {0xB460, "tile"},
                                                      {0xB470, "lino"},
                                                      {0xB480, "poured"},
                                                      {0xB490, "other"},
                                                      {0xB520, "brick"},
                                                      {0xB530, "masonry"},
                                                      {0xB540, "siding"},
                                                      {0xB550, "poured"},
                                                      {0xB560, "paint"},
                                                      {0xB570, "wallpaper"},
                                                      {0xB580, "tile"},
                                                      {0xB590, "paneling"},
                                                      {0xB5A1, "other"}}};
constexpr std::uint32_t kAllFloors = 0xB4A0, kAllWalls = 0xB5A0, kAllDoors = 0xB360;
// The catalog and the auto roof options beside the panel; in the catalog its normal view (the
// others aren't done), and there the item area, its mark, the page buttons, scrollbar, Expand,
// Organize Items and the custom content buttons. The catalog's info text.
constexpr std::uint32_t kCatalog = 0xAD502BBF, kAutoRoofOptions = 0x000000AC;
constexpr std::uint32_t kNormal = 0x1000, kExpanded = 0x2000, kWholesale = 0x3300, kWholesaleExpanded = 0x5500, kInfo = 0x4000;
constexpr std::uint32_t kArea = 0x1006, kAreaMark = 0x1007, kPrevious = 0x1004, kNext = 0x1005, kScrollbar = 0x1008, kExpand = 0x1009,
                        kCustomContent = 0x100A, kOrganize = 0x3001;
// A catalog item, as in Buy mode: the game's thumbnail made smaller, four across and two down.
constexpr std::uint32_t kThumbnail = 0xFEEDEABE, kThumbBackground = 0xA001, kThumbPicture = 0xA002, kThumbOverlay = 0xA003;
constexpr std::uint32_t kThumbMask = 0x000B0357;
constexpr int kColumns = 4, kRows = 2, kItemSide = 44, kPictureInset = 3;

// "§1,234".
std::string priceText(std::int32_t price) {
    std::string digits = std::to_string(std::abs(static_cast<long long>(price)));
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<std::size_t>(i), ",");
    }
    return "\xC2\xA7" + digits;
}

} // namespace

void BuildPanel::attach(ts2::ui::Screen& screen, int content) {
    screen_ = &screen;
    content_ = content;
    if (content < 0) {
        return;
    }
    auto& s = screen;
    s.setVisible(s.find(content, kAutoRoofOptions), false);
    s.setChecked(s.find(content, kHand), true);
    s.setChecked(s.find(content, kBuildTab), true);
    for (const std::uint32_t id : kToolsNotDone) {
        s.setEnabled(s.find(content, id), false);
    }
    // Of the categories only Floors and Wall Coverings are done.
    categories_ = s.find(content, kCategories);
    if (categories_ >= 0) {
        for (const int category : s.children(categories_)) {
            s.setEnabled(category, s.id(category) == kFloors || s.id(category) == kWalls || s.id(category) == kWallTool ||
                                       s.id(category) == kDoorsWindows || s.id(category) == kTerrain);
        }
    }
    floorSorts_ = s.find(content, kFloorSorts);
    wallSorts_ = s.find(content, kWallSorts);
    doorSorts_ = s.find(content, kDoorSorts);
    terrainSorts_ = s.find(content, kTerrainSorts);
    for (const std::uint32_t id : {kElevation, kWater, kFlattenLot}) {
        s.setEnabled(s.find(terrainSorts_, id), false);
    }
    wallToolSorts_ = s.find(content, kWallToolSorts);
    s.setEnabled(s.find(wallToolSorts_, kHalfWalls), false);
    catalog_ = s.find(content, kCatalog);
    for (const std::uint32_t id : {kExpanded, kWholesale, kWholesaleExpanded, kInfo}) {
        s.setVisible(s.find(catalog_, id), false);
    }
    const int normal = s.find(catalog_, kNormal);
    s.setVisible(normal, true);
    area_ = s.find(normal, kArea);
    s.setVisible(area_, true);
    s.setVisible(s.find(normal, kAreaMark), false);
    s.setVisible(s.find(normal, kScrollbar), false);
    s.setVisible(s.find(normal, kCustomContent), false);
    s.setEnabled(s.find(normal, kOrganize), false);
    s.setEnabled(s.find(normal, kExpand), false);
    previous_ = s.find(normal, kPrevious);
    next_ = s.find(normal, kNext);
    mask_ = s.data().image(kImages, kThumbMask).value_or(ts2::tga::Pixels{});
    if (area_ >= 0) {
        const int cellWidth = s.width(area_) / kColumns, cellHeight = s.height(area_) / kRows;
        for (int i = 0; i < kColumns * kRows; ++i) {
            const int x = (i % kColumns) * cellWidth + (cellWidth - kItemSide) / 2;
            const int y = (i / kColumns) * cellHeight + (cellHeight - kItemSide) / 2;
            const int slot = s.load(kScripts, kThumbnail, x, y, area_);
            if (slot < 0) {
                break;
            }
            s.resize(slot, kItemSide, kItemSide);
            for (const std::uint32_t id : {kThumbBackground, kThumbOverlay}) {
                const int part = s.find(slot, id);
                s.move(part, 0, 0);
                s.resize(part, kItemSide, kItemSide);
            }
            const int picture = s.find(slot, kThumbPicture);
            s.move(picture, kPictureInset, kPictureInset);
            s.resize(picture, kItemSide - 2 * kPictureInset, kItemSide - 2 * kPictureInset);
            s.setVisible(slot, false);
            slots_.push_back(slot);
            slotGuids_.push_back(0);
        }
    }
    reset();
}

void BuildPanel::reset() {
    category_ = Category::None;
    wallToolPicked_ = false;
    subsortButton_ = 0;
    page_ = 0;
    dirty_ = true;
    if (screen_ != nullptr && content_ >= 0) {
        showView();
    }
}

void BuildPanel::showView() {
    auto& s = *screen_;
    // The categories until one is chosen, then its sub-categories and the catalog.
    const bool floors = category_ == Category::Floors, walls = category_ == Category::Walls, doors = category_ == Category::Doors,
               terrain = category_ == Category::Terrain;
    s.setVisible(categories_, category_ == Category::None);
    // The Build Tools tab shares its place with each sub-category panel's Back button.
    s.setVisible(s.find(content_, kBuildTab), category_ == Category::None);
    s.setVisible(floorSorts_, floors);
    s.setVisible(wallSorts_, walls);
    s.setVisible(doorSorts_, doors);
    s.setVisible(terrainSorts_, terrain);
    s.setChecked(s.find(terrainSorts_, kPaintTool), terrain);
    for (std::size_t b = 0; b < kBrushes.size(); ++b) {
        s.setChecked(s.find(terrainSorts_, kBrushes[b]), static_cast<int>(b) == brush_);
    }
    s.setVisible(wallToolSorts_, category_ == Category::WallTool);
    s.setChecked(s.find(wallToolSorts_, kWallsAndRooms), wallToolPicked_);
    s.setVisible(catalog_, floors || walls || doors || terrain);  // the Wall tool has no catalog
    for (const auto& b : kSortButtons) {
        const std::uint32_t panel = b.id & 0xFF00;
        s.setChecked(s.find(panel == 0xB400 ? floorSorts_ : panel == 0xB500 ? wallSorts_ : doorSorts_, b.id), subsortButton_ == b.id);
    }
    s.setChecked(s.find(floorSorts_, kAllFloors), floors && subsortButton_ == 0);
    s.setChecked(s.find(wallSorts_, kAllWalls), walls && subsortButton_ == 0);
    s.setChecked(s.find(doorSorts_, kAllDoors), doors && subsortButton_ == 0);
}

std::string BuildPanel::subsortOf(std::uint32_t button) {
    const auto it = std::find_if(kSortButtons.begin(), kSortButtons.end(), [&](const SortButton& b) { return b.id == button; });
    return it != kSortButtons.end() ? it->subsort : std::string();
}

std::vector<BuildPanel::Pattern> BuildPanel::listed(const std::vector<Pattern>& all, const std::string& subsort) {
    std::vector<Pattern> out;
    for (const auto& pattern : all) {
        if (subsort.empty() || pattern.subsort == subsort) {
            out.push_back(pattern);
        }
    }
    return out;
}

BuildPanel::Click BuildPanel::clicked(int window) {
    Click out;
    if (screen_ == nullptr || content_ < 0) {
        return out;
    }
    auto& s = *screen_;
    bool inside = false;
    for (int w = window; w >= 0 && !inside; w = s.parent(w)) {
        inside = w == content_;
    }
    if (!inside) {
        return out;
    }
    out.panel = true;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        for (int w = window; w >= 0; w = s.parent(w)) {
            if (w == slots_[i]) {
                if (slotGuids_[i] != 0) {
                    (category_ == Category::Walls ? out.wall
                     : category_ == Category::Doors   ? out.object
                     : category_ == Category::Terrain ? out.paint
                                                      : out.floor) = slotGuids_[i];
                    wallToolPicked_ = false;
                }
                return out;
            }
        }
    }
    const std::uint32_t id = s.id(window);
    const int parent = s.parent(window);
    if (id == kHand) {
        wallToolPicked_ = false;
        s.setChecked(window, true);
        out.hand = true;
        return out;
    }
    if (id == kWallTool && parent == categories_) {
        category_ = Category::WallTool;
        page_ = 0;
    } else if (id == kWallsAndRooms && parent == wallToolSorts_) {
        wallToolPicked_ = true;
        out.wallTool = true;
    } else if (id == kWallToolBack && parent == wallToolSorts_) {
        category_ = Category::None;
        page_ = 0;
    } else if (id == kTerrain && parent == categories_) {
        category_ = Category::Terrain;
        subsortButton_ = 0;
        page_ = 0;
    } else if (id == kTerrainBack && parent == terrainSorts_) {
        category_ = Category::None;
        page_ = 0;
    } else if (const auto brush = std::find(kBrushes.begin(), kBrushes.end(), id); brush != kBrushes.end()) {
        brush_ = static_cast<int>(brush - kBrushes.begin());
    } else if (id == kDoorsWindows && parent == categories_) {
        category_ = Category::Doors;
        subsortButton_ = 0;
        page_ = 0;
    } else if (id == kDoorBack && parent == doorSorts_) {
        category_ = Category::None;
        page_ = 0;
    } else if ((id == kFloors || id == kWalls) && parent == categories_) {
        category_ = id == kFloors ? Category::Floors : Category::Walls;
        subsortButton_ = 0;
        page_ = 0;
    } else if ((id == kFloorBack && parent == floorSorts_) || (id == kWallBack && parent == wallSorts_)) {
        category_ = Category::None;
        page_ = 0;
    } else if ((parent == floorSorts_ || parent == wallSorts_ || parent == doorSorts_) &&
               (id == kAllFloors || id == kAllWalls || id == kAllDoors || !subsortOf(id).empty())) {
        subsortButton_ = id == kAllFloors || id == kAllWalls || id == kAllDoors || subsortButton_ == id ? 0 : id;  // clicked again, all show
        page_ = 0;
    } else if (window == previous_) {
        page_ = std::max(0, page_ - 1);
    } else if (window == next_) {
        ++page_;
    } else {
        return out;
    }
    dirty_ = true;
    showView();
    return out;
}

void BuildPanel::refresh(const Source& source) {
    const auto* all = category_ == Category::Floors  ? source.floors
                      : category_ == Category::Walls ? source.walls
                      : category_ == Category::Doors ? source.objects
                      : category_ == Category::Terrain ? source.paints
                                                     : nullptr;
    if (screen_ == nullptr || content_ < 0 || all == nullptr) {
        return;
    }
    if (dirty_) {
        listed_ = listed(*all, subsortOf(subsortButton_));
    }
    auto& s = *screen_;
    const std::size_t pageSize = std::max<std::size_t>(1, slots_.size());
    std::size_t first = static_cast<std::size_t>(page_) * pageSize;
    while (first != 0 && first >= listed_.size()) {
        --page_;
        first -= pageSize;
    }
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const bool shown = first + i < listed_.size();
        s.setVisible(slots_[i], shown);
        if (!shown) {
            slotGuids_[i] = 0;
            continue;
        }
        const auto& pattern = listed_[first + i];
        const int picture = s.find(slots_[i], kThumbPicture);
        if (slotGuids_[i] != pattern.guid || !s.visible(picture)) {
            const ts2::tga::Pixels* pixels = source.picture ? source.picture(pattern) : nullptr;
            s.setVisible(picture, pixels != nullptr);
            if (pixels != nullptr) {
                s.setImage(picture, mask_.width > 0 ? ts2::content::masked(*pixels, mask_) : *pixels);
            }
        }
        if (slotGuids_[i] != pattern.guid) {
            slotGuids_[i] = pattern.guid;
            s.setTip(s.find(slots_[i], kThumbOverlay), (pattern.name.empty() ? std::string() : pattern.name + "\n") + priceText(pattern.price));
        }
    }
    s.setVisible(previous_, page_ != 0);
    s.setVisible(next_, first + pageSize < listed_.size());
    dirty_ = false;
}

} // namespace app
