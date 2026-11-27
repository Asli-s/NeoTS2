#include "swatch_strip.hpp"

#include <algorithm>
#include <cstdint>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11;
constexpr std::uint32_t kPrevious = 0x1004, kNext = 0x1005, kArea = 0x1006;
constexpr std::uint32_t kSwatch = 0x3C605F86, kSwatchButton = 0x0001, kSwatchPicture = 0x0002, kSwatchIcon = 0x0003;
constexpr int kSwatchStep = 50;

} // namespace

void SwatchStrip::attach(ts2::ui::Screen& screen, int strip, int perPage) {
    screen_ = &screen;
    strip_ = strip;
    cells_.clear();
    const int area = screen.find(strip, kArea);
    for (int i = 0; i < perPage; ++i) {
        const int cell = screen.load(kScripts, kSwatch, screen.x(area) + i * kSwatchStep, screen.y(area), strip);
        screen.setVisible(screen.find(cell, kSwatchIcon), false);
        screen.setVisible(cell, false);
        cells_.push_back(cell);
    }
}

void SwatchStrip::set(const Choices* choices) {
    if (choices != nullptr) {
        choices_ = *choices;
    } else {
        choices_.reset();
    }
    page_ = choices_ && !cells_.empty() ? static_cast<int>(choices_->chosen / cells_.size()) : 0;
    show();
}

void SwatchStrip::setPictures(const Choices& pictures) {
    if (!choices_ || pictures.pictures.size() != choices_->pictures.size() || pictures.large.size() != choices_->large.size()) {
        return;
    }
    choices_->pictures = pictures.pictures;
    choices_->large = pictures.large;
    show();
}

void SwatchStrip::show() {
    if (screen_ == nullptr || strip_ < 0) {
        return;
    }
    auto& s = *screen_;
    const std::size_t total = count(), size = cells_.size();
    const std::size_t first = static_cast<std::size_t>(page_) * size;
    for (std::size_t i = 0; i < size; ++i) {
        const int cell = cells_[i];
        const std::size_t at = first + i;
        s.setVisible(cell, at < total);
        if (at >= total) {
            continue;
        }
        const int picture = s.find(cell, kSwatchPicture);
        s.setVisible(picture, choices_->pictures[at] != nullptr);
        if (choices_->pictures[at] != nullptr) {
            s.setImage(picture, *choices_->pictures[at]);
        }
        s.setChecked(s.find(cell, kSwatchButton), at == choices_->chosen);
    }
    s.setVisible(s.find(strip_, kPrevious), first != 0);
    s.setVisible(s.find(strip_, kNext), first + size < total);
}

std::optional<std::size_t> SwatchStrip::clicked(int window, bool& handled) {
    handled = false;
    if (screen_ == nullptr || strip_ < 0 || !choices_) {
        return std::nullopt;
    }
    auto& s = *screen_;
    if (window == s.find(strip_, kPrevious) || window == s.find(strip_, kNext)) {
        page_ = std::max(0, page_ + (s.id(window) == kNext ? 1 : -1));
        show();
        handled = true;
        return std::nullopt;
    }
    for (std::size_t i = 0; i < cells_.size(); ++i) {
        const std::size_t at = static_cast<std::size_t>(page_) * cells_.size() + i;
        if (s.id(window) == kSwatchButton && s.parent(window) == cells_[i] && at < count()) {
            choices_->chosen = at;
            show();
            handled = true;
            return at;
        }
    }
    return std::nullopt;
}

} // namespace app
