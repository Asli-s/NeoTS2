#pragma once

#include "swatch_strip.hpp"
#include "ui/screen.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace app {

// The Design tool's dialog (UI script 0x3C605F85, "object design dialog"): the object's catalog name,
// one or two strips of its colour choices, the line "Cost to Update: §15", and Accept and Cancel.
class DesignDialog {
public:
    // Shows the dialog over everything with its top left at the mouse. It goes left of the mouse when
    // the mouse is further from the screen's left edge than the dialog is wide, and above it when
    // further from the top than the dialog is high. `strips` are the object's choices, each starting
    // at its current colour (at most two are shown). `cost` is the cost line. False when the script
    // is missing.
    bool open(ts2::ui::Screen& screen, int mouseX, int mouseY, const std::string& name, const std::string& cost,
              const std::vector<SwatchStrip::Choices>& strips);
    void close(ts2::ui::Screen& screen);
    bool isOpen() const { return root_ >= 0; }
    // A released button or a click on a strip. True when the dialog is answered: `accepted` then
    // tells Accept from Cancel. Accept is there however little was picked.
    bool clicked(ts2::ui::Screen& screen, int window, bool& accepted);
    // Whether a colour was just picked, once.
    bool takePicked() { return std::exchange(picked_, false); }
    // The choice picked in each strip, in the order given.
    std::vector<std::size_t> chosen() const;
    // Puts in pictures drawn since the dialog opened, for the strips in the order given.
    void showPictures(const std::vector<SwatchStrip::Choices>& strips);

private:
    int root_ = -1;
    int modalBefore_ = -1;
    SwatchStrip strips_[2];
    std::size_t stripCount_ = 0;
    bool picked_ = false;
};

} // namespace app
