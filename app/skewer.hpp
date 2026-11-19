#pragma once

#include "formats/tga.hpp"
#include "ui/screen.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace app {

// The household's thumbnails over the puck (the game's "skewer"): Sims in a column growing up from
// the puck's skewer anchor, and pets and roommates on a smaller branch beside it. Each keeps the
// order given, the first at the top. The selected Sim's frame is lit, and each background takes
// the colour of its Sim's mood. Clicking a thumbnail picks that Sim, and the Sim the camera tracks
// has a star on its thumbnail. A column too long for the screen goes on in columns to the right.
class Skewer {
public:
    struct Member {
        std::int16_t sim = 0;                     // the Sim's object id
        std::optional<ts2::tga::Pixels> portrait;  // its whole portrait (drawn, or the character file's)
        // On the second branch: a pet (person data 0xBA not 0), a roommate (person data 0x54 bit
        // 0x800) or a Sim with Selection Flags (person data 0x9E) bit 3.
        bool branch = false;
        bool pet = false;  // shows one away icon, the pets' way
    };

    // `puck` is the loaded puck script's root window.
    void attach(ts2::ui::Screen& screen, int puck);

    // Puts up a thumbnail per member (none: the skewer is hidden).
    void setHousehold(std::vector<Member> members);
    // Puts a new portrait on a Sim's thumbnail.
    void setPortrait(std::int16_t sim, const ts2::tga::Pixels& portrait);
    // Shows the skewer in live mode only (the game hides it in the options menu).
    void setLiveMode(bool live);
    // Hides it while the free camera is on, with the rest of the UI.
    void setHidden(bool hidden);
    void select(std::int16_t sim);
    // Shows the star on the tracked Sim's thumbnail (0: on none).
    void setTracked(std::int16_t sim);
    // Colours a Sim's thumbnail background by its mood (-100..100).
    void setMood(std::int16_t sim, int mood);
    // Shows the icons for where a Sim is away (person data 0x34, the UI icon flags, one icon per bit).
    // `roommate` (person data 0x54 bit 0x800) shows the roommate work icon in place of at work.
    // A pet shows only the first of its flags' icons.
    void setAwayIcons(std::int16_t sim, std::uint16_t flags, bool roommate);
    // A thumbnail that isn't pressable can't be clicked.
    void setPressable(std::int16_t sim, bool pressable);
    // Sets the tip of a Sim's thumbnail, the Sim's name.
    void setName(std::int16_t sim, const std::string& name);

    // Whether anyone stands on the second branch.
    [[nodiscard]] bool hasBranch() const {
        return std::any_of(thumbnails_.begin(), thumbnails_.end(), [](const Thumbnail& t) { return t.branch; });
    }
    // The Sim whose thumbnail `window` is part of, if any.
    [[nodiscard]] std::optional<std::int16_t> clicked(int window) const;

private:
    void layout();
    // A portrait's middle (or its shadow's), scaled to the thumbnail and cut to its rounded shape.
    ts2::tga::Pixels thumbnailPicture(const ts2::tga::Pixels& portrait) const;

    ts2::ui::Screen* screen_ = nullptr;
    int puck_ = -1, skewer_ = -1;
    bool live_ = true, hidden_ = false;
    // The skewer's constants table, the margins and ceilings for the Sims' column then the branch.
    int offsetX_ = 1, spacer_ = 1, overflowCeiling_ = 100;
    std::array<int, 2> topMargin_{0, 0}, bottomMargin_{2, -10}, ceiling_{100, 100};
    struct Thumbnail {
        std::int16_t sim = 0;
        bool branch = false, pet = false;
        int window = -1;
        int background = -1;
        int moodRow = -1;  // the mood gradient row its background shows
        int awayFlags = -1;  // the UI icon flags its icons show
    };
    std::vector<Thumbnail> thumbnails_;
    std::int16_t selected_ = 0;
    std::int16_t tracked_ = 0;
    ts2::tga::Pixels moodGradient_, backgroundShape_;
};

} // namespace app
