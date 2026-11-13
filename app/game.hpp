// The game's window. Game is defined in menu.cpp and the game_*.cpp files beside it.
#pragma once

#include "menu.hpp"
#include "baby_name_dialog.hpp"
#include "audio/system.hpp"
#include "phone_book_dialog.hpp"
#include "type_picker_dialog.hpp"
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
#include "lot_life.hpp"
#include "lot_light.hpp"
#include "lot_ui.hpp"
#include "pie_menu_ui.hpp"
#include "lot_view.hpp"
#include "neighborhood_ui.hpp"
#include "orbit.hpp"
#include "quit_box.hpp"
#include "portraits.hpp"

#include "content/cameras.hpp"
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
    };

    // A lot built in the background, then shown.
    struct LotBuilt {
        ts2::scene::Scene lot;  // in its neighborhood when it has one
        app::LotView view;
        app::Orbit camera;
        ts2::render::IRenderer::Prepared gpu;
        std::shared_ptr<const app::RouteModelTable> routeModels;
        // For lighting it again as its clock moves on.
        std::shared_ptr<app::LotLightBase> light;
        std::string lightState;
        ts2::scene::Transform place;
        std::optional<ts2::camera::Config> freeRig;  // the free camera's settings
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
    // Shows the game's message box (the newspaper's job offer among them) when a script asks, and
    // closes it when answered with the button pressed, counted from 0.
    void updateMessage();
    void answerMessage(std::uint8_t button);
    void closeMessage();
    std::optional<ts2::tga::Pixels> framedPicture(const std::optional<ts2::tga::Pixels>& picture);
    // Whether one of the game's dialogs is open over the lot: the quit box, an object error, the
    // naming dialog, the phone book, the type picker or a message box.
    bool dialogOpen() const {
        return quitBox_.isOpen() || errorBox_.isOpen() || babyNames_.isOpen() || phoneBook_.isOpen() || typePicker_.isOpen() ||
               messageBox_.isOpen();
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
    void showLotTag();
    void placePlumbobs(std::uint64_t now);

    // The lot.
    void enterLot(std::uint32_t id);
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
    // The puck's zoom buttons play once when pressed, its turn buttons loop while held.
    void playPuckSounds(bool zoomIn, bool zoomOut, bool turning);
    // Once a frame: the listener, pause and speed from the lot, then the sound system's update.
    void updateAudio();
    void drawHousehold(std::uint64_t now);
    void sendLotChanges();
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
    std::array<int, 2> toUi(float x, float y) const { return {static_cast<int>(x / uiScale_), static_cast<int>(y / uiScale_)}; }
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
    app::BabyNameDialog babyNames_;  // the game's "Set Baby Name" dialog
    app::PhoneBookDialog phoneBook_;  // the game's Personal Phonebook
    app::TypePickerDialog typePicker_;  // the game's object type picker ("Call Sim Options")
    ts2::sim::CheatProperties cheats_;  // what the cheat window's boolProp set
    app::CheatConsole cheatConsole_{cheats_};
    bool mouseShift_ = false;  // Shift held at the last mouse move or press
    app::DialogBox errorBox_;  // the game's "Object Error" dialog
    app::DialogBox messageBox_;  // the game's message box that scripts ask with (Dialog types 0, 1, 2 and 4)
    std::optional<bool> pausedBeforeErrorBox_;  // the game stops while it is open
    std::optional<bool> pausedBeforeBabyNames_;  // the game pauses while it is open
    std::optional<bool> pausedBeforePhoneBook_;  // and while the Personal Phonebook is open
    std::optional<bool> pausedBeforeTypePicker_;  // and the object type picker
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
    std::uint32_t hovered_ = 0;
    bool hoveredSim_ = false;  // the game never highlights a Sim
    std::array<float, 3> hoveredPoint_{};  // where the mouse's ray hit it, in the scene
    std::optional<std::array<float, 2>> hoverAt_;  // where the mouse moved to since the last frame
    // The tip over a Sim in the world: the object the mouse last came to (a Sim or not), where the
    // ray hit it in the scene and when its tip showed (ns).
    std::uint32_t spotTipObject_ = 0;
    std::array<float, 3> spotTipPoint_{};
    std::optional<std::uint64_t> spotTipShown_;
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
    app::NeighborhoodUi hoodUi_;
    app::Cursors cursors_{install_, data_};
    std::string openName_;
    std::filesystem::path openFolder_;
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
