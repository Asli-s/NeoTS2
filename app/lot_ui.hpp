#pragma once

#include "live_panel.hpp"
#include "lot_life.hpp"
#include "pause_frame.hpp"

#include "queue_panel.hpp"
#include "skewer.hpp"
#include "content/object_thumbnails.hpp"
#include "content/target_pictures.hpp"
#include "sim/clock.hpp"
#include "ui/screen.hpp"

#include <chrono>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace app {

// The game's live-mode panels over a lot, placed as the game places them: the puck in the bottom left
// corner with the household's thumbnails above it, the live panel beside it (the selected Sim, the
// clock, the speed buttons) and the options menu the puck's Options button opens in the live panel's
// place. The puck's wall button opens a small menu of the four wall modes. It closes when one is
// picked or the mouse leaves the area around it.
class LotUi {
public:
    LotUi(ts2::ui::Screen& screen, int width, int height);

    // Loads the panels, hidden, a part per call. open() loads what is left. Loading them all takes a
    // while, so it is spread over the frames a lot loads in rather than done in the frame it shows.
    void load();
    // The user folder's cache of object pictures the game drew (Thumbnails/ObjectThumbnails.package).
    void setObjectThumbnails(std::filesystem::path file) {
        objectThumbnailsFile_ = std::move(file);
        objectThumbnails_.reset();
        objectPictures_.clear();
        careerIcons_.clear();
        rewardObjects_.reset();
    }
    void open();
    void close();
    // Hides every panel but the pause frame while the free camera is on, and brings them back as
    // they were.
    void hideForFreeCamera(bool hide);

    // What a released button asks for.
    enum class Action {
        None, Neighborhood, Quit, Pause, Normal, High, Ultra, OptionsOpened, OptionsClosed, RewardsOpened, RewardsClosed, SelectSim, TrackSim,
        FloorUp, FloorDown, TurnLeft, TurnRight, ZoomIn, ZoomOut, WallMode
    };
    Action clicked(int window);
    // A right click on one of the panels' buttons: on a want or fear it puts a hold on it or takes it
    // off, on a Sim's thumbnail it asks to track the Sim.
    Action rightClicked(int window, LotLife& life);
    // Marks the Sim the camera tracks (0: none) with a star on its thumbnail, and on the live
    // panel's portrait while it is the selected Sim.
    void showTracked(std::int16_t sim);
    // The Sim whose thumbnail was clicked (for SelectSim and TrackSim).
    std::int16_t clickedSim() const { return clickedSim_; }
    // The wall mode picked (for WallMode): 0 walls down, 1 cutaway, 2 walls up, 3 roofs.
    int pickedWallMode() const { return pickedWallMode_; }

    // Lights the queue icon under the mouse, and closes the wall menu once the mouse leaves its area.
    void mouseMoved(int x, int y);
    // A left button press or release: releasing it on the queue icon it was pressed on cancels
    // that interaction of the selected Sim.
    void mouseButton(int x, int y, bool down, LotLife& life);
    // Shows the wall mode in use on the puck's wall button.
    void showWallMode(int mode);

    // Shows or hides the options menu (it replaces the live panel while open).
    void showOptions(bool show);
    bool optionsShown() const;

    // Shows the simulation's clock, speed and funds.
    void show(const std::string& clock, ts2::sim::Speed speed, bool paused, std::int32_t funds);
    // Puts up the household's thumbnails.
    void showHousehold(std::vector<Skewer::Member> members);
    // Shows the lot's life: the clock, speed and funds, the selected Sim (its aspiration and needs)
    // and every household Sim's mood.
    void showLife(LotLife& life);

private:
    // Moves a panel so that its anchor (a hidden window captioned "<window id>:bl") sits on that
    // window of the puck.
    void anchor(int panel);
    int left(int window) const;
    void openWallMenu();
    void closeWallMenu();
    std::vector<WantsPanel::Want> panelWants(LotLife& life, const std::vector<ts2::swaf::Want>& saved);
    void wantPopupTexts(LotLife& life, const ts2::swaf::Want& want, const ts2::content::WantDefinition& definition, WantsPanel::Want& out);
    void showQueue(LotLife& life, std::int16_t sim);
    // An object's picture (a want's object, a memory's subject) `width` x `height` (`height` 0:
    // square), made once. Null when none. `named`: the picture a caller names instead of the
    // object's usual one (0: none).
    const ts2::tga::Pixels* objectPicture(LotLife& life, std::uint32_t guid, std::uint32_t width, std::uint32_t height = 0,
                                          std::uint32_t named = 0);
    // The icon of the career a career reward comes from. Null when none.
    const ts2::tga::Pixels* careerIcon(LotLife& life, std::uint32_t guid);
    // The rewards panel's view of the selected Sim.
    RewardsPanel::Sim rewardsOf(LotLife& life, std::int16_t sim);
    // The selected Sim's memories as the memories page shows them.
    std::vector<MemoriesPage::Memory> panelMemories(LotLife& life, std::int16_t sim);
    // The selected Sim's relationships as the relationships panel lists them.
    std::vector<RelationshipsPanel::Person> panelRelationships(LotLife& life, std::int16_t sim);
    // The middle of a neighbor's portrait at the size `pictures` makes, made once. Null when it has none.
    const ts2::tga::Pixels* targetPicture(ts2::content::TargetPictures& pictures, LotLife& life, std::int16_t neighbor);
    int top(int window) const;
    // Puts the build's version in the screen's top right corner, as the game shows its own.
    void setUpVersion();

    ts2::ui::Screen& screen_;
    int width_, height_;
    int puck_ = -1, live_ = -1, options_ = -1;
    int version_ = -1;  // the version text, -1 when it isn't shown
    int loaded_ = 0;  // parts of the panels loaded (see load), 2 when all are
    int funds_ = -1, wallMenu_ = -1;
    int wallMode_ = 2, pickedWallMode_ = 2;
    LivePanel panel_;
    Skewer skewer_;
    PauseFrame pauseFrame_;
    QueuePanel queue_;
    std::optional<std::uint32_t> queuePressed_;  // the queue icon the left button went down on
    // The pause button blinks while paused: whether it shows pressed, and since when.
    bool pauseLit_ = false, wasPaused_ = false;
    std::chrono::steady_clock::time_point pauseBlink_;
    // The inventory and rewards panel pauses the game while it is open. Unpausing closes it.
    bool rewardsPaused_ = false;
    // Hidden for the free camera, and whether the options menu was open then.
    bool hidden_ = false, optionsWereShown_ = false;
    std::int16_t clickedSim_ = 0;
    // Each household Sim's portrait, and whose the live panel shows (-1: none yet).
    std::map<std::int16_t, ts2::tga::Pixels> portraits_;
    int portraitShown_ = -1;
    std::int16_t tracked_ = 0;
    std::optional<bool> starShown_;  // whether the live panel's star shows, none until it is set
    ts2::content::TargetPictures targets_;
    // Relationship thumbnails' pictures, at their portrait window's size.
    ts2::content::TargetPictures relationshipPictures_{ts2::content::readCharacterPortrait, 56};
    // Memory objects' pictures by group (nullopt: none).
    std::map<std::uint32_t, std::optional<ts2::tga::Pixels>> memoryPictures_;
    // Portraits of the Sims memories are about, at the memory popup's portrait size.
    ts2::content::TargetPictures memoryPortraits_{ts2::content::readCharacterPortrait, 64};
    ts2::content::TargetPictures wantPopupPortraits_{ts2::content::readCharacterPortrait, 46};  // the want popup's picture size
    ts2::content::TargetPictures queuePortraits_{ts2::content::readCharacterPortrait, 40};  // a queue icon's picture size
    // By GUID, width << 16 | height and the picture named.
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, std::optional<ts2::tga::Pixels>> objectPictures_;
    std::map<std::uint32_t, std::optional<ts2::tga::Pixels>> careerIcons_;  // by the reward's GUID
    // The aspiration and career rewards (by GUID) in the order the panel lists them, made once.
    std::optional<std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>>> rewardObjects_;
    std::filesystem::path objectThumbnailsFile_;
    std::optional<ts2::content::ObjectThumbnails> objectThumbnails_;  // opened when first needed
};

} // namespace app
