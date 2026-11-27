#include "design_dialog.hpp"

#include "dialog_fills.hpp"

#include <algorithm>
#include <cstdint>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11, kScript = 0x3C605F85;
// The backgrounds for two strips and for one, the name, the strips, and the panel at the bottom with
// the cost line and the buttons.
constexpr std::uint32_t kTwoRows = 0xA1000, kOneRow = 0xA2000, kName = 0xA3000, kStrips[] = {0xA4000, 0xA5000};
constexpr std::uint32_t kPanel = 0xA6000, kCost = 0xA6001, kAccept = 0xA6002, kCancel = 0xA6003;
// Each strip's button that deletes a custom colour. When the game shows it is not known yet. For
// the game's own colours it stays hidden.
constexpr std::uint32_t kDeleteCustom = 0x000A;
constexpr int kSwatchesShown = 5;

} // namespace

bool DesignDialog::open(ts2::ui::Screen& screen, int mouseX, int mouseY, const std::string& name, const std::string& cost,
                        const std::vector<SwatchStrip::Choices>& strips) {
    close(screen);
    root_ = screen.load(kScripts, kScript, 0, 0);
    if (root_ < 0) {
        return false;
    }
    modalBefore_ = screen.modal();
    paintFills(screen, root_, kScripts, kScript);
    screen.setText(screen.find(root_, kName), name);
    screen.setText(screen.find(root_, kCost), cost);
    stripCount_ = std::min<std::size_t>(strips.size(), 2);
    for (std::size_t k = 0; k < 2; ++k) {
        const int strip = screen.find(root_, kStrips[k]);
        screen.setVisible(screen.find(strip, kDeleteCustom), false);
        strips_[k].attach(screen, strip, kSwatchesShown);
        strips_[k].set(k < stripCount_ ? &strips[k] : nullptr);
    }
    // With two strips the taller background shows. With one the second strip hides and the panel
    // moves up to the bottom of the shorter one. The dialog takes its background's size.
    const bool two = stripCount_ > 1;
    const int background = screen.find(root_, two ? kTwoRows : kOneRow);
    screen.setVisible(screen.find(root_, kTwoRows), two);
    screen.setVisible(screen.find(root_, kOneRow), !two);
    screen.setVisible(screen.find(root_, kStrips[1]), two);
    const int panel = screen.find(root_, kPanel);
    screen.move(panel, screen.x(panel), screen.height(background) - screen.height(panel));
    const int w = screen.width(background), h = screen.height(background);
    screen.resize(root_, w, h);
    screen.move(root_, mouseX > w ? mouseX - w : mouseX, mouseY > h ? mouseY - h : mouseY);
    screen.raise(root_);
    screen.setModal(root_);
    return true;
}

void DesignDialog::close(ts2::ui::Screen& screen) {
    if (root_ < 0) {
        return;
    }
    screen.remove(root_);
    screen.setModal(modalBefore_);
    root_ = -1;
    modalBefore_ = -1;
    stripCount_ = 0;
}

bool DesignDialog::clicked(ts2::ui::Screen& screen, int window, bool& accepted) {
    if (root_ < 0) {
        return false;
    }
    if (window == screen.find(root_, kAccept) || window == screen.find(root_, kCancel)) {
        accepted = window == screen.find(root_, kAccept);
        return true;
    }
    for (std::size_t k = 0; k < stripCount_; ++k) {
        bool handled = false;
        if (strips_[k].clicked(window, handled)) {
            picked_ = true;
        }
        if (handled) {
            break;
        }
    }
    return false;
}

std::vector<std::size_t> DesignDialog::chosen() const {
    std::vector<std::size_t> out;
    for (std::size_t k = 0; k < stripCount_; ++k) {
        out.push_back(strips_[k].choices()->chosen);
    }
    return out;
}

void DesignDialog::showPictures(const std::vector<SwatchStrip::Choices>& strips) {
    for (std::size_t k = 0; k < stripCount_ && k < strips.size(); ++k) {
        strips_[k].setPictures(strips[k]);
    }
}

} // namespace app
