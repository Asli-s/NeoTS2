#pragma once

#include "ui/screen.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace app {

// The Aspiration Meter popup (UI script 0x4906502A): the Sim's aspiration level and lifetime want,
// above the live panel. It shows once the mouse has rested 0.5 s on the aspiration meter's button
// and hides when the mouse leaves the button.
class AspirationPopup {
public:
    struct Lifetime {
        std::uint32_t icon = 0;  // a UI picture (group 0x499DB772), 0 for none
        std::string name;        // the name, then its aspiration and influence values on a line
        std::string blurb;
        bool fulfilled = false;  // shows the check mark
    };
    struct Content {
        std::string levelLine, level;  // "<name>'s Aspiration Level is:" and the level's name
        std::string lifetimeLine;      // "<name>'s Lifetime Want is:"
        std::optional<Lifetime> lifetime;
    };

    // The meter's button in the live panel. A click on it opens and closes the wants.
    static constexpr std::uint32_t kButton = 0xBB0001;

    // `panel` is the live panel's root window, which holds the meter's button and the popup's anchor.
    void attach(ts2::ui::Screen& screen, int panel);
    // The mouse is at (x, y) in screen pixels.
    void mouseAt(int x, int y);
    // Moves the popup on: `make` gives its content when the popup is about to show.
    void update(const std::function<Content()>& make);
    void hide();

private:
    void fill(const Content& content);
    bool overButton(int x, int y) const;

    ts2::ui::Screen* screen_ = nullptr;
    int panel_ = -1, button_ = -1, popup_ = -1;
    // From the script: the pointer's height, how far the background reaches into it, its distance
    // from the popup's right edge, its margin beside the anchor, and the popup's room under the tip.
    struct Measures {
        int pointerHeight = 0, overlap = 0, right = 0, side = 0, bottom = 0;
    } measures_;
    bool over_ = false, filled_ = false;
    int mouseX_ = 0, mouseY_ = 0;
    std::chrono::steady_clock::time_point since_;
    // How far the wants pop-out raised the wants tile as the mouse came, none when it was closed.
    std::optional<int> popoutRaise_;
};

} // namespace app
