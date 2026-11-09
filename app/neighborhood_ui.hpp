#pragma once

#include "content/ui_data.hpp"
#include "game_options_panel.hpp"
#include "graphics_options_panel.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
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
    // The screen has a new size: what sits at its bottom edge moves with it.
    void setSize(int width, int height);
    enum class Action { None, MainMenu, EnterLot, Quit, GameOptionChanged, GraphicsOptionChanged, DayNight };
    // A button released: what the player asked for.
    Action clicked(int window);
    // The puck's camera button held down. They turn and zoom while held, as the lot puck's do.
    enum class CameraButton { None, RotateLeft, RotateRight, ZoomIn, ZoomOut };
    CameraButton heldCameraButton() const;

    // Whether a click on a lot opens its card: lived-in lots always, community lots only while the
    // lots bin is open.
    bool clickable(std::uint32_t lot) const;
    // Shows a lot's card beside the lot, whose centre is at `lotX` across the screen.
    void showCard(std::uint32_t lot, int lotX);
    void hideCard();
    std::uint32_t cardLot() const { return cardLot_; }

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

    ts2::ui::Screen& screen_;
    ts2::content::UiData& data_;
    int width_, height_;
    int panel_ = -1, families_ = -1, lots_ = -1, card_ = -1, options_ = -1;
    OptionsMenu optionsMenu_{screen_};
    GameOptionsPanel gameOptions_{screen_, optionsMenu_};
    GraphicsOptionsPanel graphicsOptions_{screen_, optionsMenu_};
    std::vector<std::pair<int, bool>> hidden_;  // the panels hideAll hid, and whether each was shown
    int tag_ = -1, tagIcon_ = -1, tagText_ = -1;
    std::uint32_t tagLot_ = 0;
    int puck_ = -1;
    Strip familyStrip_, lotStrip_;
    std::vector<ts2::tga::Pixels> familyPictures_, lotPictures_;
    Bin bin_ = Bin::None;
    std::vector<std::uint32_t> occupied_;
    std::map<std::uint32_t, LotCard> cards_;
    std::uint32_t cardLot_ = 0;
};

} // namespace app
