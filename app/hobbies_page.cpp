#include "hobbies_page.hpp"

#include <algorithm>
#include <string>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11;
constexpr std::uint32_t kItemScript = 0x0BB40013;

// The page's half that holds the list, its item area, and its "maximized" and "minimized" art.
constexpr std::uint32_t kHalf = 0x1000, kArea = 0x1006;
constexpr std::uint32_t kMaximizedArt = 0xA1, kMinimizedArt = 0xA2;
// The Constants Table: two columns, five rows, no margins.
constexpr int kColumns = 2, kRows = 5;

// An item's name button, its bar's fill, the favourite's glow and the Secret Lot mark. The bar is
// the skill bar, filled in ten steps.
constexpr std::uint32_t kName = 0x1, kBar = 0x2, kFill = 0x3, kGlow = 0x5, kSecretLot = 0x7;
constexpr std::uint32_t kFillPicture = 0xAB201945;
constexpr int kSteps = 10;

// The hobbies in the game's order: person data 0xCC (Cuisine) to 0xD5 (Music & Dance), named by
// entries 1 to 10 of Live text table 0xF2. Person data 0xD7 is the favourite, by its index.
constexpr std::size_t kFirstHobby = 0xCC, kHobbies = 10, kFavourite = 0xD7;
constexpr std::uint32_t kLiveText = 0x7F96C284, kNames = 0xF2;

std::int16_t at(std::span<const std::int16_t> data, std::size_t index) { return index < data.size() ? data[index] : 0; }

} // namespace

void HobbiesPage::attach(ts2::ui::Screen& screen, int page) {
    screen_ = &screen;
    page_ = page;
    auto& s = screen;
    const int area = s.find(s.find(page, kHalf), kArea);
    if (area < 0) {
        return;
    }
    for (std::size_t i = 0; i < kHobbies; ++i) {
        Item item;
        item.window = s.load(kScripts, kItemScript, 0, 0, area);
        if (item.window < 0) {
            break;
        }
        // The columns are centred in the area, the rows start at its top.
        const int w = s.width(item.window), h = s.height(item.window);
        const int x0 = (s.width(area) - kColumns * w) >> 1;
        s.move(item.window, x0 + static_cast<int>(i % kColumns) * w, static_cast<int>(i / kColumns) * h);
        const int bar = s.find(item.window, kBar);
        item.bar = MeterBar(s, bar >= 0 ? s.find(bar, kFill) : -1, kFillPicture, false);
        s.setText(s.find(item.window, kName), s.data().text(kLiveText, kNames << 16 | static_cast<std::uint32_t>(i + 1)));
        items_.push_back(std::move(item));
    }
}

void HobbiesPage::setMaximized(bool maximized) {
    if (screen_ == nullptr) {
        return;
    }
    const int half = screen_->find(page_, kHalf);
    screen_->setVisible(screen_->find(half, kMaximizedArt), maximized);
    screen_->setVisible(screen_->find(half, kMinimizedArt), !maximized);
}

void HobbiesPage::show(std::span<const std::int16_t> personData, std::span<const std::int16_t> secretLots) {
    if (screen_ == nullptr) {
        return;
    }
    auto& s = *screen_;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        Item& item = items_[i];
        const auto hobby = static_cast<std::int16_t>(kFirstHobby + i);
        const int value = at(personData, kFirstHobby + i);
        if (value != item.value) {
            item.value = value;
            item.bar.show(value, kSteps);
        }
        // The favourite glows only once the Sim has some enthusiasm for it.
        const int glow = at(personData, kFavourite) == hobby && value > 0;
        if (glow != item.glow) {
            item.glow = glow;
            s.setVisible(s.find(s.find(item.window, kBar), kGlow), glow != 0);
        }
        const int secretLot = std::find(secretLots.begin(), secretLots.end(), hobby) != secretLots.end();
        if (secretLot != item.secretLot) {
            item.secretLot = secretLot;
            s.setVisible(s.find(item.window, kSecretLot), secretLot != 0);
        }
    }
}

} // namespace app
