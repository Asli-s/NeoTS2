#pragma once

#include "formats/tga.hpp"
#include "ui/screen.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace app {

// The live panel's wants and fears: an icon per want in the layout that fits the Sim's wants and
// fears. An icon shows the want's picture with its small badge, a burst behind Power wants and the
// influence mark on wants worth influence. A slot is refilled only when its want changes. A click on
// a want shows its popup above the panel: picture, name with its values, description and the hold hint.
class WantsPanel {
public:
    struct Want {
        std::uint32_t guid = 0;
        std::uint32_t primaryIcon = 0, secondaryIcon = 0;  // UI pictures (group 0x499DB772), 0 for none
        bool hasTarget = false;  // aimed at a Sim or an object: its picture comes from that
        const ts2::tga::Pixels* targetPicture = nullptr;  // the target's picture, when known (kept by the caller)
        bool power = false;      // a Power want or fear
        bool influence = false;  // worth influence
        bool held = false;       // held by the player (a right click): it stays until fulfilled
        // The popup's texts (the name with its aspiration and influence lines, and the description)
        // and the target's picture at the popup's size.
        std::string popupName, description;
        std::string tip;  // the icon's tooltip: the name and the values, a line each
        const ts2::tga::Pixels* popupTargetPicture = nullptr;

        bool operator==(const Want&) const = default;
    };

    // `content` is the loaded wants script's root window.
    void attach(ts2::ui::Screen& screen, int content);

    // Shows the Sim's wants and fears in the layout for its want and fear slots.
    void show(const std::vector<Want>& wants, const std::vector<Want>& fears, std::size_t wantSlots, std::size_t fearSlots);
    // Refills every slot on the next show (the pictures they point to may be gone).
    void refresh();
    // The want (index, and whether it is a fear) whose icon `window` is part of, if any.
    struct SlotRef {
        std::size_t index = 0;
        bool fear = false;
    };
    std::optional<SlotRef> slotOf(int window) const;
    // A click on a want's icon shows its popup (true when `window` is part of one).
    bool clicked(int window);
    // The mouse is at (x, y): the popup closes once it leaves the panel's hover area (the clicked
    // want stays highlighted).
    void mouseAt(int x, int y);
    // Closes the popup and the highlight.
    void hidePopup();

private:
    struct Slot {
        int window = -1;  // the slot
        int icon = -1;    // its icon (the icon script, loaded when first needed)
        std::optional<Want> shown;
        bool stale = false;  // redrawn on the next show even if the want is the same
    };
    struct Layout {
        int group = -1;
        std::size_t wantCount = 0, fearCount = 0;
        std::vector<Slot> wants, fears;
    };

    void fill(Slot& slot, const Want* want, bool fear);
    void showPopup(const Want& want);
    void highlight();
    // A window's place on the screen.
    std::array<int, 2> screenPlace(int window) const;
    const ts2::tga::Pixels& picture(std::uint32_t instance);

    ts2::ui::Screen* screen_ = nullptr;
    int content_ = -1;
    int popup_ = -1;
    std::optional<Want> popupWant_;  // the clicked want: what the popup shows
    std::vector<Want> wants_, fears_;  // as last shown
    std::vector<Layout> layouts_;
    int shown_ = -1;
    std::map<std::uint32_t, ts2::tga::Pixels> pictures_;  // UI pictures already read
};

} // namespace app
