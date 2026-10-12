#pragma once

#include "formats/tga.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace app {

// The live panel's wants and fears: an icon per want in the layout that fits the Sim's wants and
// fears. An icon shows the want's picture with its small badge, a burst behind Power wants and the
// influence mark on wants worth influence. A slot is refilled only when its want changes.
class WantsPanel {
public:
    struct Want {
        std::uint32_t guid = 0;
        std::uint32_t primaryIcon = 0, secondaryIcon = 0;  // UI pictures (group 0x499DB772); 0 for none
        bool hasTarget = false;  // aimed at a Sim or an object: its picture comes from that
        const ts2::tga::Pixels* targetPicture = nullptr;  // the target's picture, when known (kept by the caller)
        bool power = false;      // a Power want or fear
        bool influence = false;  // worth influence
    };

    // `content` is the loaded wants script's root window.
    void attach(ts2::ui::Screen& screen, int content);

    void show(const std::vector<Want>& wants, const std::vector<Want>& fears);
    // Refills every slot on the next show (the pictures they point to may be gone).
    void refresh();

private:
    struct Slot {
        int window = -1;  // the slot
        int icon = -1;    // its icon (the icon script, loaded when first needed)
        std::uint32_t guid = 0;
        const ts2::tga::Pixels* target = nullptr;
        bool filled = false;
    };
    struct Layout {
        int group = -1;
        std::size_t wantCount = 0, fearCount = 0;
        std::vector<Slot> wants, fears;
    };

    void fill(Slot& slot, const Want* want, bool fear);
    const ts2::tga::Pixels& picture(std::uint32_t instance);

    ts2::ui::Screen* screen_ = nullptr;
    std::vector<Layout> layouts_;
    int shown_ = -1;
    std::map<std::uint32_t, ts2::tga::Pixels> pictures_;  // UI pictures already read
};

} // namespace app
