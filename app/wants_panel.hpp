#pragma once

#include "formats/tga.hpp"
#include "ui/animated_bitmap.hpp"
#include "ui/screen.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace app {

// The live panel's wants and fears, in the layout that fits how many the Sim has. An icon shows the
// want's picture and badge, a burst behind Power wants and the influence mark on wants worth influence.
// A click shows the want's popup above the panel. A slot whose want changes keeps the old one while it
// animates: a glow over a satisfied want (or 2.5 seconds for a dropped one), a slot machine spin, the new want.
class WantsPanel {
public:
    struct Want {
        std::uint32_t guid = 0;
        std::int32_t argument = 0;  // what it is about: with the GUID, which want it is
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

    // Whose wants are shown and where their refill stands.
    struct Refill {
        std::int16_t sim = 0;
        bool pending = false;  // the Sim waits for new wants and fears
        // How many times each place was satisfied (fears from 1000), null for none.
        const std::map<std::size_t, std::uint32_t>* satisfied = nullptr;
    };
    // Shows the Sim's wants and fears in the layout for its want and fear slots, and moves the slots'
    // animations on. An emptied place keeps its want's icon while the Sim waits for new wants.
    void show(const std::vector<Want>& wants, const std::vector<Want>& fears, std::size_t wantSlots, std::size_t fearSlots,
              const Refill& refill);
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
        // The want the slot was last given. It is shown once the slot's animation ends.
        std::optional<Want> target;
        std::uint32_t satisfied = 0;  // the place's satisfied count when it was given `target`
        enum class Phase { None, Achieve, Change, Spin } phase = Phase::None;
        int animation = -1;  // the animated bitmap playing
        std::optional<ts2::ui::AnimatedBitmap> bitmap;
        std::chrono::steady_clock::time_point started;
    };
    struct Layout {
        int group = -1;
        std::size_t wantCount = 0, fearCount = 0;
        std::vector<Slot> wants, fears;
    };

    void fill(Slot& slot, const Want* want, bool fear);
    // Gives the slot a want (null for none), animating the change when it had one.
    void give(Slot& slot, const Want* want, bool fear, std::uint32_t satisfied, bool pending);
    // Plays one of the icon's animated bitmaps in the slot (false when it can't play).
    bool play(Slot& slot, Slot::Phase phase, std::uint32_t id);
    void animate(Slot& slot, bool fear);
    void stopAnimations(Slot& slot);
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
    std::int16_t sim_ = 0;  // whose wants are shown
    std::minstd_rand random_;
    std::map<std::uint32_t, ts2::tga::Pixels> pictures_;  // UI pictures already read
};

} // namespace app
