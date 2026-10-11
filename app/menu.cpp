// The game's main menu (the neighborhood chooser), built from its UI script, and the neighborhood
// it opens.
#include "menu.hpp"
#include "neighborhood.hpp"
#include "lot_life.hpp"
#include "lot_ui.hpp"
#include "lot_view.hpp"
#include "neighborhood_ui.hpp"
#include "orbit.hpp"

#include "content/install.hpp"
#include "content/material_scripts.hpp"
#include "content/neighborhood.hpp"
#include "content/ui_data.hpp"
#include "formats/image.hpp"
#include "formats/reia.hpp"
#include "render/renderer.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/lot_scene.hpp"
#include "ui/fonts.hpp"
#include "ui/screen.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>

namespace {

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
// The neighborhood preview and its windows by id.
constexpr std::uint32_t kPreview = 0x49001021;
constexpr std::uint32_t kPreviewPicture = 0x00002001, kPreviewDescription = 0x00002002, kPreviewName = 0x00002003, kPreviewPlay = 0x000000b2,
                        kPreviewClose = 0x000000b3;
constexpr std::uint32_t kConstants = 0x4dc1dce2, kUpperLeftSim = 0x000000e1, kLowerRightSim = 0x000000e3;
constexpr std::uint32_t kImages = 0x499DB772;
constexpr int kPerPage = 3;
constexpr int kTileGap = 4;  // between neighborhood tiles

std::optional<ts2::tga::Pixels> readImage(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return bytes.empty() ? std::nullopt : ts2::image::decode(bytes);
}

// The lot under a point of the screen: of the lots whose ground the camera's ray through the point
// crosses (at each lot's height), the nearest; 0 for none.
std::uint32_t lotAt(const ts2::render::Camera& c, int width, int height, float mx, float my,
                    const std::vector<ts2::scene::LotFootprint>& lots) {
    using ts2::render::Vec3;
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float tx = c.fovX > 0 ? std::tan(c.fovX / 2) : std::tan(c.fovY / 2) * aspect;
    const float ty = tx / aspect;
    const Vec3 f = ts2::render::normalize(ts2::render::sub(c.target, c.eye));
    const Vec3 s = ts2::render::normalize(ts2::render::cross(f, {0, 0, 1}));
    const Vec3 u = ts2::render::cross(s, f);
    const float nx = 2 * mx / static_cast<float>(width) - 1, ny = 1 - 2 * my / static_cast<float>(height);
    const Vec3 d{f[0] + s[0] * nx * tx + u[0] * ny * ty, f[1] + s[1] * nx * tx + u[1] * ny * ty, f[2] + s[2] * nx * tx + u[2] * ny * ty};
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


} // namespace

int runMenu(const std::filesystem::path& install, const std::filesystem::path& shaders, const MenuOptions& options) {
    std::printf("loading the game's UI...\n");
    ts2::content::UiData data(install);
    ts2::ui::Fonts fonts(install);
    std::printf("%zu UI and text packages, %zu font faces\n", data.packageCount(), fonts.faceCount());

    const int width = options.width, height = options.height;
    SDL_Window* window = SDL_CreateWindow("NeoTS2", width, height, options.shot.empty() ? 0 : SDL_WINDOW_HIDDEN);
    if (window == nullptr) {
        throw std::runtime_error(SDL_GetError());
    }
    {
        ts2::render::Renderer renderer(window, shaders);
        // What shows around the menu: the game's dark blue (as the game shows it).
        renderer.setClearColor(1 / 255.f, 35 / 255.f, 62 / 255.f);
        ts2::ui::Screen screen(data, fonts);
        // Placed in the middle of the screen (as the game places it at 1920 x 1080).
        const int menu = screen.load(kScripts, kMainMenu, 0, 0);
        if (menu < 0) {
            throw std::runtime_error("the main menu's UI script is missing");
        }
        const int mx = (width - screen.width(menu)) / 2, my = (height - screen.height(menu)) / 2;
        screen.move(menu, mx, my);
        // At start the blurred neighborhood picture shows behind the menu, in the middle of the
        // screen (as the game shows it).
        const int backdrop = screen.find(menu, kBackdrop);
        screen.setVisible(backdrop, true);
        screen.move(backdrop, (width - screen.width(backdrop)) / 2 - mx, (height - screen.height(backdrop)) / 2 - my);
        // A Sim in the upper left and one in the lower right, each one of the pictures listed in the
        // menu's table of constants (kUpperLeft, kLowerRight); which one the game picks is not known yet.
        std::mt19937 random(static_cast<unsigned>(SDL_GetTicksNS()));
        for (const auto& line : screen.childTexts(screen.find(menu, kConstants))) {
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
            if (auto picture = data.image(kImages, pictures[random() % pictures.size()])) {
                screen.setImage(screen.find(menu, target), std::move(*picture));
            }
        }
        // A tile per neighborhood in the thumbnail strip, three to a page, 4 apart, the row centered in
        // the strip both ways (as the game shows them).
        const auto hoods = ts2::content::listNeighborhoods(options.user / "Neighborhoods");
        std::printf("%zu neighborhoods\n", hoods.size());
        const int thumbs = screen.find(menu, kThumbs);
        std::vector<int> tiles;
        for (int i = 0; i < kPerPage; ++i) {
            tiles.push_back(screen.load(kScripts, kNeighborhoodTile, 0, 0, thumbs));
        }
        const int tileWidth = screen.width(tiles[0]), tileHeight = screen.height(tiles[0]);
        const int left = (screen.width(thumbs) - (kPerPage * tileWidth + (kPerPage - 1) * kTileGap)) / 2;
        for (int i = 0; i < kPerPage; ++i) {
            screen.move(tiles[static_cast<std::size_t>(i)], left + i * (tileWidth + kTileGap), (screen.height(thumbs) - tileHeight) / 2);
        }
        // Each tile plays its neighborhood's moving picture (<hood>_Neighborhood.reia) over and over,
        // from its first frame when the page is shown (not known yet).
        // The videos are read in the background as the menu opens and kept, so a page shows at once.
        std::vector<std::shared_future<std::vector<std::uint8_t>>> videos;
        for (const auto& hood : hoods) {
            const auto path = hood.folder / (hood.folder.filename().u8string() + u8"_Neighborhood.reia");
            videos.push_back(std::async(std::launch::async, [path] {
                std::ifstream in(path, std::ios::binary);
                return std::vector<std::uint8_t>{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            }).share());
        }
        struct Playing {
            ts2::reia::Video video;
            ts2::tga::Pixels picture;
            std::size_t frame = 0;
            int window = -1;
        };
        std::vector<Playing> playing(kPerPage);
        int page = 0;
        const int pages = std::max(1, (static_cast<int>(hoods.size()) + kPerPage - 1) / kPerPage);
        Uint64 pageShown = SDL_GetTicksNS();
        auto showPage = [&] {
            for (int i = 0; i < kPerPage; ++i) {
                const std::size_t n = static_cast<std::size_t>(page * kPerPage + i);
                const int tile = tiles[static_cast<std::size_t>(i)];
                Playing& p = playing[static_cast<std::size_t>(i)];
                p = {};
                screen.setVisible(tile, n < hoods.size());
                if (n >= hoods.size()) {
                    continue;
                }
                screen.setText(screen.find(tile, kTileName), hoods[n].name);
                screen.setTip(screen.find(tile, kTileButton), data.text(kNeighborhoodText, kTileTip));
                p.window = screen.find(tile, kTilePicture);
                p.video = ts2::reia::parse(videos[n].get());
                if (!p.video.frames.empty() && ts2::reia::decodeFrame(p.video, 0, p.picture)) {
                    screen.setImage(p.window, p.picture);
                } else if (auto picture = readImage(hoods[n].thumbnail)) {
                    screen.setImage(p.window, std::move(*picture));
                }
            }
            screen.setVisible(screen.find(menu, kPrevious), page > 0);
            screen.setVisible(screen.find(menu, kNext), page + 1 < pages);
            pageShown = SDL_GetTicksNS();
        };
        showPage();

        // The game's 3D assets, read in the background from the start so a neighborhood opens quickly.
        std::shared_future<std::shared_ptr<ts2::scene::Assets>> assets =
            std::async(std::launch::async, [install] { return std::make_shared<ts2::scene::Assets>(install); }).share();
        // A neighborhood being built in the background, then shown: its scene, camera and panel.
        struct Built {
            ts2::scene::Scene scene;
            ts2::scene::NeighborhoodSceneStats stats;
        };
        std::future<Built> building;
        bool inNeighborhood = false;
        // The neighborhood kept while a lot is shown, to go back to it; the lot being built, then shown.
        Built hood;
        app::Orbit hoodOrbit;
        struct LotBuilt {
            ts2::scene::Scene lot, skirt;
            ts2::scene::SkirtArea area;
        };
        std::future<LotBuilt> lotBuilding;
        ts2::scene::Scene lot;
        bool inLot = false;
        app::LotView lotView;
        // The lot's Sims, run by their own scripts and posed each frame (from when the lot was shown,
        // without a simulation).
        Uint64 lotShown = 0;
        std::future<std::unique_ptr<ts2::sim::LotSimulation>> lotSimBuilding;  // built beside the lot's scene
        std::unique_ptr<app::LotLife> lotLife;
        ts2::scene::ScenePose lotPose;
        // The live-mode panels over the lot; the options menu pauses the game while it's open.
        app::LotUi lotUi(screen, width, height);
        bool pausedBeforeOptions = false;
        float downX = 0, downY = 0;
        app::Orbit orbit;
        app::NeighborhoodUi hoodUi(screen, data, width, height);
        std::string openName;
        std::filesystem::path openFolder;

        // A neighborhood's preview, over the menu where the menu is: its name, description and
        // picture, and Play.
        const int preview = screen.load(kScripts, kPreview, mx, my);
        screen.setVisible(preview, false);
        std::size_t chosen = 0;
        auto showPreview = [&] {
            const auto& hood = hoods[chosen];
            screen.setText(screen.find(preview, kPreviewName), hood.name);
            screen.setText(screen.find(preview, kPreviewDescription), hood.description);
            if (auto picture = readImage(hood.thumbnail)) {
                screen.setImage(screen.find(preview, kPreviewPicture), std::move(*picture));
            }
            screen.setVisible(preview, true);
            screen.setModal(preview);
        };
        auto play = [&] {
            const double seconds = static_cast<double>(SDL_GetTicksNS() - pageShown) / 1e9;
            for (auto& p : playing) {
                if (p.video.frames.empty()) {
                    continue;
                }
                const auto count = p.video.frames.size();
                const auto at = static_cast<std::size_t>(seconds * p.video.rateNumerator / p.video.rateDenominator) % count;
                if (at == p.frame) {
                    continue;
                }
                if (at < p.frame) {  // round again: frames build on the ones before
                    p.picture = {};
                    p.frame = 0;
                    ts2::reia::decodeFrame(p.video, 0, p.picture);
                }
                while (p.frame < at) {
                    ts2::reia::decodeFrame(p.video, ++p.frame, p.picture);
                }
                screen.setImage(p.window, p.picture);
            }
        };

        // Only textures added or changed since the last frame go to the GPU.
        std::vector<ts2::scene::Image> staged;
        auto frame = [&] {
            const auto changed = screen.takeChangedTextures();
            staged.clear();
            staged.reserve(changed.size());
            std::vector<std::pair<int, const ts2::scene::Image*>> upload;
            for (const int t : changed) {
                const auto& px = screen.textures()[static_cast<std::size_t>(t)];
                staged.push_back({px.width, px.height, px.rgba});
            }
            for (std::size_t i = 0; i < changed.size(); ++i) {
                upload.emplace_back(changed[i], &staged[i]);
            }
            renderer.setSprites(upload);
            renderer.showSprites(sprites(screen, static_cast<double>(SDL_GetTicksNS()) / 1e9), inNeighborhood || inLot);
        };
        // Whether a background job is still running (a finished one can be replaced without waiting).
        auto loading = [](const auto& job) { return job.valid() && job.wait_for(std::chrono::seconds(0)) != std::future_status::ready; };
        // A background job's result once it's done. One that failed (a broken lot or neighborhood file)
        // is reported and leaves everything as it was; taking a picture, it ends the program instead.
        auto finished = [&]<typename T>(std::future<T>& job, const char* what) -> std::optional<T> {
            if (!job.valid() || loading(job)) {
                return std::nullopt;
            }
            try {
                return job.get();
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "couldn't load the %s: %s\n", what, ex.what());
                if (!options.shot.empty()) {
                    throw;
                }
                return std::nullopt;
            }
        };
        // Builds the chosen neighborhood in the background; it shows once built.
        auto open = [&] {
            if (building.valid()) {
                return;
            }
            const auto folder = hoods[chosen].folder;
            openName = hoods[chosen].name;
            openFolder = folder;
            std::printf("opening %s\n", folder.filename().string().c_str());
            building = std::async(std::launch::async, [assets, install, folder] {
                ts2::scene::SceneBuilder builder(*assets.get());
                Built b;
                b.stats = app::buildNeighborhood(builder, install, folder);
                b.scene = std::move(builder.scene());
                return b;
            });
        };
        // Builds a lot of the open neighborhood in the background; it shows once built.
        std::uint32_t enteredLot = 0;
        auto enterLot = [&](std::uint32_t id) {
            enteredLot = id;
            const auto file = openFolder / "Lots" / (openFolder.filename().u8string() + u8"_Lot" + std::u8string(reinterpret_cast<const char8_t*>(std::to_string(id).c_str())) + u8".package");
            std::printf("entering lot %u\n", id);
            // The ground around it, as in the game's lot view.
            const auto where = std::find_if(hood.stats.lotFootprints.begin(), hood.stats.lotFootprints.end(),
                                            [&](const ts2::scene::LotFootprint& f) { return f.id == id; });
            std::optional<ts2::scene::LotFootprint> footprint;
            if (where != hood.stats.lotFootprints.end()) {
                footprint = *where;
            }
            const ts2::scene::Scene* hoodScene = &hood.scene;
            const std::string terrainType = hood.stats.terrainType;
            lotSimBuilding = std::async(std::launch::async, [install, file] { return app::loadSimulation(install, file); });
            lotBuilding = std::async(std::launch::async, [assets, file, install, footprint, hoodScene, terrainType] {
                LotBuilt built;
                {
                    ts2::scene::SceneBuilder builder(*assets.get());
                    ts2::scene::addLot(builder, file);
                    built.lot = std::move(builder.scene());
                }
                if (footprint && hoodScene->terrain) {
                    const int margin = app::lotSkirtMargin(install);
                    built.area = ts2::scene::lotSkirtArea(*hoodScene->terrain, *footprint, margin);
                    ts2::scene::SceneBuilder builder(*assets.get());
                    ts2::scene::addLotSkirt(builder, *hoodScene->terrain, *footprint, margin, terrainType,
                                            ts2::content::lotSkirtRoadTextures(ts2::content::findProducts(install)));
                    built.skirt = std::move(builder.scene());
                }
                return built;
            });
        };
        if (options.play >= 0 && static_cast<std::size_t>(options.play) < hoods.size()) {
            chosen = static_cast<std::size_t>(options.play);
            open();
        }
        if (options.preview >= 0 && static_cast<std::size_t>(options.preview) < hoods.size()) {
            chosen = static_cast<std::size_t>(options.preview);
            showPreview();
        }
        if (options.hoverX >= 0) {
            screen.mouse(options.hoverX, options.hoverY, false, 0);
        }
        if (!options.shot.empty() && !building.valid()) {
            frame();
            renderer.showSprites(sprites(screen, options.hoverX >= 0 ? 10.0 : 0.0), false);
            if (!renderer.drawToFile({}, static_cast<unsigned>(width), static_cast<unsigned>(height), options.shot)) {
                throw std::runtime_error("couldn't save the picture");
            }
            std::printf("wrote %s\n", options.shot.string().c_str());
        }
        // Back to the neighborhood, as it was.
        auto leaveLot = [&] {
            inLot = false;
            lot = {};
            lotLife.reset();
            lotUi.close();
            renderer.upload(hood.scene);
            orbit = hoodOrbit;
            screen.setVisible(hoodUi.root(), true);
            inNeighborhood = true;
        };
        // The game's live-mode keys: P, Pause, ` or 0 on the number pad pause; 1, 2 and 3 pick a
        // speed; F5 opens the options.
        auto lotKey = [&](SDL_Keycode key) {
            using Action = app::LotUi::Action;
            switch (key) {
            case SDLK_P:
            case SDLK_PAUSE:
            case SDLK_GRAVE:
            case SDLK_KP_0: return Action::Pause;
            case SDLK_1:
            case SDLK_KP_1: return Action::Normal;
            case SDLK_2:
            case SDLK_KP_2: return Action::High;
            case SDLK_3:
            case SDLK_KP_3: return Action::Ultra;
            case SDLK_F5:
                lotUi.showOptions(!lotUi.optionsShown());
                return lotUi.optionsShown() ? Action::OptionsOpened : Action::OptionsClosed;
            default: return Action::None;
            }
        };
        auto lotAction = [&](app::LotUi::Action action) {
            using Action = app::LotUi::Action;
            if (action == Action::Neighborhood) {
                leaveLot();
                return;
            }
            if (!lotLife) {
                return;
            }
            switch (action) {
            case Action::Pause: lotLife->setPaused(!lotLife->paused()); break;
            case Action::Normal: lotLife->setSpeed(ts2::sim::Speed::Normal); break;
            case Action::High: lotLife->setSpeed(ts2::sim::Speed::High); break;
            case Action::Ultra: lotLife->setSpeed(ts2::sim::Speed::Ultra); break;
            case Action::OptionsOpened:
                pausedBeforeOptions = lotLife->paused();
                lotLife->setPaused(true);
                break;
            case Action::OptionsClosed: lotLife->setPaused(pausedBeforeOptions); break;
            default: break;
            }
        };

        bool running = options.shot.empty() || building.valid();
        bool down = false;
        Uint64 lastFrame = SDL_GetTicksNS();
        while (running) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE && inLot) {
                    leaveLot();
                } else if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) {
                    running = false;
                } else if (e.type == SDL_EVENT_KEY_DOWN && inLot) {
                    if (const auto action = lotKey(e.key.key); action != app::LotUi::Action::None) {
                        lotAction(action);
                    } else {
                        lotView.key(e.key.key, orbit);
                    }
                } else if (e.type == SDL_EVENT_MOUSE_WHEEL && (inNeighborhood || inLot)) {
                    app::orbitEvent(orbit, e);
                } else if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    float x = 0, y = 0;
                    SDL_GetMouseState(&x, &y);
                    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                        down = true;
                        downX = x;
                        downY = y;
                    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
                        down = false;
                        // A click (not a drag) on the neighborhood enters the lot under it.
                        // Not while a lot (or the last one's Sims) is still loading.
                        if (inNeighborhood && !lotBuilding.valid() && !loading(lotSimBuilding) && std::abs(x - downX) < 4 &&
                            std::abs(y - downY) < 4 &&
                            !screen.over(static_cast<int>(x), static_cast<int>(y))) {
                            // Only a lot a household lives on can be played.
                            const auto id = lotAt(orbit.camera(), width, height, x, y, hood.stats.lotFootprints);
                            const auto& lived = hoodUi.occupiedLots();
                            if (id && std::find(lived.begin(), lived.end(), id) != lived.end()) {
                                enterLot(id);
                            }
                        }
                    }
                    if (inLot) {
                        if (!screen.over(static_cast<int>(x), static_cast<int>(y))) {
                            app::orbitEvent(orbit, e);
                        }
                        if (const auto clicked = screen.mouse(static_cast<int>(x), static_cast<int>(y), down, static_cast<double>(SDL_GetTicksNS()) / 1e9)) {
                            lotAction(lotUi.clicked(*clicked));
                        }
                        continue;
                    }
                    if (inNeighborhood && !screen.over(static_cast<int>(x), static_cast<int>(y))) {
                        app::orbitEvent(orbit, e);
                    }
                    if (const auto clicked = screen.mouse(static_cast<int>(x), static_cast<int>(y), down, static_cast<double>(SDL_GetTicksNS()) / 1e9)) {
                        const std::uint32_t id = screen.id(*clicked);
                        std::printf("clicked 0x%08x\n", id);
                        const auto tile = std::find(tiles.begin(), tiles.end(), screen.parent(*clicked));
                        if (inNeighborhood) {
                            // Back to the menu, but not while a lot is loading: it reads this neighborhood.
                            if (hoodUi.clicked(*clicked) && !lotBuilding.valid()) {
                                inNeighborhood = false;
                                hoodUi.close();
                                screen.setVisible(menu, true);
                                screen.setVisible(preview, false);
                                screen.setModal(-1);
                                renderer.setClearColor(1 / 255.f, 35 / 255.f, 62 / 255.f);
                            }
                        } else if (screen.visible(preview)) {
                            if (id == kPreviewClose) {
                                screen.setVisible(preview, false);
                                screen.setModal(-1);
                            } else if (id == kPreviewPlay) {
                                open();
                            }
                        } else if (id == kQuit) {
                            running = false;
                        } else if (id == kNext && page + 1 < pages) {
                            ++page;
                            showPage();
                        } else if (id == kPrevious && page > 0) {
                            --page;
                            showPage();
                        } else if (id == kTileButton && tile != tiles.end()) {
                            chosen = static_cast<std::size_t>(page * kPerPage + (tile - tiles.begin()));
                            showPreview();
                        }
                    }
                }
            }
            // A neighborhood finished building: show it with its panel, the menu put away.
            if (auto opened = finished(building, "neighborhood")) {
                hood = std::move(*opened);
                // Nothing else uses the assets now (no lot loads while a neighborhood does), so the
                // problems noted while loading the last neighborhood can go.
                assets.get()->warnings.clear();
                std::printf("neighborhood %u x %u tiles, %zu lots, %zu plants\n", hood.stats.width, hood.stats.height, hood.stats.imposters,
                            hood.stats.plantsShown);
                renderer.upload(hood.scene);
                orbit = {};
                app::neighborhoodCamera(orbit, install, hood.stats);
                hoodUi.open(openFolder, openName, options.user);
                if (options.bin == "families") {
                    hoodUi.showBin(app::NeighborhoodUi::Bin::Families);
                } else if (options.bin == "lots") {
                    hoodUi.showBin(app::NeighborhoodUi::Bin::Lots);
                }
                screen.setVisible(menu, false);
                screen.setVisible(preview, false);
                screen.setModal(-1);
                renderer.setClearColor(0, 0, 0);
                inNeighborhood = true;
                if (options.lot > 0) {
                    enterLot(static_cast<std::uint32_t>(options.lot));
                } else if (options.clickX >= 0) {
                    const auto id = lotAt(orbit.camera(), width, height, static_cast<float>(options.clickX), static_cast<float>(options.clickY),
                                          hood.stats.lotFootprints);
                    std::printf("clicked lot %u\n", id);
                    if (id) {
                        enterLot(id);
                    }
                }
            }
            // A lot finished building: show it instead of the neighborhood.
            if (auto done = finished(lotBuilding, "lot")) {
                LotBuilt& built = *done;
                lot = std::move(built.lot);
                hoodOrbit = orbit;
                lotView.start(lot, orbit);
                // Shown in its neighborhood, where its imposter was.
                const auto where = std::find_if(hood.stats.lotFootprints.begin(), hood.stats.lotFootprints.end(),
                                                [&](const ts2::scene::LotFootprint& f) { return f.id == enteredLot; });
                if (where != hood.stats.lotFootprints.end()) {
                    lot = app::lotInNeighborhood(hood.scene, lot, built.skirt, *where, built.area);
                    app::placeCamera(orbit, *where);
                    orbit.minFar = 2000;
                }
                renderer.upload(lot);
                lotShown = SDL_GetTicksNS();

                screen.setVisible(hoodUi.root(), false);
                hoodUi.showBin(app::NeighborhoodUi::Bin::None);
                lotUi.open();
                inNeighborhood = false;
                inLot = true;
            }
            const Uint64 now = SDL_GetTicksNS();
            if (inLot) {
                app::orbitStep(orbit, std::min(0.1f, static_cast<float>(now - lastFrame) / 1e9f));
                lotView.view.facing = orbit.facing();
                renderer.show(ts2::scene::visibleInstances(lot, lotView.view));
                // The Sims come to life once their simulation is built.
                if (!lotLife) {
                    if (auto sim = finished(lotSimBuilding, "lot's simulation"); sim && *sim) {
                        lotLife = std::make_unique<app::LotLife>(std::move(*sim), *assets.get());
                    }
                }
                if (lotLife) {
                    lotLife->update(std::min(100.0, static_cast<double>(now - lastFrame) / 1e6), lot);
                    lotUi.show(lotLife->clockText(), lotLife->speed(), lotLife->paused(), lotLife->funds());
                    lotUi.showSim(lotLife->aspiration());
                }
                app::animateActors(lot, lotLife ? lotLife->seconds() : static_cast<double>(now - lotShown) / 1e9, renderer, lotPose);
            } else if (inNeighborhood) {
                app::orbitStep(orbit, std::min(0.1f, static_cast<float>(now - lastFrame) / 1e9f));
                renderer.setTime(static_cast<float>(static_cast<double>(now) / 1e9));
            } else {
                play();
            }
            lastFrame = now;
            frame();
            if (!options.shot.empty() && (options.lot > 0 || options.clickX >= 0 ? inLot : inNeighborhood)) {
                // A picture of the neighborhood just opened.
                if (options.view) {
                    orbit.yaw = orbit.yawGoal = (*options.view)[0];
                    orbit.pitch = (*options.view)[1];
                    orbit.distance = (*options.view)[2];
                }
                if (!renderer.drawToFile(orbit.camera(), static_cast<unsigned>(width), static_cast<unsigned>(height), options.shot)) {
                    throw std::runtime_error("couldn't save the picture");
                }
                std::printf("wrote %s\n", options.shot.string().c_str());
                running = false;
            } else if (!renderer.draw(inNeighborhood || inLot ? orbit.camera() : ts2::render::Camera{})) {
                SDL_Delay(16);
            }
        }
        // Loads still running are waited for as they go out of scope; the window goes first so that
        // quitting doesn't look frozen meanwhile.
        SDL_HideWindow(window);
    }
    SDL_DestroyWindow(window);
    return 0;
}
