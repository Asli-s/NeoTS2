#include "money_text.hpp"

#include "content/ui_text.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <string>

namespace app {

namespace {

constexpr float kLifeMs = 1500.0f;
constexpr float kRisePerMs = 0.13333334f;   // 200 pixels over the text's life
constexpr float kAlphaPerMs = 0.17f;        // 255 at the start, 0 at the end
constexpr int kOffset = 15;                 // from the mouse to the picture's right edge and top
constexpr int kWidth = 128;                 // the picture's least width
constexpr int kTextRight = 4;               // the text ends this far in from the picture's right edge

// The text in black at the eight neighbours of (2, 2), then in its colour at (2, 2).
struct Stamp {
    int dx, dy;
    bool coloured;
};
constexpr std::array<Stamp, 9> kStamps{{{1, 1, false}, {1, 2, false}, {1, 3, false}, {2, 1, false}, {2, 3, false},
                                         {3, 1, false}, {3, 2, false}, {3, 3, false}, {2, 2, true}}};

// Draws the glyph coverage of `text` over `out` at (x, y) in `colour`, alpha over alpha.
void stamp(ts2::tga::Pixels& out, const ts2::tga::Pixels& text, int x, int y, const std::array<std::uint8_t, 3>& colour) {
    for (std::uint32_t row = 0; row < text.height; ++row) {
        const int oy = y + static_cast<int>(row);
        if (oy < 0 || oy >= static_cast<int>(out.height)) {
            continue;
        }
        for (std::uint32_t col = 0; col < text.width; ++col) {
            const int ox = x + static_cast<int>(col);
            if (ox < 0 || ox >= static_cast<int>(out.width)) {
                continue;
            }
            const float a = static_cast<float>(text.rgba[(row * text.width + col) * 4 + 3]) / 255.0f;
            if (a <= 0) {
                continue;
            }
            std::uint8_t* d = &out.rgba[(static_cast<std::size_t>(oy) * out.width + static_cast<std::size_t>(ox)) * 4];
            const float da = static_cast<float>(d[3]) / 255.0f;
            const float oa = a + da * (1 - a);
            for (int c = 0; c < 3; ++c) {
                const float v = (static_cast<float>(colour[static_cast<std::size_t>(c)]) * a + static_cast<float>(d[c]) * da * (1 - a)) / oa;
                d[c] = static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f));
            }
            d[3] = static_cast<std::uint8_t>(std::clamp(oa * 255.0f, 0.0f, 255.0f));
        }
    }
}

} // namespace

ts2::tga::Pixels outlinedCostText(ts2::ui::Screen& screen, const std::string& text, const std::array<std::uint8_t, 3>& colour) {
    auto& fonts = screen.fonts();
    const auto image = fonts.draw(fonts.style("CostIndicator"), text);
    const int textW = static_cast<int>(image.pixels.width);
    const int width = std::max(kWidth, textW + kTextRight);
    const int height = static_cast<int>(image.pixels.height) + 4;
    ts2::tga::Pixels picture{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), {}};
    picture.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
    const int x = width - kTextRight - textW;
    constexpr std::array<std::uint8_t, 3> kBlack{0, 0, 0};
    for (const Stamp& s : kStamps) {
        stamp(picture, image.pixels, x + s.dx, s.dy, s.coloured ? colour : kBlack);
    }
    return picture;
}

void MoneyText::show(ts2::ui::Screen& screen, std::int32_t change, int mouseX, int mouseY) {
    // A sale (negative) shows a plus before the amount, a cost the minus of the money format.
    const std::string text = (change < 0 ? "+" : "") + ts2::content::moneyText(-std::int64_t{change});
    auto picture = outlinedCostText(screen, text, {255, 255, 255});
    const int width = static_cast<int>(picture.width), height = static_cast<int>(picture.height);
    if (window_ < 0) {
        const ts2::ui::Window window{{{"iid", "IGZWinGen"},
                                      {"winflag_visible", "yes"},
                                      {"winflag_ignoremouse", "yes"},
                                      {"paint", "yes"},
                                      {"area", std::format("(0,0,{},{})", width, height)}},
                                     {}};
        window_ = screen.addWindows({window}, 0, 0);
        if (window_ < 0) {
            return;
        }
        screen.setPassive(window_);
    }
    screen.resize(window_, width, height);
    screen.setImage(window_, std::move(picture));
    screen.raise(window_);
    screen.setVisible(window_, true);
    mouseX_ = mouseX;
    mouseY_ = mouseY;
    left_ = kLifeMs;
    place(screen);
}

void MoneyText::update(ts2::ui::Screen& screen, double seconds) {
    if (window_ < 0 || left_ <= 0) {
        return;
    }
    left_ -= static_cast<float>(seconds * 1000.0);
    if (left_ <= 0) {
        left_ = 0;
        screen.setVisible(window_, false);
        return;
    }
    place(screen);
}

void MoneyText::hide(ts2::ui::Screen& screen) {
    if (window_ >= 0) {
        screen.setVisible(window_, false);
    }
    left_ = 0;
}

void MoneyText::place(ts2::ui::Screen& screen) {
    const int y = static_cast<int>(static_cast<float>(mouseY_) - (kLifeMs - left_) * kRisePerMs);
    const int alpha = std::clamp(static_cast<int>(left_ * kAlphaPerMs), 0, 255);
    screen.move(window_, mouseX_ - kOffset - screen.width(window_), y - kOffset);
    screen.setImageColor(window_, {1, 1, 1, static_cast<float>(alpha) / 255.0f});
}

} // namespace app
