#pragma once

#include "ui/screen.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace app {

// The options menu's panels, one open at a time, in a lot's menu or the neighborhood's. While a
// panel is open the menu grows up to the panel's height and out to its right end, keeping its
// bottom, and its own parts stay where they were on screen. The menu's right end hides under the
// panel. Opening a panel closes the one that was open.
class OptionsMenu {
public:
    explicit OptionsMenu(ts2::ui::Screen& screen) : screen_(screen) {}

    void setMenu(int menu) {
        menu_ = menu;
        open_ = -1;
    }
    int menu() const { return menu_; }
    // The window of the menu's button with this id.
    int button(std::uint32_t id) const { return screen_.find(menu_, id); }

    // A panel sits in the menu where the menu's right end is, under its buttons.
    void place(int panel) {
        const int end = screen_.find(menu_, kMenuEnd), buttons = screen_.find(menu_, kMenuButtons);
        if (end >= 0 && buttons >= 0) {
            screen_.move(panel, screen_.x(end), 0);
            screen_.placeUnder(panel, buttons);
        }
    }

    bool isOpen(int panel) const { return panel >= 0 && open_ == panel; }

    void open(int panel, std::uint32_t buttonId) {
        close();
        menuSize_ = {screen_.width(menu_), screen_.height(menu_)};
        menuShift_ = std::max(0, screen_.height(panel) - menuSize_[1]);
        shiftParts(panel, menuShift_);
        screen_.resize(menu_, std::max(menuSize_[0], screen_.x(panel) + screen_.width(panel)), menuSize_[1] + menuShift_);
        screen_.setVisible(screen_.find(menu_, kMenuEnd), false);
        open_ = panel;
        openButton_ = buttonId;
        screen_.setChecked(button(buttonId), true);
        screen_.setVisible(panel, true);
    }

    void close() {
        if (open_ < 0) {
            return;
        }
        screen_.setVisible(open_, false);
        shiftParts(open_, -menuShift_);
        screen_.resize(menu_, menuSize_[0], menuSize_[1]);
        screen_.setVisible(screen_.find(menu_, kMenuEnd), true);
        screen_.setChecked(button(openButton_), false);
        open_ = -1;
    }

private:
    // The menu's right end picture and the frame holding its buttons.
    static constexpr std::uint32_t kMenuEnd = 0x0000A100, kMenuButtons = 0x0000A200;

    void shiftParts(int panel, int shift) {
        for (const int part : screen_.children(menu_)) {
            if (part != panel) {
                screen_.move(part, screen_.x(part), screen_.y(part) + shift);
            }
        }
        screen_.move(menu_, screen_.x(menu_), screen_.y(menu_) - shift);
    }

    ts2::ui::Screen& screen_;
    int menu_ = -1, open_ = -1;
    std::uint32_t openButton_ = 0;
    std::array<int, 2> menuSize_{};  // the menu's size with no panel open
    int menuShift_ = 0;              // how far its parts moved down for the open panel
};

} // namespace app
