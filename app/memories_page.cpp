#include "memories_page.hpp"

#include "content/ui_meters.hpp"

#include <algorithm>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11;
constexpr std::uint32_t kThumbnailScript = 0x4906500A;

// The page's collapsed and expanded halves: their thumbnail areas, page buttons, scroll bars, the
// expand and collapse buttons, and their "maximized" and "minimized" art.
constexpr std::uint32_t kCollapsed = 0x1000, kExpanded = 0x2000;
constexpr std::uint32_t kCollapsedArea = 0x1006, kExpandedArea = 0x2006;
constexpr std::uint32_t kPrevious[] = {0x1004, 0x2004}, kNext[] = {0x1005, 0x2005}, kScrollBars[] = {0x1008, 0x2008};
constexpr std::uint32_t kExpand = 0x1009, kCollapse = 0x2009;
constexpr std::uint32_t kMaximizedArt[] = {0xA1, 0xB1}, kMinimizedArt[] = {0xA2, 0xB2};
// The Constants Table: six columns 2 pixels apart, one row collapsed and four expanded.
constexpr std::size_t kColumns = 6, kCollapsedRows = 1, kExpandedRows = 4;
constexpr int kThumbSize = 52, kMarginX = 2;

// A thumbnail's windows: the bad and good backgrounds, the picture and the frame button.
constexpr std::uint32_t kBad = 0xA, kGood = 0xB, kPicture = 0xC, kFrame = 0xD;

// The popup: its script, background and pointer, the name, the subject's name, the portrait frame
// and portrait, and the description. It sits on the page's anchor (bottom) and centres its pointer
// over the scroll bar's place and width. The subject, portrait and description stack from y 64, and
// it is at least 171 high and 43 below the last of them.
constexpr std::uint32_t kPopupScript = 0x4906500C;
constexpr std::uint32_t kPopupBackground = 0x1, kPopupPointer = 0x2, kPopupName = 0xA1, kPopupSubject = 0xA2,
                        kPopupPortraitFrame = 0xA5, kPopupPortrait = 0xA4, kPopupDescription = 0xA3;
constexpr std::uint32_t kAnchors[] = {0x1007, 0x2007};
constexpr int kPopupTop = 64, kPopupMinHeight = 171, kPopupBottomMargin = 43;
// The background stops 17 above the popup's bottom and the pointer starts 18 above it.
constexpr int kBackgroundGap = 17, kPointerGap = 18;
// "(%s)" for the subject's name: entry 0x11 of Live text table 0x91.
constexpr std::uint32_t kLiveText = 0x7F96C284, kSubjectText = 0x91u << 16 | 0x11;

} // namespace

void MemoriesPage::attach(ts2::ui::Screen& screen, int page) {
    screen_ = &screen;
    page_ = page;
    makeThumbnails(screen.find(screen.find(page, kCollapsed), kCollapsedArea), kCollapsedRows, collapsed_);
    makeThumbnails(screen.find(screen.find(page, kExpanded), kExpandedArea), kExpandedRows, expandedThumbs_);
    // The scroll bar shows where the page is among several. Paging is by the buttons for now.
    for (const std::uint32_t bar : kScrollBars) {
        screen.setVisible(screen.find(page, bar), false);
    }
    layout();
}

void MemoriesPage::makeThumbnails(int area, std::size_t rows, std::vector<Thumbnail>& out) {
    auto& s = *screen_;
    if (area < 0) {
        return;
    }
    // The columns are centred in the area.
    const int x0 = (s.width(area) - (static_cast<int>(kColumns) * (kThumbSize + kMarginX) - kMarginX)) >> 1;
    for (std::size_t i = 0; i < kColumns * rows; ++i) {
        const int x = x0 + static_cast<int>(i % kColumns) * (kThumbSize + kMarginX), y = static_cast<int>(i / kColumns) * kThumbSize;
        Thumbnail t;
        t.window = s.load(kScripts, kThumbnailScript, x, y, area);
        if (t.window < 0) {
            break;
        }
        s.setVisible(t.window, false);
        out.push_back(t);
    }
}

bool MemoriesPage::clicked(int window) {
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
    // A memory's frame button: open its popup.
    auto& thumbs = expanded_ ? expandedThumbs_ : collapsed_;
    for (std::size_t i = 0; i < thumbs.size(); ++i) {
        if (id == kFrame && s.parent(window) == thumbs[i].window) {
            selected_ = static_cast<std::size_t>(pageNumber_) * thumbs.size() + i;
            popupOpen_ = true;
            layout();
            return true;
        }
    }
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

void MemoriesPage::setMaximized(bool maximized) {
    if (screen_ == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < std::size(kMaximizedArt); ++i) {
        const int half = screen_->find(page_, i == 0 ? kCollapsed : kExpanded);
        screen_->setVisible(screen_->find(half, kMaximizedArt[i]), maximized);
        screen_->setVisible(screen_->find(half, kMinimizedArt[i]), !maximized);
    }
}

void MemoriesPage::show(std::vector<Memory> memories) {
    if (screen_ == nullptr || memories == memories_) {
        return;
    }
    // The page stays while the list keeps its size. Other memories close the popup (a picture
    // arriving doesn't).
    if (memories.size() != memories_.size()) {
        pageNumber_ = 0;
    }
    const bool same = memories.size() == memories_.size() &&
                      std::equal(memories.begin(), memories.end(), memories_.begin(),
                                 [](const Memory& a, const Memory& b) { return a.name == b.name && a.description == b.description; });
    if (!same) {
        selected_.reset();
    }
    memories_ = std::move(memories);
    layout();
}

void MemoriesPage::layout() {
    auto& s = *screen_;
    s.setVisible(s.find(page_, kCollapsed), !expanded_);
    s.setVisible(s.find(page_, kExpanded), expanded_);
    auto& thumbs = expanded_ ? expandedThumbs_ : collapsed_;
    const std::size_t pageSize = thumbs.empty() ? 1 : thumbs.size();
    std::size_t first = static_cast<std::size_t>(pageNumber_) * pageSize;
    while (first != 0 && first >= memories_.size()) {
        --pageNumber_;
        first -= pageSize;
    }
    for (std::size_t i = 0; i < thumbs.size(); ++i) {
        Thumbnail& t = thumbs[i];
        const bool shown = first + i < memories_.size();
        s.setVisible(t.window, shown);
        if (!shown || t.shown == memories_[first + i]) {
            continue;
        }
        const Memory& m = memories_[first + i];
        t.shown = m;
        s.setVisible(s.find(t.window, kGood), m.good);
        s.setVisible(s.find(t.window, kBad), !m.good);
        s.setVisible(s.find(t.window, kPicture), m.picture != nullptr);
        if (m.picture) {
            // The picture keeps its own size, centred in the window (e.g. a 40 x 44 icon at 5, 3).
            const int window = s.find(t.window, kPicture);
            const int w = s.width(window), h = s.height(window);
            s.setImage(window, ts2::content::placed(*m.picture, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h),
                                                    (w - static_cast<int>(m.picture->width)) / 2, (h - static_cast<int>(m.picture->height)) / 2));
        }
        s.setTip(s.find(t.window, kFrame), m.name);
    }
    for (std::size_t i = 0; i < thumbs.size(); ++i) {
        s.setChecked(s.find(thumbs[i].window, kFrame), selected_ == first + i);
    }
    const int half = s.find(page_, expanded_ ? kExpanded : kCollapsed);
    const std::size_t at = expanded_ ? 1 : 0;
    s.setVisible(s.find(half, kPrevious[at]), pageNumber_ != 0);
    s.setVisible(s.find(half, kNext[at]), first + pageSize < memories_.size());
    showPopup();
}

std::array<int, 2> MemoriesPage::placeInPage(int window) const {
    std::array<int, 2> at{0, 0};
    for (int w = window; w >= 0 && w != page_; w = screen_->parent(w)) {
        at[0] += screen_->x(w);
        at[1] += screen_->y(w);
    }
    return at;
}

void MemoriesPage::mouseAt(int x, int y) {
    if (screen_ == nullptr || popup_ < 0 || !screen_->visible(popup_)) {
        return;
    }
    // The popup's hover area is the block of the open half's thumbnails (322 x 52 collapsed).
    const auto& thumbs = expanded_ ? expandedThumbs_ : collapsed_;
    if (thumbs.empty()) {
        return;
    }
    const auto first = screenPlace(thumbs.front().window), last = screenPlace(thumbs.back().window);
    const int right = last[0] + screen_->width(thumbs.back().window), bottom = last[1] + screen_->height(thumbs.back().window);
    if (x < first[0] || y < first[1] || x >= right || y >= bottom) {
        popupOpen_ = false;
        screen_->setVisible(popup_, false);
    }
}

std::array<int, 2> MemoriesPage::screenPlace(int window) const {
    std::array<int, 2> at{0, 0};
    for (int w = window; w >= 0; w = screen_->parent(w)) {
        at[0] += screen_->x(w);
        at[1] += screen_->y(w);
    }
    return at;
}

void MemoriesPage::showPopup() {
    auto& s = *screen_;
    if (!selected_ || !popupOpen_ || *selected_ >= memories_.size()) {
        s.setVisible(popup_, false);
        return;
    }
    if (popup_ < 0) {
        popup_ = s.load(kScripts, kPopupScript, 0, 0, page_);
        if (popup_ < 0) {
            return;
        }
        s.setWrapped(s.find(popup_, kPopupDescription), true);
    }
    const Memory& m = memories_[*selected_];
    auto part = [&](std::uint32_t id) { return s.find(popup_, id); };
    s.setText(part(kPopupName), m.name);
    int bottom = kPopupTop;
    const int subject = part(kPopupSubject);
    s.setVisible(subject, !m.subjectName.empty());
    if (!m.subjectName.empty()) {
        std::string text = s.data().text(kLiveText, kSubjectText);
        if (const auto at = text.find("%s"); at != std::string::npos) {
            text.replace(at, 2, m.subjectName);
        }
        s.setText(subject, text);
        bottom = s.y(subject) + s.height(subject);
    }
    const int frame = part(kPopupPortraitFrame);
    s.move(frame, s.x(frame), bottom);
    const int portrait = part(kPopupPortrait);
    s.setVisible(portrait, m.subjectPicture != nullptr);
    if (m.subjectPicture) {
        s.setImage(portrait, *m.subjectPicture);
    }
    const int description = part(kPopupDescription);
    s.move(description, s.x(description), bottom);
    s.setText(description, m.description);
    s.fitHeightToText(description);
    bottom = std::max(s.y(description) + s.height(description), s.y(frame) + s.height(frame));
    const int height = std::max(kPopupMinHeight, bottom + kPopupBottomMargin);

    // On the anchor, its pointer over the scroll bar's place.
    const int half = s.find(page_, expanded_ ? kExpanded : kCollapsed);
    const std::size_t at = expanded_ ? 1 : 0;
    const int bar = s.find(half, kScrollBars[at]), anchor = s.find(half, kAnchors[at]);
    const int width = s.width(popup_), barWidth = s.width(bar);
    const auto barAt = placeInPage(bar), anchorAt = placeInPage(anchor);
    s.resize(popup_, width, height);
    s.move(popup_, barAt[0] - (width - barWidth) / 2, anchorAt[1] + s.height(anchor) - height);
    s.resize(part(kPopupBackground), width, height - kBackgroundGap);
    const int pointer = part(kPopupPointer);
    s.move(pointer, (width - barWidth) / 2, height - kPointerGap);
    s.resize(pointer, barWidth, s.height(pointer));
    s.setVisible(popup_, true);
}

} // namespace app
