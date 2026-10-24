#include "aspiration_bars.hpp"

#include "content/ui_meters.hpp"

#include <utility>

namespace app {

namespace {

constexpr std::uint32_t kImages = 0x499DB772;
// Each bar's window, its fill and gradient pictures, and its platinum picture's window.
struct BarWindows {
    std::uint32_t bar, fill, gradient, platinum;
};
constexpr BarWindows kAspirationBar{0x4BDEFFA1, 0x0BB41003, 0x000B0151, 0x4BDEFFA2};
// The influence bar's five pictures, one per influence level (capacity 1 to 5), and their fills.
constexpr std::uint32_t kFirstCapacity = 0xCE9375A1;
constexpr std::uint32_t kCapacityFills[] = {0x0BB41022, 0x0BB41025, 0x0BB41030, 0x0BB41033, 0x0BB41036};
constexpr std::uint32_t kInfluenceGradient = 0x000B5180;
// Each capacity's animations of the level going up and down: strips of frames the game plays when
// the level changes. That isn't done, so they stay hidden (one is shown in the script).
constexpr std::uint32_t kLevelAnimations[] = {0x0E, 0x0F};
constexpr BarWindows kLifetimeBar{0x4BDEFFB1, 0x0BB41051, 0x0BB41096, 0x4BDEFFB2};
constexpr std::uint32_t kFill = 3;
// The lifetime aspiration is platinum at its top, 30000.
constexpr int kLifetimeTop = 30000;

} // namespace

void AspirationBars::attach(ts2::ui::Screen& screen, int panel) {
    screen_ = &screen;
    for (const auto& [w, bar] : {std::pair{kAspirationBar, &aspiration_}, std::pair{kLifetimeBar, &lifetime_}}) {
        const int window = screen.find(panel, w.bar);
        bar->fill = screen.find(window, kFill);
        bar->picture = screen.data().image(kImages, w.fill).value_or(ts2::tga::Pixels{});
        bar->gradient = screen.data().image(kImages, w.gradient).value_or(ts2::tga::Pixels{});
        bar->platinum = screen.find(panel, w.platinum);
        // The painted fill shows at its own size, as the script's picture did (the window is a pixel smaller).
        screen.resize(bar->fill, static_cast<int>(bar->picture.width), static_cast<int>(bar->picture.height));
        screen.setVisible(window, true);
    }
    for (std::size_t i = 0; i < influence_.size(); ++i) {
        const int capacity = screen.find(panel, kFirstCapacity + static_cast<std::uint32_t>(i));
        capacities_[i] = capacity;
        influence_[i].fill = screen.find(capacity, kFill);
        influence_[i].picture = screen.data().image(kImages, kCapacityFills[i]).value_or(ts2::tga::Pixels{});
        influence_[i].gradient = screen.data().image(kImages, kInfluenceGradient).value_or(ts2::tga::Pixels{});
        for (const std::uint32_t animation : kLevelAnimations) {
            screen.setVisible(screen.find(capacity, animation), false);
        }
    }
    if (capacities_[0] >= 0) {
        screen.setVisible(screen.parent(capacities_[0]), true);
    }
}

void AspirationBars::show(int aspirationScore, int lifetimeHappiness, int influence, int influenceLevel) {
    if (screen_ == nullptr) {
        return;
    }
    paint(aspiration_, aspirationScore, -100, 100, ts2::content::isPlatinum(aspirationScore));
    const auto happiness = static_cast<std::int16_t>(lifetimeHappiness);
    paint(lifetime_, happiness * 100 / kLifetimeTop, 0, 100, happiness == kLifetimeTop);
    for (Bar& bar : influence_) {
        paint(bar, influence, 0, 100, false);
    }
    if (influenceLevel != influenceLevel_) {
        for (std::size_t i = 0; i < capacities_.size(); ++i) {
            screen_->setVisible(capacities_[i], static_cast<int>(i) == influenceLevel);
        }
        influenceLevel_ = influenceLevel;
    }
}

void AspirationBars::paint(Bar& bar, int value, int min, int max, bool platinum) {
    const int column = ts2::content::meterRow(value, min, max, static_cast<int>(bar.gradient.width));
    if (column != bar.column && bar.fill >= 0 && bar.gradient.width > 0) {
        screen_->setImage(bar.fill, ts2::content::paintColumns(bar.picture, bar.gradient, column));
        bar.column = column;
    }
    if (bar.platinum >= 0 && static_cast<int>(platinum) != bar.platinumShown) {
        screen_->setVisible(bar.platinum, platinum);
        bar.platinumShown = platinum ? 1 : 0;
    }
}

} // namespace app
