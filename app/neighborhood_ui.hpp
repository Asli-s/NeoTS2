#pragma once

#include "content/ui_data.hpp"
#include "audio_options_panel.hpp"
#include "description_box.hpp"
#include "game_options_panel.hpp"
#include "graphics_options_panel.hpp"
#include "ui/screen.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace app {

// The game's panels over a neighborhood: the puck in the bottom left corner, the top bar with the
// neighborhood's name, and the bins the puck opens (families without a home, lots to place), placed
// as the game places them.
class NeighborhoodUi {
public:
    NeighborhoodUi(ts2::ui::Screen& screen, ts2::content::UiData& data, int width, int height);

    // What a lot's card and tag show: a household's name, picture and bio, or a community lot's
    // description, with the lot's name (its address). A lot for sale has no card, only its tag.
    struct LotCard {
        bool community = false;
        std::string address, name, text;
        ts2::tga::Pixels picture;
        bool forSale = false;
        // A household's worth (its funds and its lot's value), or the price of a lot for sale. None
        // when it isn't known: the lot has no readable simulator state, or the lot isn't shown for sale.
        std::optional<std::int64_t> money;
        // An apartment lot's households in the neighborhood's order. A household is none when no
        // family has the apartment as its home.
        struct Tenant {
            std::string name;
            std::int64_t funds = 0;
            bool atHome = false;  // its current lot is its home
            std::uint32_t lot = 0;  // its apartment's own lot, which entering it loads
            // Its Sims' portraits in the family's order, empty for a Sim without one.
            std::vector<ts2::tga::Pixels> sims;
        };
        std::vector<std::optional<Tenant>> households;
        // Its apartments, at most 4 while any is lived in, and its rent range, lowest first.
        int apartments = 0;
        std::pair<std::int64_t, std::int64_t> rent;
    };
    // What the bins show from a neighborhood's files and the user folder's lot catalog. Reading it
    // takes a while, so it is read with the neighborhood, off the main thread.
    struct Content {
        std::vector<ts2::tga::Pixels> familyPictures, lotPictures;
        std::vector<std::uint32_t> occupied;
        std::map<std::uint32_t, LotCard> cards;  // by lot id
    };
    static Content read(const std::filesystem::path& folder, const std::filesystem::path& user);

    // Shows the panels for a neighborhood (its folder in Neighborhoods and its name).
    void open(const std::filesystem::path& folder, const std::string& name, Content content);
    void close();

    enum class Bin { None, Families, Lots };
    void showBin(Bin bin);

    // Shows or hides the options menu the puck's options button opens.
    void showOptions(bool show);
    // Hides every neighborhood panel (under the neighborhood chooser), or shows them again as they were.
    void hideAll(bool hide);
    // Hides the puck in the lower left corner while the free camera is on, or shows it again.
    void hidePuck(bool hide) {
        if (puck_ >= 0) {
            screen_.setVisible(puck_, !hide);
        }
    }
    GameOptionsPanel& gameOptions() { return gameOptions_; }
    GraphicsOptionsPanel& graphicsOptions() { return graphicsOptions_; }
    AudioOptionsPanel& audioOptions() { return audioOptions_; }
    // The screen has a new size: what sits at its bottom edge moves with it.
    void setSize(int width, int height);
    enum class Action { None, MainMenu, EnterLot, Quit, GameOptionChanged, GraphicsOptionChanged, AudioOptionChanged, DayNight };
    // A button released: what the player asked for.
    Action clicked(int window);
    // The puck's camera button held down. They turn and zoom while held, as the lot puck's do.
    enum class CameraButton { None, RotateLeft, RotateRight, ZoomIn, ZoomOut };
    CameraButton heldCameraButton() const;

    // Whether a click on a lot opens its card: lived-in lots always, community lots only while the
    // lots bin is open.
    bool clickable(std::uint32_t lot) const;
    // Shows a lot's card beside the lot, whose centre is at (`lotX`, `lotY`) on screen.
    void showCard(std::uint32_t lot, int lotX, int lotY);
    // The lot's centre on screen moved: the glow behind the card follows it.
    void setCardPoint(int x, int y);
    void hideCard();
    std::uint32_t cardLot() const { return cardLot_; }
    // The lot Enter Lot loads: the card's lot, or on an apartment lot's card the chosen household's
    // apartment. 0 when there is none to enter.
    std::uint32_t lotToEnter() const;
    // The household bio box of the card on screen, or null when no household card shows.
    DescriptionBox* cardBio() { return cardLot_ != 0 && bioShown_ ? &bio_ : nullptr; }

    // Shows the tag of the lot under the mouse (0: none) over the lot's centre, at (x, y) on screen.
    void showTag(std::uint32_t lot, int x, int y);

    int root() const { return panel_; }
    // The lots a household lives on: those that can be played.
    const std::vector<std::uint32_t>& occupiedLots() const { return occupied_; }

private:
    struct Strip {
        int container = -1;         // the strip's row (window 0x1000)
        std::vector<int> tiles;     // the tiles placed in it
    };
    void layoutStrip(Strip& strip, const std::vector<ts2::tga::Pixels>& pictures);
    // Puts a glow picture in its window at (x, y) on screen, or hides the window for an empty one.
    void showGlow(int window, ts2::tga::Pixels picture, int x, int y);
    // Fills the Apartment Info layout for an apartment lot's card.
    void showApartments(const LotCard& card);
    // Shows apartment row `row` (1 to 4) chosen and the others not.
    void chooseApartment(int row);
    // Hides every apartment row's Sim thumbnails, as the game does when the card is loaded or
    // closed. Filling the card sets them only on rows with a household, so an unoccupied row, or
    // a row whose household is missing, keeps what it had.
    void clearApartmentSims();

    ts2::ui::Screen& screen_;
    ts2::content::UiData& data_;
    int width_, height_;
    int panel_ = -1, families_ = -1, lots_ = -1, card_ = -1, options_ = -1;
    // The soft glow behind the card: its box round the panel picture, drawn with the card, and its
    // reach to the lot, drawn over the box whenever the lot's point moves.
    int glow_ = -1, glowTail_ = -1;
    int glowPanel_ = -1;  // the panel picture it surrounds
    std::array<float, 4> glowPanelRect_{};  // the panel on screen
    ts2::tga::Pixels glowBoxPicture_;  // the box's picture, which the reach is drawn over
    std::array<int, 2> glowBoxAt_{};  // where that picture is on screen
    std::array<int, 2> glowPoint_{};  // the lot's point the reach was drawn for
    OptionsMenu optionsMenu_{screen_};
    GameOptionsPanel gameOptions_{screen_, optionsMenu_};
    GraphicsOptionsPanel graphicsOptions_{screen_, optionsMenu_};
    AudioOptionsPanel audioOptions_{screen_, optionsMenu_};
    std::vector<std::pair<int, bool>> hidden_;  // the panels hideAll hid, and whether each was shown
    int tag_ = -1, tagIcon_ = -1;
    std::vector<std::array<int, 3>> tagLines_;  // each line's text in the name font, in the price font, and its sun
    std::uint32_t tagLot_ = 0;
    int puck_ = -1;
    Strip familyStrip_, lotStrip_;
    std::vector<ts2::tga::Pixels> familyPictures_, lotPictures_;
    Bin bin_ = Bin::None;
    std::vector<std::uint32_t> occupied_;
    std::map<std::uint32_t, LotCard> cards_;
    std::uint32_t cardLot_ = 0;
    DescriptionBox bio_;
    bool bioShown_ = false;
    int apartmentRow_ = 0;  // the apartment card's chosen row, 0 for none
};

} // namespace app
