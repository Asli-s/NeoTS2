#pragma once

#include "live_panel.hpp"
#include "lot_life.hpp"
#include "skewer.hpp"
#include "content/target_pictures.hpp"
#include "sim/clock.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace app {

// The game's live-mode panels over a lot, placed as the game places them: the puck in the bottom left
// corner with the household's thumbnails above it, the live panel beside it (the selected Sim, the
// clock, the speed buttons) and the options menu the puck's Options button opens in the live panel's
// place.
class LotUi {
public:
    LotUi(ts2::ui::Screen& screen, int width, int height);

    void open();
    void close();

    // What a released button asks for.
    enum class Action { None, Neighborhood, Pause, Normal, High, Ultra, OptionsOpened, OptionsClosed, SelectSim };
    Action clicked(int window);
    // The Sim whose thumbnail was clicked (for SelectSim).
    std::int16_t clickedSim() const { return clickedSim_; }

    // Shows or hides the options menu (it replaces the live panel while open).
    void showOptions(bool show);
    bool optionsShown() const;

    // Shows the simulation's clock, speed and funds.
    void show(const std::string& clock, ts2::sim::Speed speed, bool paused, std::int32_t funds);
    // Puts up the household's thumbnails.
    void showHousehold(std::vector<Skewer::Member> members);
    // Shows the lot's life: the clock, speed and funds, the selected Sim (its aspiration and needs)
    // and every household Sim's mood.
    void showLife(LotLife& life);

private:
    // Moves a panel so that its anchor (a hidden window captioned "<window id>:bl") sits on that
    // window of the puck.
    void anchor(int panel);
    int left(int window) const;
    std::vector<WantsPanel::Want> panelWants(LotLife& life, const std::vector<ts2::swaf::Want>& saved);
    // The middle of a neighbor's portrait for want icons, made once; null when it has none.
    const ts2::tga::Pixels* targetPicture(LotLife& life, std::int16_t neighbor);
    int top(int window) const;

    ts2::ui::Screen& screen_;
    int width_, height_;
    int puck_ = -1, live_ = -1, options_ = -1;
    LivePanel panel_;
    Skewer skewer_;
    std::int16_t clickedSim_ = 0;
    // Each household Sim's portrait, and whose the live panel shows (-1: none yet).
    std::map<std::int16_t, ts2::tga::Pixels> portraits_;
    int portraitShown_ = -1;
    ts2::content::TargetPictures targets_;
};

} // namespace app
