#pragma once

#include "buy_panel.hpp"
#include "game_options_panel.hpp"
#include "graphics_options_panel.hpp"
#include "live_panel.hpp"
#include "lot_life.hpp"
#include "notification_stack.hpp"
#include "pause_frame.hpp"

#include "queue_panel.hpp"
#include "skewer.hpp"
#include "content/object_thumbnails.hpp"
#include "content/target_pictures.hpp"
#include "sim/clock.hpp"
#include "ui/screen.hpp"

#include <array>
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

// The live panel's portrait windows as its script describes them (see portraitWindows).
struct PortraitWindows {
    ts2::tga::Pixels shape, shadowShape;
    int shadowX = 5, shadowY = 5;
};

// The game's live-mode panels over a lot, placed as the game places them: the puck bottom left with the
// household's thumbnails above it, the live panel beside it (selected Sim, clock, speed buttons) and the
// options menu the puck's Options button opens in its place. The puck's wall button opens a menu of the
// four wall modes, which closes on a pick or when the mouse leaves the area around it.
class LotUi {
public:
    LotUi(ts2::ui::Screen& screen, int width, int height);

    // Loads the panels, hidden, a part per call. open() loads what is left. Loading them all takes a
    // while, so it is spread over the frames a lot loads in rather than done in the frame it shows.
    void load();
    // Where Game Options reads its settings and saves its choices.
    void setFolders(ts2::content::UiData& data, const std::filesystem::path& install, const std::filesystem::path& userFolder) {
        gameOptions_.readSettings(install, userFolder);
        graphicsOptions_.readSettings(data, install, userFolder);
    }
    GraphicsOptionsPanel& graphicsOptions() { return graphicsOptions_; }
    // The screen has a new size: what sits at its bottom and right edges moves with them.
    void setSize(int width, int height);
    // The name of the neighborhood the lot is in, for the Advanced Neighborhood Options panel the
    // options menu shows in the top left corner.
    void setNeighborhood(const std::string& name);
    const GameOptionsPanel& gameOptions() const { return gameOptions_; }
    GameOptionsPanel& gameOptions() { return gameOptions_; }
    std::optional<bool> takeFreeWill() { return gameOptions_.takeFreeWill(); }
    void setFreeWill(bool on) { gameOptions_.setFreeWill(on); }
    // A balloon's picture of an object, using its image `index`. None when the object has no picture.
    std::optional<ts2::tga::Pixels> balloonObjectPicture(LotLife& life, std::uint32_t guid, std::uint32_t index);
    // A balloon's picture of a memory, its object's own picture. None when the GUID isn't a memory's.
    std::optional<ts2::tga::Pixels> balloonMemoryPicture(LotLife& life, std::uint32_t guid);
    // What a want or fear shows in a balloon: a picture's name and either the picture or the Sim whose portrait it
    // is, and its badge. No name when the want shows nothing.
    struct BalloonWant {
        std::string name;
        std::int16_t neighbor = 0;
        ts2::tga::Pixels picture, badge;
    };
    BalloonWant balloonWant(LotLife& life, const ts2::swaf::Want& want);
    // A balloon's portrait of a Sim, cut to the balloon's shape. None until its character file is read, and none at all
    // for a Sim without a portrait.
    std::optional<ts2::tga::Pixels> balloonPortrait(LotLife& life, std::int16_t neighbor);
    // The user folder's cache of object pictures the game drew (Thumbnails/ObjectThumbnails.package).
    void setObjectThumbnails(std::filesystem::path file) {
        objectThumbnailsFile_ = std::move(file);
        objectThumbnails_.reset();
        objectPictures_.clear();
        wantsPending_ = true;
        careerIcons_.clear();
        rewardObjects_.reset();
    }
    void open();
    void close();
    // Whether testingCheatsEnabled is on. The version text only looks at it while the panels load.
    void setTestingCheats(bool on) { testingCheats_ = on; }
    static constexpr std::uint32_t kBalloonPictureSide = 128;
    // Hides every panel but the pause frame while the free camera is on, and brings them back as
    // they were.
    void hideForFreeCamera(bool hide);

    // What a released button asks for.
    enum class Action { None, Neighborhood, Quit, Pause, Normal, High, Ultra, OptionsOpened, OptionsClosed, GameOptionChanged, GraphicsOptionChanged, RewardsOpened, RewardsClosed, SelectSim, TrackSim, CentreOnNeighbor, CloseNotification, FloorUp, FloorDown, WallMode, ToggleCeiling, MainMenu, BuyOpened, BuyClosed, BuyItem, BuildOpened, BuildClosed, StoryOpened, StoryClosed };
    Action clicked(int window);
    // The puck's camera button held down under the mouse.
    enum class CameraButton { None, RotateLeft, RotateRight, ZoomIn, ZoomOut };
    CameraButton heldCameraButton() const;
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
    // Greys the puck's storey buttons when the view can't go further that way.
    void showStoreyLimits(bool canGoUp, bool canGoDown);
    // Shows the puck's Toggle Ceiling button lit while the ceilings are on.
    void showCeilings(bool on);

    // Shows or hides the options menu (it replaces the live panel while open).
    void showOptions(bool show);
    bool optionsShown() const;
    // Shows or hides the Buy mode panel, which replaces the live panel (and the options menu) while
    // open, as the puck's Buy button and F2 open it and its Live button and F1 close it.
    void showBuy(bool show);
    bool buyShown() const;
    // The object type a click on the Buy catalog picked (for BuyItem).
    std::uint32_t pickedBuyItem() const { return pickedBuyItem_; }
    // Whether the mode last opened (Buy, Build or Story) opened in place of the options menu (for
    // BuyOpened, BuildOpened and StoryOpened).
    bool modeOpenedFromOptions() const { return modeFromOptions_; }
    void setModeOpenedFromOptions(bool from) { modeFromOptions_ = from; }
    // Shows or hides the Build mode panel, as the puck's Build button and F3 open it and its Live
    // button and F1 close it. Its tools aren't built yet, so they are greyed.
    void showBuild(bool show);
    bool buildShown() const;
    // Shows or hides Story mode's story editor, as the puck's Story button and F4 open it and its
    // close button, the puck's Live button and F1 close it. Albums aren't read yet: it shows none.
    void showStory(bool show);
    bool storyShown() const;
    // Shows the Buy catalog's page (its pictures and names) for the lot.
    void refreshBuy(LotLife& life);

    // Shows the simulation's clock, speed and funds.
    void show(const std::string& clock, ts2::sim::Speed speed, bool paused, std::int32_t funds);
    // Puts up the household's thumbnails.
    void showHousehold(std::vector<Skewer::Member> members);
    // A Sim's name as its tips show it: first and last name with a space between, either alone when
    // the other is empty, and a witch's title round it (Live text 0x336). Only English order is
    // done. The game puts the last name first in some languages.
    std::string fullName(LotLife& life, std::int16_t sim) const;
    // Shows the lot's life: the clock, speed and funds, the selected Sim (its aspiration and needs)
    // and every household Sim's mood.
    void showLife(LotLife& life);

    // Shows the simulation's text notifications at the top right, and removes those whose time ran out.
    void showNotifications(LotLife& life, double realSeconds);
    // The neighbor whose notification picture was clicked (for CentreOnNeighbor).
    std::int16_t clickedNeighbor() const { return clickedNeighbor_; }
    // Removes the notification whose X was clicked.
    void closeClickedNotification(LotLife& life);

    // A neighbor's portrait as the Personal Phonebook shows it, null while it loads.
    const ts2::tga::Pixels* phoneBookPicture(LotLife& life, std::int16_t neighbor) { return targetPicture(phoneBookPortraits_, life, neighbor); }
    // A message box's picture, 40 x 40: the middle of a neighbor's portrait, else an object type's
    // picture. None while it loads or when there is none.
    std::optional<ts2::tga::Pixels> messagePicture(LotLife& life, std::int16_t neighbor, std::uint32_t objectType);

private:
    // Sets the Sims' names as the tips of their thumbnails and the live panel's portrait.
    void showNames(LotLife& life, std::int16_t sim);
    // Moves a panel so that its anchor (a hidden window captioned "<window id>:bl") sits on that
    // window of the puck.
    void anchor(int panel);
    // Shows or hides a mode's panel (Buy, Build or Story mode) in place of the live panel, its puck
    // button lit while it shows. Showing one hides the options menu and the other modes' panels.
    void showMode(int panel, std::uint32_t button, bool show);
    // Hides the modes' panels but `keep` (-1: all), their puck buttons unlit.
    void hideModesBut(int keep);
    // The mode panel showing, -1 when none.
    int shownMode() const;
    // Sets up the Build panel and the story editor with only what works yet usable.
    void setUpBuildPanel();
    void setUpStoryPanel();
    // Keeps the story editor in the middle of the screen.
    void centreStory();
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
    // An object's picture at its own size, the image `named` when not 0.
    std::optional<ts2::tga::Pixels> objectImage(LotLife& life, std::uint32_t guid, std::uint32_t named);
    ts2::content::TargetPictures balloonPortraits_{ts2::content::readCharacterPortrait, kBalloonPictureSide};
    std::optional<ts2::tga::Pixels> balloonShape_;  // read when first needed
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
    bool testingCheats_ = false;
    int puck_ = -1, live_ = -1, options_ = -1, buy_ = -1, build_ = -1, story_ = -1;
    BuyPanel buyPanel_;
    std::uint32_t pickedBuyItem_ = 0;
    bool modeFromOptions_ = false;
    int modeWasShown_ = -1;  // the mode's panel shown before the free camera hid the panels, -1: none
    int version_ = -1;  // the version text, -1 when it isn't shown
    int loaded_ = 0;  // parts of the panels loaded (see load), 2 when all are
    int funds_ = -1, wallMenu_ = -1, clock_ = -1;
    // The pause, normal, high and ultra speed buttons, set each frame.
    std::array<int, 4> speedButtons_{-1, -1, -1, -1};
    std::array<int, 4> cameraButtons_{-1, -1, -1, -1};  // in CameraButton's order, from RotateLeft
    int wallMode_ = 2, pickedWallMode_ = 2;
    // The Advanced Neighborhood Options panel, its strip of neighborhood buttons and the window
    // holding the button that opens and closes the strip.
    int hoodPanel_ = -1, hoodStrip_ = -1, hoodButtonHolder_ = -1;
    std::string hoodReturnTip_;  // the Return button's tip as loaded, with %s for the neighborhood
    void showHoodPanel(bool show);
    void showHoodStrip(bool show);
    LivePanel panel_;
    OptionsMenu optionsMenu_{screen_};
    GameOptionsPanel gameOptions_{screen_, optionsMenu_};
    GraphicsOptionsPanel graphicsOptions_{screen_, optionsMenu_};
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
    bool hasMembers_ = false;  // the household is known and has anyone in it
    std::int16_t clickedSim_ = 0;
    std::int16_t clickedNeighbor_ = 0;
    std::optional<std::int16_t> closedNotification_;
    NotificationStack notifications_;
    ts2::content::TargetPictures notificationPortraits_{ts2::content::readCharacterPortrait, 0};  // whole portraits
    ts2::content::TargetPictures notificationSmallPortraits_{ts2::content::readCharacterPortrait, 40};
    // Each household Sim's portrait, and whose the live panel shows (-1: none yet).
    std::map<std::int16_t, ts2::tga::Pixels> portraits_;
    int portraitShown_ = -1;
    PortraitWindows portraitWindows_;
    std::int16_t tracked_ = 0;
    std::optional<bool> starShown_;  // whether the live panel's star shows, none until it is set
    // What the names shown were made from (see showNames), and the same for this frame.
    std::vector<std::int32_t> namesShown_, namesScratch_;
    // The wants and fears the wants panel was made from, and what it shows (see showLife).
    std::vector<ts2::swaf::Want> wantsSaved_, fearsSaved_;
    std::vector<WantsPanel::Want> wantsShown_, fearsShown_;
    std::int16_t wantsSim_ = 0;
    std::uint32_t wantsNames_ = 0;
    bool wantsPending_ = true;
    ts2::content::TargetPictures targets_;
    // Relationship thumbnails' pictures, at their portrait window's size.
    ts2::content::TargetPictures relationshipPictures_{ts2::content::readCharacterPortrait, 56};
    // Memory objects' pictures by group (nullopt: none).
    std::map<std::uint32_t, std::optional<ts2::tga::Pixels>> memoryPictures_;
    // Portraits of the Sims memories are about, at the memory popup's portrait size.
    ts2::content::TargetPictures memoryPortraits_{ts2::content::readCharacterPortrait, 64};
    ts2::content::TargetPictures wantPopupPortraits_{ts2::content::readCharacterPortrait, 46};  // the want popup's picture size
    ts2::content::TargetPictures queuePortraits_{ts2::content::readCharacterPortrait, 40};  // a queue icon's picture size
    ts2::content::TargetPictures phoneBookPortraits_{ts2::content::readCharacterPortrait, 46};  // a phone book cell's picture size
    // By GUID, width << 16 | height and the picture named.
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, std::optional<ts2::tga::Pixels>> objectPictures_;
    std::map<std::uint32_t, std::optional<ts2::tga::Pixels>> careerIcons_;  // by the reward's GUID
    // The aspiration and career rewards (by GUID) in the order the panel lists them, made once.
    std::optional<std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>>> rewardObjects_;
    std::filesystem::path objectThumbnailsFile_;
    std::optional<ts2::content::ObjectThumbnails> objectThumbnails_;  // opened when first needed
};

} // namespace app
