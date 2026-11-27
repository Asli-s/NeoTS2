#pragma once

#include "formats/tga.hpp"
#include "ui/screen.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace app {

// A strip of colour choices in a UI script: page buttons (0x1004 previous, 0x1005 next) and an area
// (0x1006) filled with a page of swatch cells (UI script 0x3C605F86, 50 x 51, each a button that
// shows the chosen one, a picture and a small icon that stays hidden). The Buy catalog's description
// popup and the Design tool's dialog have such strips.
class SwatchStrip {
public:
    // Each choice's picture (null for none) and the one chosen.
    struct Choices {
        std::vector<const ts2::tga::Pixels*> pictures;
        std::vector<const ts2::tga::Pixels*> large;  // each choice's picture at a bigger size, where one shows it
        std::size_t chosen = 0;
    };

    // Makes `perPage` cells in the area of strip window `strip`.
    void attach(ts2::ui::Screen& screen, int strip, int perPage);
    int window() const { return strip_; }
    // The choices shown (none: no cells), on the page of the chosen one.
    void set(const Choices* choices);
    // New pictures for the choices shown, keeping the choice and the page. Nothing when the count differs.
    void setPictures(const Choices& pictures);
    const Choices* choices() const { return choices_ ? &*choices_ : nullptr; }
    std::size_t count() const { return choices_ ? choices_->pictures.size() : 0; }
    int perPage() const { return static_cast<int>(cells_.size()); }
    // A click on the strip's page buttons or cells: the choice picked, if a cell was clicked. A page
    // button turns the page.
    std::optional<std::size_t> clicked(int window, bool& handled);

private:
    void show();

    ts2::ui::Screen* screen_ = nullptr;
    int strip_ = -1;
    std::vector<int> cells_;
    std::optional<Choices> choices_;
    int page_ = 0;
};

} // namespace app
