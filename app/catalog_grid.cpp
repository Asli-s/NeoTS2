#include "catalog_grid.hpp"

#include "platform/platform.hpp"

#include <algorithm>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11;

// The catalog, collapsed and expanded: page buttons, the cells' area, the scroll bar and the expand
// and collapse buttons. The wholesale ones stay hidden.
constexpr std::uint32_t kInfoText = 0x00004000;
constexpr std::uint32_t kHalves[] = {0x1000, 0x2000}, kWholesaleHalves[] = {0x3300, 0x5500};
constexpr std::uint32_t kPrevious[] = {0x1004, 0x2004}, kNext[] = {0x1005, 0x2005}, kAreas[] = {0x1006, 0x2006};
constexpr std::uint32_t kScrollbars[] = {0x1008, 0x2008}, kExpand = 0x1009, kCollapse = 0x2009;
// The scroll bars' track and thumb pictures (the script's containerimage and thumbimage).
constexpr std::uint32_t kBarImages = 0x499DB772, kBarTrack = 0xA9200330, kBarThumb = 0xA9200335;
// The collections' Organize Items and Remove from Collection buttons. The game hides them in the
// other sorts, and collections aren't shown yet.
constexpr std::uint32_t kOrganize = 0x3001, kRemoveBoxes[] = {0x100A, 0x200A}, kRemove = 0x000A;
// The background and the bitmaps that stretch with a half, as the catalog widens to the screen.
constexpr std::uint32_t kStretched[2][2] = {{0x1002, 0x1008}, {0x2002, 0x2008}};
// The collapsed catalog's rounded left end and the plain strip shown in its place when joined.
constexpr std::uint32_t kLeftEnd = 0x1001, kJoinedLeft = 0x00B7;
// A catalog cell (44 x 44): its background, which holds four 44 pixel frames, its picture and button.
constexpr std::uint32_t kCell = 0x49063002, kCellBackground = 0x00000001, kCellPicture = 0x00000002, kCellButton = 0x00000003;
constexpr int kCellSide = 44;
// Cells sit 4 pixels apart, filling the area from its top left a row at a time, the rows centred
// up and down in it.
constexpr int kCellStep = 48;

// The description popup a clicked cell opens. Its bottom sits on the catalog's hidden anchor and it
// is centred over the cells. Top down it holds the name with the price after it, the description,
// the ratings and the strip of colour choices, the button beside the strip centred on it, and its
// background ends 2 pixels into the bottom strip, which closes it.
constexpr std::uint32_t kPopup = 0x49063003, kPopupAnchor[] = {0x1007, 0x2007};
constexpr std::uint32_t kPopupBackground = 0x0001, kPopupBottom = 0x0002, kPopupName = 0x3000, kPopupText = 0x4000,
                        kPopupRatings = 0x5000, kPopupChoices = 0x9000, kPopupMoreChoices = 0xA000, kPopupButton = 0x7000,
                        kPopupPictureImage = 0x8001, kPopupShortBar = 0x1000, kPopupLongBar = 0x2000;
constexpr std::uint32_t kPopupHidden[] = {0x6000, 0x7122A8B5, 0x8003, kPopupMoreChoices, kPopupLongBar};
constexpr std::uint32_t kPopupPicture = 0x8000;
// The colour choice strips, seven a page. The divider over the second is as wide as the longer
// strip's cells less 4 pixels.
constexpr std::uint32_t kStrips[] = {kPopupChoices, kPopupMoreChoices};
constexpr std::uint32_t kStripDivider = 0x2323;
constexpr int kSwatchStep = 50, kSwatchesShown = 7, kDividerShort = 4;
constexpr int kNameTop = 17, kChoicesGap = 5, kBottomGap = 16, kBackgroundOverlap = 2, kBottomLeft = 19, kBottomWidth = 622;
// The name and its price are one line, three spaces apart.
constexpr const char* kPriceGap = "   ";

} // namespace

void CatalogGrid::attach(ts2::ui::Screen& screen, int catalog) {
    screen_ = &screen;
    catalog_ = catalog;
    if (catalog < 0) {
        return;
    }
    auto& s = screen;
    popup_ = s.load(kScripts, kPopup, 0, 0);
    s.setVisible(popup_, false);
    for (const std::uint32_t id : kPopupHidden) {
        s.setVisible(s.find(popup_, id), false);
    }
    s.setWrapped(s.find(popup_, kPopupText), true);
    s.setWrapped(s.find(popup_, kPopupRatings), true);
    for (int k = 0; k < 2; ++k) {
        strips_[k].attach(s, s.find(popup_, kStrips[k]), kSwatchesShown);
    }

    s.setVisible(s.find(catalog, kInfoText), false);
    for (const std::uint32_t id : kWholesaleHalves) {
        s.setVisible(s.find(catalog, id), false);
    }
    for (int h = 0; h < 2; ++h) {
        Half& half = geometry_[h];
        halves_[h] = s.find(catalog, kHalves[h]);
        half.area = s.find(halves_[h], kAreas[h]);
        half.width = s.width(halves_[h]);
        half.areaWidth = s.width(half.area);
        s.setVisible(half.area, true);
        s.setVisible(s.find(halves_[h], kOrganize), false);
        s.setVisible(s.find(s.find(halves_[h], kRemoveBoxes[h]), kRemove), false);
        bars_[h].attach(s, s.find(halves_[h], kScrollbars[h]), kBarImages, kBarTrack, kBarThumb, true);
        // What sits right of the area keeps its distance from the half's right edge, and the
        // background, area and scroll bar stretch.
        const int areaRight = s.x(half.area) + half.areaWidth;
        for (const int c : s.children(halves_[h])) {
            const std::uint32_t id = s.id(c);
            const bool stretches = c == half.area || id == kStretched[h][0] || id == kStretched[h][1];
            if (stretches || s.x(c) >= areaRight) {
                half.parts.push_back({c, s.x(c), s.width(c), stretches});
            }
        }
    }
}

void CatalogGrid::setJoined(bool joined) {
    if (screen_ == nullptr || catalog_ < 0 || halves_[0] < 0) {
        return;
    }
    screen_->setVisible(screen_->find(halves_[0], kLeftEnd), !joined);
    screen_->setVisible(screen_->find(halves_[0], kJoinedLeft), joined);
}

void CatalogGrid::setItems(std::vector<std::uint32_t> items) {
    items_ = std::move(items);
    page_ = 0;
    closeDescription();
    if (screen_ != nullptr && catalog_ >= 0) {
        showPage();
    }
}

void CatalogGrid::fit() {
    if (screen_ == nullptr || catalog_ < 0) {
        return;
    }
    auto& s = *screen_;
    const int h = expanded_ ? 1 : 0;
    Half& half = geometry_[h];
    if (halves_[h] < 0 || half.area < 0) {
        return;
    }
    // As many columns as the items need over the half's rows, at most as many as fit before the
    // screen's right edge.
    const int rows = std::max(1, (s.height(half.area) + kCellStep - kCellSide) / kCellStep);
    const int room = s.size()[0] - s.position(halves_[h])[0] - (half.width - half.areaWidth);
    const int fitting = std::max(1, (room + kCellStep - kCellSide) / kCellStep);
    const int needed = static_cast<int>((items_.size() + static_cast<std::size_t>(rows) - 1) / static_cast<std::size_t>(rows));
    const int columns = std::clamp(needed, 1, fitting);
    const int grow = columns * kCellStep - (kCellStep - kCellSide) - half.areaWidth;
    for (const auto& part : half.parts) {
        if (part.stretches) {
            s.resize(part.window, part.width + grow, s.height(part.window));
        } else {
            s.move(part.window, part.x + grow, s.y(part.window));
        }
    }
    s.resize(halves_[h], half.width + grow, s.height(halves_[h]));
    s.resize(catalog_, half.width + grow, s.height(catalog_));
    if (static_cast<int>(slots_[h].size()) != columns * rows) {
        for (const int slot : slots_[h]) {
            s.remove(slot);
        }
        slots_[h].clear();
        const int top = (s.height(half.area) - (rows * kCellStep - (kCellStep - kCellSide))) / 2;
        for (int i = 0; i < columns * rows; ++i) {
            const int slot = s.load(kScripts, kCell, (i % columns) * kCellStep, top + (i / columns) * kCellStep, half.area);
            // The background's second frame is the blue one, its first the red one of an item the
            // household can't afford (see showAffordable).
            s.setImagePart(s.find(slot, kCellBackground), {kCellSide, 0, 2 * kCellSide, kCellSide});
            s.setVisible(slot, false);
            slots_[h].push_back(slot);
        }
    }
    showPage();
}

bool CatalogGrid::clicked(int window) {
    if (screen_ == nullptr || catalog_ < 0) {
        return false;
    }
    auto& s = *screen_;
    bool inside = false, inPopup = false;
    for (int w = window; w >= 0 && !inside && !inPopup; w = s.parent(w)) {
        inside = w == catalog_;
        inPopup = w == popup_;
    }
    if (inPopup) {
        return clickedPopup(window);
    }
    if (!inside) {
        return false;
    }
    const std::uint32_t id = s.id(window);
    const int parent = s.parent(window);
    // A cell's button picks its item, shown pressed, and opens its description.
    const auto& slots = slots_[expanded_ ? 1 : 0];
    for (std::size_t i = 0; i < slots.size() && i < shown_.size(); ++i) {
        if (id == kCellButton && parent == slots[i] && shown_[i].guid != 0) {
            chosen_ = chosenNow_ = shown_[i].guid;
            for (std::size_t k = 0; k < slots.size(); ++k) {
                s.setChecked(s.find(slots[k], kCellButton), k == i);
            }
            return true;
        }
    }
    closeDescription();
    if (id == kPrevious[0] || id == kPrevious[1]) {
        page_ = std::max(0, page_ - 1);
        showPage();
    } else if (id == kNext[0] || id == kNext[1]) {
        ++page_;
        showPage();
    } else if (id == kExpand || id == kCollapse) {
        expanded_ = id == kExpand;
        fit();
    }
    return true;
}

void CatalogGrid::pickFirst() {
    const auto first = std::find_if(items_.begin(), items_.end(), [](std::uint32_t guid) { return guid != 0; });
    if (first == items_.end()) {
        return;
    }
    firstPicked_ = *first;
    choose(*first);
}

void CatalogGrid::choose(std::uint32_t guid) {
    chosen_ = guid;
    if (screen_ == nullptr) {
        return;
    }
    const auto& slots = slots_[expanded_ ? 1 : 0];
    for (std::size_t i = 0; i < slots.size() && i < shown_.size(); ++i) {
        screen_->setChecked(screen_->find(slots[i], kCellButton), shown_[i].guid != 0 && shown_[i].guid == guid);
    }
}

void CatalogGrid::showItem(std::uint32_t guid) {
    chosen_ = guid;
    const auto it = std::find(items_.begin(), items_.end(), guid);
    if (screen_ == nullptr || it == items_.end()) {
        choose(guid);
        return;
    }
    const std::size_t pageSize = std::max<std::size_t>(1, slots_[expanded_ ? 1 : 0].size());
    page_ = static_cast<int>(static_cast<std::size_t>(it - items_.begin()) / pageSize);
    showPage();
}

void CatalogGrid::showPage() {
    auto& s = *screen_;
    const int h = expanded_ ? 1 : 0;
    s.setVisible(halves_[0], h == 0);
    s.setVisible(halves_[1], h == 1);
    const auto& slots = slots_[h];
    const std::size_t pageSize = std::max<std::size_t>(1, slots.size());
    // A page past the end goes back to the last one with items on it.
    std::size_t first = static_cast<std::size_t>(page_) * pageSize;
    while (first != 0 && first >= items_.size()) {
        --page_;
        first -= pageSize;
    }
    shown_.assign(slots.size(), Shown{});
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const bool shown = first + i < items_.size();
        s.setVisible(slots[i], shown);
        s.setVisible(s.find(slots[i], kCellPicture), false);
        if (shown) {
            shown_[i].guid = items_[first + i];
        }
        s.setChecked(s.find(slots[i], kCellButton), shown && chosen_ == shown_[i].guid);
    }
    s.setVisible(s.find(halves_[h], kPrevious[h]), page_ != 0);
    s.setVisible(s.find(halves_[h], kNext[h]), first + pageSize < items_.size());
    const int pages = static_cast<int>((items_.size() + pageSize - 1) / pageSize);
    bars_[h].set(s, pages, 1, page_);
    // Expand shows only when the collapsed catalog has more than one page. Collapse always shows.
    if (h == 0) {
        s.setVisible(s.find(halves_[0], kExpand), items_.size() > pageSize);
    }
}

void CatalogGrid::showPictures(const Picture& picture) {
    if (screen_ == nullptr || catalog_ < 0 || !screen_->visible(catalog_)) {
        return;
    }
    const auto& slots = slots_[expanded_ ? 1 : 0];
    for (std::size_t i = 0; i < shown_.size() && i < slots.size(); ++i) {
        if (shown_[i].guid == 0 || shown_[i].picture) {
            continue;
        }
        if (const auto* pixels = picture(shown_[i].guid)) {
            const int window = screen_->find(slots[i], kCellPicture);
            screen_->setImage(window, *pixels);
            screen_->setVisible(window, true);
            shown_[i].picture = true;
        }
    }
}

void CatalogGrid::showAffordable(const std::function<bool(std::uint32_t guid)>& affordable) {
    if (screen_ == nullptr || catalog_ < 0 || !screen_->visible(catalog_)) {
        return;
    }
    const auto& slots = slots_[expanded_ ? 1 : 0];
    for (std::size_t i = 0; i < shown_.size() && i < slots.size(); ++i) {
        if (shown_[i].guid == 0) {
            continue;
        }
        const bool can = affordable(shown_[i].guid);
        if (shown_[i].affordable == can) {
            continue;
        }
        shown_[i].affordable = can;
        const int left = can ? kCellSide : 0;
        screen_->setImagePart(screen_->find(slots[i], kCellBackground), {left, 0, left + kCellSide, kCellSide});
    }
}

void CatalogGrid::showDescription(const Description& description) {
    if (screen_ == nullptr || popup_ < 0) {
        return;
    }
    auto& s = *screen_;
    stripCount_ = std::min<std::size_t>(description.strips.size(), 2);
    for (std::size_t k = 0; k < 2; ++k) {
        strips_[k].set(k < stripCount_ ? &description.strips[k] : nullptr);
    }
    showStrips();
    s.setText(s.find(popup_, kPopupName), description.name + kPriceGap + description.price);
    s.setText(s.find(popup_, kPopupText), description.text);
    const int ratings = s.find(popup_, kPopupRatings);
    s.setText(ratings, description.ratings);
    s.setVisible(ratings, !description.ratings.empty());
    const int picture = s.find(popup_, kPopupPictureImage);
    s.setVisible(picture, description.picture != nullptr);
    if (description.picture != nullptr) {
        s.setImage(picture, *description.picture);
    }
    placeDescription();
    s.setVisible(popup_, true);
    s.raise(popup_);
}

void CatalogGrid::pictureChanged(std::uint32_t guid) {
    for (auto& cell : shown_) {
        if (cell.guid == guid) {
            cell.picture = false;
        }
    }
}

void CatalogGrid::showDescriptionPicture(const ts2::tga::Pixels* picture) {
    if (screen_ == nullptr || popup_ < 0) {
        return;
    }
    const int window = screen_->find(popup_, kPopupPictureImage);
    screen_->setVisible(window, picture != nullptr);
    if (picture != nullptr) {
        screen_->setImage(window, *picture);
    }
}

std::optional<std::uint32_t> CatalogGrid::itemAt(int window) const {
    if (screen_ == nullptr) {
        return std::nullopt;
    }
    const auto& slots = slots_[expanded_ ? 1 : 0];
    for (std::size_t i = 0; i < slots.size() && i < shown_.size(); ++i) {
        if (shown_[i].guid != 0 && (window == slots[i] || screen_->parent(window) == slots[i])) {
            return shown_[i].guid;
        }
    }
    return std::nullopt;
}

void CatalogGrid::showStripPictures(const std::vector<Strip>& strips) {
    for (std::size_t k = 0; k < stripCount_ && k < strips.size(); ++k) {
        strips_[k].setPictures(strips[k]);
    }
}

std::vector<std::size_t> CatalogGrid::chosenColours() const {
    std::vector<std::size_t> chosen;
    for (std::size_t k = 0; k < stripCount_; ++k) {
        chosen.push_back(strips_[k].choices()->chosen);
    }
    return chosen;
}

void CatalogGrid::showStrips() {
    auto& s = *screen_;
    const std::size_t longest = std::max(strips_[0].count(), strips_[1].count());
    s.setVisible(s.find(popup_, kPopupMoreChoices), stripCount_ > 1);
    const int divider = s.find(s.find(popup_, kPopupMoreChoices), kStripDivider);
    s.resize(divider, kSwatchStep * static_cast<int>(std::min<std::size_t>(longest, kSwatchesShown)) - kDividerShort, s.height(divider));
}

bool CatalogGrid::clickedPopup(int window) {
    auto& s = *screen_;
    for (std::size_t k = 0; k < stripCount_; ++k) {
        bool handled = false;
        const auto at = strips_[k].clicked(window, handled);
        if (at) {
            colourPicked_ = true;
            previewPicked_ = true;
            const auto& large = strips_[k].choices()->large;
            if (*at < large.size() && large[*at] != nullptr) {
                const int picture = s.find(popup_, kPopupPictureImage);
                s.setImage(picture, *large[*at]);
                s.setVisible(picture, true);
            }
        }
        if (handled) {
            return true;
        }
    }
    return false;
}

void CatalogGrid::closeDescription() {
    chosen_.reset();
    if (screen_ != nullptr && popup_ >= 0) {
        screen_->setVisible(popup_, false);
        for (const int slot : slots_[expanded_ ? 1 : 0]) {
            screen_->setChecked(screen_->find(slot, kCellButton), false);
        }
    }
}

void CatalogGrid::mouseAt(int x, int y) {
    if (screen_ != nullptr && catalog_ >= 0) {
        bars_[expanded_ ? 1 : 0].mouseMove(*screen_, x, y);
        followBar();
    }
    if (screen_ == nullptr || popup_ < 0 || !screen_->visible(popup_)) {
        return;
    }
    const auto over = [&](int window) {
        if (window < 0) {
            return false;
        }
        int left = 0, top = 0;
        for (int w = window; w >= 0; w = screen_->parent(w)) {
            left += screen_->x(w);
            top += screen_->y(w);
        }
        return x >= left && y >= top && x < left + screen_->width(window) && y < top + screen_->height(window);
    };
    if (!over(popup_) && !over(catalog_)) {
        screen_->setVisible(popup_, false);
    }
}

void CatalogGrid::mouseButton(int x, int y, bool down) {
    if (screen_ == nullptr || catalog_ < 0) {
        return;
    }
    auto& bar = bars_[expanded_ ? 1 : 0];
    if (down) {
        if (bar.mouseDown(*screen_, x, y, ts2::platform::ticksNs() / 1'000'000)) {
            closeDescription();
        }
    } else {
        bar.mouseUp(*screen_, x, y);
    }
    followBar();
}

void CatalogGrid::tick() {
    if (screen_ != nullptr && catalog_ >= 0 && bars_[expanded_ ? 1 : 0].tick(*screen_, ts2::platform::ticksNs() / 1'000'000)) {
        followBar();
    }
}

void CatalogGrid::followBar() {
    const int value = bars_[expanded_ ? 1 : 0].value();
    if (value != page_) {
        page_ = value;
        showPage();
    }
}

void CatalogGrid::placeDescription() {
    auto& s = *screen_;
    const int name = s.find(popup_, kPopupName), text = s.find(popup_, kPopupText), ratings = s.find(popup_, kPopupRatings);
    const int choices = s.find(popup_, kPopupChoices), bottom = s.find(popup_, kPopupBottom);
    s.fitHeightToText(name);
    s.fitHeightToText(text);
    s.fitHeightToText(ratings);
    s.move(name, s.x(name), kNameTop);
    s.move(text, s.x(text), s.y(name) + s.height(name));
    s.move(ratings, s.x(ratings), s.y(text) + s.height(text));
    // Without ratings, the strips sit under the description, the second 5 pixels under the first.
    const int above = s.visible(ratings) ? ratings : text;
    s.move(choices, s.x(choices), s.y(above) + s.height(above) + kChoicesGap);
    const int more = s.find(popup_, kPopupMoreChoices);
    const bool two = s.visible(more);
    s.move(more, s.x(more), s.y(choices) + s.height(choices) + kChoicesGap);
    const int last = two ? more : choices;
    // The button and the bar beside the strips sit centred on them, the long bar beside two.
    const int choicesMiddle = (s.y(choices) + s.y(last) + s.height(last)) / 2;
    s.setVisible(s.find(popup_, kPopupShortBar), !two);
    s.setVisible(s.find(popup_, kPopupLongBar), two);
    for (const std::uint32_t id : {kPopupButton, kPopupShortBar, kPopupLongBar}) {
        const int w = s.find(popup_, id);
        s.move(w, s.x(w), choicesMiddle - s.height(w) / 2);
    }
    s.move(bottom, kBottomLeft, s.y(last) + s.height(last) + kBottomGap);
    s.resize(bottom, kBottomWidth, s.height(bottom));
    const int background = s.find(popup_, kPopupBackground);
    s.resize(background, s.width(background), s.y(bottom) + kBackgroundOverlap);
    // The picture's frame sits centred up and down on the background, a pixel low.
    const int frame = s.find(popup_, kPopupPicture);
    s.move(frame, s.x(frame), (s.height(background) + kBackgroundOverlap - s.height(frame)) / 2);
    s.resize(popup_, s.width(popup_), s.y(bottom) + s.height(bottom));
    const int h = expanded_ ? 1 : 0;
    const int area = geometry_[h].area, anchor = s.find(halves_[h], kPopupAnchor[h]);
    if (area >= 0 && anchor >= 0) {
        const int x = s.position(area)[0] + (s.width(area) - s.width(popup_)) / 2;
        s.move(popup_, x, s.position(anchor)[1] + s.height(anchor) - s.height(popup_));
    }
}

} // namespace app
