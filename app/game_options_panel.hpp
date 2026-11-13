#pragma once

#include "content/user_props.hpp"
#include "options_menu.hpp"
#include "ui/screen.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace app {

// The options menu's Game Options panel (script 0x49060F02), in a lot's menu or the neighborhood's.
// Its settings groups are read from their captions, show the user folder's saved choices, and save
// a new choice to Config/userProps.xml at once.
class GameOptionsPanel {
public:
    GameOptionsPanel(ts2::ui::Screen& screen, OptionsMenu& menu) : screen_(screen), menu_(menu) {}

    // Reads the settings: the user folder's, with the install's globalProps.xml for the ones it
    // hasn't saved.
    void readSettings(const std::filesystem::path& install, const std::filesystem::path& userFolder);
    // The other panel's settings, read once and kept the same after a choice made there.
    void takeSettings(const GameOptionsPanel& other);
    // Loads the panel hidden, inside the options menu. A neighborhood has no Free Will of its own, so
    // there the panel shows its greyed frame.
    enum class Place { Lot, Neighborhood };
    void load(Place place);
    void remove();
    // Opens or closes the panel in its menu.
    void show(bool show);
    bool shown() const { return menu_.isOpen(panel_); }

    // Handles a click on the menu's Game Options button or one of the panel's windows. True when the
    // window is one of those.
    bool clicked(int window);
    // True once after a choice has changed.
    bool takeChanged();
    // The lot's Free Will, shown from now on.
    void setFreeWill(bool on) {
        freeWill_ = on;
        showFreeWill();
    }
    // The Free Will choice the player just made, once.
    std::optional<bool> takeFreeWill();
    // What lot view shows around a lot, from the Lot View Options: how many tiles of ground around
    // it (nothing for the install's default), whether the neighbors' lots and the neighborhood's
    // decorations (its plants among them) show, whether the nearest neighbors are real houses instead of
    // imposters or the imposters are drawn sharper (Lot Imposters, not in the original game and saved
    // in neots2Props.xml), and the Fade Distance squared, 0 for no fading and with real houses.
    struct LotView {
        std::optional<int> skirtMargin;
        bool neighbors = true, decorations = true, realHouses = false, highImposters = false;
        float fadeSquared = 50.0f * 50.0f;
    };
    LotView lotView() const;
    // The sound quality's AudioPerformance (0 low, 1 medium, 2 high): the saved setting through
    // Graphics Rules, else globalProps.xml's value, else medium.
    int audioPerformance() const;
    // A saved true or false setting, else globalProps.xml's, else `fallback`.
    bool flag(const std::string& key, bool fallback) const;

private:
    struct Group {
        int window = -1;
        ts2::content::OptionControl control;
    };
    void showChoices();
    void showFreeWill();
    void addImposterGroup();
    enum class LotImposters : std::uint8_t { Original, High, RealHouses };
    void chooseImposters(LotImposters kind);

    ts2::ui::Screen& screen_;
    OptionsMenu& menu_;
    int panel_ = -1;
    std::vector<Group> groups_;
    std::filesystem::path userFolder_;
    std::map<std::string, std::string> saved_;     // by saved key, as userProps.xml keeps them
    std::map<std::string, std::string> defaults_;  // globalProps.xml's settings
    std::string graphicsRules_;                     // the install's Graphics Rules.sgr
    bool changed_ = false;
    bool freeWill_ = true;
    LotImposters imposters_ = LotImposters::Original;
    int imposterGroup_ = -1;
    std::optional<bool> pickedFreeWill_;
};

} // namespace app
