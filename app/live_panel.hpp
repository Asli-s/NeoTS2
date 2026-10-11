#pragma once

#include "ui/screen.hpp"

#include <cstdint>

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

private:
    void layout();

    ts2::ui::Screen* screen_ = nullptr;
    int panel_ = -1;
    int wantsContent_ = -1, needsContent_ = -1, relationshipsContent_ = -1;
    bool wantsOpen_ = false;
    Sub sub_ = Sub::None;
};

} // namespace app
