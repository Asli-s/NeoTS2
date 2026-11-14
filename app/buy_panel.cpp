#include "buy_panel.hpp"

#include "content/ui_meters.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace app {

namespace {

namespace catalog = ts2::sim::catalog;

constexpr std::uint32_t kScripts = 0xA99D8A11, kImages = 0x499DB772;
// The sort tabs.
constexpr std::uint32_t kFunctionSort = 0x2C584E6C, kRoomSort = 0x4C578F23, kCommunitySort = 0x0D87DCF9, kCollections = 0x2CACE0CD;
// Each sort's group of buttons, and its background.
constexpr std::uint32_t kFunctionGroup = 0x00000A00, kRoomGroup = 0x00000C00, kCommunityGroup = 0x00000F00;
constexpr std::uint32_t kFunctionArt = 0x4CC48AA1, kRoomArt = 0x4CC48AA2, kCollectionsArt = 0x4CC48AA3, kCommunityArt = 0x4CC48AA5,
                        kCommunityRepeat = 0x4CC48AA7;
constexpr std::array<std::uint32_t, 5> kWholesaleArt = {0x5CC48AA1, 0x5CC48AA2, 0x5CC48AA3, 0x5CC48AA5, 0x5CC48AA6};
// The function categories are buttons 0xA100 to 0xA900, the n-th function sort bit n - 1, each with
// a sub-category panel 0xBn00 whose button 0xBn10 is Back.
constexpr std::uint32_t kFirstFunction = 0xA100, kLastFunction = 0xA900;
// The room buttons and their room sort bits.
struct RoomButton {
    std::uint32_t id;
    std::uint16_t room;
};
constexpr std::array<RoomButton, 9> kRooms = {{{0xC100, catalog::room::kLivingRoom},
                                               {0xC200, catalog::room::kDining},
                                               {0xC300, catalog::room::kOutside},
                                               {0xC400, catalog::room::kMisc},
                                               {0xC500, catalog::room::kStudy},
                                               {0xC600, catalog::room::kBedroom},
                                               {0xC700, catalog::room::kKitchen},
                                               {0xC800, catalog::room::kKids},
                                               {0xC900, catalog::room::kBathroom}}};
// The catalog: its normal view, the expanded and wholesale ones (not done), and in the normal view
// the item area, page buttons, scrollbar, Organize Items, Expand and the custom content buttons.
constexpr std::uint32_t kNormal = 0x1000, kExpanded = 0x2000, kWholesale = 0x3300, kWholesaleExpanded = 0x5500;
constexpr std::uint32_t kArea = 0x1006, kAreaMark = 0x1007, kPrevious = 0x1004, kNext = 0x1005, kScrollbar = 0x1008, kOrganize = 0x3001,
                        kExpand = 0x1009, kCustomContent = 0x100A;
// The tools: the hand, the sledgehammer, undo and redo, and the eyedropper and view buttons (not done).
constexpr std::uint32_t kHand = 0xCC607031;
constexpr std::uint32_t kSledgehammer = 0x000000AE, kUndo = 0xCC607033, kRedo = 0xCC607034;
constexpr std::array<std::uint32_t, 5> kToolsNotDone = {0xCC607032, 0xCC6070AF, 0xCC607030, 0xCC607035, 0xCC607036};
// A catalog item: the game's thumbnail (UI script 0xFEEDEABE) made smaller to fit four across and
// two down, with its background button, picture and overlay button, and the pictures' shape.
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

void BuyPanel::attach(ts2::ui::Screen& screen, int content) {
    screen_ = &screen;
    content_ = content;
    if (content < 0) {
        return;
    }
    auto& s = screen;
    for (const std::uint32_t id : {kExpanded, kWholesale, kWholesaleExpanded}) {
        s.setVisible(s.find(content, id), false);
    }
    for (const std::uint32_t id : kWholesaleArt) {
        s.setVisible(s.find(content, id), false);
    }
    const int normal = s.find(content, kNormal);
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
    s.setEnabled(s.find(content, kCommunitySort), false);
    s.setEnabled(s.find(content, kCollections), false);
    s.setChecked(s.find(content, kHand), true);
    for (const std::uint32_t id : kToolsNotDone) {
        s.setEnabled(s.find(content, id), false);
    }
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

void BuyPanel::showTools(bool sledgehammer, bool canUndo, bool canRedo) {
    if (screen_ == nullptr || content_ < 0) {
        return;
    }
    auto& s = *screen_;
    s.setChecked(s.find(content_, kHand), !sledgehammer);
    s.setChecked(s.find(content_, kSledgehammer), sledgehammer);
    s.setEnabled(s.find(content_, kUndo), canUndo);
    s.setEnabled(s.find(content_, kRedo), canRedo);
}

void BuyPanel::reset() {
    byRoom_ = false;
    category_ = 0;
    categoryButton_ = 0;
    subsort_ = 0;
    page_ = 0;
    dirty_ = true;
    if (screen_ != nullptr && content_ >= 0) {
        showSort();
    }
}

void BuyPanel::showSort() {
    auto& s = *screen_;
    s.setChecked(s.find(content_, kFunctionSort), !byRoom_);
    s.setChecked(s.find(content_, kRoomSort), byRoom_);
    s.setVisible(s.find(content_, kFunctionGroup), !byRoom_);
    s.setVisible(s.find(content_, kRoomGroup), byRoom_);
    s.setVisible(s.find(content_, kCommunityGroup), false);
    s.setVisible(s.find(content_, kFunctionArt), !byRoom_);
    s.setVisible(s.find(content_, kRoomArt), byRoom_);
    s.setVisible(s.find(content_, kCollectionsArt), false);
    s.setVisible(s.find(content_, kCommunityArt), false);
    s.setVisible(s.find(content_, kCommunityRepeat), false);
    // By function: the category buttons until one is chosen, then that category's sub-categories.
    const int first = s.find(content_, kFirstFunction);
    const int categories = first >= 0 ? s.parent(first) : -1;
    s.setVisible(categories, !byRoom_ && categoryButton_ == 0);
    for (std::uint32_t id = kFirstFunction; id <= kLastFunction; id += 0x100) {
        const std::uint32_t panel = 0xB000 | (id & 0x0F00);
        s.setVisible(s.find(content_, panel), !byRoom_ && categoryButton_ == id);
        // The chosen sub-category's button shows pressed.
        if (const int p = s.find(content_, panel); p >= 0) {
            for (const int b : s.children(p)) {
                const std::uint16_t bit = catalog::subsortOfButton(s.id(b));
                if (bit != 0) {
                    s.setChecked(b, categoryButton_ == id && subsort_ == bit);
                }
            }
        }
    }
    for (const auto& r : kRooms) {
        s.setChecked(s.find(content_, r.id), byRoom_ && category_ == r.room);
    }
}

BuyPanel::Click BuyPanel::clicked(int window) {
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
    // The tools: the hand and the sledgehammer take turns lit; undo and redo act at once.
    if (const std::uint32_t tool = s.id(window); tool == kHand || tool == kSledgehammer) {
        s.setChecked(s.find(content_, kHand), tool == kHand);
        s.setChecked(s.find(content_, kSledgehammer), tool == kSledgehammer);
        (tool == kHand ? out.hand : out.sledgehammer) = true;
        return out;
    } else if (tool == kUndo || tool == kRedo) {
        (tool == kUndo ? out.undo : out.redo) = true;
        return out;
    }
    // A catalog item.
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        for (int w = window; w >= 0; w = s.parent(w)) {
            if (w == slots_[i]) {
                if (slotGuids_[i] != 0) {
                    out.buy = slotGuids_[i];
                }
                return out;
            }
        }
    }
    const std::uint32_t id = s.id(window);
    if (id == kFunctionSort || id == kRoomSort) {
        const bool byRoom = id == kRoomSort;
        if (byRoom != byRoom_) {
            byRoom_ = byRoom;
            category_ = 0;
            categoryButton_ = 0;
            subsort_ = 0;
            page_ = 0;
        }
    } else if (id >= kFirstFunction && id <= kLastFunction && (id & 0xFF) == 0) {
        categoryButton_ = id;
        category_ = static_cast<std::uint16_t>(1u << (((id >> 8) & 0xF) - 1));
        subsort_ = 0;
        page_ = 0;
    } else if ((id & 0xF0FF) == 0xB010) {  // Back, in a sub-category panel
        categoryButton_ = 0;
        category_ = 0;
        subsort_ = 0;
        page_ = 0;
    } else if (const std::uint16_t bit = catalog::subsortOfButton(id); bit != 0) {
        subsort_ = subsort_ == bit ? 0 : bit;  // clicked again, the whole category shows
        page_ = 0;
    } else if (const auto room = std::find_if(kRooms.begin(), kRooms.end(), [&](const RoomButton& r) { return r.id == id; });
               room != kRooms.end()) {
        category_ = category_ == room->room ? 0 : room->room;
        page_ = 0;
    } else if (window == previous_) {
        page_ = std::max(0, page_ - 1);
    } else if (window == next_) {
        ++page_;
    } else {
        return out;
    }
    dirty_ = true;
    showSort();
    return out;
}

std::vector<catalog::Item> BuyPanel::listed(const std::vector<catalog::Item>& all, bool byRoom, std::uint16_t category, std::uint16_t subsort) {
    std::vector<catalog::Item> out;
    for (const auto& item : all) {
        if (category != 0 && (byRoom ? (item.rooms & category) == 0 : (item.functions & category) == 0)) {
            continue;
        }
        if (!byRoom && subsort != 0 && (item.subsorts & subsort) == 0) {
            continue;
        }
        out.push_back(item);
    }
    return out;
}

void BuyPanel::refresh(const Source& source) {
    if (screen_ == nullptr || content_ < 0 || source.items == nullptr) {
        return;
    }
    if (dirty_) {
        listed_ = listed(*source.items, byRoom_, category_, subsort_);
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
        const auto& item = listed_[first + i];
        // A picture that wasn't ready is asked for again each frame until it is.
        const int picture = s.find(slots_[i], kThumbPicture);
        if (slotGuids_[i] != item.guid || !s.visible(picture)) {
            const ts2::tga::Pixels* pixels = source.picture ? source.picture(item) : nullptr;
            s.setVisible(picture, pixels != nullptr);
            if (pixels != nullptr) {
                s.setImage(picture, mask_.width > 0 ? ts2::content::masked(*pixels, mask_) : *pixels);
            }
        }
        if (slotGuids_[i] != item.guid) {
            slotGuids_[i] = item.guid;
            const std::string name = source.name ? source.name(item) : std::string();
            s.setTip(s.find(slots_[i], kThumbOverlay), (name.empty() ? std::string() : name + "\n") + priceText(item.price));
        }
    }
    s.setVisible(previous_, page_ != 0);
    s.setVisible(next_, first + pageSize < listed_.size());
    dirty_ = false;
}

} // namespace app
