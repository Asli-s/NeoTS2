#pragma once

#include "game_options_panel.hpp"
#include "graphics_options_panel.hpp"
#include "audio_options_panel.hpp"
#include "build_panel.hpp"
#include "buy_panel.hpp"
#include "design_dialog.hpp"
#include "live_panel.hpp"
#include "pet_panel.hpp"
#include "lot_life.hpp"
#include "notification_stack.hpp"
#include "pause_frame.hpp"

#include "queue_panel.hpp"
#include "skewer.hpp"
#include "content/covering_catalog.hpp"
#include "content/object_thumbnails.hpp"
#include "content/target_pictures.hpp"
#include "sim/clock.hpp"
#include "ui/screen.hpp"

#include <array>
#include <chrono>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <optional>
#include <map>
#include <set>
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
    // `products` are the install's, kept so that Build mode's catalog needn't look for them again.
    void setFolders(ts2::content::UiData& data, const std::filesystem::path& install, std::vector<ts2::content::Product> products,
                    const std::filesystem::path& userFolder) {
        gameOptions_.readSettings(install, userFolder);
        graphicsOptions_.readSettings(data, install, userFolder);
        audioOptions_.readSettings(install, userFolder);
        install_ = install;
        products_ = std::move(products);
        coveringPictures_.reset();
        coveringsAhead_ = {};
        coveringImages_.clear();
        halfWallImages_.clear();
        coveringThumbnailsFile_ = userFolder / "Thumbnails" / "BuildModeThumbnails.package";
    }
    // The base texture of a floor (`wall` false) or wall covering's material by its catalog GUID, for
    // the catalog pictures that aren't cached.
    void setCoveringTextures(std::function<std::optional<ts2::tga::Pixels>(std::uint32_t guid, bool wall)> textures) {
        coveringTextures_ = std::move(textures);
    }
    GraphicsOptionsPanel& graphicsOptions() { return graphicsOptions_; }
    AudioOptionsPanel& audioOptions() { return audioOptions_; }
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
    // A catalog picture to draw because none is cached: an object type's (`guid`), or else a colour
    // choice's (`model` with `material` on `subset`), filed under object group `group` and `key`.
    struct PictureToDraw {
        std::uint32_t guid = 0;
        std::string model, subset, material;
        std::uint32_t group = 0, key = 0;
        // A description picture in more than one colour: the further colours (material, subset). It
        // isn't cached.
        std::vector<std::pair<std::string, std::string>> also;
        bool preview = false;
    };
    // The next such picture asked for, objects first.
    std::optional<PictureToDraw> pictureToDraw();
    // The picture drawn for `asked`, none when it couldn't be drawn. It goes into the user folder's
    // cache, as the game's drawn pictures do.
    void setDrawnPicture(const PictureToDraw& asked, std::optional<ts2::tga::Pixels> picture);
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
    enum class Action { None, Neighborhood, Quit, Save, Pause, Resume, Normal, High, Ultra, LeftLiveMode, BackToLiveMode, GameOptionChanged, GraphicsOptionChanged, AudioOptionChanged, RewardsOpened, RewardsClosed, SelectSim, TrackSim, CentreOnNeighbor, CloseNotification, SellHeld, HandTool, DesignTool, EyedropperTool, SledgehammerTool, DayNight, TopDownView, CeilingView, Undo, Redo, FloorUp, FloorDown, WallMode, ToggleCeiling, MainMenu };
    Action clicked(int window);
    // The puck's camera button held down under the mouse.
    enum class CameraButton { None, RotateLeft, RotateRight, ZoomIn, ZoomOut };
    CameraButton heldCameraButton() const;
    // A right click on one of the panels' buttons: on a want or fear it puts a hold on it or takes it
    // off, on a Sim's thumbnail it asks to track the Sim. With `ctrl` held on a catalog item, its
    // picture is drawn again.
    Action rightClicked(int window, LotLife& life, bool ctrl);
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

    // The puck's modes. Each but Live shows its own panel in the live panel's place.
    enum class Mode { Live, Buy, Build, Options };
    // F1, F2 or F3: what the puck's Live, Buy or Build button does when clicked.
    Action modeKey(Mode mode);
    Mode mode() const { return mode_; }
    bool liveMode() const { return mode_ == Mode::Live; }
    // Buy and Build mode share the hand, the tools, the views and the undo history.
    bool buyOrBuild() const { return mode_ == Mode::Buy || mode_ == Mode::Build; }
    void setMode(Mode mode);
    // The object Buy or Build mode's hand holds (0 for none). While it holds one, the held object's
    // panel takes the mode panel's place.
    void setHeld(std::uint32_t object) { heldObject_ = object; }
    // Whether the hand holds a catalog item not bought yet. Its catalog cell stays chosen until it stops.
    void setBuying(bool buying) {
        if (buying_ && !buying) {
            buyPanel_.grid().closeDescription();
        }
        buying_ = buying;
    }
    enum class Tool { Hand, Design, Eyedropper, Sledgehammer, Wall, Floor, WallPaint };
    // The tool in use, whose button shows pressed.
    void setTool(Tool tool) { tool_ = tool; }
    // Build mode's wall tool category: whether it is open, and whether the wall tool was just picked
    // there (once a call).
    bool wallCategoryOpen() const { return mode_ == Mode::Build && buildPanel_.wallCategoryOpen(); }
    bool takeWallPicked() { return buildPanel_.takeWallPicked(); }
    // Build mode's Flooring category: whether it is open, the floor clicked in its catalog (once a
    // call), which picks the floor tool, and that floor's pattern name and catalog price.
    bool floorCategoryOpen() const { return mode_ == Mode::Build && buildPanel_.floorCategoryOpen(); }
    bool takeFloorPicked() { return std::exchange(floorPicked_, false); }
    struct FloorChoice {
        std::string pattern;
        int price = 0;
    };
    std::optional<FloorChoice> floorChoice() const;
    // The same for Build mode's Wall Coverings category and the wall paint tool.
    bool wallCoveringCategoryOpen() const { return mode_ == Mode::Build && buildPanel_.wallCoveringCategoryOpen(); }
    bool takeWallCoveringPicked() { return std::exchange(wallCoveringPicked_, false); }
    std::optional<FloorChoice> wallCoveringChoice() const;
    std::optional<int> wallCoveringPrice(const std::string& pattern) const;
    // A floor pattern's catalog price, by the name the lot's pattern map holds it under (the covering's
    // GUID in hex, or its file name without "floor_"). None for a pattern the catalog doesn't have.
    std::optional<int> floorPrice(const std::string& pattern) const;
    // The catalog's floor or wall covering by the name the lot's pattern map holds it under (the
    // covering's GUID in hex, with or without leading zeros, or its file name with or without "floor_"
    // or "wall_"), or null.
    const ts2::content::Covering* covering(const std::string& pattern, ts2::content::CoveringKind kind) const;
    // The eyedropper's pick of a covering: Build mode opens on it in the Flooring or Wall Coverings
    // catalog, and it goes to the floor or wall paint tool.
    void eyedropCovering(const ts2::content::Covering& covering);
    // The eyedropper's pick of an object: Build mode for a Build catalog item, else Buy mode, opens
    // on the item's category and page once its catalog is read.
    void eyedropItem(std::uint32_t guid, bool build);
    // A wall tool entry of the catalog (Wall Tool, Room Tool and so on): its wall style, its clone
    // (the tool's sub-mode) and what each wall segment costs.
    struct WallToolChoice {
        std::uint32_t style = 0;
        int clone = 0;
        std::int32_t price = 0;
    };
    // The wall tool entry chosen in the catalog, the first one until another is clicked. With 1/2
    // Walls shown, the half wall chosen there: its GUID as the style, drawn as walls are. Empty before
    // the catalog is read.
    std::optional<WallToolChoice> wallToolChoice() const;
    // What a wall section of `style` (a wall style or a half wall's GUID) costs in the catalog, 0 for
    // a style it doesn't list.
    std::int32_t wallStylePrice(std::uint32_t style) const;
    // A foundation, deck, pool or stage product's catalog price per tile, 0 when it has none.
    std::int32_t platformPrice(std::int32_t product) const;
    // Whether a wall tool entry was just clicked, once.
    bool takeWallToolClicked() { return std::exchange(wallToolClicked_, false); }
    // Whether the top-down or the 45 degree view is on, for their buttons.
    void setViews(bool topDown, bool ceiling) {
        topDownView_ = topDown;
        ceilingView_ = ceiling;
    }
    // Opens the Design tool's dialog for `object` at UI point (x, y), its cost line showing `cost`.
    // False when the object has no colour choices or the script is missing.
    bool openDesign(LotLife& life, std::uint32_t object, int x, int y, std::int32_t cost);
    DesignDialog& designDialog() { return designDialog_; }
    const DesignDialog& designDialog() const { return designDialog_; }
    // The colours picked in the Design tool's dialog that differ from the object's own, and the
    // object's own colours they replace.
    std::vector<ts2::content::RecolourChoice> designedColours(bool replaced = false) const;
    // The colours picked in the Buy catalog item's strips that differ from the item's default ones.
    std::vector<ts2::content::RecolourChoice> boughtColours();
    // Whether a colour of the Buy catalog item was just picked, once.
    bool takeColourPicked() { return boughtFrom_ != nullptr && boughtFrom_->takeColourPicked(); }
    // The Buy or Build catalog item just clicked, once, for the hand to pick up.
    std::optional<std::uint32_t> takeBought() { return std::exchange(bought_, std::nullopt); }
    // Whether Buy mode's history has a change to undo and one to redo, for the panel's arrows.
    void setHistory(bool undo, bool redo) {
        canUndo_ = undo;
        canRedo_ = redo;
    }
    // Shows or hides the options menu (it replaces the live panel while open).
    void showOptions(bool show) { setMode(show ? Mode::Options : Mode::Live); }
    bool optionsShown() const { return mode_ == Mode::Options; }

    // Shows the simulation's clock, speed and funds.
    void show(const std::string& clock, ts2::sim::Speed speed, bool paused, std::int32_t funds);
    // Puts up the household's thumbnails.
    void showHousehold(std::vector<Skewer::Member> members);
    // Puts a household Sim's newly drawn portrait on its thumbnail and the live panel.
    void setPortrait(std::int16_t sim, const ts2::tga::Pixels& portrait);
    // The neighbors outside the household whose pictures are wanted but whose characters hold none,
    // and whose Sims are on the lot to be drawn, as the game draws a portrait then.
    const std::vector<std::int16_t>& portraitsWanted() const { return portraitsWanted_; }
    // That neighbor's drawn portrait, or nullopt when it couldn't be drawn.
    void setDrawnPortrait(std::int16_t neighbor, std::optional<ts2::tga::Pixels> portrait);
    // A Sim's name as its tips show it: first and last name with a space between, either alone when
    // the other is empty, and a witch's title round it (Live text 0x336). Only English order is
    // done. The game puts the last name first in some languages.
    std::string fullName(LotLife& life, std::int16_t sim) const;
    AspirationPopup::Content aspirationPopupContent(LotLife& life, std::int16_t sim);
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
    // The Sims' live panel and the pets' for the selected Sim and pet.
    void showSimPanel(LotLife& life, std::int16_t sim);
    void showPetPanel(LotLife& life, std::int16_t sim);
    // The live panel shown for the selected Sim, the pets' one for a pet.
    int shownPanel() const { return petShown_ ? petLive_ : live_; }
    // Shows or hides the live panel, the one for the selected Sim's kind.
    void showLivePanel(bool show);
    void showPetsBackground(bool pets);
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
    // An object's picture at its own size, the image `named` when not 0.
    std::optional<ts2::tga::Pixels> objectImage(LotLife& life, std::uint32_t guid, std::uint32_t named);
    // Draws the picture of object type `guid` again, over the one the cache has (a ctrl + right click
    // on its catalog cell). An object with a picture of its own keeps it.
    void redrawPicture(std::uint32_t guid);
    ts2::content::TargetPictures balloonPortraits_{ts2::content::readCharacterPortrait, kBalloonPictureSide};
    std::optional<ts2::tga::Pixels> balloonShape_;  // read when first needed
    // The rewards panel's view of the selected Sim.
    RewardsPanel::Sim rewardsOf(LotLife& life, std::int16_t sim);
    // Gives the Buy and Build panels their catalogs once, and the pictures and description of the
    // catalog shown.
    void showCatalogs(LotLife& life);
    void showHeld(LotLife& life);
    // The window ids of the buttons Buy and Build mode's panels share, each panel having its own.
    struct SharedButtons {
        std::uint32_t hand, design, eyedropper, sledgehammer, dayNight, topDown, ceiling, undo, redo;
    };
    // The shown panel's, or none outside Buy and Build mode.
    const SharedButtons* sharedButtons() const;
    void showBuyTools();
    void showCatalog(LotLife& life, CatalogGrid& grid);
    // The same for a catalog of wall or floor coverings.
    void showCoveringCatalog(CatalogGrid& grid, bool walls);
    void showHalfWallCatalog(CatalogGrid& grid);
    // A half wall's cached catalog picture at this size, null when none is cached.
    const ts2::tga::Pixels* halfWallPicture(std::uint32_t guid, std::uint32_t side);
    // The wall tool's entries, read from the catalog scripts and the build texts the first time.
    void loadWallTools();
    void showWallToolCatalog(CatalogGrid& grid);
    // A covering's catalog picture `side` pixels square, made once. Null when none is cached.
    const ts2::tga::Pixels* coveringPicture(std::uint32_t guid, bool wall, std::uint32_t side);
    // An item's ratings as the Buy catalog's description shows them, a line each.
    std::string catalogRatings(const ts2::objd::ObjectDefinition& definition);
    // The fireplaces of the catalog scripts: each one's mantle by its object name, with the name
    // and description of its catalog texts and its cached picture, listed cheapest first.
    void loadFireplaces(ts2::sim::CodeSource& code);
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
    int puck_ = -1, live_ = -1, options_ = -1;
    int buy_ = -1, build_ = -1;  // the Buy and Build mode panels
    int held_ = -1;              // the held object's panel (UI script 0x49060007)
    std::uint32_t heldObject_ = 0, heldFilled_ = 0;  // the object held, and the one the panel tells of
    bool heldInBuild_ = false;
    bool canUndo_ = false, canRedo_ = false;
    std::optional<std::uint32_t> bought_;  // see takeBought
    CatalogGrid* boughtFrom_ = nullptr;  // the catalog bought_ was clicked in
    bool buying_ = false;                  // see setBuying
    Tool tool_ = Tool::Hand;               // see setTool
    bool topDownView_ = false, ceilingView_ = false;  // see setViews
    DesignDialog designDialog_;
    std::vector<ts2::content::RecolourStrip> designStrips_;  // the strips the dialog opened with
    // A catalog item's colour strips as last shown, with each subset's own model, for the picture of
    // the colours picked in all of them.
    struct ShownStrips {
        std::vector<ts2::content::RecolourStrip> strips;
        std::map<std::string, std::string> models;  // by lowercase subset
    };
    std::map<std::uint32_t, ShownStrips> shownStrips_;
    std::optional<PictureToDraw> previewToDraw_;
    CatalogGrid* previewGrid_ = nullptr;
    std::optional<ts2::tga::Pixels> previewPicture_;
    std::uint32_t designObject_ = 0;                          // the object the dialog recolours
    // The swatches of the Design dialog's strips, with the pictures drawn so far.
    std::vector<SwatchStrip::Choices> designSwatches(LotLife& life);
    Mode mode_ = Mode::Live;
    int petLive_ = -1;  // the pets' live panel
    bool petShown_ = false;  // whether the selected Sim is a pet, which has its own live panel
    int version_ = -1;  // the version text, -1 when it isn't shown
    int loaded_ = 0;  // parts of the panels loaded (see load), 2 when all are
    int funds_ = -1, wallMenu_ = -1, clock_ = -1;
    // The pause, normal, high and ultra speed buttons, set each frame.
    std::array<int, 4> speedButtons_{-1, -1, -1, -1};
    // The pets' live panel has its own clock and speed buttons.
    int petClock_ = -1;
    std::array<int, 4> petSpeedButtons_{-1, -1, -1, -1};
    std::array<int, 4> cameraButtons_{-1, -1, -1, -1};  // in CameraButton's order, from RotateLeft
    int wallMode_ = 2, pickedWallMode_ = 2;
    // The Advanced Neighborhood Options panel, its strip of neighborhood buttons and the window
    // holding the button that opens and closes the strip.
    int hoodPanel_ = -1, hoodStrip_ = -1, hoodButtonHolder_ = -1;
    std::string hoodReturnTip_;  // the Return button's tip as loaded, with %s for the neighborhood
    void showHoodPanel(bool show);
    void showHoodStrip(bool show);
    LivePanel panel_;
    PetPanel petPanel_;
    OptionsMenu optionsMenu_{screen_};
    GameOptionsPanel gameOptions_{screen_, optionsMenu_};
    GraphicsOptionsPanel graphicsOptions_{screen_, optionsMenu_};
    AudioOptionsPanel audioOptions_{screen_, optionsMenu_};
    BuyPanel buyPanel_;
    BuildPanel buildPanel_;
    Skewer skewer_;
    PauseFrame pauseFrame_;
    QueuePanel queue_;
    std::optional<std::uint32_t> queuePressed_;  // the queue icon the left button went down on
    // The pause button blinks while paused: whether it shows pressed, and since when.
    bool pauseLit_ = false, wasPaused_ = false;
    std::chrono::steady_clock::time_point pauseBlink_;
    // The inventory and rewards panel pauses the game while it is open. Unpausing closes it.
    bool rewardsPaused_ = false;
    // Hidden for the free camera, and the mode in use then.
    bool hidden_ = false;
    Mode modeBeforeHide_ = Mode::Live;
    bool hasMembers_ = false;  // the household is known and has anyone in it
    std::int16_t clickedSim_ = 0;
    std::int16_t clickedNeighbor_ = 0;
    std::optional<std::int16_t> closedNotification_;
    NotificationStack notifications_;
    ts2::content::TargetPictures notificationPortraits_{ts2::content::readCharacterPortrait, 0};  // whole portraits
    ts2::content::TargetPictures notificationSmallPortraits_{ts2::content::readCharacterPortrait, 40};
    // Each household Sim's portrait, and whose the live panel shows (-1: none yet).
    std::map<std::int16_t, ts2::tga::Pixels> portraits_;
    // Portraits drawn for neighbors outside the household (see portraitsWanted), and those waiting.
    std::map<std::int16_t, std::optional<ts2::tga::Pixels>> drawnPortraits_;
    std::vector<std::int16_t> portraitsWanted_;
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
    std::map<std::uint32_t, PictureToDraw> picturesToDraw_;  // objects asked for that the game would draw, by GUID
    std::set<std::uint32_t> redraw_;  // GUIDs of objects whose cached picture is drawn again
    std::map<std::uint32_t, std::optional<ts2::tga::Pixels>> drawnPictures_;  // by GUID, at their own size
    std::map<std::pair<std::uint32_t, std::uint32_t>, PictureToDraw> coloursToDraw_;  // by group and key
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::optional<ts2::tga::Pixels>> drawnColours_;  // by group and key
    std::map<const CatalogGrid*, std::uint32_t> described_;  // the item each catalog's description shows
    bool coloursDrawn_ = false;  // a colour picture was drawn since the descriptions were last shown
    std::set<std::uint32_t> drawnNow_;  // GUIDs of objects drawn since the catalogs were last shown
    std::map<std::uint32_t, std::optional<ts2::tga::Pixels>> careerIcons_;  // by the reward's GUID
    // The aspiration and career rewards (by GUID) in the order the panel lists them, made once.
    std::optional<std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>>> rewardObjects_;
    std::filesystem::path objectThumbnailsFile_;
    // Build mode's wall and floor coverings, their cached pictures (opened when first needed) and the
    // pictures made from them, by GUID and size.
    std::filesystem::path install_, coveringThumbnailsFile_;
    std::vector<ts2::content::Product> products_;  // see setFolders
    std::vector<ts2::content::Covering> coverings_;
    // The coverings by kind and GUID, and by kind and lowercase file name, for covering().
    std::map<std::pair<bool, std::uint32_t>, std::size_t> coveringByGuid_;
    std::map<std::pair<bool, std::string>, std::size_t> coveringByFile_;
    std::uint32_t eyedropShown_ = 0;  // the item eyedropItem opens the panel on, 0 once shown
    std::vector<ts2::content::HalfWall> halfWalls_;
    bool halfWallsLoaded_ = false;
    bool wallToolsLoaded_ = false;
    bool coveringsLoaded_ = false;
    std::uint32_t halfWallChosen_ = 0;  // the GUID of the half wall chosen, 0 before one is
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::optional<ts2::tga::Pixels>> halfWallImages_;
    std::optional<ts2::content::CoveringColors> coveringPictures_;
    std::function<std::optional<ts2::tga::Pixels>(std::uint32_t, bool)> coveringTextures_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::optional<ts2::tga::Pixels>> coveringImages_;
    // The coverings' cached catalog pictures at button size, read on another thread as Build mode opens.
    std::future<std::map<std::uint32_t, ts2::tga::Pixels>> coveringsAhead_;
    // The wall tool's catalog entries (see loadWallTools), listed under their index + 1, and the one chosen.
    struct WallToolEntry {
        WallToolChoice choice;
        std::string name, description;
        std::optional<ts2::tga::Pixels> picture, bigPicture;
    };
    std::vector<WallToolEntry> wallTools_;
    std::map<std::int32_t, std::int32_t> platformPrices_;  // by product, see platformPrice
    std::size_t wallToolChosen_ = 0;
    bool wallToolClicked_ = false;
    bool floorPicked_ = false;           // see takeFloorPicked
    std::uint32_t floorChosen_ = 0;      // the floor's GUID, see floorChoice
    bool wallCoveringPicked_ = false;    // see takeWallCoveringPicked
    std::uint32_t wallCoveringChosen_ = 0;  // the wall covering's GUID, see wallCoveringChoice
    // The fireplaces' catalog texts by mantle GUID (see loadFireplaces).
    struct FireplaceText {
        std::string name, description;
        std::int32_t price = 0;
        std::optional<ts2::tga::Pixels> picture, bigPicture;  // cached pictures at button and description size
    };
    std::map<std::uint32_t, FireplaceText> fireplaces_;
    bool fireplacesLoaded_ = false;
    std::optional<ts2::content::ObjectThumbnails> objectThumbnails_;  // opened when first needed
    // The game's cache of colour choice pictures (Thumbnails/DesignModeThumbnails.package), opened when
    // first needed, and the pictures made from it.
    std::optional<ts2::content::ObjectThumbnails> colourThumbnails_;
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, std::optional<ts2::tga::Pixels>> colourPictures_;  // by group, key and size
    // The cached picture of a colour choice (see ObjectThumbnails::colourKey) fitted to width x height,
    // null when the game has none cached.
    const ts2::tga::Pixels* colourPicture(std::uint32_t group, const std::string& model, const std::string& subset, const std::string& material,
                                          std::uint32_t width, std::uint32_t height);
    // The colour strips of catalog item `guid` with a picture for each choice. Those of the item
    // being bought are kept for the purchase (see boughtColours).
    std::vector<CatalogGrid::Strip> colourStrips(LotLife& life, std::uint32_t guid, bool forBuying);
    std::vector<ts2::content::RecolourStrip> buyStrips_;
};

} // namespace app
