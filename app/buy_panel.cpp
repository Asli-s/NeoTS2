#include "buy_panel.hpp"

#include <algorithm>
#include <array>

namespace app {

namespace {

// The strip: the function sort's background, its stretching middle and end cap.
constexpr std::uint32_t kFunctionArt = 0x4CC48AA1, kMiddle = 0x4CC48AA6, kEndCap = 0x4CC48AA4;
// The art of the other sorts and of wholesale (business) lots, not shown yet.
constexpr std::uint32_t kOtherArt[] = {0x4CC48AA2, 0x4CC48AA3, 0x4CC48AA5, 0x4CC48AA7, 0x4CC48AA8, 0x5CC48AA1, 0x5CC48AA2,
                                       0x5CC48AA3, 0x5CC48AA4, 0x5CC48AA5, 0x5CC48AA6, 0x5CC48AA8};
// The sort buttons and the other sorts' containers.
constexpr std::uint32_t kFunctionSort = 0x2C584E6C, kCommunitySort = 0x0D87DCF9;
constexpr std::uint32_t kOtherSorts[] = {0x00000C00, 0x00000E00, 0x00000F00};
// The function sort's container, its category buttons and each category's subcategory buttons.
constexpr std::uint32_t kSorts = 0x00000A00, kCategories = 0x0000A000;
// A category's button, its function bit and its subcategories' container.
struct Category {
    std::uint32_t button;
    int function;
    std::uint32_t panel;
};
constexpr std::array<Category, 9> kCategoryList{{{0xA100, 0x001, 0xB100},
                                                 {0xA200, 0x002, 0xB200},
                                                 {0xA300, 0x004, 0xB300},
                                                 {0xA400, 0x008, 0xB400},
                                                 {0xA500, 0x010, 0xB500},
                                                 {0xA600, 0x020, 0xB600},
                                                 {0xA700, 0x040, 0xB700},
                                                 {0xA800, 0x080, 0xB800},
                                                 {0xA900, 0x100, 0xB900}}};
// A subcategory button and its subsort bit (0: the whole category). Button N010 of each
// container goes back to the categories.
struct Subcategory {
    std::uint32_t button;
    int subsort;
};
constexpr std::uint32_t kBackButton = 0x10;
constexpr Subcategory kSubcategories[] = {
    {0xB120, 0x01}, {0xB130, 0x02}, {0xB140, 0x04}, {0xB150, 0x08}, {0xB160, 0x80}, {0xB170, 0}, {0xB180, 0x10},
    {0xB220, 0x01}, {0xB230, 0x02}, {0xB240, 0x04}, {0xB250, 0x08}, {0xB260, 0x80}, {0xB270, 0}, {0xB280, 0x10}, {0xB290, 0x20},
    {0xB320, 0x01}, {0xB330, 0x02}, {0xB340, 0x04}, {0xB350, 0x08}, {0xB360, 0x80}, {0xB370, 0},
    {0xB420, 0x01}, {0xB430, 0x02}, {0xB440, 0x04}, {0xB450, 0x08}, {0xB460, 0x80}, {0xB470, 0},
    {0xB520, 0x01}, {0xB530, 0x02}, {0xB540, 0x04}, {0xB550, 0x08}, {0xB560, 0x80}, {0xB570, 0},
    {0xB620, 0x01}, {0xB630, 0x02}, {0xB640, 0x20}, {0xB650, 0x08}, {0xB660, 0x80}, {0xB670, 0}, {0xB680, 0x10}, {0xB690, 0x04},
    {0xB740, 0x08}, {0xB750, 0x02}, {0xB760, 0x10}, {0xB770, 0x80}, {0xB780, 0}, {0xB790, 0x20}, {0xB7A0, 0x40},
    {0xB820, 0x01}, {0xB830, 0x02}, {0xB840, 0x08}, {0xB850, 0x04}, {0xB860, 0x80}, {0xB870, 0}, {0xB880, 0x10},
    {0xB920, 0x01}, {0xB940, 0x02}, {0xB950, 0x04}, {0xB960, 0x80}, {0xB970, 0}, {0xB980, 0x08},
};
// The catalog, beside the chosen buttons.
constexpr std::uint32_t kCatalog = 0x898F42CD;

int indexOf(int function) {
    for (std::size_t i = 0; i < kCategoryList.size(); ++i) {
        if (kCategoryList[i].function == function) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

void BuyPanel::attach(ts2::ui::Screen& screen, int root) {
    screen_ = &screen;
    root_ = root;
    if (root < 0) {
        return;
    }
    auto& s = screen;
    sorts_ = s.find(root, kSorts);
    categories_ = s.find(sorts_, kCategories);
    subPanels_.clear();
    for (const auto& c : kCategoryList) {
        subPanels_.push_back(s.find(sorts_, c.panel));
    }
    subsorts_.assign(kCategoryList.size(), 0);
    s.setTiled(s.find(root, kMiddle), true);
    for (const std::uint32_t id : kOtherArt) {
        s.setVisible(s.find(root, id), false);
    }
    for (const std::uint32_t id : kOtherSorts) {
        s.setVisible(s.find(root, id), false);
    }
    s.setVisible(s.find(root, kCommunitySort), false);
    s.setVisible(s.find(root, kFunctionArt), true);
    s.setVisible(sorts_, true);
    grid_.attach(screen, s.find(root, kCatalog));
}

void BuyPanel::setCatalog(std::vector<Entry> entries) {
    entries_ = std::move(entries);
    if (screen_ != nullptr && root_ >= 0) {
        layout();
    }
}

void BuyPanel::open() {
    if (screen_ == nullptr || root_ < 0) {
        return;
    }
    function_ = -1;
    grid_.closeDescription();
    layout();
}

bool BuyPanel::clicked(int window) {
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
    for (const auto& c : kCategoryList) {
        if (id == c.button && parent == categories_) {
            function_ = c.function;
            layout();
            return true;
        }
    }
    const int category = indexOf(function_);
    if (category >= 0 && parent == subPanels_[static_cast<std::size_t>(category)]) {
        if ((id & 0xFF) == kBackButton) {
            function_ = -1;
            layout();
            return true;
        }
        for (const auto& sub : kSubcategories) {
            if (id == sub.button) {
                subsorts_[static_cast<std::size_t>(category)] = sub.subsort;
                layout();
                return true;
            }
        }
    }
    return true;
}

bool BuyPanel::showItem(std::uint32_t guid) {
    const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.guid == guid; });
    if (screen_ == nullptr || root_ < 0 || entry == entries_.end()) {
        return false;
    }
    const auto category = std::find_if(kCategoryList.begin(), kCategoryList.end(), [&](const Category& c) { return (entry->functions & c.function) != 0; });
    if (category == kCategoryList.end()) {
        return false;
    }
    int subsort = 0;
    for (const auto& sub : kSubcategories) {
        if ((sub.button & 0xFF00) == category->panel && sub.subsort != 0 && (entry->subsort & sub.subsort) != 0) {
            subsort = sub.subsort;
            break;
        }
    }
    function_ = category->function;
    subsorts_[static_cast<std::size_t>(category - kCategoryList.begin())] = subsort;
    grid_.closeDescription();
    layout();
    grid_.showItem(guid);
    return true;
}

bool BuyPanel::matches(const Entry& entry, int function, int subsort) const {
    return (entry.functions & function) != 0 && (subsort == 0 || (entry.subsort & subsort) != 0);
}

bool BuyPanel::anyMatch(int function, int subsort) const {
    return std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) { return matches(e, function, subsort); });
}

// The strip follows the buttons shown: the sort container takes their width, the middle stretches
// from the sort art to the container's right edge, and the catalog starts there. Without a catalog
// the end cap closes the strip.
void BuyPanel::layout() {
    auto& s = *screen_;
    const int category = indexOf(function_);
    s.setVisible(categories_, category < 0);
    s.setVisible(s.find(root_, kFunctionSort), category < 0);
    for (std::size_t i = 0; i < subPanels_.size(); ++i) {
        s.setVisible(subPanels_[i], static_cast<int>(i) == category);
    }
    const int shown = category < 0 ? categories_ : subPanels_[static_cast<std::size_t>(category)];
    if (shown >= 0 && sorts_ >= 0) {
        s.resize(sorts_, s.x(shown) + s.width(shown), s.height(sorts_));
    }
    // A category's subcategories with nothing in them are greyed, and the one chosen is pressed.
    std::vector<std::uint32_t> listed;
    if (category >= 0) {
        const int panel = subPanels_[static_cast<std::size_t>(category)];
        const int chosen = subsorts_[static_cast<std::size_t>(category)];
        for (const auto& sub : kSubcategories) {
            const int button = s.find(panel, sub.button);
            if (button >= 0 && s.parent(button) == panel) {
                s.setEnabled(button, anyMatch(function_, sub.subsort));
                s.setChecked(button, sub.subsort == chosen);
            }
        }
        for (const auto& e : entries_) {
            if (matches(e, function_, chosen)) {
                listed.push_back(e.guid);
            }
        }
    }

    const int art = s.find(root_, kFunctionArt), middle = s.find(root_, kMiddle), endCap = s.find(root_, kEndCap);
    const int right = s.x(sorts_) + s.width(sorts_);
    s.move(middle, s.x(art) + s.width(art), s.y(middle));
    const bool middleShown = right > s.x(middle);
    s.setVisible(middle, middleShown);
    if (middleShown) {
        s.resize(middle, right - s.x(middle), s.height(middle));
    }
    s.move(endCap, middleShown ? s.x(middle) + s.width(middle) : s.x(middle), s.y(endCap));
    const bool catalogShown = category >= 0;
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
