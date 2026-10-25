#include "meter_bar.hpp"

#include "content/ui_meters.hpp"

namespace app {

namespace {

constexpr std::uint32_t kImages = 0x499DB772;
constexpr int kMax = 1000;

} // namespace

MeterBar::MeterBar(ts2::ui::Screen& screen, int fill, std::uint32_t picture, bool vertical)
    : screen_(&screen), fill_(fill), vertical_(vertical) {
    if (fill >= 0) {
        width_ = screen.width(fill);
        height_ = screen.height(fill);
    }
    picture_ = screen.data().image(kImages, picture).value_or(ts2::tga::Pixels{});
}

void MeterBar::show(int value, int steps) {
    if (screen_ == nullptr || fill_ < 0) {
        return;
    }
    const int pixels = ts2::content::barPixels(value, 0, kMax, steps, vertical_ ? height_ : width_);
    if (pixels == pixels_) {
        return;
    }
    pixels_ = pixels;
    screen_->setVisible(fill_, pixels > 0);
    if (pixels <= 0) {
        return;
    }
    const auto w = static_cast<std::uint32_t>(width_), h = static_cast<std::uint32_t>(height_);
    const auto p = static_cast<std::uint32_t>(pixels);
    if (vertical_) {
        // The fill shows the picture's bottom rows, at the bar's bottom.
        screen_->setImage(fill_, ts2::content::placed(picture_, w, p, 0, pixels - height_));
        screen_->move(fill_, 0, height_ - pixels);
        screen_->resize(fill_, width_, pixels);
    } else {
        // The fill shows the picture's left columns, from the bar's left.
        screen_->setImage(fill_, ts2::content::placed(picture_, p, h, 0, 0));
        screen_->resize(fill_, pixels, height_);
    }
}

} // namespace app
