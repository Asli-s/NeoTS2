#pragma once

#include "platform/platform.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace app {

// The game's "Set Baby Name" dialog (UI script 0x2D91050A) for human babies: the baby panel, or the
// twins panel for two, each baby's girl or boy icon and a box holding its first name, and OK. Every
// box starts with its whole name selected and the first box has the keyboard. Only the box with the
// keyboard shows its selection and caret. A click puts the caret there, dragging selects, and typing
// replaces the selection. The other panels in the script (pets, robots, renaming an older Sim) aren't
// shown yet, and the caret doesn't blink yet.
class BabyNameDialog {
public:
    struct Baby {
        bool female = false;
        std::string firstName;
    };

    // Shows the dialog over everything, its panel in the middle of a `width` x `height` screen (half the
    // room left, rounded down), as the game places it. False when the script is missing.
    bool open(ts2::ui::Screen& screen, int width, int height, const std::vector<Baby>& babies);
    void close(ts2::ui::Screen& screen);
    bool isOpen() const { return root_ >= 0; }

    // A released button: true for OK, nothing for any other window.
    bool clickedOk(const ts2::ui::Screen& screen, int window) const;
    // The left button going down on a name box gives it the keyboard and puts its caret there.
    // Moving with the button held selects up to the mouse.
    void mouseDown(ts2::ui::Screen& screen, int x, int y);
    void mouseMoved(ts2::ui::Screen& screen, int x, int y);
    void mouseUp() { dragging_ = false; }
    // Typing into the box that has the keyboard.
    void text(ts2::ui::Screen& screen, const std::string& typed);
    void key(ts2::ui::Screen& screen, ts2::platform::Key key);

    // The names in the boxes, one per baby, in the babies' order.
    std::vector<std::string> names() const;

private:
    struct Box {
        int window = -1;           // -1: the panel has no box for this baby
        std::string name;
        std::size_t caret = 0;     // in bytes, between characters
        std::size_t anchor = 0;    // the selection runs from here to the caret
    };

    void focus(ts2::ui::Screen& screen, std::size_t box);
    void show(ts2::ui::Screen& screen, std::size_t box);
    // Takes out the focused box's selection. False when there is none.
    bool eraseSelection();

    int root_ = -1;
    int modalBefore_ = -1;
    std::vector<Box> boxes_;
    std::size_t focus_ = 0;
    bool dragging_ = false;
};

} // namespace app
