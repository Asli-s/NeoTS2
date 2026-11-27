// The game's window. Game is defined in menu.cpp and the game_*.cpp files beside it.
#pragma once

#include "menu.hpp"
#include "baby_name_dialog.hpp"
#include "audio/system.hpp"
#include "phone_book_dialog.hpp"
#include "type_picker_dialog.hpp"
#include "change_appearance_dialog.hpp"
#include "description_box.hpp"
#include "cheat_console.hpp"
#include "dialog_box.hpp"
#include "diagnostics.hpp"
#include "camera_height.hpp"
#include "click_gate.hpp"
#include "cursors.hpp"
#include "free_camera.hpp"
#include "neighborhood.hpp"
#include "lot_daylight.hpp"
#include "buy_hand.hpp"
#include "money_text.hpp"
#include "cost_labels.hpp"
#include "wall_tool.hpp"
#include "floor_tool.hpp"
#include "lot_life.hpp"
#include "tv_stations.hpp"
#include "lot_light.hpp"
#include "lot_ui.hpp"
#include "pie_menu_ui.hpp"
#include "lot_view.hpp"
#include "neighborhood_ui.hpp"
#include "orbit.hpp"
#include "quit_box.hpp"
#include "saving_popup.hpp"
#include "portraits.hpp"

#include "content/cameras.hpp"
#include "content/terrain_edit.hpp"
#include "content/wall_manager.hpp"
#include "sim/game_wide_save.hpp"
#include "sim/rooms.hpp"
#include "formats/cobject_record.hpp"
#include "ui/animated_bitmap.hpp"
#include "imposter_baker.hpp"
#include "render/frame_plan.hpp"
#include "scene/imposters.hpp"
#include "scene/object_effects.hpp"
#include "content/lighting_state.hpp"
#include "content/lot.hpp"
#include "content/material_scripts.hpp"
#include "content/neighborhood.hpp"
#include "content/ui_data.hpp"
#include "content/ui_meters.hpp"
#include "content/ui_text.hpp"
#include "content/user_props.hpp"
#include "formats/reia.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/lot_skirt.hpp"
#include "scene/pick.hpp"
#include "scene/prop_fade.hpp"
#include "ui/fonts.hpp"
#include "ui/screen.hpp"

#include "platform/platform.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace app {

// The game in its window: the main menu, a neighborhood opened from it, and a lot entered from
// that neighborhood.
class Game {
public:
    Game(ts2::content::UiData& data, ts2::ui::Fonts& fonts, ts2::render::IRenderer& renderer, const std::filesystem::path& install,
         const MenuOptions& options);
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    // Shows the menu, or what the options ask for, until the window is closed or the picture taken.
    void run();

private:
    // A neighborhood built in the background, then shown: its scene, camera and panel.
    struct Built {
        ts2::scene::Scene scene;
        ts2::scene::NeighborhoodSceneStats stats;
        app::Orbit camera;
        std::optional<ts2::camera::Config> freeRig;  // its free camera's settings
        bool realisticSea = false;                   // the realistic sea it was built with
        bool realisticPonds = false;                 // and ponds
        std::optional<ts2::scene::HoodDrift> drift;
        ts2::scene::NeighborhoodPlumbobs plumbobs;
        app::NeighborhoodUi::Content panel;
        ts2::render::IRenderer::Prepared gpu;
        app::NeighborhoodTimes times;  // its look by day and by night
        std::unique_ptr<ts2::content::EffectsFiles> effects;  // what its decorations play
        std::optional<std::int32_t> id;                        // its neighborhood id
    };

    // What the ground around a lot is built from, kept so that a new View Distance can build it again
    // while the lot is open.
    struct SkirtSource {
        std::shared_ptr<ts2::scene::Assets> assets;
        const ts2::scene::Scene* hood = nullptr;  // the neighborhood's scene, open for as long as the lot is
        ts2::scene::LotFootprint footprint;
        std::string terrainType;
        std::map<std::string, std::string> roadTextures;
        std::shared_ptr<const ts2::scene::Heights> lotHeights;
        int groundLevel = 0;
        std::optional<ts2::content::WorldBounds> worldBounds;
        ts2::scene::LotGroundLook groundLook;
        bool poolStyleSea = false, smoothLotSkirts = false;
        int defaultMargin = 9;  // globalProps.xml's lotSkirtSizeIncrease
    };

    // A lot's surroundings built again at another View Distance: the neighborhood's decorations (the
    // scene's first `decorations` instances) and the skirt, with the imposters and boundary of that area.
    struct SurroundingsBuilt {
        int margin = 0;  // its surroundingsKey
        ts2::scene::Scene scene;
        std::size_t decorations = 0;
    };

    // A neighbor's house built for lot view, with the reader of the game's files its builds use, as the
    // one the game runs on is used on the main thread meanwhile.
    struct NeighborBuilt {
        std::uint32_t id = 0;
        ts2::scene::Scene scene;
        std::vector<std::string> imageKeys;  // see SceneBuilder::imageKeys
        // For each of its images, the lot's image it uses instead (-1 for none), as the shared images
        // stood when it was built, and its images and meshes uploaded ahead with those left out.
        std::vector<int> imageAt;
        ts2::render::IRenderer::Addition gpu;
        std::shared_ptr<ts2::scene::Assets> assets;
        std::size_t worker = 0;  // the house worker that built it
        bool failed = false;  // its imposter stays, and it isn't tried again
        std::string state;  // the lighting state it was lit in
        bool relight = false;  // built again only for its light maps (see relightNeighborHouse)
    };

    // A lot built in the background, then shown.
    struct LotBuilt {
        ts2::scene::Scene lot;  // in its neighborhood when it has one
        app::LotView view;
        app::Orbit camera;
        ts2::render::IRenderer::Prepared gpu;
        std::shared_ptr<const app::RouteModelTable> routeModels;
        std::shared_ptr<ts2::content::Lot> content;  // the lot as loaded, for its walls to be changed
        // For lighting it again as its clock moves on.
        std::shared_ptr<app::LotLightBase> light;
        std::string lightState;
        ts2::scene::Transform place;
        std::optional<ts2::camera::Config> freeRig;  // the free camera's settings
        // The view the lot was saved with, in the game's coordinates, when the camera came from it.
        std::optional<ts2::savedcamera::ViewState> savedView;
        std::size_t decorations = 0;  // the scene's first instances: the neighborhood's plants and decorations
        std::size_t surroundings = 0;  // those and the skirt's instances after them
        std::shared_ptr<const SkirtSource> skirt;  // none for a lot shown without its neighborhood
        int margin = 0;                            // the skirt's View Distance, in tiles
        // The lot's own images by what they were made from (see SceneBuilder::imageKeys), from firstImage on.
        std::vector<std::string> imageKeys;
        std::size_t firstImage = 0;
    };

    // A neighborhood tile's moving picture.
    struct Playing {
        ts2::reia::Video video;
        ts2::tga::Pixels picture;
        std::size_t frame = 0;
        int window = -1;
    };

    // Whether a background job is still running (a finished one can be replaced without waiting).
    template <typename Job>
    static bool loading(const Job& job) {
        return job.valid() && job.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    }

    // The ground around the lot `margin` tiles out, and that area.
    static std::pair<ts2::scene::Scene, ts2::scene::SkirtArea> makeSkirt(const SkirtSource& s, int margin);

    // The main menu.
    void setUpMenu();
    // Fits the menu's background picture to the screen.
    void fitBackdrop();
    // Plays the plumbob above the menu's logo from its first frame, and shows the frame it is on.
    void playPlumbob();
    void showPlumbob();
    void pickMenuSims();
    void setUpTiles();
    void showPage();
    void showPreview();
    void playVideos();
    void menuClicked(int window, std::uint32_t id);

    // The neighborhood.
    void openNeighborhood();
    // Builds the neighborhood in `folder` in the background.
    std::future<Built> startNeighborhoodBuild(const std::filesystem::path& folder);
    void showNeighborhood(Built built);
    // Builds the open neighborhood's scene again for a new Water choice, and swaps it in once built,
    // keeping the camera, the panels and the time of day.
    void refreshNeighborhood();
    void showRefreshedNeighborhood(Built built);
    // Puts a new Water choice to work: around the open lot at once, in the neighborhood once it shows.
    void applySeaWater();
    // The key the surroundings of a View Distance built with or without the realistic sea are kept by.
    static int surroundingsKey(int margin, bool realisticSea) { return margin * 2 + (realisticSea ? 1 : 0); }
    void showChooser();
    bool chooserOpen() const { return (inNeighborhood_ || inLot_) && menu_ >= 0 && screen_.visible(menu_); }
    // A box or menu over the neighborhood that takes the mouse and keys: lots can't be clicked and
    // the camera only drifts behind it.
    bool neighborhoodCovered() const { return chooserOpen() || quitBox_.isOpen(); }
    // Whether the lot has something to save: as in the game, once time has run unpaused since the
    // lot loaded. The game's other reasons are not known yet.
    bool lotChanged() const { return lotRan_; }
    void openQuitBox(bool askSave = false, bool leaving = false);
    // Shows the baby naming dialog when the lot's scripts ask for names, and closes it when answered.
    void updateBabyNames();
    // Shows the game's "Object Error" dialog for the oldest script error the lot holds for it.
    void updateObjectErrors();
    void answerObjectError(std::uint32_t button);
    void answerBabyNames();
    // Shows the Personal Phonebook when a phone's script asks for a Sim, and closes it when answered.
    void updatePhoneBook();
    void answerPhoneBook(std::optional<std::int16_t> picked);
    void closePhoneBook();
    void updateTypePicker();
    void answerTypePicker(std::optional<std::uint32_t> picked);
    void closeTypePicker();
    // Shows the appearance editor when a mirror's or dresser's script asks, and closes it when answered.
    void updateChangeAppearance();
    void answerChangeAppearance(bool ok);
    void closeChangeAppearance();
    // A hair's picture for the appearance editor, drawn as the game draws it on its stock Sim of the
    // age and gender, over a clear background. Nullopt when it can't be drawn.
    std::optional<ts2::tga::Pixels> drawHairPicture(std::uint32_t age, std::uint32_t gender, const ts2::scenegraph::Link& hair);
    // The catalog picture the game draws of object type `guid` when it has none of its own.
    std::optional<ts2::tga::Pixels> drawObjectPicture(std::uint32_t guid);
    // The picture the game draws of a colour choice it hasn't cached: `model` drawn in the colours
    // given as (material, subset).
    std::optional<ts2::tga::Pixels> drawColourPicture(const std::string& model, const std::vector<std::pair<std::string, std::string>>& colours);
    // Draws a model LotLife put in the lot for a picture, then takes it out again.
    std::optional<ts2::tga::Pixels> drawPicture(const std::optional<LotLife::ObjectPicture>& picture);
    // Shows the game's message box (the newspaper's job offer among them) when a script asks, and
    // closes it when answered with the button pressed, counted from 0.
    void updateMessage();
    void answerMessage(std::uint8_t button);
    void closeMessage();
    std::optional<ts2::tga::Pixels> framedPicture(const std::optional<ts2::tga::Pixels>& picture);
    // Whether one of the game's dialogs is open over the lot: the quit box, an object error, the
    // naming dialog, the phone book, the type picker, the appearance editor, a message box or the
    // "Saving Lot" box.
    bool dialogOpen() const {
        return quitBox_.isOpen() || errorBox_.isOpen() || babyNames_.isOpen() || phoneBook_.isOpen() || typePicker_.isOpen() ||
               changeAppearance_.isOpen() || messageBox_.isOpen() || savingPopup_.isOpen() ||
               lotUi_.designDialog().isOpen();
    }
    // Typing goes on in the naming dialog after the cheat window closes over it.
    void keepBabyNameTyping() {
        if (!cheatConsole_.takesKeys() && babyNames_.isOpen()) {
            ts2::platform::startTextInput();
        }
    }
    void closeQuitBox();
    void closeChooser();
    void lotClicked(float x, float y);
    void showLotCard(std::uint32_t lot);
    // Where a lot's centre is on screen, in UI units, at its height less 2 as the game projects it.
    std::optional<std::array<int, 2>> lotCentreOnUi(std::uint32_t lot) const;
    void showLotTag();
    void placePlumbobs(std::uint64_t now);

    // The lot.
    void enterLot(std::uint32_t id);
    // Writes the lot being played back to its package, as a save does. The message says why it
    // could not be written, and is empty when it was.
    std::string saveLot();
    // The effects each object plays now, for a save (see TypeOwners::runningEffects).
    std::function<std::vector<ts2::xobj::Effect>(std::int16_t)> runningEffectsOf();
    // Saves the lot with the game's "Saving Lot" box shown, then tells `after` why the save failed
    // (empty when it didn't). The save waits for a frame drawn with the box, so the box is seen.
    void saveLotShown(std::function<void(const std::string& problem)> after);
    // Runs a save saveLotShown asked for once its box has been drawn.
    void runShownSave();
    // Where a Sim or an object is drawn on the lot, for its saved scene record (see TypeOwners::drawnPlace).
    std::optional<ts2::cobject::Place> drawnSimPlace(std::int16_t sim) const;
    // A Sim's skeleton joints as last posed, each relative to its parent joint (see TypeOwners::drawnJoints).
    std::optional<std::vector<ts2::cobject::PosedJoint>> drawnSimJoints(std::int16_t sim) const;
    void showLot(LotBuilt& built);
    void leaveLot();
    // The game's request to go to a neighborhood from a lot (Neighborhood, Return, a pick in the
    // chooser): it asks to save first when the lot has something to save, and Cancel stays.
    // `hood` is the neighborhood to open (an index into hoods_), none for the one the lot is in.
    void askToLeave(std::optional<std::size_t> hood);
    void leaveFor(std::optional<std::size_t> hood);
    std::optional<std::size_t> leaveTo_;  // where the open leave question goes on Yes or No
    std::size_t openHood_ = 0;            // the neighborhood shown, as an index into hoods_
    // The ids of the neighborhood shown and of the one shown before it, as the game keeps them for
    // global 0x2B. -1 until a neighborhood shows.
    std::int16_t currentHoodId_ = -1;
    std::int16_t lastHoodId_ = -1;
    app::LotUi::Action lotKey(const ts2::platform::Event& e);
    void lotAction(app::LotUi::Action action);
    void trackSim(std::int16_t sim);
    bool centreOnSim(std::int16_t sim);
    void showSimStorey(const ts2::sim::SimObject& object);
    void followSimStorey();
    std::optional<ts2::render::Vec3> simRoot(std::int16_t sim, const char* joint = "root_trans") const;
    bool openGroundMenu(float x, float y);
    void toggleFreeCamera();
    std::function<float(float, float, std::array<float, 4>&, unsigned&)> hoodFreeGround() const;
    std::function<void(app::Vec3&)> hoodFreeBounds() const;
    void freeCameraMouse(const ts2::platform::Event& e);
    void freeCameraLook();
    std::function<void(app::Vec3&)> keepNearLot(float inset) const;
    // What the lot or the neighborhood is seen through: the free camera while it is on.
    ts2::render::Camera viewCamera() const { return freeCam_ ? freeCam_->camera() : orbit_.camera(); }
    void showPieCursor();
    void lotMouse(const ts2::platform::Event& e, float x, float y);
    void cameraDrag(const ts2::platform::Event& e, float x, float y);
    void endCameraDrag();
    void updateLot(std::uint64_t now);
    void puckCamera();
    void toggleNight();
    void startLotLife();
    // The app's one sound system: made with the menu, its packages indexed beside it.
    void ensureAudio();
    // For a picture of the audio options: waits until the sound packages are indexed, as the stations and songs come from them.
    void waitForAudioIndex();
    void onSoundEvent(const ts2::sim::SoundEvent& event);
    // An animation's sound mark on an object. Hidden objects make no sound.
    void onAnimationSound(std::int16_t object, const std::string& name);
    // Where an object's sounds play from: a Sim's root_rot joint, else the object's own place at
    // its storey's floor height. None for an object not on the lot.
    std::optional<ts2::audio::Vec3> soundPlace(std::int16_t object) const;
    // The press sound a UI button names (btnclicksnd). A button that names none is silent.
    void playPressSound(int window);
    // The sound a live-mode key plays: only the pause and speed keys make one.
    void playKeySound(app::LotUi::Action action, bool wasPaused);
    // Sells what Buy mode's hand holds, with the game's sale sound and money text.
    void sellHeld();
    // Undoes (or redoes) Buy mode's last change, with the game's sound.
    void undoHand(bool redo);
    // The sound and money text of a purchase the hand made, or the error sound when it couldn't pay.
    void paidHand(const app::BuyHand::Paid& paid);
    // Leaving Buy mode or saving: the hand puts a lifted object down where it stands (back where it
    // was lifted from when it can't stand there), drops a catalog item not bought, and sold objects
    // are deleted for good. Going between Buy and Build mode keeps the undo history, which the two share,
    // and the sold objects with it. With `asEscape` (opening Options) the hand lets go as Esc does,
    // a lifted object going back where it was lifted from.
    void settleHand(bool keepHistory = false, bool asEscape = false);
    // One of Buy mode's tools (the Design tool, the eyedropper or the sledgehammer) takes the hand's
    // place, dropping a catalog item the hand holds. Not while the hand holds a lifted object, whose
    // panel hides the tool buttons.
    void pickBuyTool(app::LotUi::Tool tool);
    // The cursor of the Buy mode tool in use, the arrow for the hand.
    std::uint32_t buyToolCursor() const;
    // A left button let go over `object` on the lot with the eyedropper or the sledgehammer: the
    // eyedropper hands the hand a copy of it to buy, in its colours, and the sledgehammer sells it.
    void useBuyTool(std::uint32_t object);
    // Sells `object` with undo as the sledgehammer does, refused while a Sim is in its slots.
    void smashObject(std::uint32_t object);
    // The sledgehammer in Build mode on a wall or floor at `point`, as the wall and floor tools take
    // them out, with their refund and undo: one tile's length of wall, or one tile's floor. Pools
    // and stages are not done yet.
    void smashBuilt(const ts2::scene::Picker::Hit& hit, const std::array<float, 3>& point);
    void smashWall(const ts2::scene::WallInfo& wall, const std::array<float, 2>& at);
    // `level` is the floor grid's, `at` the route's x and y.
    void smashFloor(std::int32_t level, const std::array<float, 2>& at);
    // The tile the sledgehammer lights over a wall or floor hit at `point`: the wall's tile or the
    // floored tile, none over anything else.
    std::optional<ts2::render::TileHighlight> smashHighlight(const ts2::scene::Picker::Hit& hit, const std::array<float, 3>& point);
    // The catalog covering the eyedropper takes at `point`: the floor quarter's, or the covering of
    // the wall's side the point is on. Null over anything else.
    const ts2::content::Covering* eyedropCovering(const ts2::scene::Picker::Hit& hit, const std::array<float, 3>& point);
    // The eyedropper's name tip over what it would take, at `point`. Empty text takes it away.
    void showEyedropTip(const std::string& text, const std::array<float, 3>& point);
    // The sledgehammer on a foundation or deck tile under floor tile (x, y) of grid level `level`
    // (game_platforms.cpp): the game's platform tool takes that tile out. False when no foundation or
    // deck holds the tile up, for the plain floor smash.
    bool smashPlatform(std::int32_t level, std::int32_t x, std::int32_t y);
    // Undo puts the platform tile back (`in` false) and redo takes it out again.
    void togglePlatform(std::uint32_t id, bool in);
    // A texture by name as an image of the lot's scene, loaded once, -1 when missing.
    int selectionImage(const std::string& texture);
    std::map<std::string, int> selectionImages_;
    // The lit tile stays until the mouse moves or the tool changes.
    std::optional<ts2::render::TileHighlight> smashLit_;
    // The day/night button or L: the lot is lit as at night while its time of day is day or morning,
    // and as by day while it is evening or night. The clock sets the time of day from the hour again
    // as it runs, and so does leaving Buy or Build mode.
    void toggleDayNight();
    // The lot's cameras: the Live camera, the top-down view (the Build camera of BuildCamera.txt) and
    // the 45 degree view (the Ceiling camera of CeilingCamera.txt). Each keeps its own zoom and pitch.
    // The one turned on takes the point looked at and the facing from the one left, and the Build
    // camera zooms to its first level each time it comes on.
    enum class LotCamera { Live, TopDown, Ceiling };
    struct LotCameraState {
        std::optional<ts2::camera::Config> rig;
        float distance = 0, distanceGoal = 0, pitchBlend = 0, pitchBlendGoal = 0;
        float minDistance = 1, maxDistance = 300, pivotHeight = 0;
        bool used = false;
    };
    void setLotCamera(LotCamera camera);
    // The top-down or 45 degree button (or T or G): turns that view on, or back to the Live camera
    // when it is on.
    void toggleLotCamera(LotCamera camera);
    std::array<LotCameraState, 3> lotCameras_{};
    LotCamera lotCamera_ = LotCamera::Live;
    // The view Buy and Build mode show. Live mode puts the Live camera back and the view comes on
    // again on going back to Buy or Build.
    LotCamera buyCamera_ = LotCamera::Live;
    bool wasLiveMode_ = true;  // live mode was shown last frame
    bool wasBuyOrBuild_ = false;  // Buy or Build mode was shown last frame
    app::LotUi::Mode handMode_ = app::LotUi::Mode::Live;  // the lot mode the hand was last updated in
    // The Design tool's dialog answered: Accept gives the object the colours picked and takes the cost,
    // which undo gives back.
    void answerDesign(bool accepted);
    // A colour picked in the Design tool's dialog shows on the object at once, until the dialog closes.
    void previewDesign();
    // The puck's zoom buttons play once when pressed, its turn buttons loop while held.
    void playPuckSounds(bool zoomIn, bool zoomOut, bool turning);
    // Once a frame: the listener, pause and speed from the lot, then the sound system's update.
    void updateAudio();
    void drawHousehold(std::uint64_t now);
    void redrawPortrait(std::uint64_t now);
    void drawNeighborPortrait(std::uint64_t now);
    std::optional<ts2::tga::Pixels> drawSimPortrait(std::int16_t sim, std::mt19937& random);
    // The household's family picture, drawn as the game draws it at a save. Nullopt, with the reason
    // logged, when it can't be drawn.
    std::optional<ts2::tga::Pixels> drawFamilyPicture();
    // The household's FamilyThumbnails package with its picture put in, for the save. Nullopt when
    // there is no picture or package to write.
    std::optional<std::pair<std::filesystem::path, std::vector<std::uint8_t>>> familyPictureFile();
    std::optional<ts2::tga::Pixels> drawSimView(const LotLife::AppearanceHairs& sim, const ts2::scene::HairSlots& slots, const std::string& tone,
                                                unsigned width, unsigned height, float turn);
    void sendLotChanges();
    // TV stations: the lot's are set up as it starts, and play each frame (game_tv.cpp).
    void startTvStations();
    void updateTvStations();
    // The lot's instances shown this frame: those its view shows, less the neighborhood's
    // decorations when the Decorations option is off.
    std::vector<std::uint8_t> lotShown() const;
    // Shows or hides the neighbors' lots and the neighborhood's decorations around the lot at once,
    // as the game does when a Lot View Option changes.
    void applyLotViewOptions();
    std::filesystem::path imposterCache(const std::filesystem::path& hood, bool poolStylePonds) const;
    // Whether lots' ponds are drawn live over their imposters (see scene::addNeighborhoodPonds).
    static bool liveHoodPonds(bool sea, bool ponds) { return sea && ponds; }
    void startImposterBaker(bool high);
    void freeImposterBaker();
    void stepImposterBaker();
    void startNeighborHouses();
    void applyNeighborHouses();
    void showNeighborHouses();
    // The boxes of other lots (x0, y0, x1, y1 in the world) a neighbor's house leaves its ground out of.
    std::vector<std::array<float, 4>> tilesTakenFrom(const ts2::scene::LotFootprint& house) const;
    void buildNextNeighborHouse();
    void buildNextNeighborHouse(std::size_t worker);
    void addNeighborHouse(NeighborBuilt built);
    void relightNeighborHouse(NeighborBuilt built);
    void fadeNeighborHouses(const std::array<float, 3>& eye);
    // Shows the surroundings built for this View Distance, building them first when there are none.
    void showMargin(int margin);
    // Adds surroundings that finished building to the lot and shows them if they are still wanted.
    void addSurroundings(SurroundingsBuilt built);
    // Fades the neighborhood's props and the neighbors' lot imposters shown around the lot for the
    // camera at `eye` (in the lot's coordinates), as the game does each frame with the Fade Distance.
    void fadeProps(const std::array<float, 3>& eye);
    // Draws every neighbor's imposter as it is again.
    void unfadeImposters();

    // Every screen.
    // A neighborhood, a lot or the lot's Sims are still loading. The game covers this time with its
    // loading screens, which show the hourglass and take no clicks. The engine has no loading screens
    // yet and keeps the last screen up instead, so it shows the hourglass and drops clicks itself.
    bool loadingScene() const { return building_.valid() || lotBuilding_.valid() || loading(lotSimBuilding_); }
    bool blockedWhileLoading(const ts2::platform::Event& e);
    void tickGraphicsOptions(std::uint64_t ms);
    void applyGraphicsOptions();
    void applyEffectOptions();
    void applyDisplayOptions();
    // Lays the game out again when the window's size has changed, or always when `force` is set.
    void takeWindowSize(bool force = false);
    void paceFrame();
    void applyFrameLimit();
    app::GraphicsOptionsPanel& openGraphicsOptions() { return inLot_ ? lotUi_.graphicsOptions() : hoodUi_.graphicsOptions(); }
    app::AudioOptionsPanel& openAudioOptions() { return inLot_ ? lotUi_.audioOptions() : hoodUi_.audioOptions(); }
    void applyAudioOptions();
    std::uint64_t frameDueNs_ = 0;  // when the next frame may start under a frame rate limit
    int frameLimit_ = 0;
    // Under a frame rate limit that divides the screen's refresh rate, how many refreshes each frame
    // shows for. 0 when the limit is kept by the clock instead.
    int refreshesPerFrame_ = 0;
    double refreshRate_ = 0;  // the screen's, when refreshesPerFrame_ was picked
    int clockRate_ = 0;       // frames a second the clock keeps to when not waiting for the screen, 0 for none
    void startFromOptions();
    void handleEvent(const ts2::platform::Event& e);
    void mouseEvent(const ts2::platform::Event& e);
    void windowClicked(int window);
    void showSprites(double seconds, bool scene);
    // A point of the window in the UI's pixels.
    // Clamped, as a point just in front of the camera projects far off the screen.
    std::array<int, 2> toUi(float x, float y) const {
        const auto ui = [&](float v) { return static_cast<int>(std::clamp(v / uiScale_, -1e6f, 1e6f)); };
        return {ui(x), ui(y)};
    }
    void reportFrame(std::uint64_t now);
    template <typename T>
    std::optional<T> finished(std::future<T>& job, const char* what);

    // The picture the options ask for.
    bool takePicture();
    void clickPanels();

    ts2::content::UiData& data_;
    ts2::render::IRenderer& renderer_;
    const std::filesystem::path install_;
    const MenuOptions& options_;
    std::unique_ptr<ts2::audio::System> audio_;
    // The puck buttons held at the last frame, and the turn loop playing (0 none).
    bool puckZoomIn_ = false, puckZoomOut_ = false;
    std::uint32_t puckTurnSound_ = 0;
    int width_, height_;  // the window's, which the scene is drawn and picked at
    // The UI is laid out in pixels of its own, uiScale_ window pixels each, so it fills a screen
    // uiWidth_ x uiHeight_ of them (see MenuOptions::uiScale).
    float uiScale_;
    int uiPercent_ = 100;  // the UI Size asked for, which uiScale_ may lower to fit the window
    // Realistic ponds and sea, both from the Graphics options' Water (or --pool-style-ponds and
    // --pool-style-sea), read by the next lot or neighborhood built.
    bool realisticPonds_ = false, realisticSea_ = false, realisticSky_ = false;
    bool hoodSea_ = false;                 // the realistic sea the neighborhood's scene was built with
    bool hoodPonds_ = false;               // and ponds, which its lots' ponds take with the sea
    // Whether the neighborhood's scene was built with other water than chosen now.
    bool hoodWaterStale() const { return hoodSea_ != realisticSea_ || hoodPonds_ != realisticPonds_; }
    std::future<Built> hoodRefreshing_;    // the neighborhood's scene built again (see refreshNeighborhood)
    std::filesystem::path hoodRefreshFolder_;
    int uiWidth_, uiHeight_;
    ts2::ui::Screen screen_;
    int menu_ = -1;
    int menuX_ = 0, menuY_ = 0;  // where the menu is
    int preview_ = -1;
    std::uint32_t installedPacks_ = 0;     // as nGameState.IsEPInstalled tests them
    std::vector<std::string> subHoodTips_;  // the preview's sub-neighborhood buttons' tips, "%s" for the name
    app::DescriptionBox description_;       // the preview's description

    // The main menu's neighborhood tiles, kPerPage to a page.
    std::vector<ts2::content::NeighborhoodEntry> hoods_;
    std::vector<int> tiles_;
    std::vector<app::DescriptionBox> tileNames_;  // each tile's name box
    // The plumbob above the menu's logo, a strip of frames played from when the menu is made.
    int plumbob_ = -1;
    std::optional<ts2::ui::AnimatedBitmap> plumbobFrames_;
    std::uint64_t plumbobStart_ = 0;  // in ms
    std::vector<std::shared_future<std::vector<std::uint8_t>>> videos_;
    std::vector<Playing> playing_;
    int page_ = 0;
    int pages_ = 1;
    std::uint64_t pageShown_ = 0;

    // The game's 3D assets, read in the background from the start so a neighborhood opens quickly.
    std::shared_future<std::shared_ptr<ts2::scene::Assets>> assets_;
    std::future<Built> building_;
    bool inNeighborhood_ = false;
    // The neighborhood kept while a lot is shown, to go back to it (its GPU scene too). Then the lot
    // being built, then shown.
    Built hood_;
    app::Orbit hoodOrbit_;
    std::optional<ts2::scene::HoodDrift> hoodDrift_;  // the camera's drift while mouse and keys rest
    app::QuitBox quitBox_;  // the game's "Are you sure you want to quit?" box
    app::SavingPopup savingPopup_;  // the game's "Saving Lot" box
    std::function<void(const std::string&)> afterShownSave_;  // see saveLotShown
    int shownSaveFrames_ = 0;  // frames until the shown save runs, 0 for none
    app::BabyNameDialog babyNames_;  // the game's "Set Baby Name" dialog
    app::PhoneBookDialog phoneBook_;  // the game's Personal Phonebook
    app::TypePickerDialog typePicker_;  // the game's object type picker ("Call Sim Options")
    app::ChangeAppearanceDialog changeAppearance_;  // the game's appearance editor (mirrors, dressers)
    ts2::sim::CheatProperties cheats_;  // what the cheat window's boolProp set
    app::CheatConsole cheatConsole_{cheats_};
    bool mouseShift_ = false;  // Shift held at the last mouse move or press
    app::DialogBox errorBox_;  // the game's "Object Error" dialog
    app::DialogBox messageBox_;  // the game's message box that scripts ask with (Dialog types 0, 1, 2 and 4)
    std::optional<bool> pausedBeforeErrorBox_;  // the game stops while it is open
    std::optional<bool> pausedBeforeBabyNames_;  // the game pauses while it is open
    std::optional<bool> pausedBeforePhoneBook_;  // and while the Personal Phonebook is open
    std::optional<bool> pausedBeforeTypePicker_;  // and the object type picker
    std::optional<bool> pausedBeforeChangeAppearance_;  // and the appearance editor
    std::optional<std::int16_t> changeAppearanceSim_;   // the Sim the open appearance editor shows
    std::optional<bool> pausedBeforeMessage_;  // and the message box
    std::uint64_t messageAskedAt_ = 0;  // when the shown message box began waiting for its picture (ns)
    std::uint64_t messageTimeoutNs_ = 0;  // when the open message box presses its last button, 0 for never
    // Whose request the open message box shows, and its text.
    std::int16_t messageAsker_ = 0;
    std::string messageText_;
    // Whether the lot was paused before the quit box paused it. None when the box didn't pause it
    // (no lot running when it opened).
    std::optional<bool> pausedBeforeQuitBox_;
    bool lotRan_ = false;  // time has run unpaused on this lot
    std::future<LotBuilt> lotBuilding_;
    ts2::scene::Scene lot_;
    std::shared_ptr<const app::RouteModelTable> lotRouteModels_;
    bool inLot_ = false;
    app::LotView lotView_;
    // The lot's Sims, run by their own scripts and posed each frame (from when the lot was shown,
    // without a simulation).
    std::uint64_t lotShown_ = 0;
    // Real seconds the lot has run unpaused: the pool water's clock, as the game's.
    double lotRealSeconds_ = 0;
    std::future<std::unique_ptr<ts2::sim::LotSimulation>> lotSimBuilding_;  // built beside the lot's scene
    std::unique_ptr<app::LotLife> lotLife_;
    // The game-wide inventory (the Game Tips flags), read as the game starts and written as it quits.
    std::optional<ts2::sim::GameWideStore> gameWide_;
    std::shared_ptr<const ts2::content::Broadcasts> broadcasts_;  // read on the first lot
    std::unique_ptr<app::TvStations> tvStations_;
    std::int64_t tvClockMs_ = 0;  // the lot's speed clock when the stations last moved
    // What plays visual effects on the lot, kept while the lot is open. The file stays for the next lot.
    std::unique_ptr<ts2::content::EffectsFiles> effectsFiles_;
    std::unique_ptr<ts2::scene::EffectPlayer> lotEffectPlayer_;
    std::unique_ptr<ts2::scene::EffectPlayer> hoodEffectPlayer_;  // the neighborhood's decorations' effects
    ts2::scene::EffectTerrain hoodEffectTerrain_;                  // the ground and water they meet
    std::map<std::string, int> hoodEffectImages_;  // their textures by name: index into the neighborhood's images
    std::unique_ptr<ts2::scene::ObjectEffects> lotEffects_;
    std::map<std::string, int> effectImages_;  // the scene's image of each effect texture, negative for none
    // Pictures waiting for a Sim's portrait, and a want's badge to go on it.
    struct EffectPortrait {
        std::int16_t neighbor = 0;
        ts2::tga::Pixels badge;
    };
    std::map<std::string, EffectPortrait> effectPortraits_;
    double lotEffectsSeconds_ = 0;
    std::vector<ts2::scene::ParticleVertex> effectVertices_;
    std::vector<ts2::render::ParticleBatch> effectBatches_;
    void startLotEffects();
    void updateLotEffects(float realSeconds);
    void startHoodEffects();
    void updateHoodEffectState();
    void updateHoodEffects(float realSeconds);
    void placeEffectModels();
    void stopLotEffects();
    void updateLotEffectConditions();
    void doEffectCommand(const ts2::sim::EffectCommand& c);
    bool makeEffectPicture(const ts2::sim::EffectCommand& c);
    std::string makeEffectScore(int score);
    std::string makeEffectWant(const ts2::swaf::Want& want);
    ts2::scene::ObjectEffects::PlaceOf effectPlaces();
    std::unique_ptr<app::LotDaylight> lotDaylight_;
    app::LotLightOptions lotLight_;  // as last given to the lot's light
    bool householdShown_ = false;
    std::optional<std::int16_t> portraitRedraw_;  // a Sim whose portrait is drawn again once it is redressed
    std::vector<std::int16_t> householdDrawn_;  // whose portraits the household shows
    // A lot left behind: its scene and simulation are freed in the background, its GPU textures a
    // few a frame (SDL destroys what is given back in the next frame, which took 50 ms for all).
    std::future<void> lotFreeing_;
    ts2::render::IRenderer::Prepared lotLeft_;
    ts2::scene::ScenePose lotPose_;
    std::int16_t trackedSim_ = 0;  // the Sim the camera tracks while orbit_ has a subject
    std::vector<std::uint8_t> lotVisible_;  // the lot's instances shown this frame, for RayTrace
    // The ground, neighbors' lots and decorations around the lot for each View Distance built while
    // it is open (in tiles): the scene's instances [first, first + count), the first `decorations` of
    // them the neighborhood's, and the imposters and boundary that go with them. Only the one for
    // `margin_` shows. The game builds them anew on each change. Keeping them keeps the lot's own
    // instances where the Sims' code knows them.
    struct Surroundings {
        std::size_t first = 0, count = 0, decorations = 0;
        std::vector<ts2::scene::PaintedTerrain::ImposterPart> imposters;
        std::optional<ts2::scene::Scene::BoundaryClip> clip;
        bool seaHazesGround = false;  // see PaintedTerrain::seaHazesGround
        // The boxes of the neighborhood's props in the lot's coordinates, for the Fade Distance.
        // Worked out when first needed.
        std::vector<std::optional<ts2::scene::propfade::Box>> propBoxes;
        // The neighbors' lot imposters' boxes, by lot id, likewise.
        std::optional<std::map<std::uint32_t, ts2::scene::propfade::Box>> imposterBoxes;
    };
    std::map<std::uint32_t, float> imposterAlphas_;  // the neighbors' imposters drawn faded, by lot id
    std::map<int, Surroundings> surroundings_;  // by surroundingsKey
    int margin_ = 0;                            // the key of those shown
    std::shared_ptr<const SkirtSource> skirtSource_;
    std::future<SurroundingsBuilt> surroundingsBuilding_;
    // The neighbors around the lot drawn as real houses, with the Lot Imposters setting: the scene's
    // instances [first, first + count), kept after the lot's own so that the Sims' code still knows those.
    struct NeighborHouse {
        std::uint32_t id = 0;
        std::size_t first = 0, count = 0;
        ts2::scene::LotFootprint footprint;
        std::string state;  // the lighting state its light maps are lit in
        // Its light maps and incidence maps: their indexes in the house as built and in the lot's scene.
        std::vector<std::pair<std::size_t, std::size_t>> lightMaps;
        std::array<float, 4> ground{};  // its lot in the neighborhood: x0, x1, y0, y1
        ts2::scene::propfade::Box box;  // in the lot's coordinates, for the Fade Distance
        float alpha = 1;
    };
    std::vector<NeighborHouse> neighborHouses_;
    std::vector<std::array<float, 4>> neighborGrounds_;  // their lots in the neighborhood
    std::vector<ts2::scene::LotFootprint> neighborsToBuild_;  // the farthest first
    // Houses are built two at a time. Each worker has its own copy of the game's packages, opened
    // while the lot loads as opening takes a second, and builds one house at a time.
    struct HouseWorker {
        std::future<NeighborBuilt> building;
        std::uint32_t lot = 0;  // the lot it is building
        std::shared_ptr<ts2::scene::Assets> assets;
        std::future<std::shared_ptr<ts2::scene::Assets>> opening;
    };
    static constexpr std::size_t kHouseWorkers = 2;
    std::array<HouseWorker, kHouseWorkers> houseWorkers_;
    // The lot's and the houses' images by what they were made from, for the next house to use
    // instead of its own copy (see SceneBuilder::imageKeys).
    std::map<std::string, int> sharedImages_;
    app::GameOptionsPanel::LotView lotViewOptions_;  // the Lot View Options as they stand
    // The open neighborhood's High lot imposters being drawn, or the game's own pictures being put
    // back, and which of the two its imposters are or are becoming.
    std::unique_ptr<app::ImposterBaker> imposterBaker_;
    std::vector<std::future<void>> imposterBakersFreeing_;
    bool highImposters_ = false;
    // Whether the neighborhood view's imposters are High: with Lot Imposters High, or with Real
    // Houses, whose real neighbors would stand out among the game's own pictures.
    bool hoodHighImposters() {
        const auto view = hoodUi_.gameOptions().lotView();
        return view.highImposters || view.realHouses;
    }
    // The object under the mouse, highlighted while the selected Sim could use it (0: none).
    ts2::scene::Picker lotPicker_;
    app::BuyHand buyHand_;
    app::MoneyText moneyText_;
    // Build mode's wall tool (see WallTool). Each frame it finds the corner under the mouse and puts
    // its marker effect there. A left press starts a drag at that corner, and while the button is held
    // the walls planned stand on the lot with their price and size beside the mouse. Letting go builds
    // them, when the household can pay.
    // `ray` is the camera's ray through the mouse, while it is over the lot.
    void updateWallTool(std::span<const std::uint8_t> shown, const std::optional<std::array<float, 3>>& ray);
    void wallButton(bool down);
    // The wall tool's own keys, true when `key` was one.
    bool wallKey(ts2::platform::Key key);
    void stopWallTool();
    // Starts the marker effect `name` at the corner, in place of the one shown.
    void showWallMarker(const std::string& name);
    std::optional<std::array<float, 2>> wallMouse_;  // the mouse in the window, while it is over the lot
    std::array<float, 2> wallPoint_{};               // the mouse on the lot, in tiles
    std::optional<app::WallTool::Corner> wallCorner_, wallStart_;
    std::optional<app::WallTool::Plan> wallOctagon_;  // the octagonal room planned at the last press
    int wallOctagonSize_ = app::WallTool::kSmallestOctagon;
    int wallOctagonTurn_ = 0;  // the octagonal room's turn in eighths, 0, 2, 4 or 6
    app::WallTool::Mode wallMode_ = app::WallTool::Mode::Wall;
    app::WallTool::Mode wallPlanMode_ = app::WallTool::Mode::Wall;  // the mode the walls planned were planned in
    std::shared_ptr<ts2::scene::PlayingEffect> wallMarker_;
    std::string wallMarkerName_;
    std::vector<std::shared_ptr<ts2::scene::PlayingEffect>> wallPoofs_;  // kept until they finish
    // The lot's walls as the game's wall manager keeps them, made on the first change, and the walls
    // the tool shows planned: the game puts them in as walls and takes them out again as the plan changes.
    std::shared_ptr<ts2::content::Lot> lotContent_;
    std::unique_ptr<ts2::content::WallManager> wallManager_;
    std::unique_ptr<ts2::scene::SceneBuilder> wallBuilder_;  // see redrawLotWalls
    std::unique_ptr<ts2::scene::SceneBuilder> floorBuilder_;  // see floorsChanged
    // Each fence's height for its posts, measured once a lot from its post model (see fenceHeight).
    std::map<std::uint32_t, float> fenceHeights_;
    std::unique_ptr<ts2::scene::SceneBuilder> fenceModels_;
    // A wall as the tool put it in, to be taken out and put back as it was.
    struct PlacedWall {
        ts2::content::WallSegment segment;
        std::uint32_t style = 0;
        std::string left, right;  // its patterns
    };
    std::vector<PlacedWall> wallPlanned_;
    std::vector<ts2::fencepostlayer::Post> wallPlanPosts_;  // the fence posts the planned walls put up
    // The posts the planned walls and the demolish plan made another fence's, before and after.
    std::vector<std::pair<ts2::fencepostlayer::Post, ts2::fencepostlayer::Post>> wallPlanPostsChanged_, wallDemolishPostsChanged_;
    // The fence posts the demolish plan put up and took down.
    std::vector<ts2::fencepostlayer::Post> wallDemolishPosts_, wallDemolishPostsRemoved_;
    std::vector<ts2::content::WallSegment> wallPlanWanted_;
    // The ground the plan levelled, put back before the next plan, and whether the plan passed.
    ts2::content::TerrainChanges wallPlanGround_;
    bool wallPlanOk_ = true;
    // The walls each release built and the ground they levelled, by the id the undo history keeps.
    struct BuiltWalls {
        std::vector<PlacedWall> walls;
        ts2::content::TerrainChanges ground;
        bool removed = false;  // walls a demolish took out, rather than walls built
        // The objects hung on them that went with them, by the object (a multi-tile object's lead),
        // with what each sold for.
        std::vector<std::pair<std::uint32_t, std::int32_t>> sold;
        std::vector<ts2::fencepostlayer::Post> posts;  // the fence posts the change put up
        std::vector<ts2::fencepostlayer::Post> postsRemoved;  // and those it took down
        // Posts it made another fence's, before and after.
        std::vector<std::pair<ts2::fencepostlayer::Post, ts2::fencepostlayer::Post>> postsChanged;
    };
    std::vector<std::pair<std::uint32_t, std::int32_t>> wallDemolishSold_;
    bool wallDemolishOk_ = true;
    // Why the game won't take out the wall on a section, as placement error codes: a Sim beside it,
    // or an object hung on it that is in use. The objects it would take with it go in `hung`.
    std::vector<int> demolishRefusals(const ts2::content::WallSegment& section, std::vector<std::uint32_t>& hung);
    // Sells an object hung on a wall that comes out, put aside so an undo can bring it back. Gives what
    // it sold for, none when it couldn't be put aside.
    std::optional<std::int32_t> sellWithWall(std::uint32_t object);
    // The walls a demolish drag has taken out, put back before its next plan.
    std::vector<PlacedWall> wallDemolished_;
    std::vector<ts2::content::WallSegment> wallDemolishWanted_;
    std::int32_t wallRefund_ = 0;
    // Takes out the walls standing on a demolish plan's sections, in place of the ones taken out before.
    void showDemolishPlan(const std::vector<app::WallTool::Section>& sections);
    std::vector<BuiltWalls> wallsBuilt_;
    // A platform tile the sledgehammer took out, to be put back and taken out again.
    struct SmashedPlatform {
        struct Region {
            std::int32_t level = 0, x = 0, y = 0, quarter = 0, before = -1;
        };
        std::vector<Region> regions;
        std::vector<ts2::content::FloorQuarter> floorsBefore, floorsAfter;
        std::vector<PlacedWall> wallsRemoved, wallsAdded;
        std::vector<std::pair<std::uint32_t, std::int32_t>> sold;  // objects hung on the walls taken out
        ts2::content::TerrainChanges heights;
    };
    std::vector<SmashedPlatform> platformsSmashed_;
    // The walls the tool plans in `mode` from the drag's start to the mouse.
    app::WallTool::Plan wallPlan(app::WallTool::Mode mode) const;
    // Puts the walls of a plan in place of the ones planned before, and draws the lot's walls again.
    void showPlannedWalls(const app::WallTool::Plan& plan);
    // Makes the lot's wall manager on first use, told the build catalog's fences and the lot's fence posts.
    void startWallManager();
    // How tall a fence or half wall stands for its posts: the top of its post model, or of its rail
    // model when it has no post, as for its sun shadow.
    float fenceHeight(std::uint32_t style, const ts2::scene::Assets::Fence& fence);
    // Takes the planned walls and the posts they put up out again.
    void takeOutPlannedWalls();
    // The objects on a lot tile, for the game's terrain check (wall graph axes, levels as walls count them).
    std::vector<ts2::content::TerrainObject> terrainObjects(std::int32_t x, std::int32_t y, std::int32_t level);
    // Whether a model's node tree has a node named "post_trans", cached by model.
    bool modelHasPostTrans(const std::string& model);
    std::map<std::string, bool> postTransModels_;
    // Moves the lot's ground drawing to its heights again.
    void groundChanged();
    // Built walls or levelled ground stay or go (a release, undo or redo): Sims and objects stand on
    // the heights as they are now, and the simulation takes the lot's walls and ground again.
    void lotShapeChanged();
    // Whether a scene instance's object stands (true), is put aside by a sale that can be undone
    // (false) or is gone for good (none). Instances that aren't objects stand.
    std::optional<bool> instanceStands(std::size_t instance);
    // Letting go of the button: builds the walls planned or, short of money, takes them out.
    void placeWalls();
    // Keeps the walls the demolish plan took out, with their refund and undo. False when it took none
    // out or was refused.
    bool keepDemolished();
    // A release that kept its plan puffs dust at the drag's start and says so.
    void finishRelease();
    // Whether Build mode builds without charging: on a lot with no household and no Sim selected.
    bool buildFree();
    // Undo takes out the walls built with `id` (`in` false) and redo puts them back.
    void toggleBuiltWalls(std::uint32_t id, bool in);
    // Writes the wall manager's changes to the lot and draws its walls again.
    void wallsChanged();
    // Draws the lot's walls again after the wall manager changed them.
    void redrawLotWalls();
    // Draws the lot's floors again after its floor grid changed, and gives the simulation and the light
    // the lot again.
    void floorsChanged();
    // Only draws the floors again (see floorsChanged), for the floor tool's plan as it changes. Only
    // the grid levels in `levels` when it isn't empty.
    void redrawFloors(const std::set<std::int32_t>& levels = {});
    // Build mode's floor tool (game_floors.cpp): the quarters under the mouse, or those a drag from
    // where the button went down covers, shown laid (or taken up with Ctrl) with their price and size
    // until the button comes up, which pays for them.
    void updateFloorTool(std::span<const std::uint8_t> shown, const std::optional<std::array<float, 3>>& ray);
    void floorButton(bool down);
    // Keeps the floor plan shown, the last of floorsLaid_, paying `cost` (negative gives money back),
    // with `sound` (none when null) and the buy or sell sound.
    void keepFloorPlan(std::int32_t cost, const char* sound);
    void stopFloorTool();
    // Shows `plan` laid in place of the last plan shown, which goes back to what it covered.
    void showFloorPlan(const std::vector<ts2::content::FloorQuarter>& plan);
    // Undo takes out the floors laid with `id` (`in` false) and redo lays them again.
    void toggleFloors(std::uint32_t id, bool in);
    std::optional<std::array<float, 2>> floorMouse_;  // the mouse in the window, while it is over the lot
    std::optional<app::FloorSpot> floorHover_, floorStart_;
    std::vector<ts2::content::FloorQuarter> floorPlan_, floorPlanWas_;  // the plan shown and what it covered
    float floorPlanCost_ = 0;
    bool floorTipShown_ = false;  // the floor tool's refusal shows by the mouse
    std::string floorPlanKey_;  // what the plan shown was worked out for, to work it out again only on a change
    std::optional<ts2::sim::FloorSupport> floorSupport_;  // where floors may go, for the stroke being planned
    // Where floors may go, for the storeys' grids drawn while the tool is used. Kept until floors are laid,
    // taken up again or brought back, or the tool stops, as working it out takes a while.
    std::optional<ts2::sim::FloorSupport> gridSupport_;
    // Whether objects keep a floor off tile (x, y) of grid level `level`, as the game's floor check finds
    // them, cached by tile while the tool is used.
    bool floorObjectsBlock(std::int32_t level, std::int32_t x, std::int32_t y);
    std::map<std::array<std::int32_t, 3>, bool> floorBlocked_;
    // The ground storey's floored quarters on tiles whose corners aren't in one plane, as the ground was
    // last drawn under them. Only there does the drawn ground need keeping under the floor.
    std::vector<std::uint64_t> twistedFloors_;
    // The grid levels of the plan shown, drawn again when it changes.
    std::set<std::int32_t> floorPlanLevels_;
    std::vector<std::uint64_t> twistedFloors() const;
    // The quarter tile toggle (Ctrl+F): the floor tool lays one quarter at a time. The game's hand
    // places objects on quarter tiles with it too, which is not done yet.
    bool quarterTiles_ = false;
    // What each floor stroke laid and what was there, by the id the history holds.
    struct LaidFloors {
        std::vector<ts2::content::FloorQuarter> before, after;
        // The corners the floors laid levelled, as they were and as they became.
        std::vector<ts2::content::CornerHeight> heightsBefore, heightsAfter;
        // The ground a stroke on the ground storey levelled.
        ts2::content::TerrainChanges ground;
    };
    std::vector<LaidFloors> floorsLaid_;
    // Where the mouse ray meets storey `storey` (1 the ground) in route x and y, through the plane of the
    // tile reached, starting from tile centre `near` and tried again a few times, as a storey stands
    // higher over a foundation than past it.
    std::optional<std::array<float, 2>> storeyPoint(int storey, const std::array<float, 3>& ray, std::array<float, 2> near) const;
    // Build mode's wall paint tool (game_paint.cpp): the wall side under the mouse, shown with the chosen
    // covering and its price until the button comes up, which pays.
    // Ctrl puts the wall style's own pattern back, Shift takes the sides round the room that have the
    // side's covering, and a drag adds each side the mouse passes.
    void updatePaintTool(const std::optional<std::array<float, 3>>& ray);
    void paintButton(bool down);
    void stopPaintTool();
    // Undo puts back the coverings painted over with `id` (`in` false) and redo paints them again.
    void togglePaint(std::uint32_t id, bool in);
    struct PaintedSide {
        ts2::content::WallSegment side;  // the painted side is the one leftQuarter names
        std::string before, after;
    };
    // Paints one side for the plan, as the game's plan takes it. False when it can't be painted or has
    // the pattern already.
    bool planPaint(const ts2::sim::WallPiece& side, const std::string& pattern);
    // The wall side whose face the mouse ray meets first, turned so the face is on its right. None on a
    // wall's top or off the walls.
    std::optional<ts2::sim::WallPiece> wallFaceAt(const std::array<float, 3>& ray);
    std::optional<std::array<float, 2>> paintMouse_;  // the mouse in the window, while it is over the lot
    std::optional<ts2::sim::WallPiece> paintHover_;
    bool paintHeld_ = false;
    std::vector<PaintedSide> paintPlan_;  // shown painted on the walls
    std::int32_t paintPlanCost_ = 0;
    std::string paintPlanKey_;  // what the plan shown was worked out for
    std::string paintPlanMode_;  // the Ctrl and Shift state and covering it was worked out with
    // The lot's walls and fences as the room outline layers them, made when first needed after the walls change.
    std::optional<ts2::sim::WallOutline> paintOutline_;
    std::vector<std::vector<PaintedSide>> paintsDone_;  // by the id the history holds
    // After the hand moved, put down, bought, sold or moved back objects: each door and window whose cuts
    // aren't where it stands cuts the wall faces where it stands now instead (none while a sale puts it
    // aside or while the hand holds it where it can't go), and the walls are drawn again.
    void recutWalls();
    std::optional<std::map<std::string, std::vector<ts2::scene::WallMask>>> wallMasks_;  // see recutWalls, by model
    bool recutPending_ = false;  // recutWalls once the lot's scene shows the hand's change
    bool carriedLastFrame_ = false;  // the hand held something last frame
    // The wall faces each door and window cut at the last recutWalls, by object id: its instance,
    // model (lower case) and the walls it cut.
    struct Cuts {
        std::size_t instance = 0;
        std::string model;
        std::vector<int> walls;
    };
    std::map<std::uint32_t, Cuts> cutsOf_;
    app::CostLabels wallLabels_;
    std::array<int, 2> lotMouseUi_{};  // where the mouse last was over the lot, in UI pixels
    // Buy mode's drag turn: where the button went down while the hand held an object, the cursor of
    // the quarter dragged toward, and the floor point the held object was last moved to.
    std::optional<std::array<float, 2>> buyDragFrom_;
    std::optional<std::uint32_t> buyDragCursor_;
    std::array<float, 3> buyFloor_{};
    // The press lifted the held object and hasn't turned it yet: letting go then keeps holding it.
    bool buyDragLifted_ = false;
    // Where the hand grabbed what it lifted, from the held object's origin in the scene, and how far
    // on screen that origin is from the grabbed point, so the grabbed point stays under the mouse. A
    // catalog item has none.
    std::optional<std::array<float, 3>> buyGrab_;
    std::array<float, 2> buyGrabScreen_{};
    void dragTurnHand(float x, float y);
    std::uint32_t buyHovered_ = 0;  // the object under Buy mode's hand that it could lift
    app::LotUi::Tool buyTool_ = app::LotUi::Tool::Hand;  // Buy mode's tool in use
    std::uint32_t designPressed_ = 0;  // the object the Design tool's press was on
    std::uint32_t designObject_ = 0;   // the object the Design tool's dialog is for
    // What a change with the Design tool costs.
    static constexpr std::int32_t kDesignCost = 15;
    std::uint32_t hovered_ = 0;
    bool hoveredSim_ = false;  // the game never highlights a Sim
    std::array<float, 3> hoveredPoint_{};  // where the mouse's ray hit it, in the scene
    std::optional<std::array<float, 2>> hoverAt_;  // where the mouse moved to since the last frame
    // The tip over a Sim in the world: the object the mouse last came to (a Sim or not), where the
    // ray hit it in the scene and when its tip showed (ns).
    std::uint32_t spotTipObject_ = 0;
    std::array<float, 3> spotTipPoint_{};
    std::optional<std::uint64_t> spotTipShown_;
    // The balloon over a spot of the lot (Screen::setWorldTip): the scene point it points at and when
    // it showed (ns).
    std::array<float, 3> worldTipPoint_{};
    std::optional<std::uint64_t> worldTipShown_;
    std::string eyedropTip_;  // the name the eyedropper's tip shows, empty while it shows none
    void showWorldTip(const std::string& text, const std::array<float, 3>& point);
    void refusedHand(const app::BuyHand::Paid& paid);
    void hideWorldTip();
    // The live-mode panels over the lot. The options menu pauses the game while it's open.
    app::LotUi lotUi_;
    // The pie menu of the object clicked. The object stays highlighted while it is open.
    app::PieMenuUi pieMenu_;
    std::uint32_t pieObject_ = 0;
    bool pausedBeforeOptions_ = false, pausedBeforeRewards_ = false;
    float downX_ = 0, downY_ = 0;
    std::optional<std::array<float, 2>> dragFrom_;  // where a camera drag (right button) started
    int pressMark_ = -1;              // the square at that point
    // A middle-button turn of the camera: where it started in the window, and the picture there.
    std::optional<std::array<float, 2>> turnFrom_;
    int turnMark_ = -1;
    // The cursors for what the mouse was last over, on the lot and on the neighborhood.
    std::uint32_t liveCursor_ = app::Cursors::kArrow;
    std::uint32_t hoodCursor_ = app::Cursors::kArrow;
    // A lot clicked once on the neighborhood: its card opens unless a second click comes in time.
    std::uint32_t clickedLot_ = 0;
    std::uint64_t clickedAt_ = 0;
    app::Orbit orbit_;
    // The game's Camera Rotation option set to Sims 1 Style, which turns the middle-button drag and
    // comma and period the other way.
    bool sims1Turn_ = false;
    app::CameraHeight cameraHeight_;
    // The free camera while it is on (Tab), and its settings.
    std::optional<app::FreeCamera> freeCam_;
    std::optional<ts2::camera::Config> freeRig_;
    // The neighborhood's free camera settings, and its own camera while the free camera is on.
    std::optional<ts2::camera::Config> hoodFreeRig_;
    std::optional<app::Orbit> hoodOrbitBefore_;
    std::optional<std::array<float, 2>> freeMouse_;  // where the pointer moved to since the last frame
    ts2::scene::Transform lotPlace_;  // where the lot shown stands in its neighborhood
    // The saved view the lot camera started from, and the target and yaw it became in the
    // neighborhood. While the camera is still there a save writes the view's own values, which the
    // move into the neighborhood and back would round.
    struct LoadedView {
        ts2::savedcamera::ViewState saved;
        app::Vec3 target{};
        float yaw = 0;
    };
    std::optional<LoadedView> loadedView_;
    app::NeighborhoodUi hoodUi_;
    app::Cursors cursors_{install_, data_};
    std::string openName_;
    std::filesystem::path openFolder_;
    std::filesystem::path lotFile_;  // the package of the lot being played
    bool lotSaved_ = false;  // the lot has been saved once for --save-lot
    std::size_t chosen_ = 0;
    // A neighborhood tile clicked once, waiting for a second click, and when.
    std::optional<std::size_t> clickedTile_;
    std::uint64_t tileClickedAt_ = 0;

    // Textures added or changed go to the GPU when a sprite draws them, and the others a few a
    // frame: panels load many pictures they keep hidden, and making a GPU texture for each at
    // once took a third of a second.
    std::vector<ts2::scene::Image> staged_;
    std::set<int> unsent_;
    // When the neighborhood and lot being loaded were asked for, for their load times in the log.
    std::uint64_t hoodLoadStarted_ = 0, lotLoadStarted_ = 0;
    std::uint64_t hoodShownAt_ = 0;  // when the neighborhood was shown, the plumbobs' clock
    std::vector<std::pair<std::size_t, ts2::scene::Transform>> plumbobPlaces_;
    std::vector<ts2::scene::EffectModels::Placed> effectModelPlaces_;
    std::uint32_t enteredLot_ = 0;

    bool running_ = true;
    bool down_ = false;
    app::ClickGate loadingGate_;  // drops clicks while a scene loads (see loadingScene)
    std::uint64_t lastFrame_ = 0;
    std::uint64_t lastSlowFrame_ = 0, lastTimings_ = 0;  // 0: no slow frame yet
    // The slow steps of showing a lot (the lot, its simulation, the household's portraits) take a
    // frame each, so no frame waits for two. A picture being taken waits for all.
    bool stepTaken_ = false;
    bool windowShown_ = false;  // the window, made hidden, is shown after the first frame
    // A picture's clicks are made once, then the lot plays on for --run seconds.
    bool uiClicked_ = false;
    bool groundClicked_ = false;
    bool menuChoiceMade_ = false;  // the --choose menu choice is made
    bool lotHovered_ = false;
    std::uint8_t mouseHeld_ = 0;  // one bit for each MouseButton held
    std::optional<std::uint64_t> runStarted_;
};

} // namespace app
