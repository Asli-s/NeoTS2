#pragma once

#include "formats/scenegraph.hpp"
#include "scene/outfit_hair.hpp"
#include "ui/screen.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace app {

// The game's appearance editor (Dialog type 0x15, UI script 0x8C159280 "Change Appearance"), opened
// from a mirror or dresser: a view of the Sim beside tabs for hair, jewelry, makeup, glasses,
// costume makeup, eyebrows, stubble and beards, then OK and Cancel. The hair page lists the hairs
// of a hair colour, 8 a page, with the arrows shown as the game shows them, each hair with its
// picture. The colour swatches pick the colour as the game's do, and a hair's item picks that hair
// for every outfit that has one. The Sim view shows the Sim with the colour and hair picked, and
// dragging it turns the Sim. The
// grey swatch and the other pages are not filled in yet.
class ChangeAppearanceDialog {
public:
    // Shows the dialog over everything, in the middle of a `width` x `height` screen. False when the
    // script is missing.
    bool open(ts2::ui::Screen& screen, int width, int height);
    void close(ts2::ui::Screen& screen);
    bool isOpen() const { return root_ >= 0; }
    // A hair's picture, asked for the first time its page is shown while the dialog is open. Nullopt
    // keeps the item's own picture.
    using PictureOf = std::function<std::optional<ts2::tga::Pixels>(const ts2::scenegraph::Link&)>;
    // The hairs offered in a hair colour (a hair tone GUID), in the order shown. With `onlyTone`,
    // hairs of no colour of their own are left out.
    using HairsOf = std::function<std::vector<ts2::scenegraph::Link>(const std::string& tone, bool onlyTone)>;
    // What the hair page asks of the Sim builder: the hairs and pictures above, a hair's own colour
    // (empty for none) and the version of a hair an outfit category wears in a colour.
    struct HairSource {
        HairsOf hairsOf;
        PictureOf pictureOf;
        std::function<std::string(const ts2::scenegraph::Link&)> toneOf;
        std::function<std::optional<ts2::scenegraph::Link>(const ts2::scenegraph::Link& hair, std::uint32_t category,
                                                           const std::string& tone, bool customTones)>
            outfitHair;
        // The Sim view's picture of the Sim with these hairs and colour, `width` x `height` pixels,
        // the Sim turned `turn` radians.
        std::function<std::optional<ts2::tga::Pixels>(const ts2::scene::HairSlots& slots, const std::string& tone, unsigned width,
                                                      unsigned height, float turn)>
            simView;
    };
    // Fills the hair page for a Sim of hair colour `tone` with hairs `slots` in its outfits: its
    // colour's swatch is picked and its hairs listed from the first page, each with its picture, and
    // its everyday hair shows marked.
    void setHairs(ts2::ui::Screen& screen, const std::string& tone, const ts2::scene::HairSlots& slots, HairSource source);
    const std::vector<ts2::scenegraph::Link>& hairs() const { return hairs_; }
    std::size_t firstHair() const { return first_; }
    // The hair colour and the outfits' hairs as picked, for OK to give the Sim.
    const std::string& hairTone() const { return workingTone_; }
    const ts2::scene::HairSlots& hairSlots() const { return slots_; }
    // A released button. The answer once given: true for OK, false for Cancel. The page arrows turn
    // the hair page and a hair's item picks that hair.
    std::optional<bool> clicked(ts2::ui::Screen& screen, int window);
    // Whether a point (in UI units) is over the Sim view.
    bool overSimView(const ts2::ui::Screen& screen, int x, int y) const;
    // Dragging the Sim view: from where the drag starts, to where the pointer is now, and its end,
    // which gives the middle of the view.
    void startTurn(int x);
    void turnTo(int x);
    std::array<int, 2> endTurn(const ts2::ui::Screen& screen);
    bool turning() const { return turnFrom_.has_value(); }
    // Once a frame: draws the Sim view again when it has turned.
    void update(ts2::ui::Screen& screen);

private:
    void showHairPage(ts2::ui::Screen& screen);
    void pickColour(ts2::ui::Screen& screen, const std::string& tone);
    void pickCustom(ts2::ui::Screen& screen);
    void checkSwatch(ts2::ui::Screen& screen, std::uint32_t swatch);
    void cutSwatches(ts2::ui::Screen& screen);
    void markChosen(ts2::ui::Screen& screen);
    void setWorkingTone(const std::string& tone);
    void pickHair(ts2::ui::Screen& screen, const ts2::scenegraph::Link& hair);
    void showSimView(ts2::ui::Screen& screen);

    int root_ = -1;
    int modalBefore_ = -1;
    std::vector<ts2::scenegraph::Link> hairs_;
    HairSource source_;
    std::string tone_;  // the colour picked, as its swatch's tone
    std::vector<ts2::scenegraph::Link> customHairs_;
    std::size_t first_ = 0;
    std::vector<int> items_;
    std::string workingTone_;  // the Sim's hair colour as picked, a hair tone GUID
    float turn_ = 0;           // the Sim view's turn, in radians, kept until the dialog closes
    std::optional<int> turnFrom_;  // the pointer's last x while dragging
    bool turned_ = false;      // the Sim view waits to be drawn again
    ts2::scene::HairSlots slots_;
    // The pictures already drawn, by hair, kept until the dialog closes.
    std::map<std::array<std::uint32_t, 4>, std::optional<ts2::tga::Pixels>> pictures_;
};

} // namespace app
