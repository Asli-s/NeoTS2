// The game's main menu (the neighborhood chooser), built from its UI script, and the neighborhood
// it opens.
#include "menu.hpp"
#include "baby_name_dialog.hpp"
#include "cheat_console.hpp"
#include "dialog_box.hpp"
#include "diagnostics.hpp"
#include "paths/game_folders.hpp"
#include "camera_height.hpp"
#include "crash_handler.hpp"
#include "click_gate.hpp"
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
#include "render/frame_plan.hpp"
#include "scene/object_effects.hpp"
#include "content/install.hpp"
#include "content/lighting_state.hpp"
#include "content/lot.hpp"
#include "diag/log.hpp"
#include "diag/timing.hpp"
#include "content/material_scripts.hpp"
#include "content/neighborhood.hpp"
#include "content/ui_data.hpp"
#include "content/ui_meters.hpp"
#include "content/ui_text.hpp"
#include "content/user_props.hpp"
#include "formats/image.hpp"
#include "formats/reia.hpp"
#include "render/make_renderer.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/lot_scene.hpp"
#include "scene/lot_skirt.hpp"
#include "scene/pick.hpp"
#include "scene/pool_water.hpp"
#include "scene/prop_fade.hpp"
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
#include <fstream>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>

namespace pf = ts2::platform;

namespace {

// Closing waits for a lot or neighborhood still loading, which takes a few seconds at most.
constexpr unsigned kMaxClosingSeconds = 30;

constexpr std::uint32_t kScripts = 0xA99D8A11;
constexpr std::size_t kLifeStage = 0x3A, kAgeDaysLeft = 0x5B;  // person data
// Live text: a new baby's first name until it is named, a boy's and a girl's.
constexpr std::uint32_t kLiveText = 0x7F96C284, kBabyBoyText = 0x00950001, kBabyGirlText = 0x00950002;  // string ids, leakcheck: ok
// The Live text's name tables Make New Character names its Sims from: human male and female first
// names and surnames, then the pets'.
constexpr std::uint16_t kNameTables[] = {0x96, 0x97, 0x98, 0xCC, 0xEA, 0xEB, 0xEC, 0xED};
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
// Kept between frames so the list reuses its storage.
const std::vector<std::int16_t>& simsInView(const ts2::scene::Scene& scene, const ts2::render::Camera& c, int width, int height) {
    using ts2::render::Vec3;
    thread_local std::vector<std::int16_t> seen;
    seen.clear();
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

// Where a point of the scene shows on the screen, none behind the camera.
std::optional<std::array<int, 2>> screenPoint(const ts2::render::Camera& c, int width, int height, const ts2::render::Vec3& point) {
    using ts2::render::Vec3;
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float tx = c.fovX > 0 ? std::tan(c.fovX / 2) : std::tan(c.fovY / 2) * aspect;
    const float ty = tx / aspect;
    const Vec3 f = ts2::render::normalize(ts2::render::sub(c.target, c.eye));
    const Vec3 s = ts2::render::normalize(ts2::render::cross(f, {0, 0, 1}));
    const Vec3 u = ts2::render::cross(s, f);
    const Vec3 p = ts2::render::sub(point, c.eye);
    const float z = ts2::render::dot(p, f);
    if (!(z > 0) || !(tx > 0) || !(ty > 0)) {
        return std::nullopt;
    }
    const float nx = ts2::render::dot(p, s) / (z * tx), ny = ts2::render::dot(p, u) / (z * ty);
    const float x = (nx + 1) * static_cast<float>(width) / 2, y = (1 - ny) * static_cast<float>(height) / 2;
    // Far off the screen only for a broken camera, kept to what an int holds.
    if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1e6f || std::abs(y) > 1e6f) {
        return std::nullopt;
    }
    return std::array<int, 2>{static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))};
}

// The live-mode cursor for what is under the mouse: a Sim or an object the selected Sim can use,
// the ground (floor or terrain), pool water (the game's water check gives the usable cursor), or
// anything else.
std::uint32_t liveCursor(const ts2::scene::Instance* under, bool usable, bool sim) {
    if (usable) {
        return sim ? app::Cursors::kLivePerson : app::Cursors::kLiveObjectAvailable;
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

// The UI's scale in a window: `percent` / 100, made smaller when needed so the UI keeps a screen of
// at least 800 x 600 of its pixels, the smallest the game lays it out for (its Low screen resolution).
// Never below 1.
float uiScaleFor(int percent, int width, int height) {
    const float fit = std::min(static_cast<float>(width) / 800, static_cast<float>(height) / 600);
    const float scale = std::max(1.0f, std::min(static_cast<float>(percent) / 100, fit));
    if (scale * 100 < static_cast<float>(percent)) {
        ts2::diag::info(std::format("the UI is drawn at {:.0f}% to fit the window", scale * 100));
    }
    return scale;
}

// One side of a quad enlarged to window pixels: its edges a0..a1 and texture coordinates t0..t1.
// An outer edge is rounded out to a whole pixel, so a picture laid over another covers every pixel
// it touches and the one below never shows what it hides there. An edge shared with another piece
// of the same picture is rounded to the nearest pixel, so the pieces neither overlap nor part. The
// texture coordinates follow, so the picture stays where it was.
void snapSide(float& a0, float& a1, float& t0, float& t1, bool joined0, bool joined1) {
    const float b0 = joined0 ? std::round(a0) : std::floor(a0), b1 = joined1 ? std::round(a1) : std::ceil(a1);
    if (a1 > a0) {
        const float perPixel = (t1 - t0) / (a1 - a0);
        t0 += (b0 - a0) * perPixel;
        t1 += (b1 - a1) * perPixel;
    }
    a0 = b0;
    a1 = b1;
}

// The UI's quads as sprites in window pixels, `scale` times their size in UI pixels, into `out`.
void sprites(const ts2::ui::Screen& screen, double seconds, float scale, std::vector<ts2::render::Sprite>& out) {
    // Kept between frames so it reuses its storage.
    thread_local std::vector<ts2::ui::Quad> quads;
    screen.quads(seconds, quads);
    out.clear();
    for (const auto& q : quads) {
        ts2::render::Sprite s;
        s.x0 = q.x0 * scale;
        s.y0 = q.y0 * scale;
        s.x1 = q.x1 * scale;
        s.y1 = q.y1 * scale;
        s.u0 = q.u0;
        s.v0 = q.v0;
        s.u1 = q.u1;
        s.v1 = q.v1;
        if (scale > 1) {
            snapSide(s.x0, s.x1, s.u0, s.u1, q.joined & 1, q.joined & 4);
            snapSide(s.y0, s.y1, s.v0, s.v1, q.joined & 2, q.joined & 8);
        }
        s.texture = q.texture;
        for (int k = 0; k < 4; ++k) {
            s.color[k] = q.color[static_cast<std::size_t>(k)];
        }
        out.push_back(s);
    }
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

// The ground around the lot `margin` tiles out, and that area.
std::pair<ts2::scene::Scene, ts2::scene::SkirtArea> makeSkirt(const SkirtSource& s, int margin) {
    const auto area = ts2::scene::lotSkirtArea(*s.hood->terrain, s.footprint, margin);
    ts2::scene::SceneBuilder builder(*s.assets);
    builder.setPoolStyleSea(s.poolStyleSea);
    builder.setSmoothLotSkirts(s.smoothLotSkirts);
    ts2::scene::addLotSkirt(builder, *s.hood->terrain, s.footprint, margin, s.terrainType, s.roadTextures, s.lotHeights.get(), s.groundLevel,
                            s.worldBounds ? &*s.worldBounds : nullptr, &s.groundLook);
    return {std::move(builder.scene()), area};
}

// A lot's surroundings built again at another View Distance: the neighborhood's decorations (the
// scene's first `decorations` instances) and the skirt, with the imposters and boundary of that area.
struct SurroundingsBuilt {
    int margin = 0;
    ts2::scene::Scene scene;
    std::size_t decorations = 0;
};

// A neighbor's house built for lot view, with the reader of the game's files its builds use, as the
// one the game runs on is used on the main thread meanwhile.
struct NeighborBuilt {
    std::uint32_t id = 0;
    ts2::scene::Scene scene;
    std::shared_ptr<ts2::scene::Assets> assets;
    bool failed = false;  // its imposter stays, and it isn't tried again
};

// Builds a neighbor's lot as it looks from outside. It is lit once by day, as a played lot is by day so that
// its ground meets the ground around it, keeps what shows with its walls up and its roofs on, and takes no
// part in the lot's views.
ts2::scene::Scene neighborHouse(ts2::scene::Assets& assets, const std::filesystem::path& file) {
    using namespace ts2::scene;
    SceneBuilder builder(assets);
    const auto lot = ts2::content::loadLot(file);
    addLot(builder, lot, file);
    app::lightLot(assets, builder.scene(), lot, file, "day");
    Scene scene = std::move(builder.scene());
    View view;
    view.level = topLevel(scene);
    view.walls = WallView::Up;
    const auto visible = visibleInstances(scene, view);
    std::vector<Instance> kept;
    kept.reserve(scene.instances.size());
    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        if (visible[i]) {
            Instance& instance = scene.instances[i];
            instance.part = Part::Other;
            instance.underside = Instance{}.underside;
            instance.walls.clear();
            instance.neighborHouse = true;
            kept.push_back(std::move(instance));
        }
    }
    scene.instances = std::move(kept);
    scene.walls.clear();
    scene.actors.clear();
    scene.placedObjects.clear();
    scene.clip.reset();
    return scene;
}

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
    // Shows the baby naming dialog when the lot's scripts ask for names, and closes it when answered.
    void updateBabyNames();
    // Shows the game's "Object Error" dialog for the oldest script error the lot holds for it.
    void updateObjectErrors();
    void answerObjectError(std::uint32_t button);
    void answerBabyNames();
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
    app::LotUi::Action lotKey(pf::Key key);
    void lotAction(app::LotUi::Action action);
    void trackSim(std::int16_t sim);
    bool centreOnSim(std::int16_t sim);
    void showSimStorey(const ts2::sim::SimObject& object);
    void followSimStorey();
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
    void puckCamera();
    void startLotLife();
    void drawHousehold(std::uint64_t now);
    void sendLotChanges();
    // The lot's instances shown this frame: those its view shows, less the neighborhood's
    // decorations when the Decorations option is off.
    std::vector<std::uint8_t> lotShown() const;
    // Shows or hides the neighbors' lots and the neighborhood's decorations around the lot at once,
    // as the game does when a Lot View Option changes.
    void applyLotViewOptions();
    void startNeighborHouses();
    void applyNeighborHouses();
    void showNeighborHouses();
    void buildNextNeighborHouse();
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
    bool blockedWhileLoading(const pf::Event& e);
    void tickGraphicsOptions(std::uint64_t ms);
    void applyGraphicsOptions();
    void applyEffectOptions();
    void applyDisplayOptions();
    void takeWindowSize();
    void paceFrame();
    app::GraphicsOptionsPanel& openGraphicsOptions() { return inLot_ ? lotUi_.graphicsOptions() : hoodUi_.graphicsOptions(); }
    std::uint64_t frameDueNs_ = 0;  // when the next frame may start under a frame rate limit
    int frameLimit_ = 0;
    void startFromOptions();
    void handleEvent(const pf::Event& e);
    void mouseEvent(const pf::Event& e);
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
    int width_, height_;  // the window's, which the scene is drawn and picked at
    // The UI is laid out in pixels of its own, uiScale_ window pixels each, so it fills a screen
    // uiWidth_ x uiHeight_ of them (see MenuOptions::uiScale).
    float uiScale_;
    int uiWidth_, uiHeight_;
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
    app::BabyNameDialog babyNames_;  // the game's "Set Baby Name" dialog
    ts2::sim::CheatProperties cheats_;  // what the cheat window's boolProp set
    app::CheatConsole cheatConsole_{cheats_};
    bool mouseShift_ = false;  // Shift held at the last mouse move or press
    app::DialogBox errorBox_;  // the game's "Object Error" dialog
    std::optional<bool> pausedBeforeErrorBox_;  // the game stops while it is open
    std::optional<bool> pausedBeforeBabyNames_;  // the game pauses while it is open
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
        // The boxes of the neighborhood's props in the lot's coordinates, for the Fade Distance.
        // Worked out when first needed.
        std::vector<std::optional<ts2::scene::propfade::Box>> propBoxes;
        // The neighbors' lot imposters' boxes, by lot id, likewise.
        std::optional<std::map<std::uint32_t, ts2::scene::propfade::Box>> imposterBoxes;
    };
    std::map<std::uint32_t, float> imposterAlphas_;  // the neighbors' imposters drawn faded, by lot id
    std::map<int, Surroundings> surroundings_;
    std::vector<ts2::scene::PaintedTerrain::ImposterPart> impostersShown(const Surroundings& around) const;
    int margin_ = 0;
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
    std::future<NeighborBuilt> neighborBuilding_;
    std::shared_ptr<ts2::scene::Assets> neighborAssets_;
    app::GameOptionsPanel::LotView lotViewOptions_;  // the Lot View Options as they stand
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
    app::ClickGate loadingGate_;  // drops clicks while a scene loads (see loadingScene)
    std::uint64_t lastFrame_ = 0;
    std::uint64_t lastSlowFrame_ = 0, lastTimings_ = 0;  // 0: no slow frame yet
    // The slow steps of showing a lot (the lot, its simulation, the household's portraits) take a
    // frame each, so no frame waits for two. A picture being taken waits for all.
    bool stepTaken_ = false;
    // A picture's clicks are made once, then the lot plays on for --run seconds.
    bool uiClicked_ = false;
    bool groundClicked_ = false;
    bool lotHovered_ = false;
    std::uint8_t mouseHeld_ = 0;  // one bit for each MouseButton held
    std::optional<std::uint64_t> runStarted_;
};

Game::Game(ts2::content::UiData& data, ts2::ui::Fonts& fonts, ts2::render::IRenderer& renderer, const std::filesystem::path& install,
           const MenuOptions& options)
    : data_(data), renderer_(renderer), install_(install), options_(options), width_(options.width), height_(options.height),
      uiScale_(uiScaleFor(options.uiScale, width_, height_)), uiWidth_(static_cast<int>(static_cast<float>(width_) / uiScale_)),
      uiHeight_(static_cast<int>(static_cast<float>(height_) / uiScale_)), screen_(data, fonts), lotUi_(screen_, uiWidth_, uiHeight_),
      pieMenu_(screen_, data), hoodUi_(screen_, data, uiWidth_, uiHeight_) {
    setUpMenu();
    pickMenuSims();
    setUpTiles();
    showPage();
    assets_ = std::async(std::launch::async, [install] { return std::make_shared<ts2::scene::Assets>(install); }).share();
    // A neighborhood's preview, over the menu where the menu is: its name, description and
    // picture, and Play.
    preview_ = screen_.load(kScripts, kPreview, menuX_, menuY_);
    screen_.setVisible(preview_, false);
    sims1Turn_ = ts2::content::readUserProps(options.user).flag("rotateCameraSims1Mode").value_or(false);
    lotUi_.setFolders(data, install, options.user);
    hoodUi_.gameOptions().takeSettings(lotUi_.gameOptions());
    hoodUi_.graphicsOptions().takeSettings(lotUi_.graphicsOptions());
    applyGraphicsOptions();
    lotUi_.graphicsOptions().setWindowSize(width_, height_);
    hoodUi_.graphicsOptions().setWindowSize(width_, height_);
    frameLimit_ = lotUi_.graphicsOptions().display().frameLimit;
    renderer_.setWaitForScreen(frameLimit_ == app::GraphicsOptionsPanel::kWaitForScreen);
    const int mostSamples = renderer_.capabilities().mostSmoothEdgeSamples;
    lotUi_.graphicsOptions().setMostSmoothEdges(mostSamples);
    hoodUi_.graphicsOptions().setMostSmoothEdges(mostSamples);
    renderer_.setSmoothEdges(lotUi_.graphicsOptions().display().smoothEdges);
    renderer_.setAmbientOcclusion(ts2::content::kAmbientOcclusionStrengths[static_cast<std::size_t>(lotUi_.graphicsOptions().display().ambientOcclusion)]);
}

void Game::setUpMenu() {
    screen_.setSize(uiWidth_, uiHeight_);
    // Placed in the middle of the screen (as the game places it at 1920 x 1080).
    menu_ = screen_.load(kScripts, kMainMenu, 0, 0);
    if (menu_ < 0) {
        throw std::runtime_error("the main menu's UI script is missing");
    }
    menuX_ = (uiWidth_ - screen_.width(menu_)) / 2;
    menuY_ = (uiHeight_ - screen_.height(menu_)) / 2;
    screen_.move(menu_, menuX_, menuY_);
    // At start the blurred neighborhood picture shows behind the menu, in the middle of the
    // screen (as the game shows it).
    const int backdrop = screen_.find(menu_, kBackdrop);
    screen_.setVisible(backdrop, true);
    screen_.move(backdrop, (uiWidth_ - screen_.width(backdrop)) / 2 - menuX_, (uiHeight_ - screen_.height(backdrop)) / 2 - menuY_);
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
        if (const auto* file = effects.all()) {
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
    } else if (options_.bin == "gameoptions") {
        hoodUi_.showOptions(true);
        hoodUi_.gameOptions().show(true);
    } else if (options_.bin == "graphicsoptions") {
        hoodUi_.showOptions(true);
        hoodUi_.graphicsOptions().show(true);

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
    // The box takes the mouse, so a camera drag or turn under way ends here, as its button coming
    // up would never reach the camera.
    endCameraDrag();
    if (inNeighborhood_) {
        hoodUi_.showTag(0, 0, 0);
        cursors_.show(app::Cursors::kArrow);
    }
    orbit_.keysOn = false;
    // On a lot the game pauses while the box is open.
    if (inLot_ && lotLife_ && !quitBox_.isOpen()) {
        pausedBeforeQuitBox_ = lotLife_->paused();
        lotLife_->setPaused(true);
    }
    quitBox_.open(screen_, data_, uiWidth_, uiHeight_, askSave);
}

// The game asks for names when a baby is born, and pauses while it asks. OK puts the speed back as it
// was. Only human babies have their panel shown so far. For others the names stay as they are, as OK
// on the untouched boxes would leave them.
void Game::updateBabyNames() {
    constexpr std::size_t kAge = 0x3A, kGender = 0x41, kSpecies = 0xBA;
    constexpr std::int16_t kBaby = 1;
    auto& world = lotLife_->simulation().world();
    const std::vector<std::int16_t>& asked = world.namesAsked();
    if (asked.empty()) {
        babyNames_.close(screen_);
        return;
    }
    if (babyNames_.isOpen()) {
        return;
    }
    // The game picks the panel by the first Sim (temp 0) and whether a second (temp 1) is there.
    // One Sim older than a baby gets a rename panel, a pet a pet panel, a robot the robot panel.
    // Anyone else gets the baby panel, or the twins panel with a second Sim. Only the panel's
    // boxes are named, the other Sims asked keep their names.
    const std::int16_t first = asked[0], second = asked.size() > 1 ? asked[1] : std::int16_t{0};
    const bool onlyFirst = std::all_of(asked.begin() + 1, asked.end(), [](std::int16_t id) { return id == 0; });
    const bool rename = first != 0 && onlyFirst && world.neighborPersonValue(first, kAge) > kBaby;
    const bool pet = first != 0 && world.neighborPersonValue(first, kSpecies) != 0;
    std::vector<app::BabyNameDialog::Baby> babies;
    for (const std::int16_t id : {first, second}) {
        if (id != 0) {
            const auto name = lotLife_->neighborName(id);
            babies.push_back({world.neighborPersonValue(id, kGender) == 1, name ? name->first : std::string{}});
        }
    }
    // Robots aren't told apart yet and get the baby panels.
    if (first != 0 && !rename && !pet && babyNames_.open(screen_, uiWidth_, uiHeight_, babies)) {
        endCameraDrag();  // the dialog takes the mouse, as the quit box does
        pausedBeforeBabyNames_ = lotLife_->paused();
        lotLife_->setPaused(true);
    } else {
        ts2::diag::warnOnce("baby names: only the baby and twins panels are shown so far, other names are kept");
        world.answerNames(true, {});
    }
}

namespace {
// The error dialog's buttons, in the game's order.
constexpr std::uint32_t kErrorCancel = 1, kErrorReset = 2, kErrorDelete = 3;
} // namespace

void Game::updateObjectErrors() {
    if (!lotLife_ || errorBox_.isOpen() || quitBox_.isOpen() || babyNames_.isOpen()) {
        return;
    }
    auto& sim = lotLife_->simulation();
    if (sim.objectErrors().empty()) {
        return;
    }
    const auto& error = sim.objectErrors().front();
    const auto* object = sim.world().find(error.object);
    const std::string name = object != nullptr ? object->definition.name : std::string("?");
    // The stack trace goes where the game puts its own, the Logs folder of the user folder, named alike.
    // Its lines are the engine's own: the game's file also dumps every global and object.
    const auto logs = openFolder_.parent_path().parent_path() / "Logs";
    const auto file = logs / std::format("ObjectError_{}_t{}.txt", ts2::diag::utf8(openFolder_.filename()), error.tick);
    std::ofstream out;
    if (std::error_code made; !openFolder_.empty() && (std::filesystem::create_directories(logs, made), !made)) {
        out.open(file);
    }
    out << "Build: " << app::buildVersion() << "\n\nObject id: " << error.object << "\nname: " << name << "\nError: " << error.text << "\n";
    // An error from a tree another object ran on it carries only that error's text.
    if (error.run.group != 0) {
        const auto tree = sim.code().behavior(error.run.group, error.run.instance);
        out << "Tree: id " << error.run.instance << " name '" << (tree ? tree->name : std::string()) << "' group "
            << std::format("0x{:08X}", error.run.group) << "\nNode: " << error.run.pc << "\n";
    }
    if (!error.run.calls.empty()) {
        out << "Stack from the bottom: " << error.run.calls << "\n";
    }
    const bool saved = out.is_open() && out.good();
    out.close();
    // The game's text: a first line from its program, the error, then where the trace went.
    std::string body = std::format("An error occurred in object \"{}\" #{}, \nError: {}\n", name, error.object, error.text);
    // The game writes the path with forward slashes.
    std::string path = ts2::diag::utf8(file);
    std::ranges::replace(path, '\\', '/');
    body += saved ? std::format(" The stack trace is saved in \"{}\".", path) : std::string(" The stack trace could not be saved.");
    errorBox_.open(screen_, uiWidth_, uiHeight_, "Object Error", body, {{kErrorCancel, "Cancel"}, {kErrorReset, "Reset"}, {kErrorDelete, "Delete"}});
    endCameraDrag();
    pausedBeforeErrorBox_ = lotLife_->paused();
    lotLife_->setPaused(true);
}

void Game::answerObjectError(std::uint32_t button) {
    using Answer = ts2::sim::LotSimulation::ErrorAnswer;
    if (lotLife_) {
        lotLife_->simulation().answerObjectError(button == kErrorReset ? Answer::Reset : button == kErrorDelete ? Answer::Delete : Answer::Cancel);
        if (pausedBeforeErrorBox_) {
            lotLife_->setPaused(*pausedBeforeErrorBox_);
        }
    }
    pausedBeforeErrorBox_.reset();
    errorBox_.close(screen_);
}

void Game::answerBabyNames() {
    if (lotLife_) {
        lotLife_->simulation().world().answerNames(true, babyNames_.names());
        if (pausedBeforeBabyNames_) {
            lotLife_->setPaused(*pausedBeforeBabyNames_);
        }
    }
    pausedBeforeBabyNames_.reset();
    babyNames_.close(screen_);
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
    if (const auto [ux, uy] = toUi(x, y); !down_ && !screen_.over(ux, uy)) {
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
    const auto [tagX, tagY] = toUi(at[0], at[1]);
    hoodUi_.showTag(lot, tagX, tagY);
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
    hoodUi_.showCard(lot, toUi(at[0], at[1])[0]);
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
    const app::GameOptionsPanel::LotView lotViewOptions = lotUi_.gameOptions().lotView();
    lotBuilding_ = std::async(std::launch::async, [assets = assets_, file, install = install_, footprint, hoodScene, terrainType, &renderer = renderer_,
                                                 lotViewOptions,
                                                 poolStylePonds = options_.poolStylePonds, poolStyleSea = options_.poolStyleSea,
                                                 smoothLotSkirts = options_.smoothLotSkirts] {
        LotBuilt built;
        // The lot's ground heights, which the skirt meets at the lot's edges.
        std::shared_ptr<const ts2::scene::Heights> lotHeights;
        int groundLevel = 0;
        std::optional<ts2::content::WorldBounds> worldBounds;  // for the beach the skirt's sea covers
        ts2::scene::LotGroundLook groundLook;                  // blended onto the skirt with smooth lot skirts
        {
            ts2::scene::SceneBuilder builder(*assets.get());
            builder.setPoolStylePonds(poolStylePonds);
            builder.setSmoothLotSkirts(smoothLotSkirts);
            if (footprint && hoodScene->terrain) {
                float sea[3] = {0, 0, hoodScene->terrain->waterLevel};
                footprint->place.rigidInverse().apply(sea, true);
                builder.setLotSea(sea[2]);
            }
            const auto lot = ts2::content::loadLot(file);
            worldBounds = lot.worldBounds;
            if (smoothLotSkirts) {
                groundLook = ts2::scene::lotGroundLook(lot);
            }
            if (const ts2::scene::Heights heights(lot); heights.grid() != nullptr) {
                lotHeights = std::make_shared<const ts2::scene::Heights>(std::make_shared<const ts2::array3d::Array3D>(*heights.grid()));
                groundLevel = ts2::content::groundGridLevel(lot);
            }
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
            built.camera.lotKeys = true;
        }
        built.freeRig = ts2::content::findCamera(install, "CameraMan");
        // Shown in its neighborhood, where its imposter was.
        if (footprint) {
            ts2::scene::Scene skirt;
            ts2::scene::SkirtArea area;
            if (hoodScene->terrain) {
                auto source = std::make_shared<SkirtSource>();
                source->assets = assets.get();
                source->hood = hoodScene;
                source->footprint = *footprint;
                source->terrainType = terrainType;
                source->roadTextures = ts2::content::lotSkirtRoadTextures(ts2::content::findProducts(install));
                source->lotHeights = lotHeights;
                source->groundLevel = groundLevel;
                source->worldBounds = worldBounds;
                source->groundLook = groundLook;
                source->poolStyleSea = poolStyleSea;
                source->smoothLotSkirts = smoothLotSkirts;
                source->defaultMargin = app::lotSkirtMargin(install);
                built.margin = lotViewOptions.skirtMargin.value_or(source->defaultMargin);
                std::tie(skirt, area) = makeSkirt(*source, built.margin);
                built.skirt = std::move(source);
            }
            built.lot = app::lotInNeighborhood(*hoodScene, std::move(built.lot), std::move(skirt), *footprint, area, &built.decorations,
                                               &built.surroundings);
            ts2::scene::lightLotSkirt(built.lot, ts2::content::stateTime(built.lightState) != ts2::content::StateTime::Night);
            app::placeCamera(built.camera, *footprint);
            built.place = footprint->place;
            built.camera.minFar = 2000;
        }
        ts2::scene::poolwater::lightPoolWater(built.lot, ts2::content::stateTime(built.lightState) != ts2::content::StateTime::Night);
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
        lotDaylight_ = std::make_unique<app::LotDaylight>(std::move(built.light), built.lightState, environment, built.place);
    }
    surroundings_.clear();
    skirtSource_ = built.skirt;
    margin_ = built.margin;
    if (skirtSource_) {
        Surroundings& first = surroundings_[margin_];
        first.count = built.surroundings;
        first.decorations = built.decorations;
        if (lot_.terrain) {
            first.imposters = lot_.terrain->imposters;
        }
        first.clip = lot_.clip;
    }
    applyLotViewOptions();
    hoodOrbit_ = orbit_;
    // Toggle Ceiling stays as the player left it on the next lot, as the game keeps it in its properties.
    const bool ceilings = lotView_.view.ceilings;
    lotView_ = built.view;
    lotView_.view.ceilings = ceilings;
    orbit_ = built.camera;
    orbit_.smoothFollow = options_.smoothFollow;
    orbit_.goal = orbit_.target;
    orbit_.useRigEasing();
    cameraHeight_ = {};
    lotPlace_ = built.place;
    applyNeighborHouses();
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
    hoveredSim_ = false;
    spotTipObject_ = 0;
    spotTipShown_.reset();
    screen_.setSpotTip({});
    lotShown_ = ts2::platform::ticksNs();
    lotRealSeconds_ = 0;
    ts2::diag::info(std::format("lot {} shown after {:.1f} s", enteredLot_, secondsSince(lotLoadStarted_)));

    screen_.setVisible(hoodUi_.root(), false);
    hoodUi_.showOptions(false);
    hoodUi_.showBin(app::NeighborhoodUi::Bin::None);
    hoodUi_.showTag(0, 0, 0);
    lotUi_.setTestingCheats(cheats_.testingCheats());
    lotUi_.open();
    inNeighborhood_ = false;
    inLot_ = true;
}

// Back to the neighborhood, as it was.
void Game::leaveLot() {
    ts2::diag::step("back to the neighborhood");
    endCameraDrag();
    inLot_ = false;
    if (surroundingsBuilding_.valid()) {
        surroundingsBuilding_.wait();
        surroundingsBuilding_ = {};
    }
    neighborsToBuild_.clear();
    if (neighborBuilding_.valid()) {
        neighborBuilding_.wait();
        neighborBuilding_ = {};
    }
    neighborHouses_.clear();
    neighborGrounds_.clear();
    renderer_.setNeighborGrounds(neighborGrounds_);
    surroundings_.clear();
    skirtSource_.reset();
    unfadeImposters();
    // Freeing all of the lot at once took a fifth of a second.
    lotLeft_ = renderer_.swap(std::move(hood_.gpu));
    stopLotEffects();
    lotFreeing_ = std::async(std::launch::async, [scene = std::move(lot_), life = std::move(lotLife_), daylight = std::move(lotDaylight_), neighborAssets = std::move(neighborAssets_)]() mutable {
        daylight.reset();
        scene = {};
        life.reset();
        neighborAssets.reset();
    });
    neighborAssets_.reset();
    lot_ = {};
    // Poses are kept by object id, and the next lot reuses ids.
    lotPose_ = {};
    lotRouteModels_.reset();
    if (freeCam_) {
        freeCam_.reset();
        freeMouse_.reset();
        lotUi_.hideForFreeCamera(false);
        pf::showCursor();
    }
    lotUi_.close();
    pieMenu_.close();
    babyNames_.close(screen_);
    pausedBeforeBabyNames_.reset();
    hovered_ = 0;
    hoveredSim_ = false;
    spotTipObject_ = 0;
    spotTipShown_.reset();
    screen_.setSpotTip({});
    renderer_.setHighlight(0, 0);
    renderer_.setImposterLight({1, 1, 1});
    renderer_.setImpostersShown(true);
    orbit_ = hoodOrbit_;
    orbit_.dragSpeed = {0, 0};
    screen_.setVisible(hoodUi_.root(), true);
    inNeighborhood_ = true;
}

// The game's live-mode keys. P, Pause, ` or 0 on the number pad pause. 1, 2 and 3 pick a
// speed. F5 opens the options. Q asks to quit, as in most of the game's modes.
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
    case pf::Key::Q: return Action::Quit;
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
        lotView_.stepLevel(action == Action::FloorUp ? 1 : -1, orbit_);
        return;
    }
    if (action == Action::WallMode) {
        lotView_.setWallMode(lotUi_.pickedWallMode());
        return;
    }
    if (action == Action::ToggleCeiling) {
        lotView_.view.ceilings = !lotView_.view.ceilings;
        ts2::diag::step(lotView_.view.ceilings ? "view: ceilings on" : "view: ceilings off");
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
    case Action::GraphicsOptionChanged:
        hoodUi_.graphicsOptions().takeSettings(lotUi_.graphicsOptions());
        applyGraphicsOptions();
        applyDisplayOptions();
        break;
    // The game turns the camera the new way the moment Camera Rotation is picked.
    case Action::GameOptionChanged:
        sims1Turn_ = lotUi_.gameOptions().flag("rotateCameraSims1Mode", sims1Turn_);
        hoodUi_.gameOptions().takeSettings(lotUi_.gameOptions());
        applyLotViewOptions();
        applyNeighborHouses();
        renderer_.show(lotShown());
        if (const auto freeWill = lotUi_.takeFreeWill(); freeWill && lotLife_) {
            lotLife_->simulation().globals()[ts2::sim::global::kFreeWill] = *freeWill ? 1 : 0;
        }
        break;
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
// isn't on it. The view first changes storey to the Sim's (see showSimStorey).
bool Game::centreOnSim(std::int16_t sim) {
    const auto root = simRoot(sim);
    if (!root) {
        return false;
    }
    if (const auto* object = lotLife_ ? lotLife_->simulation().world().find(sim) : nullptr) {
        showSimStorey(*object);
    }
    orbit_.centreOn(*root);
    return true;
}

// The view's storey for a Sim the camera centres on or tracks, as the game picks it: a Sim indoors
// gets its storey either way, a Sim outside only when the view is below it. So a tracked Sim walking
// down from a deck to the garden leaves the view on the deck's storey.
void Game::showSimStorey(const ts2::sim::SimObject& object) {
    if (object.level <= 0 || object.x == ts2::sim::kOffLot || object.y == ts2::sim::kOffLot) {
        return;
    }
    const int storey = lotView_.ground + object.level - 1;
    // Positions are kept in 1/65536 tiles.
    const float x = static_cast<float>(object.x) / 65536, y = static_cast<float>(object.y) / 65536;
    // Outside as the room's "outside" field has it: a room its outside layer's walls don't enclose.
    // A fenced yard is a room of its own but still outside.
    const auto& rooms = lotLife_->simulation().world().rooms();
    const ts2::sim::RoomInfo* room = rooms.room(rooms.roomAtPosition(object.level - 1, x, y));
    const bool indoors = room != nullptr && room->layerRegion[0] > 0;
    if (indoors || lotView_.view.level < storey) {
        lotView_.changeLevel(storey - lotView_.view.level, orbit_);
    }
}

// While the camera tracks a Sim, the view keeps to the Sim's storey by the same rule as centring:
// it moves as soon as the Sim reaches another storey or walks indoors from a deck or the yard. A
// storey change by the player ends the tracking, so the rule never fights the player.
void Game::followSimStorey() {
    if (const auto* object = orbit_.subject && lotLife_ ? lotLife_->simulation().world().find(trackedSim_) : nullptr) {
        showSimStorey(*object);
    }
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
    const auto hit = lotPicker_.pick(lot_, lotVisible_, eye, ray, &lotPose_);
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
    const auto [ux, uy] = toUi(x, y);
    pieMenu_.open(std::move(ground.choices), ux, uy, uiWidth_, uiHeight_);
    return true;
}

// The right button held from a press on the lot or neighborhood drags the camera: a small square
// marks the press point, and once the mouse is more than 8 pixels from it (along either axis) the
// cursor is the turned drag arrow pointing from the press point to the mouse. Within those 8
// pixels it is the cursor for what is under the mouse.
void Game::cameraDrag(const pf::Event& e, float x, float y) {
    // The middle button turns the camera: the pointer hides and stays where it was pressed (the
    // game puts it back after every move), with the game's picture centred on that point.
    const auto [ux, uy] = toUi(x, y);
    if (e.type == pf::Event::Type::MouseDown && e.button == pf::MouseButton::Middle && !pieMenu_.isOpen() && !turnFrom_ && !screen_.over(ux, uy)) {
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
        screen_.move(turnMark_, ux - screen_.width(turnMark_) / 2, uy - screen_.height(turnMark_) / 2);
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
            orbit_.turnMove[0] += sims1Turn_ ? -dx : dx;
            orbit_.turnMove[1] += dy;
            if (pf::Window* window = pf::mouseFocus()) {
                window->warpMouse((*turnFrom_)[0], (*turnFrom_)[1]);
            }
        }
        return;
    }
    if (e.type == pf::Event::Type::MouseDown && e.button == pf::MouseButton::Right && !pieMenu_.isOpen() && !screen_.over(ux, uy)) {
        dragFrom_ = {x, y};
        screen_.remove(pressMark_);
        pressMark_ = screen_.load(kUiScripts, kPressMark, 0, 0);
        screen_.setPassive(pressMark_);
        screen_.move(pressMark_, ux - screen_.width(pressMark_) / 2, uy - screen_.height(pressMark_) / 2);
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
    hoveredSim_ = false;
    spotTipObject_ = 0;
    spotTipShown_.reset();
    screen_.setSpotTip({});
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
    const auto [ix, iy] = toUi(x, y);
    if (e.type == pf::Event::Type::MouseMotion || e.type == pf::Event::Type::MouseDown) {
        mouseShift_ = e.shift;
    }
    if (babyNames_.isOpen() && !quitBox_.isOpen()) {
        cursors_.show(app::Cursors::kArrow);
        if (e.type == pf::Event::Type::MouseDown && e.button == pf::MouseButton::Left) {
            babyNames_.mouseDown(screen_, ix, iy);
        } else if (e.type == pf::Event::Type::MouseUp && e.button == pf::MouseButton::Left) {
            babyNames_.mouseUp();
        } else if (e.type == pf::Event::Type::MouseMotion) {
            babyNames_.mouseMoved(screen_, ix, iy);
        }
        if (const auto clicked = screen_.mouse(ix, iy, down_, static_cast<double>(ts2::platform::ticksNs()) / 1e9);
            clicked && babyNames_.clickedOk(screen_, *clicked)) {
            answerBabyNames();
        }
        return;
    }
    if (quitBox_.isOpen() || errorBox_.isOpen()) {
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
        const std::uint32_t part = lotLife_->partAt(hovered_, hoveredPoint_, lot_);
        if (auto choices = lotLife_->pieMenu(part, mouseShift_); !choices.empty()) {
            pieObject_ = part;
            pieMenu_.open(std::move(choices), ix, iy, uiWidth_, uiHeight_);
            showPieCursor();
            return;
        }
    }
    // A left click on empty floor or ground opens the menu of a destination made there.
    if (lotLife_ && hovered_ == 0 && leftDown && !lotUi_.optionsShown() && !screen_.over(ix, iy) && openGroundMenu(x, y)) {
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

// The puck's camera buttons turn and zoom while held. Sims 1 Style swaps the arrows, as it does
// comma and period.
void Game::puckCamera() {
    using Button = app::LotUi::CameraButton;
    const Button held = lotUi_.heldCameraButton();
    const int turn = held == Button::RotateLeft ? -1 : held == Button::RotateRight ? 1 : 0;
    orbit_.puckTurn = sims1Turn_ ? -turn : turn;
    orbit_.puckZoom = held == Button::ZoomIn ? -1 : held == Button::ZoomOut ? 1 : 0;
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
        puckCamera();
        orbit_.keysTaken = cheatConsole_.takesKeys();
        orbit_.gamePixel = pf::firstWindowDisplayScale();
        app::orbitStep(orbit_, dt, width_, height_, [&] { cameraHeight_.apply(orbit_, lot_, lotVisible_, lotPlace_, lotView_.view.level); });
        lotView_.view.facing = orbit_.facing();
    }
    lotUi_.showTracked(orbit_.subject ? trackedSim_ : 0);
    {
        const auto eye = lotCamera().eye;
        float at[3] = {eye[0], eye[1], eye[2]};
        lotPlace_.rigidInverse().apply(at, true);
        lotView_.view.eyeHeight = at[2];
        fadeProps({at[0], at[1], at[2]});
    }
    const auto shown = lotShown();
    lotVisible_ = shown;
    renderer_.show(shown);
    lotUi_.showWallMode(lotView_.wallMode());
    lotUi_.showStoreyLimits(lotView_.canGoUp(), lotView_.canGoDown());
    lotUi_.showCeilings(lotView_.view.ceilings);
    // The game reads testingCheatsEnabled each time it checks a line, so a change shows at once.
    if (lotLife_) {
        lotLife_->setTestingCheats(cheats_.testingCheats());
    }
    // The options menu is not live mode: nothing on the lot is hovered or clicked and the cursor is
    // the arrow. Only the camera moves.
    if (lotUi_.optionsShown()) {
        hovered_ = 0;
        hoveredSim_ = false;
        liveCursor_ = app::Cursors::kArrow;
        if (hoverAt_) {
            cursors_.show(liveCursor_);
            hoverAt_.reset();
        }
        if (spotTipObject_ != 0) {
            spotTipObject_ = 0;
            screen_.setSpotTip({});
            spotTipShown_.reset();
        }
    }
    // The game looks for the object under the mouse as it moves with no button held.
    if (hoverAt_ && lotLife_ && !freeCam_) {
        const auto ray = screenRay(orbit_.camera(), width_, height_, (*hoverAt_)[0], (*hoverAt_)[1]);
        const auto hit = lotPicker_.pick(lot_, shown, orbit_.camera().eye, ray, &lotPose_);
        const std::uint32_t object = hit ? hit->object : 0;
        hovered_ = lotLife_->hoverable(object, mouseShift_) ? object : 0;
        if (hit) {
            const auto& eye = orbit_.camera().eye;
            hoveredPoint_ = {eye[0] + ray[0] * hit->distance, eye[1] + ray[1] * hit->distance, eye[2] + ray[2] * hit->distance};
        }
        hoveredSim_ = hovered_ != 0 && lotLife_->isSim(hovered_);
        liveCursor_ = liveCursor(hit ? &lot_.instances[hit->instance] : nullptr, hovered_ != 0, hoveredSim_);
        cursors_.show(liveCursor_);
        // A Sim the mouse comes to shows its name at once, pointing at where the ray hit it. Moving
        // over the same Sim changes nothing, and the tip goes when the mouse leaves it.
        if (object != spotTipObject_) {
            spotTipObject_ = object;
            spotTipPoint_ = hoveredPoint_;
            const bool sim = object != 0 && lotLife_->isSim(object);
            screen_.setSpotTip(sim ? lotUi_.fullName(*lotLife_, static_cast<std::int16_t>(object)) : std::string());
            spotTipShown_ = sim ? std::optional(now) : std::nullopt;
        }
        hoverAt_.reset();
    }
    // It hides 3 s after it showed, and shows again only once the mouse has been on something else.
    constexpr std::uint64_t kSpotTipTimeoutNs = 3'000'000'000;
    if (spotTipShown_ && (freeCam_ || now - *spotTipShown_ >= kSpotTipTimeoutNs)) {
        screen_.setSpotTip({});
        spotTipShown_.reset();
    }
    // It points at the same spot of the scene however the camera moves.
    if (spotTipShown_) {
        const auto at = screenPoint(orbit_.camera(), width_, height_, spotTipPoint_);
        if (at) {
            const auto [tipX, tipY] = toUi(static_cast<float>((*at)[0]), static_cast<float>((*at)[1]));
            screen_.moveSpotTip(tipX, tipY);
        } else {
            screen_.setSpotTip({});
            spotTipShown_.reset();
        }
    }
    renderer_.setHighlight(hoveredSim_ ? 0 : hovered_, ts2::scene::hoverAmbient(static_cast<double>(now) / 1e9));
    const bool shooting = !options_.shot.empty();
    if (!lotLife_ && (!stepTaken_ || shooting)) {
        startLotLife();
    }
    // Drawn once the lot's textures are on the GPU, as reading a picture back waits for them.
    if (lotLife_ && !householdShown_ && ((!stepTaken_ && renderer_.uploaded()) || shooting)) {
        drawHousehold(now);
    }
    // A Sim born or joining during play gets its portrait.
    if (lotLife_ && householdShown_ && lotLife_->household() != householdDrawn_) {
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
        updateBabyNames();
        updateObjectErrors();
        followSimStorey();
    }
    app::animateActors(lot_, lotLife_ ? lotLife_->seconds() : static_cast<double>(now - lotShown_) / 1e9, renderer_, lotPose_);
    updateLotEffects(dt);
    if (!lotLife_ || !lotLife_->paused()) {
        lotRealSeconds_ += std::min(0.1, static_cast<double>(now - lastFrame_) / 1e9);
    }
    renderer_.setTime(static_cast<float>(lotRealSeconds_));
    if (lotLife_) {
        sendLotChanges();
    }
}

ts2::scene::ObjectEffects::PlaceOf Game::effectPlaces() {
    // Objects count levels from 1 at the ground storey.
    return [this](std::int16_t object, const std::string& bone) { return lotLife_->effectPlace(lot_, lotPose_, object, bone, lotView_.view.level - lotView_.ground + 1); };
}

// Scripts start effects from the simulation's first tick on.
void Game::startLotEffects() {
    if (!effectsFiles_) {
        effectsFiles_ = std::make_unique<ts2::content::EffectsFiles>(install_);
    }
    const ts2::effects::File* file = effectsFiles_->all();
    if (!file) {
        return;
    }
    lotEffectPlayer_ = std::make_unique<ts2::scene::EffectPlayer>(*file, static_cast<std::uint32_t>(lastFrame_) | 1);
    lotEffects_ = std::make_unique<ts2::scene::ObjectEffects>(*lotEffectPlayer_);
    applyEffectOptions();
    // The sea's look is chosen as a lot opens, so a change waits for the next lot.
    renderer_.setSeaReflections(lotUi_.graphicsOptions().propOn("EffectsQuality", "enableOceanReflection"));
    lotEffectsSeconds_ = lotLife_->seconds();
    updateLotEffectConditions();
    lotEffectPlayer_->update(0, 0);  // takes the state at once, so the first effects are built for it
    lotLife_->simulation().world().setEffectCommands([this](const ts2::sim::EffectCommand& c) { doEffectCommand(c); });
}

void Game::doEffectCommand(const ts2::sim::EffectCommand& c) {
    using Do = ts2::sim::EffectCommand::Do;
    const auto kind = static_cast<ts2::scene::ObjectEffects::Kind>(c.kind);
    switch (c.what) {
    case Do::Start: {
        std::string picture;
        if (c.pictureNumber) {
            picture = makeEffectScore(*c.pictureNumber);
        } else if (c.pictureWant) {
            picture = makeEffectWant(*c.pictureWant);
        } else if (!c.picture.empty() && (c.pictureGuid == 0 || makeEffectPicture(c))) {
            picture = c.picture;
        }
        if (c.queued) {
            const auto now = static_cast<std::int64_t>(lotLife_->simulation().world().ticks());
            const std::int64_t expires = std::max<std::int64_t>(now + c.queueWait, 0);
            lotEffects_->startQueued(c.object, c.name, c.bone, c.number, c.keep, c.hard, c.queuePriority, static_cast<std::uint64_t>(expires), picture, c.firstFrame, effectPlaces());
            break;
        }
        lotEffects_->start(c.object, c.name, c.bone, c.number, kind, c.keep, c.hard, effectPlaces());
        if (!picture.empty()) {
            lotEffects_->setPicture(c.object, c.name, c.bone, c.number, picture, c.firstFrame);
        }
        break;
    }
    case Do::StartOnce: lotEffects_->startOnce(c.object, c.name, c.bone, effectPlaces()); break;
    case Do::Stop: lotEffects_->stop(c.object, c.name, c.bone, c.number, c.hard); break;
    case Do::StopAll:
        lotEffects_->stopAll(c.object, kind, c.hard);
        if (c.clearsQueue) {
            lotEffects_->clearQueue(c.object);
        }
        break;
    }
}

// A score's picture is made the first time that score is shown, and kept under its text. Gives the picture's name.
std::string Game::makeEffectScore(int score) {
    constexpr const char* kFont = "HeadlineEffectTextureText";
    const std::string text = (score < 0 ? "-" : "+") + ts2::content::groupedNumber(std::abs(score));
    std::string name = "HeadlineEffectTextTexture_" + text;
    if (!effectImages_.contains(name)) {
        auto& fonts = screen_.fonts();
        const auto& style = fonts.style(kFont);
        auto picture = ts2::content::headlineTextPicture(fonts.draw(style, text).pixels, fonts.lineHeight(style));
        effectImages_[name] = static_cast<int>(lot_.images.size());
        lot_.images.push_back({picture.width, picture.height, std::move(picture.rgba)});
        renderer_.addMaterials(lot_);
    }
    return name;
}

// A want's picture is made the first time it is shown and kept under its name, as an object's is. A Sim's portrait
// comes later. Gives the picture's name, empty when the want shows nothing.
std::string Game::makeEffectWant(const ts2::swaf::Want& want) {
    auto shown = lotUi_.balloonWant(*lotLife_, want);
    if (shown.name.empty() || effectImages_.contains(shown.name)) {
        return shown.name;
    }
    if (shown.neighbor != 0) {
        effectPortraits_[shown.name] = {shown.neighbor, std::move(shown.badge)};
        return shown.name;
    }
    auto picture = ts2::content::wantBalloonPicture(shown.picture, shown.badge, app::LotUi::kBalloonPictureSide);
    effectImages_[shown.name] = static_cast<int>(lot_.images.size());
    lot_.images.push_back({picture.width, picture.height, std::move(picture.rgba)});
    renderer_.addMaterials(lot_);
    return shown.name;
}

// A picture of an object or a Sim gets its image the first time it is asked for, and keeps it under its name. A Sim's
// portrait comes later, once its character file is read. False when there is no picture, so the effect keeps its own.
bool Game::makeEffectPicture(const ts2::sim::EffectCommand& c) {
    const auto known = effectImages_.find(c.picture);
    if (known != effectImages_.end()) {
        return known->second >= 0 || effectPortraits_.contains(c.picture);
    }
    if (c.pictureNeighbor != 0) {
        effectPortraits_[c.picture] = {c.pictureNeighbor, {}};
        return true;
    }
    int& image = effectImages_[c.picture];
    image = -1;
    if (auto picture = c.pictureIsMemory ? lotUi_.balloonMemoryPicture(*lotLife_, c.pictureGuid) : lotUi_.balloonObjectPicture(*lotLife_, c.pictureGuid, c.pictureIndex)) {
        image = static_cast<int>(lot_.images.size());
        lot_.images.push_back({picture->width, picture->height, std::move(picture->rgba)});
        renderer_.addMaterials(lot_);
    }
    return image >= 0;
}

void Game::stopLotEffects() {
    effectPortraits_.clear();
    effectVertices_.clear();
    effectBatches_.clear();
    renderer_.showParticles(effectVertices_, effectBatches_);
    lotEffects_.reset();
    lotEffectPlayer_.reset();
    effectImages_.clear();
}

// The level of detail goes with the camera's zoom, and the state bits say the time of day (bits 0 and 1). Bit 11 is
// the camera being outside a room, which it is taken to be for now. Bit 13 is the season's. The bits of
// weather are not set yet, so none of its effects play.
void Game::updateLotEffectConditions() {
    lotEffectPlayer_->setDetail(!freeCam_ && orbit_.rig ? ts2::camera::effectDetailAt(*orbit_.rig, orbit_.distance) : 1.0f);
    constexpr std::uint32_t kAlways = 0x800 | 0x2000;
    std::uint32_t time = 0x1;
    if (lotDaylight_) {
        using ts2::content::StateTime;
        switch (ts2::content::stateTime(lotDaylight_->state())) {
        case StateTime::Morning: time = 0x2; break;
        case StateTime::Day: time = 0x1; break;
        case StateTime::Evening: time = 0x3; break;
        case StateTime::Night: time = 0; break;
        }
    }
    lotEffectPlayer_->setState(kAlways | time);
}

// Effects follow their objects, play on and hand their sprites to the renderer.
void Game::updateLotEffects(float realSeconds) {
    if (!lotEffectPlayer_ || !lotLife_) {
        return;
    }
    updateLotEffectConditions();
    {
        const auto camera = lotCamera();
        const auto ahead = ts2::render::normalize(ts2::render::sub(camera.target, camera.eye));
        const auto beside = ts2::render::normalize(ts2::render::cross(ahead, {0, 0, 1}));
        const auto over = ts2::render::cross(beside, ahead);
        lotEffectPlayer_->setCamera({camera.eye[0], camera.eye[1], camera.eye[2]}, {ahead[0], ahead[1], ahead[2]}, {over[0], over[1], over[2]});
    }
    lotLife_->clipEffects(lot_, [this](const ts2::sim::EffectCommand& c) { doEffectCommand(c); });
    if (!lotLife_->paused()) {
        lotEffects_->advanceQueues(lotLife_->simulation().world().ticks(), effectPlaces());
    }
    lotEffects_->follow(effectPlaces());
    const double seconds = lotLife_->seconds();
    lotEffectPlayer_->update(static_cast<float>(std::clamp(seconds - lotEffectsSeconds_, 0.0, 1.0)), realSeconds);
    lotEffectsSeconds_ = seconds;

    const auto camera = lotCamera();
    const auto view = ts2::render::FrameView::make(camera, width_, height_);
    ts2::scene::ParticleView sprites;
    const auto towards = ts2::render::normalize(ts2::render::sub(camera.target, camera.eye));
    const auto side = ts2::render::normalize(ts2::render::cross(towards, {0, 0, 1}));
    sprites.forward = {towards[0], towards[1], towards[2]};
    sprites.right = {side[0], side[1], side[2]};
    const auto top = ts2::render::cross(side, towards);
    sprites.up = {top[0], top[1], top[2]};
    std::copy(view.viewProjection.m.begin(), view.viewProjection.m.end(), sprites.clip.begin());

    effectVertices_.clear();
    effectBatches_.clear();
    bool newImages = false;
    ts2::scene::appendEffectSprites(*lotEffectPlayer_, sprites, [&](const std::string& name) {
        const auto [known, isNew] = effectImages_.try_emplace(name, -1);
        const auto portrait = effectPortraits_.find(name);
        if (portrait != effectPortraits_.end()) {
            if (auto picture = lotUi_.balloonPortrait(*lotLife_, portrait->second.neighbor)) {
                if (portrait->second.badge.width > 0) {
                    *picture = ts2::content::wantBalloonPicture(*picture, portrait->second.badge, app::LotUi::kBalloonPictureSide);
                }
                known->second = static_cast<int>(lot_.images.size());
                lot_.images.push_back({picture->width, picture->height, std::move(picture->rgba)});
                newImages = true;
                effectPortraits_.erase(portrait);
            }
        } else if (isNew) {
            if (const auto image = assets_.get()->texture(name)) {
                known->second = static_cast<int>(lot_.images.size());
                lot_.images.push_back(*image);
                newImages = true;
            }
        }
        return known->second;
    }, effectVertices_, effectBatches_);
    if (newImages) {
        renderer_.addMaterials(lot_);
    }
    renderer_.showParticles(effectVertices_, effectBatches_);
}

// The Sims come to life once their simulation is built.
void Game::startLotLife() {
    if (auto sim = finished(lotSimBuilding_, "lot's simulation"); sim && *sim) {
        stepTaken_ = true;
        lotLife_ = std::make_unique<app::LotLife>(std::move(*sim), *assets_.get(), lotRouteModels_);
        lotLife_->loadSimsInBackground(install_);
        // As the game's simulator starts, Free Will takes the lot's own saved choice.
        lotUi_.setFreeWill(lotLife_->simulation().globals()[ts2::sim::global::kFreeWill] != 0);
        lotLife_->simulation().world().setAsksNames(true);
        lotLife_->simulation().world().setBabyFirstNames(data_.text(kLiveText, kBabyBoyText), data_.text(kLiveText, kBabyGirlText));
        std::map<std::uint16_t, std::vector<std::string>> nameTables;
        for (const std::uint16_t table : kNameTables) {
            nameTables[table] = data_.texts(kLiveText, table);
        }
        lotLife_->simulation().world().setNameTables(std::move(nameTables));
        lotLife_->traceIn(lot_, lotPose_, lotVisible_);
        startLotEffects();
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
// their character file's portrait. The oldest come first, which the skewer puts at the top: by life
// stage (person data 0x3A), then by fewer days left in it (person data 0x5B), then by neighbor id.
void Game::drawHousehold(std::uint64_t now) {
    householdShown_ = true;
    householdDrawn_ = lotLife_->household();
    std::vector<app::Skewer::Member> members;
    std::mt19937 poseRandom(static_cast<unsigned>(now));
    for (const std::int16_t id : householdDrawn_) {
        auto portrait = app::drawPortrait(lot_, renderer_, lotLife_->animations(), static_cast<std::uint32_t>(id), poseRandom);
        members.push_back({id, portrait ? std::move(portrait) : lotLife_->portrait(id)});
    }
    const auto age = [&](std::int16_t sim) {
        const auto data = lotLife_->personData(sim);
        const int stage = data.size() > kLifeStage ? data[kLifeStage] : 0;
        const int daysLeft = data.size() > kAgeDaysLeft ? data[kAgeDaysLeft] : 0;
        return std::tuple(-stage, daysLeft, lotLife_->neighborId(sim));
    };
    std::stable_sort(members.begin(), members.end(), [&](const auto& a, const auto& b) { return age(a.sim) < age(b.sim); });
    renderer_.show(lotShown());
    lotUi_.showHousehold(std::move(members));
    ts2::diag::step("household portraits drawn");
}

std::vector<std::uint8_t> Game::lotShown() const {
    auto shown = ts2::scene::visibleInstances(lot_, lotView_.view);
    const auto hide = [&](std::size_t first, std::size_t count) {
        for (std::size_t i = first; i < first + count && i < shown.size(); ++i) {
            shown[i] = 0;
        }
    };
    for (const auto& [margin, around] : surroundings_) {
        if (margin != margin_) {
            hide(around.first, around.count);
        } else if (!lotViewOptions_.decorations) {
            hide(around.first, around.decorations);
        }
    }
    if (!lotViewOptions_.realHouses) {
        for (const NeighborHouse& house : neighborHouses_) {
            hide(house.first, house.count);
        }
    }
    return shown;
}

void Game::applyLotViewOptions() {
    lotViewOptions_ = lotUi_.gameOptions().lotView();
    renderer_.setImpostersShown(lotViewOptions_.neighbors);
    if (skirtSource_) {
        showMargin(lotViewOptions_.skirtMargin.value_or(skirtSource_->defaultMargin));
    }
}

// With Real Houses, the nearest neighbors around the lot start to build, one at a time. Their imposters
// show until each is ready. Each costs about 200 MB of video memory and 400 MB of memory, so only the
// nearest are built. A house already built stays, ready to show again.
void Game::startNeighborHouses() {
    // No more than the renderer can leave out of the ground around the lot.
    constexpr std::size_t kMostHouses = ts2::render::kMaxNeighborGrounds;
    neighborsToBuild_.clear();
    const auto around = surroundings_.find(margin_);
    if (around == surroundings_.end() || !lotViewOptions_.realHouses) {
        return;
    }
    const float x = lotPlace_.m[3], y = lotPlace_.m[7];
    std::set<std::uint32_t> ids;
    for (const auto& part : around->second.imposters) {
        ids.insert(part.lot);
    }
    for (const auto& footprint : hood_.stats.lotFootprints) {
        const bool built = std::any_of(neighborHouses_.begin(), neighborHouses_.end(), [&](const NeighborHouse& house) { return house.id == footprint.id; });
        if (ids.contains(footprint.id) && !built) {
            neighborsToBuild_.push_back(footprint);
        }
    }
    const auto away = [&](const ts2::scene::LotFootprint& f) {
        const float dx = (f.x0 + f.x1) / 2 - x, dy = (f.y0 + f.y1) / 2 - y;
        return dx * dx + dy * dy;
    };
    std::sort(neighborsToBuild_.begin(), neighborsToBuild_.end(), [&](const auto& a, const auto& b) { return away(a) < away(b); });
    const std::size_t room = kMostHouses - std::min(kMostHouses, neighborHouses_.size());
    if (neighborsToBuild_.size() > room) {
        neighborsToBuild_.resize(room);
    }
    std::reverse(neighborsToBuild_.begin(), neighborsToBuild_.end());
    ts2::diag::info(std::format("lot view: {} neighbors to build as real houses", neighborsToBuild_.size()));
}

// The neighbors' imposters of a View Distance, less those with a real house showing.
std::vector<ts2::scene::PaintedTerrain::ImposterPart> Game::impostersShown(const Surroundings& around) const {
    auto parts = around.imposters;
    if (lotViewOptions_.realHouses) {
        std::erase_if(parts, [&](const ts2::scene::PaintedTerrain::ImposterPart& part) {
            return std::any_of(neighborHouses_.begin(), neighborHouses_.end(), [&](const NeighborHouse& house) { return house.id == part.lot; });
        });
    }
    return parts;
}

// Real Houses in force: any left to build start, and what is built shows.
void Game::applyNeighborHouses() {
    showNeighborHouses();
    startNeighborHouses();
}

// The houses or their imposters show, and the ground around the lot makes room for the houses.
void Game::showNeighborHouses() {
    const auto around = surroundings_.find(margin_);
    if (lot_.terrain && around != surroundings_.end()) {
        lot_.terrain->imposters = impostersShown(around->second);
        for (const auto& [lot, alpha] : imposterAlphas_) {
            renderer_.setImposterAlpha(lot, 1);
        }
        imposterAlphas_.clear();
        renderer_.setSurroundings(lot_);
    }
    neighborGrounds_.clear();
    for (const NeighborHouse& house : neighborHouses_) {
        if (lotViewOptions_.realHouses) {
            neighborGrounds_.push_back(house.ground);
        }
    }
    renderer_.setNeighborGrounds(neighborGrounds_);
}

void Game::buildNextNeighborHouse() {
    if (!inLot_ || neighborsToBuild_.empty() || neighborBuilding_.valid()) {
        return;
    }
    const std::uint32_t id = neighborsToBuild_.back().id;
    const auto file = openFolder_ / "Lots" / (openFolder_.filename().u8string() + u8"_Lot" + std::u8string(reinterpret_cast<const char8_t*>(std::to_string(id).c_str())) + u8".package");
    neighborBuilding_ = std::async(std::launch::async, [install = install_, assets = neighborAssets_, file, id]() mutable {
        NeighborBuilt built;
        built.id = id;
        try {
            built.assets = assets ? std::move(assets) : std::make_shared<ts2::scene::Assets>(install);
            built.scene = neighborHouse(*built.assets, file);
        } catch (const std::exception& ex) {
            ts2::diag::error(std::format("couldn't build neighbor's house, lot {}: {}", id, ex.what()));
            built.failed = true;
        }
        return built;
    });
}

void Game::addNeighborHouse(NeighborBuilt built) {
    neighborAssets_ = std::move(built.assets);
    const auto where = std::find_if(neighborsToBuild_.begin(), neighborsToBuild_.end(), [&](const ts2::scene::LotFootprint& f) { return f.id == built.id; });
    if (!inLot_ || where == neighborsToBuild_.end()) {
        return;
    }
    const ts2::scene::LotFootprint footprint = *where;
    neighborsToBuild_.erase(where);
    if (built.failed) {
        return;
    }
    NeighborHouse house;
    house.id = footprint.id;
    house.first = lot_.instances.size();
    house.count = built.scene.instances.size();
    const std::size_t firstImage = lot_.images.size();
    app::appendScene(lot_, std::move(built.scene), footprint.place);
    renderer_.addMaterials(lot_);
    renderer_.addMeshes(lot_);
    for (std::size_t i = house.first; i < house.first + house.count; ++i) {
        renderer_.setInstance(lot_, i);
    }
    // The pictures now on the GPU are never read again, unlike the lot's own light maps.
    for (std::size_t i = firstImage; i < lot_.images.size(); ++i) {
        lot_.images[i].rgba = {};
        lot_.images[i].smallerLevels = {};
    }
    house.ground = {footprint.x0, footprint.x1, footprint.y0, footprint.y1};
    const ts2::scene::Transform toLot = lotPlace_.rigidInverse();
    float low[3] = {footprint.x0, footprint.y0, footprint.z}, high[3] = {footprint.x1, footprint.y1, footprint.top};
    toLot.apply(low, true);
    toLot.apply(high, true);
    for (std::size_t c = 0; c < 3; ++c) {
        house.box.low[c] = std::min(low[c], high[c]);
        house.box.high[c] = std::max(low[c], high[c]);
    }
    neighborHouses_.push_back(house);
    showNeighborHouses();
    renderer_.show(lotShown());
    ts2::diag::info(std::format("lot view: lot {} shows as a real house ({} instances)", footprint.id, house.count));
}

// A house between the camera and the lot fades as its imposter would.
void Game::fadeNeighborHouses(const std::array<float, 3>& eye) {
    namespace propfade = ts2::scene::propfade;
    const float fade = lotViewOptions_.fadeSquared;
    const auto reach = fade > 0 ? propfade::reach(eye, lot_.lotSize) : std::nullopt;
    for (NeighborHouse& house : neighborHouses_) {
        const float alpha = reach ? propfade::imposterAlpha(house.box, eye, *reach, fade) : 1.0f;
        if (alpha == house.alpha) {
            continue;
        }
        house.alpha = alpha;
        const std::optional<std::array<float, 4>> color = alpha == 1.0f ? std::nullopt : std::optional(std::array<float, 4>{1, 1, 1, alpha});
        for (std::size_t i = house.first; i < house.first + house.count && i < lot_.instances.size(); ++i) {
            lot_.instances[i].shapeColor = color;
            renderer_.setShapeColor(i, color);
        }
    }
}

void Game::showMargin(int margin) {
    if (margin == margin_ || !skirtSource_) {
        return;
    }
    const auto built = surroundings_.find(margin);
    if (built == surroundings_.end()) {
        // One build at a time. The one that finishes looks for the margin wanted then.
        if (!surroundingsBuilding_.valid()) {
            surroundingsBuilding_ = std::async(std::launch::async, [source = skirtSource_, margin] {
                auto [skirt, area] = makeSkirt(*source, margin);
                SurroundingsBuilt out;
                out.margin = margin;
                out.scene = app::lotInNeighborhood(*source->hood, ts2::scene::Scene{}, std::move(skirt), source->footprint, area, &out.decorations);
                return out;
            });
        }
        return;
    }
    margin_ = margin;
    if (lot_.terrain) {
        lot_.terrain->imposters = impostersShown(built->second);
    }
    lot_.clip = built->second.clip;
    renderer_.setSurroundings(lot_);
}

void Game::addSurroundings(SurroundingsBuilt built) {
    if (!inLot_ || !skirtSource_ || surroundings_.contains(built.margin)) {
        return;
    }
    Surroundings around;
    around.first = lot_.instances.size();
    around.count = built.scene.instances.size();
    around.decorations = built.decorations;
    if (built.scene.terrain) {
        around.imposters = std::move(built.scene.terrain->imposters);
    }
    around.clip = built.scene.clip;
    app::appendScene(lot_, std::move(built.scene), ts2::scene::Transform{});
    // Lit for the time of day the lot is lit for now, as the rest of its skirt.
    const bool day = !lotDaylight_ || ts2::content::stateTime(lotDaylight_->state()) != ts2::content::StateTime::Night;
    ts2::scene::lightLotSkirt(lot_, day);
    ts2::scene::poolwater::lightPoolWater(lot_, day);
    renderer_.addMaterials(lot_);
    renderer_.addMeshes(lot_);
    for (std::size_t i = around.first; i < around.first + around.count; ++i) {
        renderer_.setInstance(lot_, i);
        lotPicker_.update(lot_, i);
    }
    ts2::diag::info(std::format("lot view: the ground {} tiles around the lot built ({} instances)", built.margin, around.count));
    surroundings_.emplace(built.margin, std::move(around));
    showMargin(lotViewOptions_.skirtMargin.value_or(skirtSource_->defaultMargin));
}

void Game::fadeProps(const std::array<float, 3>& eye) {
    namespace propfade = ts2::scene::propfade;
    fadeNeighborHouses(eye);
    const auto around = surroundings_.find(margin_);
    if (around == surroundings_.end()) {
        return;
    }
    Surroundings& shown = around->second;
    if (shown.propBoxes.size() != shown.decorations) {
        const ts2::scene::Transform toLot = lotPlace_.rigidInverse();
        // The lot's tiles in the neighborhood. Its record places it on whole tiles.
        std::array<int, 4> lotTiles{};
        if (skirtSource_) {
            const auto& f = skirtSource_->footprint;
            lotTiles = {static_cast<int>(std::lround(f.x0 / 10)), static_cast<int>(std::lround(f.y0 / 10)), static_cast<int>(std::lround(f.x1 / 10)),
                        static_cast<int>(std::lround(f.y1 / 10))};
        }
        // Only the plants and decorations the game puts on its fade list get a box (see
        // fadesInLotView), and the rest are drawn as they are. Whether the game fades bridges is not
        // known yet, so they don't fade.
        shown.propBoxes.clear();
        for (std::size_t i = shown.first; i < shown.first + shown.decorations; ++i) {
            const auto* instance = i < lot_.instances.size() ? &lot_.instances[i] : nullptr;
            std::optional<propfade::Box> box;
            if (skirtSource_ && instance && instance->ground && instance->mesh >= 0 && static_cast<std::size_t>(instance->mesh) < lot_.meshes.size()) {
                box = propfade::placedBox(lot_.meshes[static_cast<std::size_t>(instance->mesh)], toLot * instance->transform);
                if (box && !propfade::fadesInLotView(*box, propfade::groundTiles(*instance->ground), lotTiles)) {
                    box.reset();
                }
            }
            shown.propBoxes.push_back(box);
        }
    }
    // With the Fade Distance off, or the camera over the lot, every prop is drawn as it is.
    const float fade = lotViewOptions_.fadeSquared;
    const auto reach = fade > 0 ? propfade::reach(eye, lot_.lotSize) : std::nullopt;
    for (std::size_t k = 0; k < shown.propBoxes.size(); ++k) {
        std::optional<float> alpha = 1.0f;
        if (reach && shown.propBoxes[k]) {
            alpha = propfade::alpha(*shown.propBoxes[k], eye, *reach, fade);
        }
        if (!alpha) {
            continue;
        }
        const std::optional<std::array<float, 4>> color =
            *alpha == 1.0f ? std::nullopt : std::optional(std::array<float, 4>{1, 1, 1, *alpha});
        const std::size_t i = shown.first + k;
        if (i < lot_.instances.size() && lot_.instances[i].shapeColor != color) {
            lot_.instances[i].shapeColor = color;
            renderer_.setShapeColor(i, color);
        }
    }

    // The neighbors' imposters: each a box around all of its parts, its ground among them.
    if (!shown.imposterBoxes) {
        const ts2::scene::Transform toLot = lotPlace_.rigidInverse();
        constexpr std::size_t kVertexFloats = 7;  // x, y, z, u, v, then the second set's u, v
        auto& boxes = shown.imposterBoxes.emplace();
        for (const auto& part : shown.imposters) {
            for (std::size_t v = 0; v + 2 < part.vertices.size(); v += kVertexFloats) {
                float at[3] = {part.vertices[v], part.vertices[v + 1], part.vertices[v + 2]};
                if (!std::isfinite(at[0]) || !std::isfinite(at[1]) || !std::isfinite(at[2])) {
                    continue;
                }
                toLot.apply(at, true);
                const auto [it, added] = boxes.try_emplace(part.lot, propfade::Box{{at[0], at[1], at[2]}, {at[0], at[1], at[2]}});
                for (std::size_t c = 0; c < 3 && !added; ++c) {
                    it->second.low[c] = std::min(it->second.low[c], at[c]);
                    it->second.high[c] = std::max(it->second.high[c], at[c]);
                }
            }
        }
    }
    std::map<std::uint32_t, float> alphas;
    if (reach) {
        for (const auto& [lot, box] : *shown.imposterBoxes) {
            if (const float a = propfade::imposterAlpha(box, eye, *reach, fade); a != 1.0f) {
                alphas.emplace(lot, a);
            }
        }
    }
    for (const auto& [lot, a] : imposterAlphas_) {
        if (!alphas.contains(lot)) {
            renderer_.setImposterAlpha(lot, 1);
        }
    }
    for (const auto& [lot, a] : alphas) {
        const auto was = imposterAlphas_.find(lot);
        if (was == imposterAlphas_.end() || was->second != a) {
            renderer_.setImposterAlpha(lot, a);
        }
    }
    imposterAlphas_ = std::move(alphas);
}

void Game::unfadeImposters() {
    for (const auto& [lot, a] : imposterAlphas_) {
        renderer_.setImposterAlpha(lot, 1);
    }
    imposterAlphas_.clear();
}

// What the Sims changed on the lot this frame goes to the GPU and the picker.
void Game::sendLotChanges() {
    if (!lotLife_->changedInstances().empty()) {
        renderer_.addMaterials(lot_);
        renderer_.addMeshes(lot_);
        for (const std::size_t instance : lotLife_->changedInstances()) {
            renderer_.setInstance(lot_, instance);
            // The scene keeps where a Sim or what it holds was loaded. It stays where this frame posed it.
            const auto posed = std::find_if(lotPose_.places.begin(), lotPose_.places.end(), [&](const auto& p) { return p.first == instance; });
            if (posed != lotPose_.places.end()) {
                renderer_.setPlace(instance, posed->second);
            }
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
    for (const std::size_t instance : lotLife_->shapeColored()) {
        renderer_.setShapeColor(instance, lot_.instances[instance].shapeColor);
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
        const auto [ux, uy] = toUi(static_cast<float>(options_.hoverX), static_cast<float>(options_.hoverY));
        screen_.mouse(ux, uy, false, 0);
    }
    if (!options_.shot.empty() && !building_.valid()) {
        showSprites(options_.hoverX >= 0 ? 10.0 : 0.0, false);
        if (!renderer_.drawToFile({}, static_cast<unsigned>(width_), static_cast<unsigned>(height_), options_.shot)) {
            throw std::runtime_error("couldn't save the picture");
        }
        ts2::diag::info("wrote " + ts2::diag::utf8(options_.shot));
    }
}

// How long a minimized game waits for the system before it looks again.
constexpr int kMinimizedWaitMs = 100;

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
        // While the window is minimized the game stops, its simulation and drawing alike, and only
        // waits for the system, unless the player turned Sim While Minimized on.
        if (running_ && pf::firstWindowMinimized() && !lotUi_.gameOptions().flag("simInBackground", false)) {
            pf::waitForEvent(kMinimizedWaitMs);
            continue;
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
            lotUi_.setTestingCheats(cheats_.testingCheats());
            lotUi_.load();
        }
        if (auto done = finished(lotBuilding_, "lot")) {
            showLot(*done);
        }
        if (auto around = finished(surroundingsBuilding_, "lot surroundings")) {
            addSurroundings(std::move(*around));
        }
        if (auto house = finished(neighborBuilding_, "neighbor's house")) {
            addNeighborHouse(std::move(*house));
        }
        buildNextNeighborHouse();
        const std::uint64_t now = ts2::platform::ticksNs();
        reportFrame(now);
        if (!inLot_ && !inNeighborhood_) {
            cursors_.show(app::Cursors::kArrow);
        }
        cursors_.setBusy(loadingScene());
        cursors_.animate(static_cast<double>(now) / 1e9);
        cheatConsole_.tick(screen_, now / 1'000'000);
        tickGraphicsOptions(now / 1'000'000);
        if (clickedTile_ && now - tileClickedAt_ > kDoubleClickNs) {
            chosen_ = *clickedTile_;
            clickedTile_.reset();
            showPreview();
        }
        if (inLot_) {
            updateLot(now);
        } else if (inNeighborhood_) {
            // A picture is taken without the drift, so the same view gives the same picture.
            orbit_.keysTaken = cheatConsole_.takesKeys();
            orbit_.gamePixel = pf::firstWindowDisplayScale();
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
        paceFrame();
    }
}

// The keep-these-settings box counts down wherever it is open, and may put the settings back.
void Game::tickGraphicsOptions(std::uint64_t ms) {
    auto& lot = lotUi_.graphicsOptions();
    auto& hood = hoodUi_.graphicsOptions();
    lot.tick(ms);
    hood.tick(ms);
    if (lot.takeChanged()) {
        hood.takeSettings(lot);
        applyGraphicsOptions();
        applyDisplayOptions();
    }
    if (hood.takeChanged()) {
        lot.takeSettings(hood);
        applyGraphicsOptions();
        applyDisplayOptions();
    }
}

// Puts the display choices in force to work, once after they change.
void Game::applyDisplayOptions() {
    auto& lot = lotUi_.graphicsOptions();
    auto& hood = hoodUi_.graphicsOptions();
    const bool lotChanged = lot.takeDisplayChanged(), hoodChanged = hood.takeDisplayChanged();
    if (!lotChanged && !hoodChanged) {
        return;
    }
    const auto display = (lotChanged ? lot : hood).display();
    ts2::platform::setFirstWindowFullscreen(display.fullscreen);
    if (!display.fullscreen) {
        ts2::platform::resizeFirstWindow(display.width, display.height);
    }
    takeWindowSize();
    frameLimit_ = display.frameLimit;
    renderer_.setWaitForScreen(frameLimit_ == app::GraphicsOptionsPanel::kWaitForScreen);
    renderer_.setSmoothEdges(display.smoothEdges);
    renderer_.setAmbientOcclusion(ts2::content::kAmbientOcclusionStrengths[static_cast<std::size_t>(display.ambientOcclusion)]);
}

// Lays the game out for the window's size when it has changed.
void Game::takeWindowSize() {
    int width = 0, height = 0;
    ts2::platform::firstWindowSize(width, height);
    if (width <= 0 || height <= 0 || (width == width_ && height == height_)) {
        return;
    }
    width_ = width;
    height_ = height;
    uiScale_ = uiScaleFor(options_.uiScale, width_, height_);
    uiWidth_ = static_cast<int>(static_cast<float>(width_) / uiScale_);
    uiHeight_ = static_cast<int>(static_cast<float>(height_) / uiScale_);
    screen_.setSize(uiWidth_, uiHeight_);
    lotUi_.setSize(uiWidth_, uiHeight_);
    hoodUi_.setSize(uiWidth_, uiHeight_);
    menuX_ = (uiWidth_ - screen_.width(menu_)) / 2;
    menuY_ = (uiHeight_ - screen_.height(menu_)) / 2;
    screen_.move(menu_, menuX_, menuY_);
    screen_.move(preview_, menuX_, menuY_);
    const int backdrop = screen_.find(menu_, kBackdrop);
    screen_.move(backdrop, (uiWidth_ - screen_.width(backdrop)) / 2 - menuX_, (uiHeight_ - screen_.height(backdrop)) / 2 - menuY_);
    for (auto* panel : {&lotUi_.graphicsOptions(), &hoodUi_.graphicsOptions()}) {
        panel->setWindowSize(width_, height_);
        panel->centreBox();
    }
}

// Waits out what is left of the frame under a frame rate limit.
void Game::paceFrame() {
    if (frameLimit_ <= 0) {
        return;
    }
    const std::uint64_t now = ts2::platform::ticksNs();
    if (frameDueNs_ > now) {
        // The last millisecond is waited in a spin, as a sleep may overshoot it.
        const std::uint64_t left = frameDueNs_ - now;
        if (left > 2'000'000) {
            ts2::platform::delayMs(static_cast<std::uint32_t>(left / 1'000'000 - 1));
        }
        while (ts2::platform::ticksNs() < frameDueNs_) {
        }
    }
    frameDueNs_ = std::max(frameDueNs_, now) + 1'000'000'000ull / static_cast<std::uint64_t>(frameLimit_);
}

// Graphic Effects: which of an effect's versions play, and how many particles they try to stay under.
void Game::applyEffectOptions() {
    constexpr int kHighestLevel = 3, kMostParticles = 10000;
    if (lotEffectPlayer_) {
        const auto& graphics = lotUi_.graphicsOptions();
        lotEffectPlayer_->setLevel(static_cast<std::uint16_t>(std::max(graphics.propNumber("EffectsQuality", "effectPriorityLevel", kHighestLevel), 0)));
        lotEffectPlayer_->setParticleTarget(static_cast<std::uint32_t>(std::max(graphics.propNumber("EffectsQuality", "maxParticlesTarget", kMostParticles), 0)));
    }
}

// The Graphics options in force, unless the command line turned a feature off.
void Game::applyGraphicsOptions() {
    const auto& graphics = lotUi_.graphicsOptions();
    applyEffectOptions();
    renderer_.setBumpMapping(options_.bumpMapping && graphics.propOn("MaterialDetail", "bumpMapping"));
    renderer_.setFloorAndWallNormalMapping(options_.wallBumps && graphics.propOn("LightingQuality", "floorAndWallNormalMapping"));
    renderer_.setSpecularHighlights(graphics.propOn("LightingQuality", "specHighlights"));
    renderer_.setShadowOptions({graphics.propOn("Shadows", "simShadows"), graphics.propOn("Shadows", "objectShadows"), graphics.propOn("Shadows", "guob")});
    renderer_.setReflections(graphics.propOn("Reflection", "reflectionWithExtraViewer"));
    const app::LotLightOptions light{graphics.propOn("Shadows", "heightMapShadows"), graphics.propOn("LightingQuality", "portalLighting")};
    if (light.sunShadow != lotLight_.sunShadow || light.portals != lotLight_.portals) {
        lotLight_ = light;
        app::setLotLightOptions(light);
        if (lotDaylight_) {
            lotDaylight_->lightAgain();
        }
    }
}

// Whether a click is dropped because a scene is loading (see app::ClickGate).
bool Game::blockedWhileLoading(const pf::Event& e) {
    if (e.type != pf::Event::Type::MouseDown && e.type != pf::Event::Type::MouseUp) {
        return false;
    }
    return loadingGate_.drops(e.type == pf::Event::Type::MouseDown, static_cast<unsigned>(e.button), loadingScene());
}

void Game::handleEvent(const pf::Event& e) {
    // The left, middle and right buttons held, which a key press carries in the game.
    if ((e.type == pf::Event::Type::MouseDown || e.type == pf::Event::Type::MouseUp) && e.button != pf::MouseButton::Other) {
        const auto bit = static_cast<std::uint8_t>(1u << static_cast<unsigned>(e.button));
        mouseHeld_ = e.type == pf::Event::Type::MouseDown ? mouseHeld_ | bit : mouseHeld_ & ~bit;
    }
    if (blockedWhileLoading(e)) {
        return;
    }
    // Mouse movement, clicks, the wheel and keys all end the neighborhood camera's drift.
    const bool input = e.type == pf::Event::Type::MouseMotion || e.type == pf::Event::Type::MouseDown || e.type == pf::Event::Type::MouseUp ||
                       e.type == pf::Event::Type::MouseWheel || e.type == pf::Event::Type::KeyDown || e.type == pf::Event::Type::KeyUp;
    if (inNeighborhood_ && hoodDrift_ && input) {
        hoodDrift_->interrupt();
    }
    // Sims 1 Style turns the other way by comma and period too.
    const pf::Key key = !sims1Turn_ ? e.key : e.key == pf::Key::Comma ? pf::Key::Period : e.key == pf::Key::Period ? pf::Key::Comma : e.key;
    // The turn keys stop turning when let go, wherever the key went meanwhile.
    if (e.type == pf::Event::Type::KeyUp && (key == pf::Key::Comma || key == pf::Key::Period)) {
        orbit_.turnKey = 0;
    }
    // Ctrl+Shift+C opens and closes the cheat window in the lot and the neighborhood ("Toggle Cheat
    // Entry Window" in the game's keys for most modes). The window takes the keyboard while its text field has it.
    if (e.type == pf::Event::Type::KeyDown && !e.repeat && e.key == pf::Key::C && e.ctrl && e.shift && !e.alt && (inLot_ || inNeighborhood_)) {
        cheatConsole_.toggle(screen_, uiWidth_);
        keepBabyNameTyping();
        return;
    }
    // The scroll bars of the Graphics options' lists take the mouse while held.
    if (inLot_ || inNeighborhood_) {
        float mx = 0, my = 0;
        pf::mousePosition(mx, my);
        const auto [ux, uy] = toUi(mx, my);
        auto& graphics = openGraphicsOptions();
        if (graphics.holdsMouse() && e.type == pf::Event::Type::MouseMotion) {
            graphics.mouseMove(uy);
            return;
        }
        if (graphics.holdsMouse() && e.type == pf::Event::Type::MouseUp) {
            graphics.mouseUp();
            return;
        }
        if (e.type == pf::Event::Type::MouseDown && e.button == pf::MouseButton::Left && graphics.mouseDown(ux, uy)) {
            return;
        }
        if (e.type == pf::Event::Type::MouseWheel && graphics.wheel(ux, uy, static_cast<int>(std::lround(e.wheel)))) {
            return;
        }
    }
    // A mouse press anywhere takes the keyboard from the cheat window unless it is in its text field,
    // and a press on its bar goes no further. While its scroll bar is held it takes the mouse.
    if ((e.type == pf::Event::Type::MouseMotion || e.type == pf::Event::Type::MouseUp) && cheatConsole_.holdsMouse()) {
        float mx = 0, my = 0;
        pf::mousePosition(mx, my);
        const auto [ux, uy] = toUi(mx, my);
        if (e.type == pf::Event::Type::MouseMotion) {
            cheatConsole_.mouseMove(screen_, ux, uy);
        } else {
            cheatConsole_.mouseUp(screen_, ux, uy);
        }
        return;
    }
    if (e.type == pf::Event::Type::MouseDown && cheatConsole_.isOpen()) {
        float mx = 0, my = 0;
        pf::mousePosition(mx, my);
        const auto [ux, uy] = toUi(mx, my);
        const bool onBar = cheatConsole_.mouseDown(screen_, ux, uy, e.button == pf::MouseButton::Left, ts2::platform::ticksNs() / 1'000'000);
        keepBabyNameTyping();
        if (onBar) {
            return;
        }
    }
    if (e.type == pf::Event::Type::MouseWheel && cheatConsole_.isOpen()) {
        float mx = 0, my = 0;
        pf::mousePosition(mx, my);
        const auto [ux, uy] = toUi(mx, my);
        // Three lines a notch, the wheel's usual step.
        if (cheatConsole_.scroll(screen_, ux, uy, static_cast<int>(std::lround(e.wheel * 3)))) {
            return;
        }
    }
    if (cheatConsole_.takesKeys() && (e.type == pf::Event::Type::Text || e.type == pf::Event::Type::KeyDown || e.type == pf::Event::Type::KeyUp)) {
        if (e.type == pf::Event::Type::Text) {
            cheatConsole_.text(screen_, e.text);
        } else if (e.type == pf::Event::Type::KeyDown) {
            std::optional<ts2::sim::CheatLot> lot;
            if (lotLife_) {
                lot.emplace(lotLife_->simulation().world(), lotLife_->simulation().globals());
            }
            cheatConsole_.key(screen_, e.key, lot ? &*lot : nullptr);
            keepBabyNameTyping();
        } else if (freeCam_) {
            freeCam_->key(e.scancode, false);
        }
        return;
    }
    // The naming dialog takes the keyboard while it is open. Keys let go still stop the camera's
    // turn above and the free camera's moves, so none goes on after the dialog.
    if (babyNames_.isOpen() && e.type != pf::Event::Type::Quit && e.type != pf::Event::Type::MouseMotion &&
        e.type != pf::Event::Type::MouseDown && e.type != pf::Event::Type::MouseUp && e.type != pf::Event::Type::MouseWheel) {
        if (e.type == pf::Event::Type::Text) {
            babyNames_.text(screen_, e.text);
        } else if (e.type == pf::Event::Type::KeyDown) {
            babyNames_.key(screen_, e.key);
        } else if (e.type == pf::Event::Type::KeyUp && freeCam_) {
            freeCam_->key(e.scancode, false);
        }
        return;
    }
    const bool freeToggle = e.type == pf::Event::Type::KeyDown && !e.repeat && inLot_ && !quitBox_.isOpen() &&
                            (e.scancode == pf::Key::Tab || e.scancode == pf::Key::Y || (freeCam_ && e.scancode == pf::Key::Escape));
    if (freeToggle) {
        toggleFreeCamera();
    } else if (freeCam_ && (e.type == pf::Event::Type::KeyDown || e.type == pf::Event::Type::KeyUp) &&
               (e.repeat || freeCam_->key(e.scancode, e.type == pf::Event::Type::KeyDown))) {
        // The free camera's own keys.
    } else if (e.type == pf::Event::Type::KeyDown && quitBox_.isOpen()) {
        // Escape answers No, or Cancel on the save question. As in the game's message box, only a
        // press on its own counts: not a held key's repeat, not with Shift, Ctrl or Alt, and not
        // while a mouse button is held.
        if (e.key == pf::Key::Escape && !e.repeat && !e.shift && !e.ctrl && !e.alt && mouseHeld_ == 0) {
            closeQuitBox();
        }
    } else if (e.type == pf::Event::Type::Quit) {
        ts2::diag::step("window closed");
        running_ = false;
    } else if (e.type == pf::Event::Type::KeyDown && inLot_ && pieMenu_.isOpen()) {
        // As in the game, Tab shows the next page as "More..." does, Shift does nothing, and any other
        // key, Escape too, closes an open pie menu. Escape does nothing else there.
        if (e.key == pf::Key::Tab) {
            pieMenu_.tab();
        } else if (e.key != pf::Key::Shift) {
            pieMenu_.close();
        }
    } else if (e.type == pf::Event::Type::KeyDown && !e.repeat && inNeighborhood_ && !neighborhoodCovered() && e.key == pf::Key::Q) {
        openQuitBox();  // Q asks to quit in the neighborhood too
    } else if (e.type == pf::Event::Type::KeyDown && inLot_) {
        if (const auto action = lotKey(e.key); action != app::LotUi::Action::None) {
            lotAction(action);
        } else if (!e.repeat || (key != pf::Key::Comma && key != pf::Key::Period)) {
            lotView_.key(key, orbit_);
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
        const auto [ux, uy] = toUi(x, y);
        if (inNeighborhood_ && !neighborhoodCovered() && !lotBuilding_.valid() && !loading(lotSimBuilding_) && std::abs(x - downX_) < 4 && std::abs(y - downY_) < 4 &&
            !screen_.over(ux, uy)) {
            lotClicked(x, y);
        }
    }
    if (inLot_ && freeCam_) {
        freeCameraMouse(e);
        return;
    }
    if (inLot_) {
        if (!quitBox_.isOpen() && !babyNames_.isOpen()) {
            cameraDrag(e, x, y);
        }
        lotMouse(e, x, y);
        return;
    }
    if (inNeighborhood_ && !neighborhoodCovered()) {
        cameraDrag(e, x, y);
    }
    if (const auto [ux, uy] = toUi(x, y); const auto clicked = screen_.mouse(ux, uy, down_, static_cast<double>(ts2::platform::ticksNs()) / 1e9)) {
        windowClicked(*clicked);
    }
}

void Game::windowClicked(int window) {
    if (const auto button = errorBox_.clicked(screen_, window)) {
        answerObjectError(*button);
        return;
    }
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
        } else if (action == app::NeighborhoodUi::Action::GraphicsOptionChanged) {
            lotUi_.graphicsOptions().takeSettings(hoodUi_.graphicsOptions());
            applyGraphicsOptions();
            applyDisplayOptions();
        } else if (action == app::NeighborhoodUi::Action::GameOptionChanged) {
            // A lot reads the Lot View Options when it opens. Camera Rotation turns the camera the new way at once.
            lotUi_.gameOptions().takeSettings(hoodUi_.gameOptions());
            sims1Turn_ = lotUi_.gameOptions().flag("rotateCameraSims1Mode", sims1Turn_);
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
    thread_local std::vector<ts2::render::Sprite> shown;
    sprites(screen_, seconds, uiScale_, shown);
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
    renderer_.setSpriteScale(uiScale_);
    renderer_.showSprites(shown, scene);
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
    // In the neighborhood the clicks go to its panels, as a player's would.
    if (inNeighborhood_ && !inLot_ && !options_.uiClicks.empty() && !uiClicked_) {
        uiClicked_ = true;
        for (const auto& [wx, wy, kind] : options_.uiClicks) {
            const auto [cx, cy] = toUi(static_cast<float>(wx), static_cast<float>(wy));
            screen_.mouse(cx, cy, true, 0);
            if (const auto clicked = screen_.mouse(cx, cy, false, 0)) {
                windowClicked(*clicked);
            }
        }
        showSprites(static_cast<double>(ts2::platform::ticksNs()) / 1e9, true);
    }
    if (inLot_ && !options_.uiClicks.empty() && !uiClicked_) {
        uiClicked_ = true;
        clickPanels();
        showSprites(static_cast<double>(ts2::platform::ticksNs()) / 1e9, true);
    }
    // The mouse resting on the lot (--hover) is picked up by the next frame, once the Sims are there.
    if (inLot_ && options_.hoverX >= 0 && !lotHovered_) {
        if (lotLife_ && householdShown_) {
            lotHovered_ = true;
            hoverAt_ = {static_cast<float>(options_.hoverX), static_cast<float>(options_.hoverY)};
        }
        return false;
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
        for (const auto& [wx, wy, kind] : options_.uiClicks) {
            const auto [cx, cy] = toUi(static_cast<float>(wx), static_cast<float>(wy));
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
    for (const auto& [wx, wy, right] : options_.uiClicks) {
        const auto [cx, cy] = toUi(static_cast<float>(wx), static_cast<float>(wy));
        if (right == 2) {
            // A second before the picture, so a tip that shows after a while and hides 3 s later is in it.
            screen_.mouse(cx, cy, false, static_cast<double>(ts2::platform::ticksNs()) / 1e9 - 1.0);
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
        // The picture is taken before the next frame would bring the puck up to date.
        lotUi_.showStoreyLimits(lotView_.canGoUp(), lotView_.canGoDown());
        lotUi_.showCeilings(lotView_.view.ceilings);
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
    // The cheat window's output asks Windows for a typewriter font, as the game's does. Lucida
    // Console at 10 pixels has the game's character width.
    if (const auto system = ts2::paths::systemFontsFolder(); !system.empty()) {
        (void)fonts.addStyle("CheatConsole = \"Lucida Console\", \"10\", \"\", 0x00000000", system / "lucon.ttf");
    }
    ts2::diag::info(std::format("{} UI and text packages, {} font faces", data.packageCount(), fonts.faceCount()));

    const int width = options.width, height = options.height;
    pf::WindowOptions windowOptions;
    windowOptions.title = "NeoTS2";
    windowOptions.width = width;
    windowOptions.height = height;
    windowOptions.resizable = false;
    windowOptions.highDensity = false;
    windowOptions.hidden = !options.shot.empty();
    windowOptions.fitScreen = options.fitScreen && options.shot.empty();
    std::unique_ptr<pf::Window> window;
    {
        const auto rendererOwner = app::openRenderer(window, options.backend, windowOptions, shaders, !options.shot.empty());
        ts2::render::IRenderer& renderer = *rendererOwner;
        // The window can come smaller than asked (SDL 1.2 keeps it on the screen): the game is laid out for the one it has.
        MenuOptions sized = options;
        int actualWidth = 0, actualHeight = 0;
        // Unless told otherwise, the window opens as the Graphics options last left it.
        if (options.shot.empty()) {
            const auto saved = app::GraphicsOptionsPanel::savedDisplay(options.user);
            if (saved.fullscreen) {
                ts2::platform::setFirstWindowFullscreen(true);
            } else if (saved.width > 0 && !options.sizeGiven && !options.fitScreen) {
                ts2::platform::resizeFirstWindow(saved.width, saved.height);
            }
        }
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
