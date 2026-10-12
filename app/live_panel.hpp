#pragma once

#include "mood_meter.hpp"
#include "needs_panel.hpp"
#include "wants_panel.hpp"
#include "ui/screen.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace app {

// The live panel's two parts that open and close on their own: wants and fears (the aspiration
// button) and the sub-panel the small buttons pick (needs or relationships). The panel's frame
// (caps, tiles, button column, mood meter) is laid out the way the game lays it out for each state.
class LivePanel {
public:
    enum class Sub { None, Needs, Relationships };

    // `panel` is the loaded live panel script's root window.
    void attach(ts2::ui::Screen& screen, int panel);

    // Handles a click on one of the panel's own buttons; returns whether it was one.
    bool clicked(int window);

    // Shows the aspiration button for the selected Sim's Aspiration person data (one flag set).
    void setAspiration(std::int16_t aspiration);
    // Shows the selected Sim's needs (life stage and motives as whole numbers).
    void setNeeds(std::int16_t lifeStage, const std::array<int, 16>& motives) { needs_.show(lifeStage, motives); }
    // Shows the selected Sim's wants and fears.
    void setWants(const std::vector<WantsPanel::Want>& wants, const std::vector<WantsPanel::Want>& fears) {
        wants_.show(wants, fears);
    }
    // Shows the selected Sim's mood and aspiration score (person data 0xA0) on the mood meter.
    void setMood(int mood, int aspirationScore) { mood_.show(mood, aspirationScore); }
    // Refills every want icon on the next setWants.
    void refreshWants() { wants_.refresh(); }

private:
    void layout();

    ts2::ui::Screen* screen_ = nullptr;
    int panel_ = -1;
    int wantsContent_ = -1, needsContent_ = -1, relationshipsContent_ = -1;
    bool wantsOpen_ = false;
    Sub sub_ = Sub::None;
    NeedsPanel needs_;
    MoodMeter mood_;
    WantsPanel wants_;
};

} // namespace app
