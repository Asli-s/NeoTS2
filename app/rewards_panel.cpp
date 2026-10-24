#include "rewards_panel.hpp"

#include "content/ui_meters.hpp"

#include <algorithm>
#include <iterator>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11, kImages = 0x499DB772;

// The backgrounds by tab, normal and with the wants open. The inventory and cart also have expanded ones.
constexpr std::uint32_t kCareerArt[] = {0x20000000, 0x10000000}, kAspirationArt[] = {0x40000000, 0x30000000};
constexpr std::uint32_t kInventoryArt[] = {0x12000000, 0x11000000}, kInventoryExpandedArt[] = {0x18000000, 0x17000000};
constexpr std::uint32_t kCartArt[] = {0x1C000000, 0x1B000000}, kCartExpandedArt[] = {0x1E000000, 0x1D000000};
// Each tab's contents, and the "inventory is empty" message.
constexpr std::uint32_t kCareerList = 0x50000000, kAspirationList = 0x60000000, kInventoryList = 0x13000000,
                        kCartList = 0x19000000, kAddToInventory = 0x16000000;
constexpr std::uint32_t kAddButton = 0x16001000, kAddText = 0x16001001, kAddPicture = 0x16001002, kEmptyText = 0x16001003;
// The tab buttons. The aspiration tab's button shows the Sim's aspiration (buttons 1-8) and comes
// in two sets, for when a reward can be had or not. So does the career tab's.
constexpr std::uint32_t kInventoryButton = 0x14000000, kCartButton = 0x1A000000;
constexpr std::uint32_t kAspirationOpen = 0xC0000000, kAspirationClosed = 0xD0000000;
constexpr std::uint32_t kCareerOpen = 0xA0000000, kCareerClosed = 0xB0000000;
// The hand (picks objects up), undo and redo.
constexpr std::uint32_t kHand = 0x70000000, kUndo = 0x80000000, kRedo = 0x90000000;
// An aspiration flag's button, as the aspiration meter picks it.
struct AspirationButton {
    std::int16_t flag;
    std::uint32_t button;
};
constexpr AspirationButton kAspirationButtons[] = {{0x01, 5}, {0x02, 1}, {0x04, 2}, {0x10, 3}, {0x20, 4}, {0x40, 6}, {0x80, 7}, {0x100, 8}};
// The aspiration tab's points, and the lifetime aspiration tree's buttons and glow.
constexpr std::uint32_t kPoints = 0xA1;
constexpr std::uint32_t kTreeOpen = 0xE0000000, kTreeClosed = 0xE0000001, kTreeGlow = 0xE0000005;

// A list's windows: its halves (the inventory's), page buttons, expand buttons and scrollbars.
constexpr std::uint32_t kHalves[] = {0x1000, 0x2000};
constexpr std::uint32_t kAreas[] = {0x1006, 0x2006}, kPrevious[] = {0x1004, 0x2004}, kNext[] = {0x1005, 0x2005};
constexpr std::uint32_t kExpand = 0x1009, kCollapse = 0x2009, kScrollbars[] = {0x1008, 0x2008};
// The thumbnail scripts (inventory, aspiration rewards, career rewards) and their windows.
constexpr std::uint32_t kInventoryThumbnail = 0x49064812, kAspirationThumbnail = 0x49064805, kCareerThumbnail = 0x49064810;
constexpr std::uint32_t kThumbButton = 0xA001, kThumbPicture = 0xA002, kThumbOverlay = 0xA003, kThumbCount = 0x2000;
constexpr std::uint32_t kThumbCareerIcon = 0xA006;
// The pictures' shapes, for things that can be had or not, in the small and the large thumbnails.
constexpr std::uint32_t kSmallEnabledMask = 0x000B0357, kSmallDisabledMask = 0x000B0358;
constexpr std::uint32_t kLargeEnabledMask = 0x000B0349, kLargeDisabledMask = 0x000B0346;
// The rows of each half, as the script's constants give them.
constexpr int kRows[] = {1, 2};

// A number with commas between its thousands.
std::string grouped(int value) {
    std::string digits = std::to_string(std::abs(static_cast<long long>(value)));
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<std::size_t>(i), ",");
    }
    return (value < 0 ? "-" : "") + digits;
}

} // namespace

void RewardsPanel::attach(ts2::ui::Screen& screen, int content) {
    screen_ = &screen;
    content_ = content;
    if (content < 0) {
        return;
    }
    makeList(inventory_, kInventoryList, true, kInventoryThumbnail, kSmallEnabledMask, kSmallDisabledMask);
    makeList(aspiration_, kAspirationList, false, kAspirationThumbnail, kSmallEnabledMask, kSmallDisabledMask);
    makeList(career_, kCareerList, false, kCareerThumbnail, kLargeEnabledMask, kLargeDisabledMask);
    // Undo and redo of moved objects aren't done.
    screen.setEnabled(screen.find(content, kUndo), false);
    screen.setEnabled(screen.find(content, kRedo), false);
    screen.setChecked(screen.find(content, kHand), true);
    layout();
}

void RewardsPanel::makeList(List& list, std::uint32_t group, bool expands, std::uint32_t thumbnailScript,
                            std::uint32_t enabledMask, std::uint32_t disabledMask) {
    auto& s = *screen_;
    list.group = s.find(content_, group);
    list.enabledMask = s.data().image(kImages, enabledMask).value_or(ts2::tga::Pixels{});
    list.disabledMask = s.data().image(kImages, disabledMask).value_or(ts2::tga::Pixels{});
    for (int h = 0; h < (expands ? 2 : 1); ++h) {
        list.halves[h] = expands ? s.find(list.group, kHalves[h]) : list.group;
        const int area = s.find(list.halves[h], kAreas[h]);
        s.setVisible(s.find(list.halves[h], kScrollbars[h]), false);
        // As many columns as fit the area, centred.
        const int probe = s.load(kScripts, thumbnailScript, 0, 0, area);
        if (probe < 0) {
            continue;
        }
        const int w = std::max(1, s.width(probe)), height = std::max(1, s.height(probe));
        s.remove(probe);
        const int columns = s.width(area) / w;
        const int left = (s.width(area) - columns * w) / 2;
        for (int i = 0; i < columns * kRows[h]; ++i) {
            const int slot = s.load(kScripts, thumbnailScript, left + (i % columns) * w, (i / columns) * height, area);
            s.setVisible(slot, false);
            list.slots[h].push_back(slot);
            list.shown[h].push_back(Item{nullptr, 0, false, -1});
        }
    }
}

bool RewardsPanel::clicked(int window) {
    if (screen_ == nullptr || content_ < 0) {
        return false;
    }
    auto& s = *screen_;
    bool inside = false;
    for (int w = window; w >= 0 && !inside; w = s.parent(w)) {
        inside = w == content_;
    }
    if (!inside) {
        return false;
    }
    const std::uint32_t id = s.id(window);
    const int parent = s.parent(window);
    if (parent >= 0 && (s.id(parent) == kAspirationOpen || s.id(parent) == kAspirationClosed)) {
        selectTab(kAspiration);
    } else if (id == kCareerOpen || id == kCareerClosed) {
        selectTab(kCareer);
    } else if (id == kInventoryButton) {
        selectTab(kInventory);
    } else if (id == kCartButton) {
        selectTab(kCart);
    } else if (!listClicked(tab_ == kCareer ? career_ : tab_ == kAspiration ? aspiration_ : inventory_, id)) {
        return false;
    }
    layout();
    return true;
}

bool RewardsPanel::listClicked(List& list, std::uint32_t id) {
    if (id == kPrevious[0] || id == kPrevious[1]) {
        list.page = std::max(0, list.page - 1);
    } else if (id == kNext[0] || id == kNext[1]) {
        ++list.page;
    } else if ((id == kExpand || id == kCollapse) && list.halves[1] >= 0) {
        list.expanded = id == kExpand;
    } else {
        return false;
    }
    return true;
}

void RewardsPanel::setMaximized(bool maximized) {
    if (maximized != maximized_) {
        maximized_ = maximized;
        layout();
    }
}

void RewardsPanel::show(const Sim& sim) {
    if (sim == sim_) {
        return;
    }
    sim_ = sim;
    if (screen_ != nullptr && content_ >= 0) {
        layout();
    }
}

bool RewardsPanel::available(int tab) const {
    switch (tab) {
    case kCareer: return sim_.careerTab;
    case kAspiration: return sim_.aspirationTab;
    case kCart: return sim_.cartTab;
    default: return true;
    }
}

void RewardsPanel::selectTab(int tab) { tab_ = available(tab) ? tab : kInventory; }

void RewardsPanel::layout() {
    auto& s = *screen_;
    if (!available(tab_)) {
        tab_ = kInventory;
    }
    const int wide = maximized_ ? 1 : 0;
    const bool empty = sim_.inventory.empty();
    const std::uint32_t art = tab_ == kCareer       ? kCareerArt[wide]
                              : tab_ == kAspiration ? kAspirationArt[wide]
                              : tab_ == kCart       ? (inventory_.expanded ? kCartExpandedArt[wide] : kCartArt[wide])
                                                    : (inventory_.expanded && !empty ? kInventoryExpandedArt[wide] : kInventoryArt[wide]);
    for (const auto* set : {kCareerArt, kAspirationArt, kInventoryArt, kInventoryExpandedArt, kCartArt, kCartExpandedArt}) {
        for (int i = 0; i < 2; ++i) {
            s.setVisible(s.find(content_, set[i]), set[i] == art);
        }
    }
    s.setVisible(career_.group, tab_ == kCareer);
    s.setVisible(aspiration_.group, tab_ == kAspiration);
    s.setVisible(inventory_.group, tab_ == kInventory && !empty);
    s.setVisible(s.find(content_, kCartList), tab_ == kCart);
    // An empty inventory says so in place of the list.
    const int add = s.find(content_, kAddToInventory);
    s.setVisible(add, tab_ == kInventory && empty);
    for (const std::uint32_t id : {kAddButton, kAddText, kAddPicture}) {
        s.setVisible(s.find(add, id), false);
    }
    s.setVisible(s.find(add, kEmptyText), true);

    // The tab buttons: the chosen one pressed, the ones the Sim can't use disabled. The aspiration
    // and career buttons show an open chest when a reward can be had.
    std::uint32_t aspirationButton = 0;
    for (const auto& [flag, button] : kAspirationButtons) {
        if (sim_.aspiration == flag) {
            aspirationButton = button;
        }
    }
    auto any = [](const std::vector<Item>& items) { return std::any_of(items.begin(), items.end(), [](const Item& i) { return i.enabled; }); };
    const bool aspirationOpen = any(sim_.aspirationRewards), careerOpen = any(sim_.careerRewards);
    for (const std::uint32_t group : {kAspirationOpen, kAspirationClosed}) {
        const int g = s.find(content_, group);
        s.setVisible(g, (group == kAspirationOpen) == aspirationOpen);
        s.setEnabled(g, sim_.aspirationTab);
        for (std::uint32_t id = 1; id <= 8; ++id) {
            const int button = s.find(g, id);
            s.setVisible(button, id == aspirationButton);
            s.setEnabled(button, sim_.aspirationTab);
            s.setChecked(button, tab_ == kAspiration);
        }
    }
    for (const std::uint32_t id : {kCareerOpen, kCareerClosed}) {
        const int button = s.find(content_, id);
        s.setVisible(button, (id == kCareerOpen) == careerOpen);
        s.setEnabled(button, sim_.careerTab);
        s.setChecked(button, tab_ == kCareer);
    }
    s.setChecked(s.find(content_, kInventoryButton), tab_ == kInventory);
    s.setEnabled(s.find(content_, kCartButton), sim_.cartTab);
    s.setChecked(s.find(content_, kCartButton), tab_ == kCart);

    if (tab_ == kInventory && !empty) {
        layoutList(inventory_, sim_.inventory);
    } else if (tab_ == kAspiration) {
        s.setText(s.find(aspiration_.group, kPoints), grouped(sim_.points));
        // The lifetime aspiration tree: open with a glow when it has levels to spend, else closed.
        s.setVisible(s.find(aspiration_.group, kTreeOpen), sim_.lifetimeTree && sim_.lifetimeLevels);
        s.setVisible(s.find(aspiration_.group, kTreeClosed), !(sim_.lifetimeTree && sim_.lifetimeLevels));
        s.setEnabled(s.find(aspiration_.group, kTreeClosed), sim_.lifetimeTree);
        s.setVisible(s.find(aspiration_.group, kTreeGlow), sim_.lifetimeTree && sim_.lifetimeLevels);
        layoutList(aspiration_, sim_.aspirationRewards);
    } else if (tab_ == kCareer) {
        layoutList(career_, sim_.careerRewards);
    }
}

void RewardsPanel::layoutList(List& list, const std::vector<Item>& items) {
    auto& s = *screen_;
    const int h = list.expanded && list.halves[1] >= 0 ? 1 : 0;
    if (list.halves[1] >= 0) {
        s.setVisible(list.halves[0], h == 0);
        s.setVisible(list.halves[1], h == 1);
    }
    const auto& slots = list.slots[h];
    const std::size_t pageSize = std::max<std::size_t>(1, slots.size());
    // A page past the end goes back to the last one with items on it.
    std::size_t first = static_cast<std::size_t>(list.page) * pageSize;
    while (first != 0 && first >= items.size()) {
        --list.page;
        first -= pageSize;
    }
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const bool shown = first + i < items.size();
        s.setVisible(slots[i], shown);
        if (shown) {
            showItem(list, slots[i], items[first + i], list.shown[h][i]);
        }
    }
    // The page buttons show only when they lead somewhere.
    s.setVisible(s.find(list.halves[h], kPrevious[h]), list.page != 0);
    s.setVisible(s.find(list.halves[h], kNext[h]), first + pageSize < items.size());
}

void RewardsPanel::showItem(const List& list, int slot, const Item& item, Item& shown) {
    if (item == shown) {
        return;
    }
    shown = item;
    auto& s = *screen_;
    s.setEnabled(s.find(slot, kThumbButton), item.enabled);
    const int picture = s.find(slot, kThumbPicture);
    s.setVisible(picture, item.picture != nullptr);
    if (item.picture != nullptr) {
        // The picture takes the shape of the thumbnail's frame. What can't be had is grey.
        s.setImage(picture, item.enabled ? ts2::content::masked(*item.picture, list.enabledMask)
                                         : ts2::content::masked(ts2::content::greyed(*item.picture), list.disabledMask));
    }
    if (const int icon = s.find(slot, kThumbCareerIcon); icon >= 0) {
        s.setVisible(icon, item.icon != nullptr);
        if (item.icon != nullptr) {
            s.setImage(icon, *item.icon);
        }
    }
    if (item.price > 0) {
        s.setTip(s.find(slot, kThumbOverlay), grouped(item.price));
    }
    const int count = s.find(slot, kThumbCount);
    s.setVisible(count, item.count >= 2);
    if (item.count >= 2) {
        s.setText(count, "x" + std::to_string(item.count));
    }
}

} // namespace app
