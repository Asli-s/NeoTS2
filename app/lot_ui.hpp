#pragma once

#include "live_panel.hpp"
#include "sim/clock.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <string>

namespace app {

// The game's live-mode panels over a lot, placed as the game places them: the puck in the bottom left
// corner, the live panel beside it (the selected Sim, the clock, the speed buttons) and the options
// menu the puck's Options button opens in the live panel's place.
class LotUi {
public:
    LotUi(ts2::ui::Screen& screen, int width, int height);

    void open();
    void close();

    // What a released button asks for.
    enum class Action { None, Neighborhood, Pause, Normal, High, Ultra, OptionsOpened, OptionsClosed };
    Action clicked(int window);

    // Shows or hides the options menu (it replaces the live panel while open).
    void showOptions(bool show);
    bool optionsShown() const;

    // Shows the simulation's clock, speed and funds.
    void show(const std::string& clock, ts2::sim::Speed speed, bool paused, std::int32_t funds);
    // Shows the selected Sim's aspiration.
    void showSim(std::int16_t aspiration) { panel_.setAspiration(aspiration); }

private:
    // Moves a panel so that its anchor (a hidden window captioned "<window id>:bl") sits on that
    // window of the puck.
    void anchor(int panel);
    int left(int window) const;
    int top(int window) const;

    ts2::ui::Screen& screen_;
    int width_, height_;
    int puck_ = -1, live_ = -1, options_ = -1;
    LivePanel panel_;
};

} // namespace app
