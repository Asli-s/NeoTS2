#pragma once

#include "ui/screen.hpp"

#include <cstdint>

namespace app {

// The game's "Saving Lot" box (UI script 0x49065010): its caption and a house picture, in the middle
// of the screen, over everything and taking the mouse while a lot saves.
class SavingPopup {
public:
    // Shows the box in the middle of a `width` x `height` screen. Closing it makes the window that
    // was modal before modal again.
    void open(ts2::ui::Screen& screen, int width, int height) {
        close(screen);
        root_ = screen.load(kScripts, kScript, 0, 0);
        if (root_ < 0) {
            return;
        }
        screen.move(root_, (width - screen.width(root_)) / 2, (height - screen.height(root_)) / 2);
        modalBefore_ = screen.modal();
        screen.raise(root_);
        screen.setModal(root_);
    }

    void close(ts2::ui::Screen& screen) {
        if (root_ >= 0) {
            screen.remove(root_);
            screen.setModal(modalBefore_);
            root_ = -1;
            modalBefore_ = -1;
        }
    }

    bool isOpen() const { return root_ >= 0; }

private:
    static constexpr std::uint32_t kScripts = 0xA99D8A11, kScript = 0x49065010;
    int root_ = -1;
    int modalBefore_ = -1;
};

} // namespace app
