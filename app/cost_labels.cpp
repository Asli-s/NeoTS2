#include "cost_labels.hpp"

#include "money_text.hpp"

#include "content/ui_text.hpp"

namespace app {

namespace {

constexpr int kOffset = 15;      // from the mouse to the texts' right edges, and up to the price's top
constexpr int kSizeBelow = 5;    // from the mouse down to the size's top
constexpr float kPriceAlpha = 150.0f / 255.0f;
constexpr std::array<std::uint8_t, 3> kYellow{255, 255, 40}, kRed{255, 40, 40}, kWhite{255, 255, 255};

} // namespace

void CostLabels::show(ts2::ui::Screen& screen, std::int32_t cost, bool cantAfford, const std::string& size, int mouseX, int mouseY) {
    // Money back (negative) shows a plus, a cost the minus of the money format.
    const std::string price = (cost < 0 ? "+" : "") + ts2::content::moneyText(-std::int64_t{cost});
    // The colour is part of the picture, so a change of colour draws it again.
    if (cantAfford != cantAfford_) {
        priceText_.clear();
        cantAfford_ = cantAfford;
    }
    place(screen, price_, price, cantAfford ? kRed : kYellow, cantAfford ? 1.0f : kPriceAlpha, mouseX, mouseY - kOffset);
    if (size.empty()) {
        if (size_ >= 0) {
            screen.setVisible(size_, false);
        }
        return;
    }
    place(screen, size_, size, kWhite, 1.0f, mouseX, mouseY + kSizeBelow);
}

void CostLabels::hide(ts2::ui::Screen& screen) {
    for (const int window : {price_, size_}) {
        if (window >= 0) {
            screen.setVisible(window, false);
        }
    }
}

void CostLabels::place(ts2::ui::Screen& screen, int& window, const std::string& text, const std::array<std::uint8_t, 3>& colour, float alpha,
                       int mouseX, int top) {
    std::string& shown = &window == &price_ ? priceText_ : sizeText_;
    if (window < 0) {
        const ts2::ui::Window w{{{"iid", "IGZWinGen"}, {"winflag_visible", "yes"}, {"winflag_ignoremouse", "yes"}, {"paint", "yes"}, {"area", "(0,0,1,1)"}},
                                {}};
        window = screen.addWindows({w}, 0, 0);
        if (window < 0) {
            return;
        }
        screen.setPassive(window);
        shown.clear();
    }
    if (text != shown) {
        auto picture = outlinedCostText(screen, text, colour);
        screen.resize(window, static_cast<int>(picture.width), static_cast<int>(picture.height));
        screen.setImage(window, std::move(picture));
        shown = text;
    }
    screen.setImageColor(window, {1, 1, 1, alpha});
    screen.raise(window);
    screen.setVisible(window, true);
    screen.move(window, mouseX - kOffset - screen.width(window), top);
}

} // namespace app
