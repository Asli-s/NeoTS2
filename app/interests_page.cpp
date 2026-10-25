#include "interests_page.hpp"

#include <algorithm>
#include <string>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11;
constexpr std::uint32_t kItemScript = 0x4906500E;

// The page's collapsed and expanded halves: their item areas, page buttons, the expand and collapse
// buttons, and their "maximized" and "minimized" art.
constexpr std::uint32_t kCollapsed = 0x1000, kExpanded = 0x2000;
constexpr std::uint32_t kCollapsedArea = 0x1006, kExpandedArea = 0x2006;
constexpr std::uint32_t kPrevious[] = {0x1004, 0x2004}, kNext[] = {0x1005, 0x2005};
constexpr std::uint32_t kExpand = 0x1009, kCollapse = 0x2009;
constexpr std::uint32_t kMaximizedArt[] = {0xA1, 0xB1}, kMinimizedArt[] = {0xA2, 0xB2};
// The Constants Table: three columns, three rows collapsed and six expanded, no margins.
constexpr int kColumns = 3, kCollapsedRows = 3, kExpandedRows = 6;

// An item's name button and its bar's fill, filled in ten steps like the personality traits.
constexpr std::uint32_t kName = 0x1, kBar = 0x2, kFill = 0x3;
constexpr std::uint32_t kFillPicture = 0xAB200480;
constexpr int kSteps = 10;

// The topics in the game's order: person data 0x7C (Politics) to 0x8D (Sci-Fi), named by entries
// 1 to 18 of Live text table 0x8C.
constexpr std::size_t kFirstInterest = 0x7C, kTopics = 18;
constexpr std::uint32_t kLiveText = 0x7F96C284, kNames = 0x8C;

} // namespace

void InterestsPage::attach(ts2::ui::Screen& screen, int page) {
    screen_ = &screen;
    page_ = page;
    makeItems(screen.find(screen.find(page, kCollapsed), kCollapsedArea), kCollapsedRows, collapsed_);
    makeItems(screen.find(screen.find(page, kExpanded), kExpandedArea), kExpandedRows, expandedItems_);
    values_.assign(kTopics, 0);
    layout();
}

void InterestsPage::makeItems(int area, int rows, std::vector<Item>& out) {
    auto& s = *screen_;
    if (area < 0) {
        return;
    }
    for (int i = 0; i < kColumns * rows; ++i) {
        Item item;
        item.window = s.load(kScripts, kItemScript, 0, 0, area);
        if (item.window < 0) {
            break;
        }
        // The columns are centred in the area, the rows start at its top.
        const int w = s.width(item.window), h = s.height(item.window);
        const int x0 = (s.width(area) - kColumns * w) >> 1;
        s.move(item.window, x0 + (i % kColumns) * w, (i / kColumns) * h);
        s.setVisible(item.window, false);
        const int bar = s.find(item.window, kBar);
        item.bar = MeterBar(s, bar >= 0 ? s.find(bar, kFill) : -1, kFillPicture, false);
        out.push_back(std::move(item));
    }
}

bool InterestsPage::clicked(int window) {
    if (screen_ == nullptr || page_ < 0) {
        return false;
    }
    auto& s = *screen_;
    bool inside = false;
    for (int w = window; w >= 0 && !inside; w = s.parent(w)) {
        inside = w == page_;
    }
    if (!inside) {
        return false;
    }
    const std::uint32_t id = s.id(window);
    if (id == kPrevious[0] || id == kPrevious[1]) {
        pageNumber_ = std::max(0, pageNumber_ - 1);
    } else if (id == kNext[0] || id == kNext[1]) {
        ++pageNumber_;
    } else if (id == kExpand || id == kCollapse) {
        expanded_ = id == kExpand;
    } else {
        return false;
    }
    layout();
    return true;
}

void InterestsPage::setMaximized(bool maximized) {
    if (screen_ == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < std::size(kMaximizedArt); ++i) {
        const int half = screen_->find(page_, i == 0 ? kCollapsed : kExpanded);
        screen_->setVisible(screen_->find(half, kMaximizedArt[i]), maximized);
        screen_->setVisible(screen_->find(half, kMinimizedArt[i]), !maximized);
    }
}

void InterestsPage::show(std::span<const std::int16_t> personData) {
    if (screen_ == nullptr) {
        return;
    }
    bool changed = false;
    for (std::size_t i = 0; i < kTopics && i < values_.size(); ++i) {
        const std::int16_t value = kFirstInterest + i < personData.size() ? personData[kFirstInterest + i] : 0;
        changed = changed || value != values_[i];
        values_[i] = value;
    }
    if (changed) {
        layout();
    }
}

void InterestsPage::layout() {
    auto& s = *screen_;
    s.setVisible(s.find(page_, kCollapsed), !expanded_);
    s.setVisible(s.find(page_, kExpanded), expanded_);
    auto& items = expanded_ ? expandedItems_ : collapsed_;
    const std::size_t pageSize = items.empty() ? 1 : items.size();
    std::size_t first = static_cast<std::size_t>(pageNumber_) * pageSize;
    while (first != 0 && first >= kTopics) {
        --pageNumber_;
        first -= pageSize;
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
        Item& item = items[i];
        const std::size_t topic = first + i;
        s.setVisible(item.window, topic < kTopics);
        if (topic >= kTopics) {
            continue;
        }
        if (item.topic != static_cast<int>(topic)) {
            item.topic = static_cast<int>(topic);
            const std::string name = s.data().text(kLiveText, kNames << 16 | static_cast<std::uint32_t>(topic + 1));
            const int button = s.find(item.window, kName);
            s.setText(button, name);
            s.setTip(button, name);
        }
        item.bar.show(values_[topic], kSteps);
    }
    const int half = s.find(page_, expanded_ ? kExpanded : kCollapsed);
    const std::size_t at = expanded_ ? 1 : 0;
    s.setVisible(s.find(half, kPrevious[at]), pageNumber_ != 0);
    s.setVisible(s.find(half, kNext[at]), first + pageSize < kTopics);
}

} // namespace app
