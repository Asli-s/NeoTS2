#pragma once

#include "formats/tga.hpp"
#include "game_scroll_bar.hpp"
#include "swatch_strip.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace app {

// The catalog of the Buy and Build panels: a paged grid of item cells in a collapsed and an
// expanded half, widened to the screen's right edge, and the description popup a clicked cell
// opens. The panels pick the items and lay the catalog beside their buttons.
class CatalogGrid {
public:
    // An item's picture, null while there is none or it isn't read yet.
    using Picture = std::function<const ts2::tga::Pixels*(std::uint32_t guid)>;
    // A strip of colour choices in the description popup. Its large pictures are at the popup
    // picture's size.
    using Strip = SwatchStrip::Choices;
    // What the description popup shows of the chosen item.
    struct Description {
        std::string name, price, text, ratings;
        const ts2::tga::Pixels* picture = nullptr;
        std::vector<Strip> strips;  // at most two
    };

    // `catalog` is the catalog's window in the panel's script.
    void attach(ts2::ui::Screen& screen, int catalog);
    int window() const { return catalog_; }
    // The items listed, in order. A new list starts on its first page.
    void setItems(std::vector<std::uint32_t> items);
    const std::vector<std::uint32_t>& items() const { return items_; }
    // Handles a click on the catalog's windows (cells, page and expand buttons). Returns whether it was one.
    bool clicked(int window);
    // Sizes the catalog's half to as many columns as its items need over its rows, at most as many
    // as fit before the screen's right edge. Called after setItems.
    void fit();
    // Joins the collapsed catalog to the art left of it: a plain strip takes the place of its
    // rounded left end, as for a Build mode category with no buttons of its own.
    void setJoined(bool joined);
    // Puts the pictures on the cells shown that don't have theirs yet.
    void showPictures(const Picture& picture);
    // Shows each shown cell blue, or red when `affordable` says the household can't buy its item.
    void showAffordable(const std::function<bool(std::uint32_t guid)>& affordable);
    // The item whose cell was just clicked, once, for its description popup.
    std::optional<std::uint32_t> takeChosen() { return std::exchange(chosenNow_, std::nullopt); }
    // Shows item `guid`'s cell pressed, as if it had been clicked, without its description popup.
    void choose(std::uint32_t guid);
    // Turns to the page item `guid` is on and shows its cell pressed, as choose does.
    void showItem(std::uint32_t guid);
    // Picks the first item listed, shown pressed without its description popup, as opening a Build
    // mode category or sub-category does. The pick is taken once.
    void pickFirst();
    std::optional<std::uint32_t> takeFirstPicked() { return std::exchange(firstPicked_, std::nullopt); }
    void showDescription(const Description& description);
    // The picture of item `guid` changed: its cell takes the new one from the next showPictures.
    void pictureChanged(std::uint32_t guid);
    void showDescriptionPicture(const ts2::tga::Pixels* picture);
    // The item whose cell `window` is or holds, if any.
    std::optional<std::uint32_t> itemAt(int window) const;
    // New pictures for the colour strips of the description shown, its colours chosen kept.
    void showStripPictures(const std::vector<Strip>& strips);
    void closeDescription();
    // The mouse is at screen (x, y): the description popup hides once the mouse is over neither it
    // nor the catalog. The chosen item stays chosen.
    void mouseAt(int x, int y);
    // The left button went down (or up) at screen (x, y), for the scroll bar under the pages.
    void mouseButton(int x, int y, bool down);
    // Called each frame: a press held on the scroll bar's track keeps paging.
    void tick();
    // The colour chosen in each strip of the description shown.
    std::vector<std::size_t> chosenColours() const;
    // Whether a colour was just picked, once.
    bool takeColourPicked() { return std::exchange(colourPicked_, false); }
    // The same for the description picture, which shows every strip's colour.
    bool takePreviewPicked() { return std::exchange(previewPicked_, false); }

private:
    void showPage();
    void placeDescription();
    void showStrips();
    bool clickedPopup(int window);

    ts2::ui::Screen* screen_ = nullptr;
    int catalog_ = -1;
    int halves_[2] = {-1, -1};   // collapsed and expanded
    std::vector<int> slots_[2];  // the cells made in each half's area
    // Each half's size as its script gives it, and the windows that move or stretch as it widens.
    struct Part {
        int window = -1, x = 0, width = 0;
        bool stretches = false;
    };
    struct Half {
        int area = -1, width = 0, areaWidth = 0;
        std::vector<Part> parts;
    };
    Half geometry_[2];
    // What each cell of the half shown shows: the item and whether its picture is on it.
    struct Shown {
        std::uint32_t guid = 0;
        bool picture = false;
        std::optional<bool> affordable;  // which background frame the cell shows, once set
    };
    std::vector<Shown> shown_;
    bool expanded_ = false;
    int page_ = 0;
    // Each half's scroll bar, whose value is the page shown.
    GameScrollBar bars_[2];
    void followBar();
    std::vector<std::uint32_t> items_;
    int popup_ = -1;  // the description popup (UI script 0x49063003)
    SwatchStrip strips_[2];
    std::size_t stripCount_ = 0;  // the strips the description shown has
    bool colourPicked_ = false;
    bool previewPicked_ = false;
    std::optional<std::uint32_t> chosen_, chosenNow_, firstPicked_;
};

} // namespace app
