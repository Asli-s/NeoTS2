#pragma once

#include "formats/tga.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace app {

// The household's thumbnails over the puck (the game's "skewer"): one per Sim, the first family
// member at the bottom, the column growing up from the puck's skewer anchor. The selected Sim's
// frame is lit, and each background takes the colour of its Sim's mood. Clicking a thumbnail
// picks that Sim.
class Skewer {
public:
    struct Member {
        std::int16_t sim = 0;                     // the Sim's object id
        std::optional<ts2::tga::Pixels> portrait;  // its whole portrait (drawn, or the character file's)
    };

    // `puck` is the loaded puck script's root window.
    void attach(ts2::ui::Screen& screen, int puck);

    // Puts up a thumbnail per member (none: the skewer is hidden).
    void setHousehold(std::vector<Member> members);
    // Shows the skewer in live mode only (the game hides it in the options menu).
    void setLiveMode(bool live);
    void select(std::int16_t sim);
    // Colours a Sim's thumbnail background by its mood (-100..100).
    void setMood(std::int16_t sim, int mood);

    // The Sim whose thumbnail `window` is part of, if any.
    [[nodiscard]] std::optional<std::int16_t> clicked(int window) const;

private:
    void layout();
    // A portrait's middle (or its shadow's), scaled to the thumbnail and cut to its rounded shape.
    ts2::tga::Pixels thumbnailPicture(const ts2::tga::Pixels& portrait) const;

    ts2::ui::Screen* screen_ = nullptr;
    int puck_ = -1, skewer_ = -1;
    bool live_ = true;
    // The skewer's constants table.
    int offsetX_ = 1, spacer_ = 1, topMargin_ = 0, bottomMargin_ = 2;
    struct Thumbnail {
        std::int16_t sim = 0;
        int window = -1;
        int background = -1;
        int moodRow = -1;  // the mood gradient row its background shows
    };
    std::vector<Thumbnail> thumbnails_;
    std::int16_t selected_ = 0;
    ts2::tga::Pixels moodGradient_, backgroundShape_;
};

} // namespace app
