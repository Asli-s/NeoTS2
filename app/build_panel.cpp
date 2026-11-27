#include "build_panel.hpp"

#include <algorithm>
#include <array>

namespace app {

namespace {

// The strip: the categories' background, its stretching middle and end cap. A category with no
// buttons of its own (Garage, Architecture) shows the no subsort background instead, which runs on
// into its catalog. The other backgrounds (collections and the wholesale ones) aren't shown yet.
constexpr std::uint32_t kArt = 0x000000B1, kNoSubsortArt = 0x000000B2, kMiddle = 0x000000B4, kEndCap = 0x000000B5;
constexpr std::uint32_t kOtherArt[] = {0x000000B3, 0x000000F1, 0x000000F2, 0x000000F3, 0x000000F4, 0x000000F5};
// The sort container, its category buttons and each category's own buttons (button N010 of each
// goes back). The categories: wall tool, foundations, doors and windows, flooring, wall coverings,
// stairs, terrain, garden center, roofs, miscellaneous, garage and architecture.
constexpr std::uint32_t kSorts = 0x000000A8, kCategories = 0x0000A000;
constexpr std::array<std::uint32_t, 12> kCategoryButtons{0xA100, 0xA200, 0xA300, 0xA400, 0xA500, 0xA600,
                                                         0xA700, 0xA800, 0xA900, 0xAA00, 0xAB00, 0xAC00};
constexpr std::array<std::uint32_t, 12> kCategoryPanels{0xB100, 0xB200, 0xB300, 0xB400, 0xB500, 0xB600,
                                                        0xB700, 0xB800, 0xB900, 0xBA00, 0xBB00, 0xBC00};
constexpr std::uint32_t kBackButton = 0x10;
// The Build Tools button lies over the left of each back button, and goes back to the categories too.
constexpr std::uint32_t kBuildToolsButton = 0x000000A6;
constexpr std::uint32_t kHalfWalls = 0x0000B130;
constexpr std::uint32_t kCatalog = 0xAD502BBF;
// The auto roof and roof angle option panels, shown only with their tools.
constexpr std::uint32_t kToolOptions[] = {0x000000AC, 0x000000AD};

// A catalog button and the build type and subsort bits it lists (subsort 0: any). A button can list
// several, one after another. Covering buttons list the walls or floors of a subsort by name
// (none: all of them).
struct Filter {
    std::uint16_t type, subsort;
};
enum class Kind { Objects, Walls, Floors, Tools, HalfWalls, Fireplaces };
struct CatalogButton {
    std::uint32_t button;  // 0: the category itself, which has no buttons but lists on opening
    std::size_t category;
    std::array<Filter, 5> filters;
    int count;
    Kind kind = Kind::Objects;
    const char* subsort = nullptr;
};
constexpr CatalogButton kCatalogButtons[] = {
    // Walls & Rooms lists the wall tool's own catalog entries (see setTools), 1/2 Walls the half walls.
    {BuildPanel::kWallsAndRoomsButton, 0, {}, 0, Kind::Tools},
    {kHalfWalls, 0, {}, 0, Kind::HalfWalls},
    // Flooring: stone, brick, wood, carpet, tile, linoleum, poured, other and all.
    {0xB420, 3, {}, 0, Kind::Floors, "stone"},
    {0xB430, 3, {}, 0, Kind::Floors, "brick"},
    {0xB440, 3, {}, 0, Kind::Floors, "wood"},
    {0xB450, 3, {}, 0, Kind::Floors, "carpet"},
    {0xB460, 3, {}, 0, Kind::Floors, "tile"},
    {0xB470, 3, {}, 0, Kind::Floors, "lino"},
    {0xB480, 3, {}, 0, Kind::Floors, "poured"},
    {0xB490, 3, {}, 0, Kind::Floors, "other"},
    {0xB4A0, 3, {}, 0, Kind::Floors, nullptr},
    // Wall coverings: brick, masonry, siding, poured, paint, wallpaper, tile, paneling, other and all.
    {0xB520, 4, {}, 0, Kind::Walls, "brick"},
    {0xB530, 4, {}, 0, Kind::Walls, "masonry"},
    {0xB540, 4, {}, 0, Kind::Walls, "siding"},
    {0xB550, 4, {}, 0, Kind::Walls, "poured"},
    {0xB560, 4, {}, 0, Kind::Walls, "paint"},
    {0xB570, 4, {}, 0, Kind::Walls, "wallpaper"},
    {0xB580, 4, {}, 0, Kind::Walls, "tile"},
    {0xB590, 4, {}, 0, Kind::Walls, "paneling"},
    {0xB5A1, 4, {}, 0, Kind::Walls, "other"},
    {0xB5A0, 4, {}, 0, Kind::Walls, nullptr},
    // Doors and windows: doors, two-story windows, one-story windows, arches, multi-story doors and
    // arches, and all of them in that order.
    {0xB320, 2, {{{8, 0x01}}}, 1},
    {0xB330, 2, {{{8, 0x02}}}, 1},
    {0xB340, 2, {{{8, 0x04}}}, 1},
    {0xB350, 2, {{{8, 0x10}}}, 1},
    {0xB370, 2, {{{8, 0x100}}}, 1},
    {0xB360, 2, {{{8, 0x01}, {8, 0x02}, {8, 0x04}, {8, 0x10}, {8, 0x100}}}, 5},
    // Stairs.
    {0xB630, 5, {{{1, 0x20}}}, 1},
    // Garden center: trees, shrubs, flowers and all.
    {0xB820, 7, {{{4, 0x01}}}, 1},
    {0xB830, 7, {{{4, 0x02}}}, 1},
    {0xB840, 7, {{{4, 0x04}}}, 1},
    {0xB850, 7, {{{4, 0}}}, 1},
    // Miscellaneous: columns, fireplaces, pools, gates, multi-story columns, connecting arches,
    // elevators. Fireplaces list the catalog scripts' fireplaces (see setFireplaces).
    {0xBA20, 9, {{{1, 0x08}}}, 1},
    {0xBA30, 9, {}, 0, Kind::Fireplaces},
    {0xBA40, 9, {{{1, 0x40}}}, 1},
    {0xBA60, 9, {{{8, 0x08}}}, 1},
    {0xBA70, 9, {{{1, 0x100}}}, 1},
    {0xBA80, 9, {{{1, 0x200}}}, 1},
    {0xBA90, 9, {{{1, 0x800}}}, 1},
    // Garage and architecture list as they open.
    {0, 10, {{{1, 0x400}}}, 1},
    {0, 11, {{{1, 0x1000}}}, 1},
};

const CatalogButton* findButton(std::size_t category, std::uint32_t button) {
    for (const auto& b : kCatalogButtons) {
        if (b.category == category && b.button == button) {
            return &b;
        }
    }
    return nullptr;
}

} // namespace

void BuildPanel::attach(ts2::ui::Screen& screen, int root) {
    screen_ = &screen;
    root_ = root;
    if (root < 0) {
        return;
    }
    auto& s = screen;
    sorts_ = s.find(root, kSorts);
    categories_ = s.find(sorts_, kCategories);
    for (const std::uint32_t id : kCategoryPanels) {
        subPanels_.push_back(s.find(sorts_, id));
    }
    // The Build Tools button and the back buttons it lies over look like one icon and act as one.
    constexpr int kBackGroup = 1;
    s.setButtonGroup(s.find(root, kBuildToolsButton), kBackGroup);
    for (std::size_t i = 0; i < kCategoryPanels.size(); ++i) {
        s.setButtonGroup(s.find(subPanels_[i], kCategoryPanels[i] | kBackButton), kBackGroup);
    }
    chosen_.assign(kCategoryPanels.size(), 0);
    // Until a button is picked, a category lists its first catalog button.
    for (const auto& b : kCatalogButtons) {
        if (b.button != 0 && chosen_[b.category] == 0) {
            chosen_[b.category] = b.button;
        }
    }
    s.setTiled(s.find(root, kMiddle), true);
    for (const std::uint32_t id : kOtherArt) {
        s.setVisible(s.find(root, id), false);
    }
    s.setVisible(s.find(root, kArt), true);
    for (const std::uint32_t id : kToolOptions) {
        s.setVisible(s.find(root, id), false);
    }
    grid_.attach(screen, s.find(root, kCatalog));
}

void BuildPanel::setCatalog(std::vector<Entry> entries) {
    entries_ = std::move(entries);
    if (screen_ != nullptr && root_ >= 0) {
        layout();
    }
}

void BuildPanel::setCoverings(std::vector<CoveringEntry> coverings) {
    coverings_ = std::move(coverings);
    if (screen_ != nullptr && root_ >= 0) {
        layout();
    }
}

void BuildPanel::open() {
    if (screen_ != nullptr && root_ >= 0) {
        category_ = -1;
        grid_.closeDescription();
        layout();
    }
}

void BuildPanel::showCovering(std::uint32_t guid, bool wall) {
    if (screen_ == nullptr || root_ < 0) {
        return;
    }
    const auto covering = std::find_if(coverings_.begin(), coverings_.end(), [&](const auto& c) { return c.guid == guid && c.wall == wall; });
    const int category = wall ? kWallCoveringCategory : kFloorCategory;
    const Kind kind = wall ? Kind::Walls : Kind::Floors;
    // The covering's own sub-category, else All.
    std::uint32_t button = 0, all = 0;
    for (const auto& b : kCatalogButtons) {
        if (b.category != static_cast<std::size_t>(category) || b.kind != kind) {
            continue;
        }
        if (b.subsort == nullptr) {
            all = b.button;
        } else if (button == 0 && covering != coverings_.end() && covering->subsort == b.subsort) {
            button = b.button;
        }
    }
    category_ = category;
    chosen_[static_cast<std::size_t>(category)] = button != 0 ? button : all;
    grid_.closeDescription();
    layout();
    grid_.showItem(guid);
}

bool BuildPanel::showItem(std::uint32_t guid) {
    if (screen_ == nullptr || root_ < 0) {
        return false;
    }
    if (std::find(fireplaces_.begin(), fireplaces_.end(), guid) != fireplaces_.end()) {
        category_ = kMiscCategory;
        chosen_[kMiscCategory] = kFireplacesButton;
        grid_.closeDescription();
        layout();
        grid_.showItem(guid);
        return true;
    }
    const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.guid == guid; });
    if (entry == entries_.end()) {
        return false;
    }
    for (const auto& b : kCatalogButtons) {
        for (int f = 0; f < b.count; ++f) {
            const Filter filter = b.filters[static_cast<std::size_t>(f)];
            if ((entry->types & filter.type) != 0 && (filter.subsort == 0 || (entry->subsort & filter.subsort) != 0)) {
                category_ = static_cast<int>(b.category);
                chosen_[b.category] = b.button;
                grid_.closeDescription();
                layout();
                grid_.showItem(guid);
                return true;
            }
        }
    }
    return false;
}

bool BuildPanel::clicked(int window) {
    if (screen_ == nullptr || root_ < 0) {
        return false;
    }
    // The grid's description popup is a window of its own, outside the panel.
    if (grid_.clicked(window)) {
        return true;
    }
    auto& s = *screen_;
    bool inside = false;
    for (int w = window; w >= 0 && !inside; w = s.parent(w)) {
        inside = w == root_;
    }
    if (!inside) {
        return false;
    }
    grid_.closeDescription();
    const std::uint32_t id = s.id(window);
    const int parent = s.parent(window);
    if (id == kBuildToolsButton && parent == root_) {
        category_ = -1;
        layout();
        return true;
    }
    for (std::size_t i = 0; i < kCategoryButtons.size(); ++i) {
        if (id == kCategoryButtons[i] && parent == categories_) {
            category_ = static_cast<int>(i);
            // The wall tool's category starts the wall tool at once.
            wallPicked_ = category_ == kWallCategory;
            layout();
            grid_.pickFirst();
            return true;
        }
    }
    if (category_ >= 0 && parent == subPanels_[static_cast<std::size_t>(category_)]) {
        const auto category = static_cast<std::size_t>(category_);
        const bool back = (id & 0xFF) == kBackButton;
        const bool picked = !back && findButton(category, id) != nullptr;
        if (back) {
            category_ = -1;
        } else if (picked) {
            chosen_[category] = id;
            wallPicked_ = id == kWallsAndRoomsButton || id == kHalfWalls;
        }
        layout();
        // Opening a category or sub-category puts its catalog's first item in the hand.
        if (picked) {
            grid_.pickFirst();
        }
    }
    return true;
}

void BuildPanel::setHalfWalls(std::vector<std::uint32_t> guids) {
    halfWalls_ = std::move(guids);
    if (screen_ != nullptr && root_ >= 0) {
        layout();
    }
}

void BuildPanel::setFireplaces(std::vector<std::uint32_t> mantles) {
    fireplaces_ = std::move(mantles);
    if (screen_ != nullptr && root_ >= 0) {
        layout();
    }
}

void BuildPanel::setTools(std::vector<std::uint32_t> tools) {
    tools_ = std::move(tools);
    if (screen_ != nullptr && root_ >= 0 && listing_ == Listing::Tools) {
        layout();
    }
}

void BuildPanel::setWallTool(bool on) {
    if (screen_ != nullptr && category_ == kWallCategory) {
        screen_->setChecked(screen_->find(subPanels_[kWallCategory], chosen_[kWallCategory]), on);
    }
}

// Laid out as the Buy panel's strip: the sort container takes the shown buttons' width, the middle
// stretches from the art to its right edge, and the catalog starts there or the end cap closes it.
void BuildPanel::layout() {
    auto& s = *screen_;
    s.setVisible(categories_, category_ < 0);
    s.setChecked(s.find(root_, kBuildToolsButton), category_ < 0);
    for (std::size_t i = 0; i < subPanels_.size(); ++i) {
        s.setVisible(subPanels_[i], static_cast<int>(i) == category_);
    }
    const int shown = category_ < 0 ? categories_ : subPanels_[static_cast<std::size_t>(category_)];
    if (shown >= 0 && sorts_ >= 0) {
        s.resize(sorts_, s.x(shown) + s.width(shown), s.height(sorts_));
    }
    // The catalog of the chosen button, the category's own for those without buttons.
    const CatalogButton* listing = nullptr;
    if (category_ >= 0) {
        const auto category = static_cast<std::size_t>(category_);
        listing = findButton(category, chosen_[category]);
        if (listing == nullptr) {
            listing = findButton(category, 0);
        }
        const int panel = subPanels_[category];
        if (category_ == kWallCategory) {
            s.setEnabled(s.find(panel, kHalfWalls), !halfWalls_.empty());
        }
        for (const auto& b : kCatalogButtons) {
            if (b.category == category && b.button != 0) {
                s.setChecked(s.find(panel, b.button), listing == &b);
            }
        }
    }
    std::vector<std::uint32_t> listed;
    listing_ = listing == nullptr || listing->kind == Kind::Objects || listing->kind == Kind::Fireplaces ? Listing::Objects
               : listing->kind == Kind::Walls                       ? Listing::Walls
               : listing->kind == Kind::Tools                       ? Listing::Tools
               : listing->kind == Kind::HalfWalls                   ? Listing::HalfWalls
                                                                    : Listing::Floors;
    if (listing != nullptr && listing->kind == Kind::Tools) {
        listed = tools_;
    } else if (listing != nullptr && listing->kind == Kind::HalfWalls) {
        listed = halfWalls_;
    } else if (listing != nullptr && listing->kind == Kind::Fireplaces) {
        listed = fireplaces_;
    } else if (listing != nullptr && listing->kind != Kind::Objects) {
        for (const auto& c : coverings_) {
            if (c.wall == (listing->kind == Kind::Walls) && (listing->subsort == nullptr || c.subsort == listing->subsort)) {
                listed.push_back(c.guid);
            }
        }
    }
    if (listing != nullptr) {
        for (int f = 0; f < listing->count; ++f) {
            const Filter filter = listing->filters[static_cast<std::size_t>(f)];
            for (const auto& e : entries_) {
                if ((e.types & filter.type) != 0 && (filter.subsort == 0 || (e.subsort & filter.subsort) != 0)) {
                    listed.push_back(e.guid);
                }
            }
        }
    }
    const bool noSubsort = listing != nullptr && listing->button == 0;
    s.setVisible(s.find(root_, kArt), !noSubsort);
    s.setVisible(s.find(root_, kNoSubsortArt), noSubsort);
    grid_.setJoined(noSubsort);
    const int art = s.find(root_, noSubsort ? kNoSubsortArt : kArt), middle = s.find(root_, kMiddle), endCap = s.find(root_, kEndCap);
    const int right = s.x(sorts_) + s.width(sorts_);
    s.move(middle, s.x(art) + s.width(art), s.y(middle));
    const bool middleShown = right > s.x(middle);
    s.setVisible(middle, middleShown);
    if (middleShown) {
        s.resize(middle, right - s.x(middle), s.height(middle));
    }
    s.move(endCap, middleShown ? s.x(middle) + s.width(middle) : s.x(middle), s.y(endCap));
    const bool catalogShown = listing != nullptr;
    const int catalog = grid_.window();
    s.move(catalog, right, s.y(catalog));
    s.setVisible(catalog, catalogShown);
    grid_.setItems(std::move(listed));
    if (catalogShown) {
        grid_.fit();
    }
    s.setVisible(endCap, !catalogShown);
    s.resize(root_, catalogShown ? s.x(catalog) + s.width(catalog) : s.x(endCap) + s.width(endCap), s.height(root_));
}

} // namespace app
