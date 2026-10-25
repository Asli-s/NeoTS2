// The game's main menu (the neighborhood chooser), built from its UI script, and the neighborhood
// it opens.
#include "menu.hpp"
#include "camera_height.hpp"
#include "crash_handler.hpp"
#include "cursors.hpp"
#include "free_camera.hpp"
#include "message_box.hpp"
#include "open_renderer.hpp"
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
#include "content/install.hpp"
#include "content/lighting_state.hpp"
#include "content/lot.hpp"
#include "diag/log.hpp"
#include "diag/timing.hpp"
#include "content/material_scripts.hpp"
#include "content/neighborhood.hpp"
#include "content/ui_data.hpp"
#include "formats/image.hpp"
#include "formats/reia.hpp"
#include "render/make_renderer.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/lot_scene.hpp"
#include "scene/lot_skirt.hpp"
#include "scene/pick.hpp"
#include "ui/fonts.hpp"
#include "ui/screen.hpp"

#include "platform/platform.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>

namespace pf = ts2::platform;

namespace {

// Closing waits for a lot or neighborhood still loading, which takes a few seconds at most.
constexpr unsigned kMaxClosingSeconds = 30;

constexpr std::uint32_t kScripts = 0xA99D8A11;
constexpr std::uint32_t kMainMenu = 0x49001017;
constexpr std::uint32_t kNeighborhoodTile = 0x49001018;
// Windows of the main menu by id.
constexpr std::uint32_t kBackdrop = 0x0da36c7d, kThumbs = 0x00001006, kQuit = 0x000000a5, kPrevious = 0x00001004, kNext = 0x00001005;
// Windows of a neighborhood tile by id.
constexpr std::uint32_t kTilePicture = 0x000000a1, kTileName = 0x000000a5;
constexpr std::uint32_t kTileButton = 0x000000a2;
// The tile's tooltip, which the game sets itself: "Click for Preview of Neighborhood".
constexpr std::uint32_t kNeighborhoodText = 0x7F3A262A, kTileTip = 0x0086003F;  // a string id, leakcheck: ok
// A second click on a lot within this long enters it instead of opening its card.
constexpr std::uint64_t kDoubleClickNs = 250000000;
// The square marking where a camera drag started (a 15 x 15 picture), from the UI scripts.
constexpr std::uint32_t kUiScripts = 0xA99D8A11, kPressMark = 0xFFFF8000;
// The picture a middle-button camera turn shows where it was pressed, while the pointer is hidden.
// The neighborhood's turn mark has only the side arrows, as its camera shows no tilt.
constexpr std::uint32_t kTurnMarkGroup = 0x499DB772, kTurnMark = 0xCF092010, kTurnMarkSides = 0xCF092011;
// The neighborhood preview and its windows by id.
constexpr std::uint32_t kPreview = 0x49001021;
constexpr std::uint32_t kPreviewPicture = 0x00002001, kPreviewDescription = 0x00002002, kPreviewName = 0x00002003, kPreviewPlay = 0x000000b2,
                        kPreviewClose = 0x000000b3;
constexpr std::uint32_t kConstants = 0x4dc1dce2, kUpperLeftSim = 0x000000e1, kLowerRightSim = 0x000000e3;
// The menu's buttons at the start (Quit) and when opened from a neighborhood (Close, Quit).
constexpr std::uint32_t kStartButtons = 0x8dc06b6a, kNeighborhoodButtons = 0x00003000, kChooserClose = 0x000000a3,
                        kChooserQuit = 0x000000a4;
constexpr std::uint32_t kImages = 0x499DB772;
constexpr int kPerPage = 3;
constexpr int kTileGap = 4;  // between neighborhood tiles

std::optional<ts2::tga::Pixels> readImage(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return bytes.empty() ? std::nullopt : ts2::image::decode(bytes);
}

// The Sims (object ids) the camera sees: their middle inside the view.
std::vector<std::int16_t> simsInView(const ts2::scene::Scene& scene, const ts2::render::Camera& c, int width, int height) {
    using ts2::render::Vec3;
    std::vector<std::int16_t> seen;
    if (width <= 0 || height <= 0) {
        return seen;
    }
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float tx = c.fovX > 0 ? std::tan(c.fovX / 2) : std::tan(c.fovY / 2) * aspect;
    const float ty = tx / aspect;
    const Vec3 f = ts2::render::normalize(ts2::render::sub(c.target, c.eye));
    const Vec3 s = ts2::render::normalize(ts2::render::cross(f, {0, 0, 1}));
    const Vec3 u = ts2::render::cross(s, f);
    constexpr float kMiddleHeight = 0.8f;
    for (const auto& actor : scene.actors) {
        const Vec3 p = ts2::render::sub({actor.place.m[3], actor.place.m[7], actor.place.m[11] + kMiddleHeight}, c.eye);
        const float z = ts2::render::dot(p, f);
        if (actor.object != 0 && z > 0 && std::abs(ts2::render::dot(p, s)) <= z * tx && std::abs(ts2::render::dot(p, u)) <= z * ty) {
            seen.push_back(static_cast<std::int16_t>(actor.object));
        }
    }
    return seen;
}

// The direction of the camera's ray through a point of the screen (it starts at the eye).
ts2::render::Vec3 screenRay(const ts2::render::Camera& c, int width, int height, float mx, float my) {
    using ts2::render::Vec3;
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float tx = c.fovX > 0 ? std::tan(c.fovX / 2) : std::tan(c.fovY / 2) * aspect;
    const float ty = tx / aspect;
    const Vec3 f = ts2::render::normalize(ts2::render::sub(c.target, c.eye));
    const Vec3 s = ts2::render::normalize(ts2::render::cross(f, {0, 0, 1}));
    const Vec3 u = ts2::render::cross(s, f);
    const float nx = 2 * mx / static_cast<float>(width) - 1, ny = 1 - 2 * my / static_cast<float>(height);
    return {f[0] + s[0] * nx * tx + u[0] * ny * ty, f[1] + s[1] * nx * tx + u[1] * ny * ty, f[2] + s[2] * nx * tx + u[2] * ny * ty};
}

// The live-mode cursor for what is under the mouse: an object the selected Sim can use, the ground
// (floor or terrain), pool water (the game's water check gives the usable cursor), or anything
// else. Sims are not picked yet, so the person cursor isn't shown.
std::uint32_t liveCursor(const ts2::scene::Instance* under, bool usable) {
    if (usable) {
        return app::Cursors::kLiveObjectAvailable;
    }
    if (under && under->object == 0 && under->name == "pool water") {
        return app::Cursors::kLiveObjectAvailable;
    }
    if (under && under->object == 0 && (under->name == "terrain" || under->part == ts2::scene::Part::Floor)) {
        return app::Cursors::kLiveNothing;
    }
    return app::Cursors::kLiveObjectUnavailable;
}

// The lot under a point of the screen: of the lots whose ground the camera's ray through the point
// crosses (at each lot's height), the nearest. 0 for none.
std::uint32_t lotAt(const ts2::render::Camera& c, int width, int height, float mx, float my,
                    const std::vector<ts2::scene::LotFootprint>& lots) {
    const ts2::render::Vec3 d = screenRay(c, width, height, mx, my);
    std::uint32_t best = 0;
    float nearest = 1e30f;
    for (const auto& lot : lots) {
        if (std::abs(d[2]) < 1e-6f) {
            continue;
        }
        const float t = (lot.z - c.eye[2]) / d[2];
        const float x = c.eye[0] + d[0] * t, y = c.eye[1] + d[1] * t;
        if (t > 0 && t < nearest && x >= lot.x0 && x < lot.x1 && y >= lot.y0 && y < lot.y1) {
            nearest = t;
            best = lot.id;
        }
    }
    return best;
}

std::vector<ts2::render::Sprite> sprites(const ts2::ui::Screen& screen, double seconds) {
    std::vector<ts2::render::Sprite> out;
    for (const auto& q : screen.quads(seconds)) {
        ts2::render::Sprite s;
        s.x0 = q.x0;
        s.y0 = q.y0;
        s.x1 = q.x1;
        s.y1 = q.y1;
        s.u0 = q.u0;
        s.v0 = q.v0;
        s.u1 = q.u1;
        s.v1 = q.v1;
        s.texture = q.texture;
        for (int k = 0; k < 4; ++k) {
            s.color[k] = q.color[static_cast<std::size_t>(k)];
        }
        out.push_back(s);
    }
    return out;
}

// A neighborhood built in the background, then shown: its scene, camera and panel.
struct Built {
    ts2::scene::Scene scene;
    ts2::scene::NeighborhoodSceneStats stats;
    app::Orbit camera;
    std::optional<ts2::scene::HoodDrift> drift;
    ts2::scene::NeighborhoodPlumbobs plumbobs;
    app::NeighborhoodUi::Content panel;
    ts2::render::IRenderer::Prepared gpu;
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
bool loading(const Job& job) {
    return job.valid() && job.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

double secondsSince(std::uint64_t started) {
    return static_cast<double>(ts2::platform::ticksNs() - started) / 1e9;
}

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
    // The main menu.
    void setUpMenu();
    void pickMenuSims();
    void setUpTiles();
    void showPage();
    void showPreview();
    void playVideos();
    void menuClicked(int window, std::uint32_t id);

    // The neighborhood.
    void openNeighborhood();
    void showNeighborhood(Built built);
    void showChooser();
    bool chooserOpen() const { return inNeighborhood_ && menu_ >= 0 && screen_.visible(menu_); }
    // A box or menu over the neighborhood that takes the mouse and keys: lots can't be clicked and
    // the camera only drifts behind it.
    bool neighborhoodCovered() const { return chooserOpen() || quitBox_.isOpen(); }
    // Whether the lot has something to save: as in the game, once time has run unpaused since the
    // lot loaded. The game's other reasons are not known yet.
    bool lotChanged() const { return lotRan_; }
    void openQuitBox(bool askSave = false);
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
    app::LotUi::Action lotKey(pf::Key key);
    void lotAction(app::LotUi::Action action);
    void trackSim(std::int16_t sim);
    bool centreOnSim(std::int16_t sim);
    std::optional<ts2::render::Vec3> simRoot(std::int16_t sim) const;
    bool openGroundMenu(float x, float y);
    void toggleFreeCamera();
    void freeCameraMouse(const pf::Event& e);
    void freeCameraLook();
    std::function<void(app::Vec3&)> keepNearLot(float inset) const;
    ts2::render::Camera lotCamera() const { return freeCam_ ? freeCam_->camera() : orbit_.camera(); }
    void showPieCursor();
    void lotMouse(const pf::Event& e, float x, float y);
    void cameraDrag(const pf::Event& e, float x, float y);
    void endCameraDrag();
    void updateLot(std::uint64_t now);
    void startLotLife();
    void drawHousehold(std::uint64_t now);
    void sendLotChanges();

    // Every screen.
    void startFromOptions();
    void handleEvent(const pf::Event& e);
    void mouseEvent(const pf::Event& e);
    void windowClicked(int window);
    void showSprites(double seconds, bool scene);
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
    const int width_, height_;
    ts2::ui::Screen screen_;
    int menu_ = -1;
    int menuX_ = 0, menuY_ = 0;  // where the menu is
    int preview_ = -1;

    // The main menu's neighborhood tiles, kPerPage to a page.
    std::vector<ts2::content::NeighborhoodEntry> hoods_;
    std::vector<int> tiles_;
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
    std::future<std::unique_ptr<ts2::sim::LotSimulation>> lotSimBuilding_;  // built beside the lot's scene
    std::unique_ptr<app::LotLife> lotLife_;
    std::unique_ptr<app::LotDaylight> lotDaylight_;
    bool householdShown_ = false;
    // A lot left behind: its scene and simulation are freed in the background, its GPU textures a
    // few a frame (SDL destroys what is given back in the next frame, which took 50 ms for all).
    std::future<void> lotFreeing_;
    ts2::render::IRenderer::Prepared lotLeft_;
    ts2::scene::ScenePose lotPose_;
    std::int16_t trackedSim_ = 0;  // the Sim the camera tracks while orbit_ has a subject
    std::vector<std::uint8_t> lotVisible_;  // the lot's instances shown this frame, for RayTrace
    // The object under the mouse, highlighted while the selected Sim could use it (0: none).
    ts2::scene::Picker lotPicker_;
    std::uint32_t hovered_ = 0;
    std::optional<std::array<float, 2>> hoverAt_;  // where the mouse moved to since the last frame
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
    app::CameraHeight cameraHeight_;
    // The free camera while it is on (Tab), and its settings.
    std::optional<app::FreeCamera> freeCam_;
    std::optional<ts2::camera::Config> freeRig_;
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
    std::uint32_t enteredLot_ = 0;

    bool running_ = true;
    bool down_ = false;
    std::uint64_t lastFrame_ = 0;
    std::uint64_t lastSlowFrame_ = 0, lastTimings_ = 0;  // 0: no slow frame yet
    // The slow steps of showing a lot (the lot, its simulation, the household's portraits) take a
    // frame each, so no frame waits for two. A picture being taken waits for all.
    bool stepTaken_ = false;
    // A picture's clicks are made once, then the lot plays on for --run seconds.
    bool uiClicked_ = false;
    bool groundClicked_ = false;
    std::optional<std::uint64_t> runStarted_;
};

Game::Game(ts2::content::UiData& data, ts2::ui::Fonts& fonts, ts2::render::IRenderer& renderer, const std::filesystem::path& install,
           const MenuOptions& options)
    : data_(data), renderer_(renderer), install_(install), options_(options), width_(options.width), height_(options.height),
      screen_(data, fonts), lotUi_(screen_, width_, height_), pieMenu_(screen_), hoodUi_(screen_, data, width_, height_) {
    setUpMenu();
    pickMenuSims();
    setUpTiles();
    showPage();
    assets_ = std::async(std::launch::async, [install] { return std::make_shared<ts2::scene::Assets>(install); }).share();
    // A neighborhood's preview, over the menu where the menu is: its name, description and
    // picture, and Play.
    preview_ = screen_.load(kScripts, kPreview, menuX_, menuY_);
    screen_.setVisible(preview_, false);
}

void Game::setUpMenu() {
    screen_.setSize(width_, height_);
    // Placed in the middle of the screen (as the game places it at 1920 x 1080).
    menu_ = screen_.load(kScripts, kMainMenu, 0, 0);
    if (menu_ < 0) {
        throw std::runtime_error("the main menu's UI script is missing");
    }
    menuX_ = (width_ - screen_.width(menu_)) / 2;
    menuY_ = (height_ - screen_.height(menu_)) / 2;
    screen_.move(menu_, menuX_, menuY_);
    // At start the blurred neighborhood picture shows behind the menu, in the middle of the
    // screen (as the game shows it).
    const int backdrop = screen_.find(menu_, kBackdrop);
    screen_.setVisible(backdrop, true);
    screen_.move(backdrop, (width_ - screen_.width(backdrop)) / 2 - menuX_, (height_ - screen_.height(backdrop)) / 2 - menuY_);
}

// A Sim in the upper left and one in the lower right, each one of the pictures listed in the
// menu's table of constants (kUpperLeft, kLowerRight). Which one the game picks is not known yet.
void Game::pickMenuSims() {
    std::mt19937 random(static_cast<unsigned>(ts2::platform::ticksNs()));
    for (const auto& line : screen_.childTexts(screen_.find(menu_, kConstants))) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string name = line.substr(0, eq);
        const std::uint32_t target = name == "kUpperLeft" ? kUpperLeftSim : name == "kLowerRight" ? kLowerRightSim : 0;
        std::vector<std::uint32_t> pictures;
        for (std::size_t at = eq + 1; at < line.size();) {
            const auto end = line.find(';', at);
            const std::string id = line.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (!id.empty()) {
                pictures.push_back(static_cast<std::uint32_t>(std::strtoul(id.c_str(), nullptr, 16)));
            }
            at = end == std::string::npos ? line.size() : end + 1;
        }
        if (target == 0 || pictures.empty()) {
            continue;
        }
        if (auto picture = data_.image(kImages, pictures[random() % pictures.size()])) {
            screen_.setImage(screen_.find(menu_, target), std::move(*picture));
        }
    }
}

// A tile per neighborhood in the thumbnail strip, three to a page, 4 apart, the row centered in
// the strip both ways (as the game shows them).
void Game::setUpTiles() {
    hoods_ = ts2::content::listNeighborhoods(options_.user / "Neighborhoods");
    ts2::diag::info(std::format("{} neighborhoods", hoods_.size()));
    const int thumbs = screen_.find(menu_, kThumbs);
    for (int i = 0; i < kPerPage; ++i) {
        tiles_.push_back(screen_.load(kScripts, kNeighborhoodTile, 0, 0, thumbs));
    }
    const int tileWidth = screen_.width(tiles_[0]), tileHeight = screen_.height(tiles_[0]);
    const int left = (screen_.width(thumbs) - (kPerPage * tileWidth + (kPerPage - 1) * kTileGap)) / 2;
    for (int i = 0; i < kPerPage; ++i) {
        screen_.move(tiles_[static_cast<std::size_t>(i)], left + i * (tileWidth + kTileGap), (screen_.height(thumbs) - tileHeight) / 2);
    }
    // Each tile plays its neighborhood's moving picture (<hood>_Neighborhood.reia) over and over,
    // from its first frame when the page is shown (not known yet).
    // The videos are read in the background as the menu opens and kept, so a page shows at once.
    for (const auto& hood : hoods_) {
        const auto path = hood.folder / (hood.folder.filename().u8string() + u8"_Neighborhood.reia");
        videos_.push_back(std::async(std::launch::async, [path] {
            std::ifstream in(path, std::ios::binary);
            return std::vector<std::uint8_t>{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        }).share());
    }
    playing_.resize(kPerPage);
    pages_ = std::max(1, (static_cast<int>(hoods_.size()) + kPerPage - 1) / kPerPage);
}

void Game::showPage() {
    for (int i = 0; i < kPerPage; ++i) {
        const std::size_t n = static_cast<std::size_t>(page_ * kPerPage + i);
        const int tile = tiles_[static_cast<std::size_t>(i)];
        Playing& p = playing_[static_cast<std::size_t>(i)];
        p = {};
        screen_.setVisible(tile, n < hoods_.size());
        if (n >= hoods_.size()) {
            continue;
        }
        screen_.setText(screen_.find(tile, kTileName), hoods_[n].name);
        screen_.setTip(screen_.find(tile, kTileButton), data_.text(kNeighborhoodText, kTileTip));
        p.window = screen_.find(tile, kTilePicture);
        p.video = ts2::reia::parse(videos_[n].get());
        if (!p.video.frames.empty() && ts2::reia::decodeFrame(p.video, 0, p.picture)) {
            screen_.setImage(p.window, p.picture);
        } else if (auto picture = readImage(hoods_[n].thumbnail)) {
            screen_.setImage(p.window, std::move(*picture));
        }
    }
    screen_.setVisible(screen_.find(menu_, kPrevious), page_ > 0);
    screen_.setVisible(screen_.find(menu_, kNext), page_ + 1 < pages_);
    pageShown_ = ts2::platform::ticksNs();
}

void Game::showPreview() {
    const auto& hood = hoods_[chosen_];
    screen_.setText(screen_.find(preview_, kPreviewName), hood.name);
    screen_.setText(screen_.find(preview_, kPreviewDescription), hood.description);
    if (auto picture = readImage(hood.thumbnail)) {
        screen_.setImage(screen_.find(preview_, kPreviewPicture), std::move(*picture));
    }
    screen_.setVisible(preview_, true);
    screen_.setModal(preview_);
}

void Game::playVideos() {
    const double seconds = static_cast<double>(ts2::platform::ticksNs() - pageShown_) / 1e9;
    for (auto& p : playing_) {
        if (p.video.frames.empty()) {
            continue;
        }
        const auto count = p.video.frames.size();
        const auto at = static_cast<std::size_t>(seconds * p.video.rateNumerator / p.video.rateDenominator) % count;
        if (at == p.frame) {
            continue;
        }
        // Frames build on the ones before, so a broken one stops the video at the picture so far.
        bool broken = false;
        if (at < p.frame) {  // round again
            p.picture = {};
            p.frame = 0;
            broken = !ts2::reia::decodeFrame(p.video, 0, p.picture);
        }
        while (!broken && p.frame < at) {
            broken = !ts2::reia::decodeFrame(p.video, ++p.frame, p.picture);
        }
        if (broken) {
            ts2::diag::warning(std::format("menu video: frame {} is broken; the video stops there", p.frame));
            p.video.frames.clear();
        }
        screen_.setImage(p.window, p.picture);
    }
}

void Game::menuClicked(int window, std::uint32_t id) {
    const auto tile = std::find(tiles_.begin(), tiles_.end(), screen_.parent(window));
    if (screen_.visible(preview_)) {
        if (id == kPreviewClose) {
            screen_.setVisible(preview_, false);
            screen_.setModal(-1);
        } else if (id == kPreviewPlay) {
            openNeighborhood();
        }
    } else if (id == kChooserClose && inNeighborhood_) {
        closeChooser();
    } else if (id == kChooserQuit && inNeighborhood_) {
        // As in the game: the chooser closes and the quit box asks.
        closeChooser();
        openQuitBox();
    } else if (id == kQuit) {
        // At the start the quit box asks over the open chooser.
        openQuitBox();
    } else if (id == kNext && page_ + 1 < pages_) {
        ++page_;
        showPage();
    } else if (id == kPrevious && page_ > 0) {
        --page_;
        showPage();
    } else if (id == kTileButton && tile != tiles_.end()) {
        // A second click on the same tile soon after opens its neighborhood without the preview.
        // Otherwise the preview opens once the wait is over. The wait is the one the game uses for
        // clicks on lots. The tiles' own wait is not known yet.
        const auto index = static_cast<std::size_t>(page_ * kPerPage + (tile - tiles_.begin()));
        const std::uint64_t now = ts2::platform::ticksNs();
        if (clickedTile_ == index && now - tileClickedAt_ <= kDoubleClickNs) {
            clickedTile_.reset();
            chosen_ = index;
            openNeighborhood();
        } else {
            clickedTile_ = index;
            tileClickedAt_ = now;
        }
    }
}

// Builds the chosen neighborhood in the background. It shows once built.
void Game::openNeighborhood() {
    if (building_.valid()) {
        return;
    }
    const auto folder = hoods_[chosen_].folder;
    openName_ = hoods_[chosen_].name;
    openFolder_ = folder;
    ts2::diag::step("opening neighborhood " + ts2::diag::utf8(folder.filename()));
    hoodLoadStarted_ = ts2::platform::ticksNs();
    building_ = std::async(std::launch::async, [assets = assets_, install = install_, folder, user = options_.user, seed = options_.seed,
                                                &renderer = renderer_] {
        ts2::scene::SceneBuilder builder(*assets.get());
        Built b;
        ts2::content::EffectsFiles effects(install);
        b.stats = app::buildNeighborhood(builder, install, folder, effects, seed);
        app::addPlumbobs(builder, effects, folder, b.stats, b.plumbobs);
        b.scene = std::move(builder.scene());
        app::neighborhoodCamera(b.camera, install, b.stats);
        if (const auto* file = effects.newest()) {
            b.drift = ts2::scene::HoodDrift::load(*file, static_cast<std::uint32_t>(ts2::platform::ticksNs()));
            if (!b.drift) {
                ts2::diag::warning("the neighborhood camera won't drift: the effects file has no usable nhoodcam effect");
            }
        }
        b.panel = app::NeighborhoodUi::read(folder, user);
        b.gpu = renderer.prepare(b.scene);
        return b;
    });
}

// Shows a neighborhood that finished building with its panel, the menu put away.
void Game::showNeighborhood(Built built) {
    hood_ = std::move(built);
    // Nothing else uses the assets now (no lot loads while a neighborhood does), so the
    // problems noted while loading the last neighborhood can go.
    assets_.get()->warnings.clear();
    ts2::diag::info(std::format("neighborhood {} x {} tiles, {} lots, {} plants, {} decorations, built in {:.1f} s", hood_.stats.width,
                                hood_.stats.height, hood_.stats.imposters, hood_.stats.plantsShown, hood_.stats.decorationsShown,
                                secondsSince(hoodLoadStarted_)));
    renderer_.upload(std::move(hood_.gpu));
    hoodShownAt_ = ts2::platform::ticksNs();
    orbit_ = hood_.camera;
    hoodDrift_ = hood_.drift;
    hoodUi_.open(openFolder_, openName_, std::move(hood_.panel));
    if (options_.bin == "families") {
        hoodUi_.showBin(app::NeighborhoodUi::Bin::Families);
    } else if (options_.bin == "lots") {
        hoodUi_.showBin(app::NeighborhoodUi::Bin::Lots);
    } else if (options_.bin == "options") {
        hoodUi_.showOptions(true);

    }
    screen_.setVisible(menu_, false);
    screen_.setVisible(preview_, false);
    screen_.setModal(-1);
    if (options_.bin == "chooser") {
        showChooser();
    } else if (options_.bin == "quit") {
        openQuitBox();
    }
    renderer_.setClearColor(0, 0, 0);
    inNeighborhood_ = true;
    if (options_.lot > 0) {
        enterLot(static_cast<std::uint32_t>(options_.lot));
    } else if (options_.clickX >= 0) {
        const auto id = lotAt(orbit_.camera(), width_, height_, static_cast<float>(options_.clickX), static_cast<float>(options_.clickY),
                              hood_.stats.lotFootprints);
        ts2::diag::step(std::format("clicked lot {}", id));
        if (id) {
            enterLot(id);
        }
    } else if (options_.cardX >= 0) {
        const auto id = lotAt(orbit_.camera(), width_, height_, static_cast<float>(options_.cardX), static_cast<float>(options_.cardY),
                              hood_.stats.lotFootprints);
        if (hoodUi_.clickable(id)) {
            showLotCard(id);
        }
    }
}

// Main Menu in the neighborhood (top bar or options) shows the neighborhood chooser over the
// neighborhood, as the game does: no backdrop, and the "from neighborhood" buttons, Close and Quit,
// in place of the start's Quit. Picking a neighborhood there opens it as at the start.
void Game::showChooser() {
    endCameraDrag();
    screen_.setVisible(screen_.find(menu_, kBackdrop), false);
    screen_.setVisible(screen_.find(menu_, kStartButtons), false);
    screen_.setVisible(screen_.find(menu_, kNeighborhoodButtons), true);
    hoodUi_.hideAll(true);
    hoodUi_.showTag(0, 0, 0);
    orbit_.keysOn = false;
    cursors_.show(app::Cursors::kArrow);
    screen_.setVisible(menu_, true);
    screen_.raise(menu_);
    screen_.raise(preview_);  // the preview a tile opens stays over the chooser
    screen_.setModal(menu_);
}

void Game::closeChooser() {
    clickedTile_.reset();  // a tile clicked just before has no preview to open now
    screen_.setVisible(menu_, false);
    screen_.setModal(-1);
    hoodUi_.hideAll(false);
    orbit_.keysOn = true;
}

// The quit box, with any tile click still waiting for its second click dropped, so no preview
// opens over the box. The camera takes no input while it is open.
void Game::openQuitBox(bool askSave) {
    clickedTile_.reset();
    clickedLot_ = 0;
    if (inNeighborhood_) {
        hoodUi_.showTag(0, 0, 0);
        endCameraDrag();
        cursors_.show(app::Cursors::kArrow);
    }
    orbit_.keysOn = false;
    // On a lot the game pauses while the box is open.
    if (inLot_ && lotLife_ && !quitBox_.isOpen()) {
        pausedBeforeQuitBox_ = lotLife_->paused();
        lotLife_->setPaused(true);
    }
    quitBox_.open(screen_, data_, width_, height_, askSave);
}

void Game::closeQuitBox() {
    if (inLot_ && lotLife_ && quitBox_.isOpen() && pausedBeforeQuitBox_) {
        lotLife_->setPaused(*pausedBeforeQuitBox_);
    }
    pausedBeforeQuitBox_.reset();
    quitBox_.close(screen_);
    orbit_.keysOn = !chooserOpen();
}

// A click on a lot starts a short wait: a second click within it enters a lot a household lives
// on, otherwise the lot's card opens when it ends. A click on no lot that can be clicked closes the
// card.
void Game::lotClicked(float x, float y) {
    const auto id = lotAt(orbit_.camera(), width_, height_, x, y, hood_.stats.lotFootprints);
    if (!hoodUi_.clickable(id)) {
        clickedLot_ = 0;
        hoodUi_.hideCard();
        return;
    }
    const std::uint64_t now = ts2::platform::ticksNs();
    const auto& lived = hoodUi_.occupiedLots();
    if (clickedLot_ == id && now - clickedAt_ <= kDoubleClickNs && std::find(lived.begin(), lived.end(), id) != lived.end()) {
        clickedLot_ = 0;
        hoodUi_.hideCard();
        enterLot(id);
        return;
    }
    clickedLot_ = id;
    clickedAt_ = now;
}

// The tag of the lot under the mouse (not under the panels), placed by where the camera shows the
// lot's centre, 2 below its ground.
// The neighborhood's plumbobs as they are now, their sprites facing the camera.
void Game::placePlumbobs(std::uint64_t now) {
    if (hood_.plumbobs.empty()) {
        return;
    }
    const auto camera = orbit_.camera();
    const auto forward = ts2::render::normalize(ts2::render::sub(camera.target, camera.eye));
    // Looking straight down, the world's y stands in for up so right stays defined (as the renderer does).
    const auto side = ts2::render::cross(forward, {0, 0, 1});
    const auto right = ts2::render::normalize(ts2::render::dot(side, side) > 1e-8f ? side : ts2::render::cross(forward, {0, 1, 0}));
    const auto up = ts2::render::cross(right, forward);
    plumbobPlaces_.clear();
    hood_.plumbobs.place(static_cast<double>(now - hoodShownAt_) / 1e9, right, up, plumbobPlaces_);
    for (const auto& [instance, place] : plumbobPlaces_) {
        renderer_.setPlace(instance, place);
    }
}

void Game::showLotTag() {
    if (neighborhoodCovered()) {
        return;
    }
    float x = 0, y = 0;
    pf::mousePosition(x, y);
    if (options_.hoverX >= 0) {
        x = static_cast<float>(options_.hoverX);
        y = static_cast<float>(options_.hoverY);
    }
    std::uint32_t lot = 0;
    if (!down_ && !screen_.over(static_cast<int>(x), static_cast<int>(y))) {
        lot = lotAt(orbit_.camera(), width_, height_, x, y, hood_.stats.lotFootprints);
    }
    const auto where = std::find_if(hood_.stats.lotFootprints.begin(), hood_.stats.lotFootprints.end(),
                                    [&](const ts2::scene::LotFootprint& f) { return f.id == lot; });
    // A lot under the mouse shows the cursor of something that can be used. A camera drag or turn
    // keeps its own cursor.
    hoodCursor_ = where == hood_.stats.lotFootprints.end() ? app::Cursors::kArrow : app::Cursors::kLiveObjectAvailable;
    if (!dragFrom_ && !turnFrom_) {
        cursors_.show(hoodCursor_);
    }
    if (where == hood_.stats.lotFootprints.end()) {
        hoodUi_.showTag(0, 0, 0);
        return;
    }
    const auto at = app::screenPoint(orbit_.camera(), width_, height_, {(where->x0 + where->x1) / 2, (where->y0 + where->y1) / 2, where->z - 2});
    hoodUi_.showTag(lot, static_cast<int>(at[0]), static_cast<int>(at[1]));
}

// The card opens beside the lot's centre as the camera shows it.
void Game::showLotCard(std::uint32_t lot) {
    if (neighborhoodCovered()) {
        return;
    }
    const auto where = std::find_if(hood_.stats.lotFootprints.begin(), hood_.stats.lotFootprints.end(),
                                    [&](const ts2::scene::LotFootprint& f) { return f.id == lot; });
    if (where == hood_.stats.lotFootprints.end()) {
        return;
    }
    const auto at = app::screenPoint(orbit_.camera(), width_, height_, {(where->x0 + where->x1) / 2, (where->y0 + where->y1) / 2, where->z});
    hoodUi_.showCard(lot, static_cast<int>(at[0]));
}

// Builds a lot of the open neighborhood in the background. It shows once built.
void Game::enterLot(std::uint32_t id) {
    enteredLot_ = id;
    const auto file = openFolder_ / "Lots" / (openFolder_.filename().u8string() + u8"_Lot" + std::u8string(reinterpret_cast<const char8_t*>(std::to_string(id).c_str())) + u8".package");
    ts2::diag::step(std::format("entering lot {}", id));
    lotLoadStarted_ = ts2::platform::ticksNs();
    // The ground around it, as in the game's lot view.
    const auto where = std::find_if(hood_.stats.lotFootprints.begin(), hood_.stats.lotFootprints.end(),
                                    [&](const ts2::scene::LotFootprint& f) { return f.id == id; });
    std::optional<ts2::scene::LotFootprint> footprint;
    if (where != hood_.stats.lotFootprints.end()) {
        footprint = *where;
    }
    const ts2::scene::Scene* hoodScene = &hood_.scene;
    const std::string terrainType = hood_.stats.terrainType;
    lotSimBuilding_ = std::async(std::launch::async, [install = install_, file] { return app::loadSimulation(install, file); });
    lotUi_.setObjectThumbnails(openFolder_.parent_path().parent_path() / "Thumbnails" / "ObjectThumbnails.package");
    lotBuilding_ = std::async(std::launch::async, [assets = assets_, file, install = install_, footprint, hoodScene, terrainType, &renderer = renderer_] {
        LotBuilt built;
        {
            ts2::scene::SceneBuilder builder(*assets.get());
            const auto lot = ts2::content::loadLot(file);
            ts2::scene::addLot(builder, lot, file);
            built.lightState = app::savedLightingState(assets.get()->lighting(), lot);
            built.light = app::lightLot(*assets.get(), builder.scene(), lot, file, built.lightState);
            built.lot = std::move(builder.scene());
        }
        built.routeModels = app::readRouteModels(*assets.get(), file);
        built.view.start(built.lot, built.camera);
        // The game's live-mode camera gives the field of view, the zoom range and the pitch.
        if (auto rig = ts2::content::findCamera(install, "Live"); rig && !rig->levels.empty()) {
            built.camera.maxDistance = rig->levels.front().distance;
            built.camera.minDistance = rig->levels.back().distance;
            built.camera.rig = std::move(rig);
            built.camera.pivotHeight = 1;  // LiveCamera.txt's pivotheightabovefloor
            // The game's defaultcamerapitch: where the pitch starts between the zoom's least and most.
            built.camera.pitchBlend = built.camera.pitchBlendGoal = 0.15f;
            built.camera.zoomTo(built.camera.distance);
            built.camera.distanceGoal = built.camera.distance;
        }
        built.freeRig = ts2::content::findCamera(install, "CameraMan");
        // Shown in its neighborhood, where its imposter was.
        if (footprint) {
            ts2::scene::Scene skirt;
            ts2::scene::SkirtArea area;
            if (hoodScene->terrain) {
                const int margin = app::lotSkirtMargin(install);
                area = ts2::scene::lotSkirtArea(*hoodScene->terrain, *footprint, margin);
                ts2::scene::SceneBuilder builder(*assets.get());
                ts2::scene::addLotSkirt(builder, *hoodScene->terrain, *footprint, margin, terrainType,
                                        ts2::content::lotSkirtRoadTextures(ts2::content::findProducts(install)));
                skirt = std::move(builder.scene());
            }
            built.lot = app::lotInNeighborhood(*hoodScene, std::move(built.lot), std::move(skirt), *footprint, area);
            ts2::scene::lightLotSkirt(built.lot, ts2::content::stateTime(built.lightState) != ts2::content::StateTime::Night);
            app::placeCamera(built.camera, *footprint);
            built.place = footprint->place;
            built.camera.minFar = 2000;
        }
        built.gpu = renderer.prepare(built.lot);
        return built;
    });
}

// Shows a lot that finished building instead of the neighborhood.
void Game::showLot(LotBuilt& built) {
    endCameraDrag();
    lot_ = std::move(built.lot);
    lotRouteModels_ = std::move(built.routeModels);
    {
        // Imposters around the lot take the lighting state's environment colour.
        const auto* state = ts2::content::findState(assets_.get()->lighting(), built.lightState);
        const std::array<float, 3> environment = state ? state->environment : std::array<float, 3>{1, 1, 1};
        renderer_.setImposterLight(environment);
        renderer_.setNight(ts2::content::stateTime(built.lightState) == ts2::content::StateTime::Night);
        lotDaylight_ = std::make_unique<app::LotDaylight>(std::move(built.light), built.lightState, environment, built.place);
    }
    hoodOrbit_ = orbit_;
    lotView_ = built.view;
    orbit_ = built.camera;
    orbit_.goal = orbit_.target;
    orbit_.useRigEasing();
    cameraHeight_ = {};
    lotPlace_ = built.place;
    if (orbit_.rig) {
        orbit_.limitGoal = keepNearLot(orbit_.rig->param("constraintwidth", 0));
    }
    freeCam_.reset();
    freeRig_ = std::move(built.freeRig);
    householdShown_ = false;
    stepTaken_ = true;
    hood_.gpu = renderer_.swap(std::move(built.gpu));
    lotPicker_.prepare(lot_);
    hovered_ = 0;
    lotShown_ = ts2::platform::ticksNs();
    ts2::diag::info(std::format("lot {} shown after {:.1f} s", enteredLot_, secondsSince(lotLoadStarted_)));

    screen_.setVisible(hoodUi_.root(), false);
    hoodUi_.showBin(app::NeighborhoodUi::Bin::None);
    hoodUi_.showTag(0, 0, 0);
    lotUi_.open();
    inNeighborhood_ = false;
    inLot_ = true;
}

// Back to the neighborhood, as it was.
void Game::leaveLot() {
    ts2::diag::step("back to the neighborhood");
    endCameraDrag();
    inLot_ = false;
    // Freeing all of the lot at once took a fifth of a second.
    lotLeft_ = renderer_.swap(std::move(hood_.gpu));
    lotFreeing_ = std::async(std::launch::async, [scene = std::move(lot_), life = std::move(lotLife_), daylight = std::move(lotDaylight_)]() mutable {
        daylight.reset();
        scene = {};
        life.reset();
    });
    lot_ = {};
    lotRouteModels_.reset();
    if (freeCam_) {
        freeCam_.reset();
        freeMouse_.reset();
        lotUi_.hideForFreeCamera(false);
        pf::showCursor();
    }
    lotUi_.close();
    pieMenu_.close();
    hovered_ = 0;
    renderer_.setHighlight(0, 0);
    renderer_.setImposterLight({1, 1, 1});
    renderer_.setNight(false);
    orbit_ = hoodOrbit_;
    orbit_.dragSpeed = {0, 0};
    screen_.setVisible(hoodUi_.root(), true);
    inNeighborhood_ = true;
}

// The game's live-mode keys. P, Pause, ` or 0 on the number pad pause. 1, 2 and 3 pick a
// speed. F5 opens the options.
app::LotUi::Action Game::lotKey(pf::Key key) {
    using Action = app::LotUi::Action;
    switch (key) {
    case pf::Key::P:
    case pf::Key::Pause:
    case pf::Key::Grave:
    case pf::Key::Pad0: return Action::Pause;
    case pf::Key::Num1:
    case pf::Key::Pad1: return Action::Normal;
    case pf::Key::Num2:
    case pf::Key::Pad2: return Action::High;
    case pf::Key::Num3:
    case pf::Key::Pad3: return Action::Ultra;
    case pf::Key::F5:
        lotUi_.showOptions(!lotUi_.optionsShown());
        return lotUi_.optionsShown() ? Action::OptionsOpened : Action::OptionsClosed;
    default: return Action::None;
    }
}

void Game::lotAction(app::LotUi::Action action) {
    using Action = app::LotUi::Action;
    if (action == Action::Neighborhood) {
        leaveLot();
        return;
    }
    if (action == Action::Quit) {
        openQuitBox(lotChanged());
        return;
    }
    if (action == Action::FloorUp || action == Action::FloorDown) {
        lotView_.changeLevel(action == Action::FloorUp ? 1 : -1, orbit_);
        return;
    }
    if (action == Action::WallMode) {
        lotView_.setWallMode(lotUi_.pickedWallMode());
        return;
    }
    if (!lotLife_) {
        return;
    }
    switch (action) {
    case Action::Pause: lotLife_->setPaused(!lotLife_->paused()); break;
    case Action::Normal: lotLife_->setSpeed(ts2::sim::Speed::Normal); break;
    case Action::High: lotLife_->setSpeed(ts2::sim::Speed::High); break;
    case Action::Ultra: lotLife_->setSpeed(ts2::sim::Speed::Ultra); break;
    case Action::OptionsOpened:
        pausedBeforeOptions_ = lotLife_->paused();
        lotLife_->setPaused(true);
        break;
    case Action::OptionsClosed: lotLife_->setPaused(pausedBeforeOptions_); break;
    case Action::RewardsOpened:
        pausedBeforeRewards_ = lotLife_->paused();
        lotLife_->setPaused(true);
        break;
    case Action::RewardsClosed: lotLife_->setPaused(pausedBeforeRewards_); break;
    case Action::SelectSim:
        // Clicking the selected Sim's thumbnail again centres the camera on the Sim and ends any
        // tracking.
        if (lotUi_.clickedSim() == lotLife_->selected()) {
            orbit_.subject = nullptr;
            centreOnSim(lotUi_.clickedSim());
        } else {
            lotLife_->select(lotUi_.clickedSim());
        }
        break;
    case Action::TrackSim: trackSim(lotUi_.clickedSim()); break;
    default: break;
    }
}

// A right click on a Sim's thumbnail: the camera centres on the Sim and tracks it, or stops
// tracking it when it already does. A Sim that isn't on the lot is not tracked.
void Game::trackSim(std::int16_t sim) {
    if (orbit_.subject && trackedSim_ == sim) {
        orbit_.subject = nullptr;
        return;
    }
    if (!centreOnSim(sim)) {
        return;
    }
    trackedSim_ = sim;
    orbit_.subject = [this, sim] { return simRoot(sim); };
}

// Centres the camera on a Sim on the lot, ending any tracking, and returns false for a Sim that
// isn't on it. The view first changes storey to the Sim's. A Sim indoors gets its storey either way.
// A Sim outside (room 0) gets it only when the view is below it.
bool Game::centreOnSim(std::int16_t sim) {
    const auto root = simRoot(sim);
    if (!root) {
        return false;
    }
    if (const auto* object = lotLife_ ? lotLife_->simulation().world().find(sim) : nullptr; object && object->level > 0) {
        const int storey = lotView_.ground + object->level - 1;
        // Positions are kept in 1/65536 tiles.
        const float x = static_cast<float>(object->x) / 65536, y = static_cast<float>(object->y) / 65536;
        const bool indoors = lotLife_->simulation().world().rooms().roomAtPosition(object->level - 1, x, y) > 0;
        if (indoors || lotView_.view.level < storey) {
            lotView_.changeLevel(storey - lotView_.view.level, orbit_);
        }
    }
    orbit_.centreOn(*root);
    return true;
}

// Where a Sim's root_trans joint is in the world as last posed, none for a Sim not on the lot.
std::optional<ts2::render::Vec3> Game::simRoot(std::int16_t sim) const {
    for (const auto& [actor, nodes] : lotPose_.nodes) {
        if (actor >= lot_.actors.size() || lot_.actors[actor].object != static_cast<std::uint32_t>(sim) || !lot_.actors[actor].rig) {
            continue;
        }
        const int root = lot_.actors[actor].rig->find("root_trans");
        if (root < 0 || static_cast<std::size_t>(root) >= nodes.size()) {
            return std::nullopt;
        }
        const auto place = lot_.actors[actor].place * nodes[static_cast<std::size_t>(root)];
        float at[3] = {0, 0, 0};
        place.apply(at, true);
        return ts2::render::Vec3{at[0], at[1], at[2]};
    }
    return std::nullopt;
}

// A click on the lot where no object is highlighted: on empty floor or ground (not pool water,
// whose Swim Here destination isn't ported yet) the selected Sim's destination is made there
// and its pie menu opens. False when nothing opens.
bool Game::openGroundMenu(float x, float y) {
    const auto ray = screenRay(orbit_.camera(), width_, height_, x, y);
    const auto& eye = orbit_.camera().eye;
    const auto hit = lotPicker_.pick(lot_, lotVisible_, eye, ray);
    if (!hit || !lotLife_) {
        return false;
    }
    const auto& instance = lot_.instances[hit->instance];
    const bool floor = instance.part == ts2::scene::Part::Floor && instance.name != "pool water";
    const bool terrain = instance.name == "terrain";
    if (instance.object != 0 || !(floor || terrain)) {
        return false;
    }
    const int storey = terrain ? 1 : instance.level - lot_.groundLevel + 1;
    const std::array<float, 3> at{eye[0] + ray[0] * hit->distance, eye[1] + ray[1] * hit->distance, eye[2] + ray[2] * hit->distance};
    auto ground = lotLife_->groundPieMenu(at, storey);
    if (ground.choices.empty()) {
        return false;
    }
    pieObject_ = ground.object;
    pieMenu_.open(std::move(ground.choices), static_cast<int>(x), static_cast<int>(y), width_, height_);
    return true;
}

// The right button held from a press on the lot or neighborhood drags the camera: a small square
// marks the press point, and once the mouse is more than 8 pixels from it (along either axis) the
// cursor is the turned drag arrow pointing from the press point to the mouse. Within those 8
// pixels it is the cursor for what is under the mouse.
void Game::cameraDrag(const pf::Event& e, float x, float y) {
    // The middle button turns the camera: the pointer hides and stays where it was pressed (the
    // game puts it back after every move), with the game's picture centred on that point.
    if (e.type == pf::Event::Type::MouseDown && e.button == pf::MouseButton::Middle && !pieMenu_.isOpen() && !turnFrom_ &&
        !screen_.over(static_cast<int>(x), static_cast<int>(y))) {
        turnFrom_ = {e.x, e.y};
        ts2::ui::Window mark;
        // Its magenta shows through, as it does in the game.
        const std::uint32_t markImage = inLot_ ? kTurnMark : kTurnMarkSides;
        mark.attributes = {{"iid", "IGZWinBMP"}, {"paint", "yes"}, {"area", "(0,0,1,1)"}, {"winflag_visible", "yes"}, {"winflag_pbufftrans", "yes"},
                           {"image", std::format("{{{:08x},{:08x}}}", kTurnMarkGroup, markImage)}};
        turnMark_ = screen_.addWindows({mark}, 0, 0);
        screen_.setPassive(turnMark_);
        if (const auto image = data_.image(kTurnMarkGroup, markImage)) {
            screen_.resize(turnMark_, static_cast<int>(image->width), static_cast<int>(image->height));
        }
        screen_.move(turnMark_, static_cast<int>(x) - screen_.width(turnMark_) / 2, static_cast<int>(y) - screen_.height(turnMark_) / 2);
        pf::hideCursor();
        return;
    }
    if (e.type == pf::Event::Type::MouseUp && e.button == pf::MouseButton::Middle && turnFrom_) {
        pf::showCursor();
        turnFrom_.reset();
        screen_.remove(turnMark_);
        turnMark_ = -1;
        return;
    }
    // Each move turns by how far the pointer went from the press point, and the pointer goes back
    // there (the move that causes lands on the press point and turns nothing).
    if (e.type == pf::Event::Type::MouseMotion && turnFrom_) {
        const float dx = e.x - (*turnFrom_)[0], dy = e.y - (*turnFrom_)[1];
        if (dx != 0 || dy != 0) {
            orbit_.turnMove[0] += dx;
            orbit_.turnMove[1] += dy;
            if (pf::Window* window = pf::mouseFocus()) {
                window->warpMouse((*turnFrom_)[0], (*turnFrom_)[1]);
            }
        }
        return;
    }
    if (e.type == pf::Event::Type::MouseDown && e.button == pf::MouseButton::Right && !pieMenu_.isOpen() &&
        !screen_.over(static_cast<int>(x), static_cast<int>(y))) {
        dragFrom_ = {x, y};
        screen_.remove(pressMark_);
        pressMark_ = screen_.load(kUiScripts, kPressMark, 0, 0);
        screen_.setPassive(pressMark_);
        screen_.move(pressMark_, static_cast<int>(x) - screen_.width(pressMark_) / 2, static_cast<int>(y) - screen_.height(pressMark_) / 2);
    } else if (e.type == pf::Event::Type::MouseUp && e.button == pf::MouseButton::Right && dragFrom_) {
        dragFrom_.reset();
        orbit_.dragSpeed = {0, 0};
        screen_.remove(pressMark_);
        pressMark_ = -1;
        hoverAt_ = {x, y};  // the live cursor again, for what is under the mouse now
    } else if (e.type == pf::Event::Type::MouseMotion && dragFrom_) {
        const float dx = x - (*dragFrom_)[0], dy = y - (*dragFrom_)[1];
        // Every move scrolls (which ends any tracking), but within 8 pixels of the press point the
        // scroll speed is zero.
        const bool dragging = std::max(std::abs(dx), std::abs(dy)) > 8;
        orbit_.scrollBy(dragging ? dx * 0.25f : 0.0f, dragging ? dy * 0.25f : 0.0f, width_, height_);
        if (dragging) {
            cursors_.showDragArrow(-std::atan2(dy, dx));
        } else {
            cursors_.show(inLot_ ? liveCursor_ : hoodCursor_);
        }
    }
}

// Ends a camera drag or turn under way, as when the view changes before the button comes up: its
// marks go and the pointer shows again.
void Game::endCameraDrag() {
    dragFrom_.reset();
    orbit_.dragSpeed = {0, 0};
    screen_.remove(pressMark_);
    pressMark_ = -1;
    if (turnFrom_) {
        turnFrom_.reset();
        screen_.remove(turnMark_);
        turnMark_ = -1;
        pf::showCursor();
    }
}

// An open pie menu shows the arrow, not the live cursor of what is under it.
void Game::showPieCursor() {
    hoverAt_.reset();
    cursors_.show(app::Cursors::kArrow);
}

// As the game keeps its lot cameras' goals: between the lot's edges moved in by the camera's
// constraint width (negative: out past them), along each of the lot's axes, the far edge winning
// when they cross. Nothing is kept while the lot's size is unknown.
std::function<void(app::Vec3&)> Game::keepNearLot(float inset) const {
    if (lot_.lotSize[0] <= 0 || lot_.lotSize[1] <= 0) {
        return {};
    }
    return [place = lotPlace_, size = lot_.lotSize, inset](app::Vec3& goal) {
        float at[3] = {goal[0], goal[1], goal[2]};
        place.rigidInverse().apply(at, true);
        for (std::size_t k = 0; k < 2; ++k) {
            at[k] = std::min(std::max(at[k], inset), size[k] - inset);
        }
        place.apply(at, true);
        goal = {at[0], at[1], goal[2]};
    };
}

// Tab (or Y) in a lot turns the free camera on, and Tab, Y or Escape off. On, the pointer hides and
// is held in the middle of the window, where the mouse looks round from. Off, the lot camera takes
// over from its view.
void Game::toggleFreeCamera() {
    if (freeCam_) {
        auto toLot = [this](const app::Vec3& p) {
            float at[3] = {p[0], p[1], p[2]};
            lotPlace_.rigidInverse().apply(at, true);
            return app::Vec3{at[0], at[1], at[2]};
        };
        auto fromLot = [this](const app::Vec3& p) {
            float at[3] = {p[0], p[1], p[2]};
            lotPlace_.apply(at, true);
            return app::Vec3{at[0], at[1], at[2]};
        };
        freeCam_->leave(orbit_, toLot, fromLot);
        freeCam_.reset();
        freeMouse_.reset();
        lotUi_.hideForFreeCamera(false);
        pf::showCursor();
        return;
    }
    if (!freeRig_ || !orbit_.rig) {
        return;
    }
    pieMenu_.close();
    endCameraDrag();
    freeCam_.emplace(*freeRig_);
    freeCam_->enter(orbit_);
    hovered_ = 0;
    // Only the pause frame stays on screen, as in the game.
    lotUi_.hideForFreeCamera(true);
    pf::hideCursor();
}

// The mouse while the free camera is on: where the pointer went, for the next frame.
void Game::freeCameraMouse(const pf::Event& e) {
    if (e.type == pf::Event::Type::MouseMotion) {
        freeMouse_ = std::array<float, 2>{e.x, e.y};
    }
}

// As the game: once a frame the free camera looks round by how far the pointer went from the
// middle of the window, and the pointer goes back there (while the window has the mouse).
void Game::freeCameraLook() {
    pf::Window* window = pf::mouseFocus();
    if (!freeMouse_ || !window) {
        freeMouse_.reset();
        return;
    }
    int w = 0, h = 0;
    window->size(w, h);
    const float cx = static_cast<float>(w / 2), cy = static_cast<float>(h / 2);
    const float dx = (*freeMouse_)[0] - cx, dy = (*freeMouse_)[1] - cy;
    freeMouse_.reset();
    if (dx != 0 || dy != 0) {
        freeCam_->look(dx, dy);
        window->warpMouse(cx, cy);
    }
}

// The mouse over the lot: the open pie menu takes it first, then a click on an object or on the
// ground, then the panels and the camera.
void Game::lotMouse(const pf::Event& e, float x, float y) {
    const int ix = static_cast<int>(x), iy = static_cast<int>(y);
    if (quitBox_.isOpen()) {
        cursors_.show(app::Cursors::kArrow);
        if (const auto clicked = screen_.mouse(ix, iy, down_, static_cast<double>(ts2::platform::ticksNs()) / 1e9)) {
            windowClicked(*clicked);
        }
        return;
    }
    if (pieMenu_.isOpen()) {
        using Mouse = app::PieMenuUi::Mouse;
        const bool leftButton = e.type != pf::Event::Type::MouseMotion && e.button == pf::MouseButton::Left;
        const bool right = e.type == pf::Event::Type::MouseDown && e.button == pf::MouseButton::Right;
        const Mouse what = right ? Mouse::RightDown
                           : leftButton ? (e.type == pf::Event::Type::MouseDown ? Mouse::LeftDown : Mouse::LeftUp)
                                        : Mouse::Move;
        if (const auto picked = pieMenu_.mouse(ix, iy, what); picked && lotLife_) {
            lotLife_->choose(pieObject_, *picked);
        }
        if (!pieMenu_.isOpen()) {
            hoverAt_ = {x, y};  // the live cursor again, for what is under the mouse now
        }
        return;
    }
    const bool leftDown = e.type == pf::Event::Type::MouseDown && e.button == pf::MouseButton::Left;
    // A left click on the highlighted object opens its pie menu for the selected Sim.
    if (lotLife_ && hovered_ != 0 && leftDown && !screen_.over(ix, iy)) {
        if (auto choices = lotLife_->pieMenu(hovered_); !choices.empty()) {
            pieObject_ = hovered_;
            pieMenu_.open(std::move(choices), ix, iy, width_, height_);
            showPieCursor();
            return;
        }
    }
    // A left click on empty floor or ground opens the menu of a destination made there.
    if (lotLife_ && hovered_ == 0 && leftDown && !screen_.over(ix, iy) && openGroundMenu(x, y)) {
        showPieCursor();
        return;
    }
    lotUi_.mouseMoved(ix, iy);
    if (e.type != pf::Event::Type::MouseMotion && e.button == pf::MouseButton::Left && lotLife_) {
        lotUi_.mouseButton(ix, iy, e.type == pf::Event::Type::MouseDown, *lotLife_);
    }
    if (e.type == pf::Event::Type::MouseDown && e.button == pf::MouseButton::Right && lotLife_) {
        if (const auto button = screen_.buttonAt(ix, iy)) {
            lotAction(lotUi_.rightClicked(*button, *lotLife_));
        }
    }
    if (!screen_.over(ix, iy)) {
        app::orbitEvent(orbit_, e);
        if (e.type == pf::Event::Type::MouseMotion && !down_ && !dragFrom_) {
            hoverAt_ = {x, y};
        }
    } else {
        cursors_.show(app::Cursors::kArrow);
    }
    if (const auto clicked = screen_.mouse(ix, iy, down_, static_cast<double>(ts2::platform::ticksNs()) / 1e9)) {
        lotAction(lotUi_.clicked(*clicked));
    }
}

// A frame of the lot: the camera, the object under the mouse, the Sims and what they changed.
void Game::updateLot(std::uint64_t now) {
    const float dt = std::min(0.1f, static_cast<float>(now - lastFrame_) / 1e9f);    if (freeCam_) {
        freeCameraLook();
        // The game's free camera samples its corners half a tile out, at half their height.
        freeCam_->run(
            dt,
            [&](float x, float y, std::array<float, 4>& corners, unsigned& counter) {
                return cameraHeight_.groundBelow(lot_, lotVisible_, lotPlace_, x, y, corners, counter, 0.5f, 0.5f);
            },
            keepNearLot(freeCam_->constraintWidth()));
        lotView_.view.facing = freeCam_->facing();
    } else {
        app::orbitStep(orbit_, dt, width_, height_, [&] { cameraHeight_.apply(orbit_, lot_, lotVisible_, lotPlace_, lotView_.view.level); });
        lotView_.view.facing = orbit_.facing();
    }
    lotUi_.showTracked(orbit_.subject ? trackedSim_ : 0);
    const auto shown = ts2::scene::visibleInstances(lot_, lotView_.view);
    lotVisible_ = shown;
    renderer_.show(shown);
    lotUi_.showWallMode(lotView_.wallMode());
    // The game looks for the object under the mouse as it moves with no button held.
    if (hoverAt_ && lotLife_ && !freeCam_) {
        const auto ray = screenRay(orbit_.camera(), width_, height_, (*hoverAt_)[0], (*hoverAt_)[1]);
        const auto hit = lotPicker_.pick(lot_, shown, orbit_.camera().eye, ray);
        const std::uint32_t object = hit ? lot_.instances[hit->instance].object : 0;
        hovered_ = lotLife_->hoverable(object) ? object : 0;
        liveCursor_ = liveCursor(hit ? &lot_.instances[hit->instance] : nullptr, hovered_ != 0);
        cursors_.show(liveCursor_);
        hoverAt_.reset();
    }
    renderer_.setHighlight(hovered_, ts2::scene::hoverAmbient(static_cast<double>(now) / 1e9));
    const bool shooting = !options_.shot.empty();
    if (!lotLife_ && (!stepTaken_ || shooting)) {
        startLotLife();
    }
    // Drawn once the lot's textures are on the GPU, as reading a picture back waits for them.
    if (lotLife_ && !householdShown_ && ((!stepTaken_ && renderer_.uploaded()) || shooting)) {
        drawHousehold(now);
    }
    if (lotLife_) {
        lotLife_->markOnScreen(simsInView(lot_, lotCamera(), width_, height_));
        lotLife_->setCamera(lotCamera().eye);
        lotLife_->update(std::min(100.0, static_cast<double>(now - lastFrame_) / 1e6), lot_);
        lotRan_ = lotRan_ || !lotLife_->paused();
        if (lotDaylight_) {
            lotDaylight_->update(static_cast<double>(now - lastFrame_) / 1e9, lotLife_->simulation().globals(), *assets_.get(), lot_, renderer_,
                                 *lotLife_);
        }
        lotUi_.showLife(*lotLife_);
    }
    app::animateActors(lot_, lotLife_ ? lotLife_->seconds() : static_cast<double>(now - lotShown_) / 1e9, renderer_, lotPose_);
    if (lotLife_) {
        sendLotChanges();
    }
}

// The Sims come to life once their simulation is built.
void Game::startLotLife() {
    if (auto sim = finished(lotSimBuilding_, "lot's simulation"); sim && *sim) {
        stepTaken_ = true;
        lotLife_ = std::make_unique<app::LotLife>(std::move(*sim), *assets_.get(), lotRouteModels_);
        lotLife_->traceIn(lot_, lotPose_, lotVisible_);
        // A lot opens paused, as in the game, after the game's 10 ticks of loading. Sims start
        // their main loop in them (Sims without turn-ons get theirs there). The ticks need the
        // scene traced in, for the scripts' ray tests and the Sims' sizes.
        constexpr int kLoadTicks = 10;
        for (int i = 0; i < kLoadTicks; ++i) {
            lotLife_->simulation().tick();
        }
        lotLife_->setPaused(!options_.unpaused);
        lotRan_ = false;
        ts2::diag::step(std::format("the lot's simulation is ready after {:.1f} s", secondsSince(lotLoadStarted_)));
    }
}

// The household's thumbnails: Sims on the lot are drawn anew, as the game does. The others keep
// their character file's portrait.
void Game::drawHousehold(std::uint64_t now) {
    householdShown_ = true;
    std::vector<app::Skewer::Member> members;
    std::mt19937 poseRandom(static_cast<unsigned>(now));
    for (const std::int16_t id : lotLife_->household()) {
        auto portrait = app::drawPortrait(lot_, renderer_, lotLife_->animations(), static_cast<std::uint32_t>(id), poseRandom);
        members.push_back({id, portrait ? std::move(portrait) : lotLife_->portrait(id)});
    }
    renderer_.show(ts2::scene::visibleInstances(lot_, lotView_.view));
    lotUi_.showHousehold(std::move(members));
    ts2::diag::step("household portraits drawn");
}

// What the Sims changed on the lot this frame goes to the GPU and the picker.
void Game::sendLotChanges() {
    if (!lotLife_->changedInstances().empty()) {
        renderer_.addMaterials(lot_);
        renderer_.addMeshes(lot_);
        for (const std::size_t instance : lotLife_->changedInstances()) {
            renderer_.setInstance(lot_, instance);
            lotPicker_.update(lot_, instance);
        }
    }
    for (const auto& [instance, place] : lotLife_->movedObjects()) {
        renderer_.setPlace(instance, place);
        lotPicker_.update(lot_, instance);
    }
    for (const std::size_t instance : lotLife_->relitInstances()) {
        renderer_.setLights(lot_, instance);
    }
    if (!lotLife_->changedMaterials().empty()) {
        renderer_.addMaterials(lot_);
        for (const auto& [instance, materials] : lotLife_->changedMaterials()) {
            renderer_.setMaterials(instance, materials);
        }
    }
}

// What the options ask for at start: a neighborhood opened or previewed, the mouse resting
// somewhere, and a picture of the menu.
void Game::startFromOptions() {
    if (options_.play >= 0 && static_cast<std::size_t>(options_.play) < hoods_.size()) {
        chosen_ = static_cast<std::size_t>(options_.play);
        openNeighborhood();
    }
    if (options_.preview >= 0 && static_cast<std::size_t>(options_.preview) < hoods_.size()) {
        chosen_ = static_cast<std::size_t>(options_.preview);
        showPreview();
    }
    if (options_.hoverX >= 0) {
        screen_.mouse(options_.hoverX, options_.hoverY, false, 0);
    }
    if (!options_.shot.empty() && !building_.valid()) {
        showSprites(options_.hoverX >= 0 ? 10.0 : 0.0, false);
        if (!renderer_.drawToFile({}, static_cast<unsigned>(width_), static_cast<unsigned>(height_), options_.shot)) {
            throw std::runtime_error("couldn't save the picture");
        }
        ts2::diag::info("wrote " + ts2::diag::utf8(options_.shot));
    }
}

void Game::run() {
    startFromOptions();
    running_ = options_.shot.empty() || building_.valid();
    lastFrame_ = ts2::platform::ticksNs();
    lastTimings_ = lastFrame_;
    while (running_) {
        ts2::diag::Timer frameTimer("frame");
        stepTaken_ = false;
        pf::Event e;
        while (pf::pollEvent(e)) {
            handleEvent(e);
        }
        if (options_.takeInstallWarning && options_.shot.empty()) {
            if (const std::string warning = options_.takeInstallWarning(); !warning.empty()) {
                app::showMessage(app::MessageKind::Warning, warning, pf::mouseFocus());
            }
        }
        if (auto opened = finished(building_, "neighborhood")) {
            showNeighborhood(std::move(*opened));
        }
        lotLeft_.releaseSome(32);
        // The lot's panels load while the lot does, in a frame of their own.
        if (loading(lotBuilding_)) {
            lotUi_.load();
        }
        if (auto done = finished(lotBuilding_, "lot")) {
            showLot(*done);
        }
        const std::uint64_t now = ts2::platform::ticksNs();
        reportFrame(now);
        if (!inLot_ && !inNeighborhood_) {
            cursors_.show(app::Cursors::kArrow);
        }
        if (clickedTile_ && now - tileClickedAt_ > kDoubleClickNs) {
            chosen_ = *clickedTile_;
            clickedTile_.reset();
            showPreview();
        }
        if (inLot_) {
            updateLot(now);
        } else if (inNeighborhood_) {
            // A picture is taken without the drift, so the same view gives the same picture.
            app::orbitStep(orbit_, std::min(0.1f, static_cast<float>(now - lastFrame_) / 1e9f), width_, height_, [this] {
                if (hoodDrift_ && options_.shot.empty()) {
                    app::applyDrift(orbit_, hoodDrift_->step(orbit_.target, 1.0f / 120));
                }
            });
            showLotTag();
            if (clickedLot_ != 0 && now - clickedAt_ > kDoubleClickNs) {
                showLotCard(clickedLot_);
                clickedLot_ = 0;
            }
            renderer_.setTime(static_cast<float>(static_cast<double>(now) / 1e9));
            placePlumbobs(now);
        }
        // The chooser's tiles play their videos over the neighborhood too.
        if (!inLot_ && (!inNeighborhood_ || chooserOpen())) {
            playVideos();
        }
        lastFrame_ = now;
        showSprites(static_cast<double>(ts2::platform::ticksNs()) / 1e9, inNeighborhood_ || inLot_);
        // Clicks on the lot's panels wait for the household to be in them.
        const bool panelsReady = options_.uiClicks.empty() || householdShown_;
        if (!options_.shot.empty() && (options_.lot > 0 || options_.clickX >= 0 ? inLot_ && panelsReady : inNeighborhood_)) {
            if (takePicture()) {
                running_ = false;
            }
        } else if (!renderer_.draw(inLot_ ? lotCamera() : inNeighborhood_ ? orbit_.camera() : ts2::render::Camera{})) {
            ts2::platform::delayMs(16);
        }
    }
}

void Game::handleEvent(const pf::Event& e) {
    // Mouse movement, clicks, the wheel and keys all end the neighborhood camera's drift.
    const bool input = e.type == pf::Event::Type::MouseMotion || e.type == pf::Event::Type::MouseDown || e.type == pf::Event::Type::MouseUp ||
                       e.type == pf::Event::Type::MouseWheel || e.type == pf::Event::Type::KeyDown || e.type == pf::Event::Type::KeyUp;
    if (inNeighborhood_ && hoodDrift_ && input) {
        hoodDrift_->interrupt();
    }
    const bool freeToggle = e.type == pf::Event::Type::KeyDown && !e.repeat && inLot_ && !quitBox_.isOpen() &&
                            (e.scancode == pf::Key::Tab || e.scancode == pf::Key::Y || (freeCam_ && e.scancode == pf::Key::Escape));
    if (freeToggle) {
        toggleFreeCamera();
    } else if (freeCam_ && (e.type == pf::Event::Type::KeyDown || e.type == pf::Event::Type::KeyUp) &&
               (e.repeat || freeCam_->key(e.scancode, e.type == pf::Event::Type::KeyDown))) {
        // The free camera's own keys.
    } else if (e.type == pf::Event::Type::KeyDown && quitBox_.isOpen()) {
        if (e.key == pf::Key::Escape) {
            closeQuitBox();  // Escape answers No, or Cancel on the save question
        }
    } else if (e.type == pf::Event::Type::Quit) {
        ts2::diag::step("window closed");
        running_ = false;
    } else if (e.type == pf::Event::Type::KeyDown && inLot_ && pieMenu_.isOpen()) {
        // Any key, Escape too, closes an open pie menu. Escape does nothing else in the game.
        pieMenu_.close();
    } else if (e.type == pf::Event::Type::KeyDown && inLot_) {
        if (const auto action = lotKey(e.key); action != app::LotUi::Action::None) {
            lotAction(action);
        } else {
            lotView_.key(e.key, orbit_);
        }
    } else if (e.type == pf::Event::Type::MouseWheel && (inNeighborhood_ || inLot_) && !neighborhoodCovered()) {
        app::orbitEvent(orbit_, e);
    } else if (e.type == pf::Event::Type::MouseMotion || e.type == pf::Event::Type::MouseDown || e.type == pf::Event::Type::MouseUp) {
        mouseEvent(e);
    }
}

void Game::mouseEvent(const pf::Event& e) {
    float x = 0, y = 0;
    pf::mousePosition(x, y);
    if (e.type == pf::Event::Type::MouseDown && e.button == pf::MouseButton::Left) {
        down_ = true;
        downX_ = x;
        downY_ = y;
    } else if (e.type == pf::Event::Type::MouseUp && e.button == pf::MouseButton::Left) {
        down_ = false;
        // A click (not a drag) on the neighborhood enters the lot under it.
        // Not while a lot (or the last one's Sims) is still loading.
        if (inNeighborhood_ && !neighborhoodCovered() && !lotBuilding_.valid() && !loading(lotSimBuilding_) && std::abs(x - downX_) < 4 && std::abs(y - downY_) < 4 &&
            !screen_.over(static_cast<int>(x), static_cast<int>(y))) {
            lotClicked(x, y);
        }
    }
    if (inLot_ && freeCam_) {
        freeCameraMouse(e);
        return;
    }
    if (inLot_) {
        if (!quitBox_.isOpen()) {
            cameraDrag(e, x, y);
        }
        lotMouse(e, x, y);
        return;
    }
    if (inNeighborhood_ && !neighborhoodCovered()) {
        cameraDrag(e, x, y);
    }
    if (const auto clicked = screen_.mouse(static_cast<int>(x), static_cast<int>(y), down_, static_cast<double>(ts2::platform::ticksNs()) / 1e9)) {
        windowClicked(*clicked);
    }
}

void Game::windowClicked(int window) {
    if (const auto answer = quitBox_.clicked(screen_, window)) {
        // On the save question No quits too, and Yes saves first in the game. Saving isn't done yet.
        const bool quit = *answer == app::QuitBox::Answer::Yes || (*answer == app::QuitBox::Answer::No && quitBox_.asksSave());
        closeQuitBox();
        if (quit) {
            ts2::diag::step("quitting (quit box)");
            running_ = false;
        }
        return;
    }
    const std::uint32_t id = screen_.id(window);
    ts2::diag::info(std::format("clicked 0x{:08x}", id));
    if (inNeighborhood_ && !screen_.visible(menu_)) {
        // The chooser, but not while a lot is loading: it reads this neighborhood.
        const auto action = hoodUi_.clicked(window);
        if (action == app::NeighborhoodUi::Action::Quit) {
            openQuitBox();
        } else if (action == app::NeighborhoodUi::Action::MainMenu && !lotBuilding_.valid()) {
            showChooser();
        } else if (action == app::NeighborhoodUi::Action::EnterLot && !lotBuilding_.valid() && !loading(lotSimBuilding_)) {
            const std::uint32_t lot = hoodUi_.cardLot();
            hoodUi_.hideCard();
            enterLot(lot);
        }
    } else {
        menuClicked(window, id);
    }
}

void Game::showSprites(double seconds, bool scene) {
    for (const int t : screen_.takeChangedTextures()) {
        unsent_.insert(t);
    }
    auto shown = sprites(screen_, seconds);
    std::vector<int> send;
    for (const auto& sprite : shown) {
        if (unsent_.erase(sprite.texture)) {
            send.push_back(sprite.texture);
        }
    }
    constexpr std::size_t kSpare = 16;
    for (std::size_t i = 0; i < kSpare && !unsent_.empty(); ++i) {
        send.push_back(*unsent_.begin());
        unsent_.erase(unsent_.begin());
    }
    std::erase_if(send, [&](int t) { return screen_.textures()[static_cast<std::size_t>(t)].width == 0; });
    staged_.clear();
    staged_.reserve(send.size());
    std::vector<std::pair<int, const ts2::scene::Image*>> upload;
    for (const int t : send) {
        const auto& px = screen_.textures()[static_cast<std::size_t>(t)];
        staged_.push_back({px.width, px.height, px.rgba});
        upload.emplace_back(t, &staged_.back());
    }
    renderer_.setSprites(upload);
    renderer_.showSprites(std::move(shown), scene);
}

// A frame that took long is what a tester sees as a stutter: where it happened, at most one line
// every 10 seconds. The timing report goes to the log as often.
void Game::reportFrame(std::uint64_t now) {
    constexpr double kSlowFrameMs = 100;
    constexpr std::uint64_t kLogGapNs = 10'000'000'000;
    if (const double ms = static_cast<double>(now - lastFrame_) / 1e6; ms > kSlowFrameMs && (lastSlowFrame_ == 0 || now - lastSlowFrame_ > kLogGapNs)) {
        lastSlowFrame_ = now;
        const std::string where = inLot_ ? std::format("in lot {}", enteredLot_) : inNeighborhood_ ? "in the neighborhood" : "in the menu";
        ts2::diag::warning(std::format("slow frame: {:.0f} ms {}", ms, where));
    }
    if (ts2::diag::timingsOn() && now - lastTimings_ > kLogGapNs) {
        lastTimings_ = now;
        ts2::diag::info("timings:\n" + ts2::diag::takeTimings());
    }
}

// A background job's result once it's done. One that failed (a broken lot or neighborhood file)
// is reported and leaves everything as it was. When taking a picture, it ends the program instead.
template <typename T>
std::optional<T> Game::finished(std::future<T>& job, const char* what) {
    if (!job.valid() || loading(job)) {
        return std::nullopt;
    }
    try {
        return job.get();
    } catch (const std::exception& ex) {
        ts2::diag::error(std::format("couldn't load the {}: {}", what, ex.what()));
        if (!options_.shot.empty()) {
            throw;
        }
        return std::nullopt;
    }
}

// The picture the options ask for, of the neighborhood or lot just opened, after the clicks
// asked for. False while it waits for the Sims or for the lot to play on.
bool Game::takePicture() {
    if (inLot_ && !options_.uiClicks.empty() && !uiClicked_) {
        uiClicked_ = true;
        clickPanels();
        showSprites(static_cast<double>(ts2::platform::ticksNs()) / 1e9, true);
    }
    // The ground click waits for the Sims to come to life.
    if (inLot_ && options_.groundClick && !groundClicked_ && !lotLife_) {
        return false;
    }
    if (inLot_ && options_.groundClick && !groundClicked_) {
        groundClicked_ = true;
        const auto [gx, gy, choice] = *options_.groundClick;
        if (openGroundMenu(static_cast<float>(gx), static_cast<float>(gy)) && choice >= 0) {
            if (static_cast<std::size_t>(choice) < pieMenu_.choices().size()) {
                lotLife_->choose(pieObject_, pieMenu_.choices()[static_cast<std::size_t>(choice)]);
            }
            pieMenu_.close();
        }
        showSprites(static_cast<double>(ts2::platform::ticksNs()) / 1e9, true);
    }
    if (inLot_ && options_.runSeconds > 0) {
        if (!runStarted_) {
            runStarted_ = ts2::platform::ticksNs();
        }
        if (secondsSince(*runStarted_) < options_.runSeconds) {
            return false;
        }
        // The mouse rests where it was asked to again, as if for a second, so tips of what the
        // lot shows by now are in the picture.
        const double nowSeconds = static_cast<double>(ts2::platform::ticksNs()) / 1e9;
        for (const auto& [cx, cy, kind] : options_.uiClicks) {
            if (kind == 2) {
                screen_.mouse(cx, cy, false, nowSeconds - 1);
                lotUi_.mouseMoved(cx, cy);
            }
        }
        showSprites(nowSeconds, true);
    }
    if (options_.view) {
        orbit_.yaw = orbit_.yawGoal = (*options_.view)[0];
        orbit_.pitch = (*options_.view)[1];
        orbit_.distance = (*options_.view)[2];
        orbit_.previous.reset();  // drawn exactly there, not blended from the last step
    }
    // The water's ripples and the plumbobs move with time: a picture shows them at time 0, so it repeats.
    renderer_.setTime(0);
    if (inNeighborhood_) {
        placePlumbobs(hoodShownAt_);
    }
    if (!renderer_.drawToFile(orbit_.camera(), static_cast<unsigned>(width_), static_cast<unsigned>(height_), options_.shot)) {
        throw std::runtime_error("couldn't save the picture");
    }
    ts2::diag::info("wrote " + ts2::diag::utf8(options_.shot));
    return true;
}

// The clicks on the lot's panels the options ask for before the picture.
void Game::clickPanels() {
    for (const auto& [cx, cy, right] : options_.uiClicks) {
        if (right == 2) {
            screen_.mouse(cx, cy, false, 0);
            lotUi_.mouseMoved(cx, cy);
        } else if (right != 0) {
            if (const auto button = screen_.buttonAt(cx, cy); button && lotLife_) {
                lotAction(lotUi_.rightClicked(*button, *lotLife_));
            }
        } else {
            screen_.mouse(cx, cy, true, 0);
            if (const auto clicked = screen_.mouse(cx, cy, false, 0)) {
                lotAction(lotUi_.clicked(*clicked));
            }
            if (lotLife_) {
                lotUi_.mouseButton(cx, cy, true, *lotLife_);
                lotUi_.mouseButton(cx, cy, false, *lotLife_);
            }
        }
        if (lotLife_) {
            lotUi_.showLife(*lotLife_);
        }
    }
}

} // namespace

int runMenu(const std::filesystem::path& install, const std::filesystem::path& shaders, const MenuOptions& options) {
    ts2::diag::info("loading the game's UI");
    ts2::content::UiData data(install);
    ts2::ui::Fonts fonts(install);
    ts2::diag::info(std::format("{} UI and text packages, {} font faces", data.packageCount(), fonts.faceCount()));

    const int width = options.width, height = options.height;
    pf::WindowOptions windowOptions;
    windowOptions.title = "NeoTS2";
    windowOptions.width = width;
    windowOptions.height = height;
    windowOptions.resizable = false;
    windowOptions.highDensity = false;
    windowOptions.hidden = !options.shot.empty();
    std::unique_ptr<pf::Window> window;
    {
        const auto rendererOwner = app::openRenderer(window, options.backend, windowOptions, shaders, !options.shot.empty());
        ts2::render::IRenderer& renderer = *rendererOwner;
        // The window can come smaller than asked (SDL 1.2 keeps it on the screen): the game is laid out for the one it has.
        MenuOptions sized = options;
        int actualWidth = 0, actualHeight = 0;
        window->size(actualWidth, actualHeight);
        if (actualWidth > 0 && actualHeight > 0) {
            sized.width = actualWidth;
            sized.height = actualHeight;
        }
        renderer.setBumpMapping(options.bumpMapping);
        renderer.setFloorAndWallNormalMapping(options.wallBumps);
        ts2::diag::info(std::format("drawing with {} in a {} x {} window", renderer.device(), sized.width, sized.height));
        ts2::diag::setTimings(options.timings);
        // What shows around the menu: the game's dark blue (as the game shows it).
        renderer.setClearColor(1 / 255.f, 35 / 255.f, 62 / 255.f);
        {
            Game game(data, fonts, renderer, install, sized);
            game.run();
            // Loads still running are waited for as the game goes. The window goes first so that
            // quitting doesn't look frozen meanwhile. Normal closing takes a second or two.
            window->hide();
            app::crash::watchClosing(kMaxClosingSeconds);
            if (ts2::diag::timingsOn()) {
                ts2::diag::info("timings:\n" + ts2::diag::takeTimings());
            }
        }
    }
    return 0;
}
