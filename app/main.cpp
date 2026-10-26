// NeoTS2: opens a saved lot in a window, or shows the game's main menu.
//   neots2 <install folder> --menu [--user <user folder>] [--size w h] [--preview n]
//          [--play n [--bin families|lots|options|chooser|quit] [--lot id] [--click x y] [--card x y]]
//          [--hover x y] [--ui-click x y | --ui-right-click x y | --ui-hover x y
//          (on the lot's panels before the picture, repeatable)]
//          [--shot <out.png>] [--seed n (the neighborhood ground's variants, so pictures repeat)]
//          [--renderer sdlgpu|gl2 (what draws: SDL's GPU API, or OpenGL 2.1)]
//          [--smooth-follow (the camera glides after a followed Sim, not in the original game)]
//   neots2 <install folder> <lot.package> [--shot <out.png> [yaw pitch distance]]
//          [--seed n (a neighborhood's ground variants, so pictures repeat)]
//          [--renderer sdlgpu|gl2 (what draws: SDL's GPU API, or OpenGL 2.1)]
//          [--walls up|cutaway|down] [--level n (1: the ground floor)] [--target x y z] [--roofs off]
//          [--time seconds (into the Sims' animations, for --shot)]
//          [--choose object choice (the selected Sim's pick from the object's menu, by its place)]
// Mouse: middle-drag turns, right-drag pans, the wheel zooms. Keys: WASD or arrows pan, Q/E or
// comma/period turn a quarter, Z/X or +/- zoom, Page Up/Down change storey, 1/2/3 or Tab walls
// up/cutaway/down, R roofs on/off. --shot draws one frame to a file and exits.
#include "content/cameras.hpp"
#include "content/install.hpp"
#include "content/lighting.hpp"
#include "content/lighting_state.hpp"
#include "content/lot.hpp"
#include "content/material_scripts.hpp"
#include "content/save_folder.hpp"
#include "diag/log.hpp"
#include "diag/timing.hpp"
#include "diagnostics.hpp"
#include "formats/nhood_paint_rules.hpp"
#include "menu.hpp"
#include "message_box.hpp"
#include "open_renderer.hpp"
#include "neighborhood.hpp"
#include "lot_life.hpp"
#include "lot_daylight.hpp"
#include "lot_light.hpp"
#include "lot_view.hpp"
#include "orbit.hpp"
#include "paths/game_folders.hpp"
#include "paths/names.hpp"
#include "paths/save_folders.hpp"
#include "render/make_renderer.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/lot_scene.hpp"
#include "scene/neighborhood_scene.hpp"
#include "scene/view.hpp"
#include "sim/clock.hpp"

#include "platform/entry.hpp"
#include "platform/platform.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

using app::kPi;

// A built executable keeps its shaders beside itself. A macOS app keeps them in Resources, where
// they are data rather than code inside Contents/MacOS and can be signed as part of the bundle.
std::filesystem::path shaderFolder() {
    const std::filesystem::path beside = ts2::platform::basePath() / "shaders";
#ifdef __APPLE__
    const std::filesystem::path resources = ts2::platform::basePath().parent_path() / "Resources" / "shaders";
    std::error_code error;
    if (std::filesystem::is_directory(resources, error)) {
        return resources;
    }
#endif
    return beside;
}
using app::kStorey;
using app::Orbit;
using ts2::render::Vec3;
using ts2::scene::WallView;

// The platform hands main its arguments in UTF-8.
std::filesystem::path utf8Path(const char* text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text)));
}

// Takes the save folder's lock for the game. Returns why the game can't use the folder, for the
// player, or nothing when it can.
std::string takeSaveFolder(const std::filesystem::path& install, const std::filesystem::path& user,
                           std::optional<ts2::paths::SaveFolderLock>& lock) {
    const std::filesystem::path folder = ts2::paths::saveFolderPath(user);
    const std::string where = "\n\n" + ts2::paths::utf8Text(folder.empty() ? user : folder) + "\n\n";
    std::string problem;
    if (const char* refused = ts2::paths::saveFolderRefusal(user, install)) {
        problem = refused;
    } else {
        // NeoTS2 never makes the original games' "EA Games" folder. Without it there is no save
        // folder there to keep safe.
        const bool original = ts2::paths::isOriginalGamesFolder(user);
        std::error_code error;
        if (!folder.empty() && !original) {
            std::filesystem::create_directories(folder.parent_path(), error);
        }
        if (folder.empty() || !original || std::filesystem::is_directory(folder.parent_path(), error)) {
            lock.emplace(user);
        }
        if (lock && lock->busy()) {
            return "Another NeoTS2 is already playing with this save folder:" + where + "Close it first, or choose another save folder.";
        }
        if (!lock || lock->held()) {
            return {};
        }
        problem = lock->problem();
    }
    return "NeoTS2 can't use this save folder:" + where + problem + "\n\nChoose another save folder, one in your Documents for example.";
}

// Looks for damaged packages beside the game, as opening every package takes many seconds on a
// cold start. The game's frame loop takes the result once, which logs it and warns the player.
class DamageScan {
public:
    explicit DamageScan(std::filesystem::path install) : install_(std::move(install)), state_(std::make_shared<State>()) {
        // Detached, so closing never waits for it.
        std::thread([state = state_, install = install_] {
            ts2::paths::DamagedPackages found;
            try {
                found = ts2::paths::findDamagedPackages(install);
            } catch (const std::exception&) {
                return;  // out of memory: no answer
            }
            const std::lock_guard<std::mutex> guard(state->lock);
            state->result = std::move(found);
        }).detach();
    }

    // The warning for the player once the scan has found damaged files, given once. Empty otherwise.
    std::string take() {
        ts2::paths::DamagedPackages damaged;
        {
            const std::lock_guard<std::mutex> guard(state_->lock);
            if (!state_->result || taken_) {
                return {};
            }
            damaged = std::move(*state_->result);
        }
        taken_ = true;
        if (damaged.count == 0) {
            return {};
        }
        std::string examples;
        for (const auto& file : damaged.examples) {
            examples += "\n" + ts2::paths::utf8Text(file.lexically_relative(install_));
        }
        ts2::diag::warning(std::format("{} of the install's packages are damaged or incomplete:{}", damaged.count, examples));
        return std::format("{} of the game's files are damaged or incomplete, for example:\n{}\n\nRepair or reinstall the game, "
                           "then start NeoTS2 again. NeoTS2 goes on now, but some of the game may be missing.",
                           damaged.count, examples);
    }

private:
    struct State {
        std::mutex lock;
        std::optional<ts2::paths::DamagedPackages> result;
    };
    std::filesystem::path install_;
    std::shared_ptr<State> state_;
    bool taken_ = false;
};

// Writes the packs found in the install and the folder each is in, so a player's log shows a pack
// that is missing or in a folder NeoTS2 doesn't know. Returns a warning for the player when files
// are still downloading, or nothing.
std::string checkInstallFiles(const std::filesystem::path& install) {
    std::string warning;
    try {
        const auto products = ts2::content::findProducts(install);
        std::string list;
        for (const auto& product : products) {
            std::filesystem::path folder = product.objectPackages.front().parent_path();
            while (folder.has_relative_path() && ts2::paths::foldedName(folder.filename()) != "tsdata") {
                folder = folder.parent_path();
            }
            list += std::format("{}{} ({})", list.empty() ? "" : ", ", product.code == "?" ? "unknown, left out" : product.name,
                                ts2::paths::utf8Text(folder.parent_path().lexically_relative(install)));
        }
        ts2::diag::info(std::format("{} products in {}: {}", products.size(), ts2::paths::utf8Text(install), list));
        if (ts2::paths::checkInstall(install).downloading) {
            ts2::diag::warning("some of the install's files are still downloading (_DiP_Staged), so their packs are missing");
            warning = "Some of the game's files are still downloading. Let the download finish, then start NeoTS2 again.";
        }
    } catch (const std::exception& ex) {
        ts2::diag::warning(std::string("the install's packs can't be listed: ") + ex.what());
    }
    return warning;
}

const char* wallViewName(WallView v) {
    return v == WallView::Up ? "up" : v == WallView::Cutaway ? "cutaway" : "down";
}

struct Options {
    const char* shot = nullptr;
    bool camera = false;
    float yaw = 0, pitch = 0, distance = 0;
    WallView walls = WallView::Up;
    bool target = false;
    Vec3 at{0, 0, 0};  // what the camera looks at, instead of the lot's middle
    int level = 0;  // counted from 1 for the ground floor, 0 for the top storey
    bool roofs = true;
    float fov = 0;  // degrees, up and down, and 0 keeps the default
    int shotWidth = 1280, shotHeight = 800;
    double time = 0;  // seconds into the Sims' animations a shot shows
    // An object's menu choice made for the selected Sim as the lot comes to life (--choose: object id,
    // the choice's place in the menu).
    std::optional<std::array<int, 2>> choose;
    bool bumpMapping = true;
    bool wallBumps = true;
    int bench = 0;  // frames to time before the shot (--bench)
    std::string lightState;  // a lighting state to light the lot with, in place of its clock's
    int hour = -1, minute = 0;  // the lot's clock to start from (--clock), -1 for the saved one
    std::optional<std::uint32_t> seed;  // the neighborhood ground's variants (--seed), random when empty
    ts2::render::Backend backend = ts2::render::defaultBackend();  // what draws (--renderer)
};

bool parseOptions(int argc, char** argv, Options& o) {
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--shot" && i + 1 < argc) {
            o.shot = argv[++i];
            if (i + 3 < argc && std::string_view(argv[i + 1]).substr(0, 2) != "--") {
                o.camera = true;
                o.yaw = std::stof(argv[i + 1]);
                o.pitch = std::stof(argv[i + 2]);
                o.distance = std::stof(argv[i + 3]);
                i += 3;
            }
        } else if (arg == "--walls" && i + 1 < argc) {
            const std::string_view v = argv[++i];
            if (v == "up") {
                o.walls = WallView::Up;
            } else if (v == "cutaway") {
                o.walls = WallView::Cutaway;
            } else if (v == "down") {
                o.walls = WallView::Down;
            } else {
                return false;
            }
        } else if (arg == "--target" && i + 3 < argc) {
            o.target = true;
            o.at = {std::stof(argv[i + 1]), std::stof(argv[i + 2]), std::stof(argv[i + 3])};
            i += 3;
        } else if (arg == "--fov" && i + 1 < argc) {
            o.fov = std::stof(argv[++i]);
        } else if (arg == "--size" && i + 2 < argc) {
            o.shotWidth = std::stoi(argv[i + 1]);
            o.shotHeight = std::stoi(argv[i + 2]);
            i += 2;
        } else if (arg == "--roofs" && i + 1 < argc && std::string_view(argv[i + 1]) == "off") {
            o.roofs = false;
            ++i;
        } else if (arg == "--level" && i + 1 < argc) {
            o.level = std::stoi(argv[++i]);
        } else if (arg == "--time" && i + 1 < argc) {
            o.time = std::stod(argv[++i]);
        } else if (arg == "--choose" && i + 2 < argc) {
            o.choose = std::array<int, 2>{std::stoi(argv[i + 1]), std::stoi(argv[i + 2])};
            i += 2;
        } else if (arg == "--clock" && i + 2 < argc) {
            o.hour = std::clamp(std::stoi(argv[i + 1]), 0, 23);
            o.minute = std::clamp(std::stoi(argv[i + 2]), 0, 59);
            i += 2;
        } else if (arg == "--light-state" && i + 1 < argc) {
            o.lightState = argv[++i];
        } else if (arg == "--bumps" && i + 1 < argc && std::string_view(argv[i + 1]) == "off") {
            o.bumpMapping = false;
            ++i;
        } else if (arg == "--wall-bumps" && i + 1 < argc && std::string_view(argv[i + 1]) == "off") {
            o.wallBumps = false;
            ++i;
        } else if (arg == "--bench" && i + 1 < argc) {
            o.bench = std::clamp(std::stoi(argv[++i]), 0, 100000);
        } else if (arg == "--seed" && i + 1 < argc) {
            o.seed = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--renderer" && i + 1 < argc) {
            const auto backend = ts2::render::backendNamed(argv[++i]);
            if (!backend || !ts2::render::available(*backend)) {
                return false;
            }
            o.backend = *backend;
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    app::startDiagnostics(argc, argv);
    if (argc >= 3 && std::string_view(argv[2]) == "--menu") {
        MenuOptions menu;
        for (int i = 3; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--shot" && i + 1 < argc) {
                menu.shot = utf8Path(argv[++i]);
            } else if (arg == "--hover" && i + 2 < argc) {
                menu.hoverX = std::stoi(argv[i + 1]);
                menu.hoverY = std::stoi(argv[i + 2]);
                i += 2;
            } else if (arg == "--preview" && i + 1 < argc) {
                menu.preview = std::stoi(argv[++i]);
            } else if (arg == "--play" && i + 1 < argc) {
                menu.play = std::stoi(argv[++i]);
            } else if (arg == "--bin" && i + 1 < argc) {
                menu.bin = argv[++i];
            } else if (arg == "--lot" && i + 1 < argc) {
                menu.lot = std::stoi(argv[++i]);
            } else if (arg == "--view" && i + 3 < argc) {
                menu.view = std::array<float, 3>{std::stof(argv[i + 1]), std::stof(argv[i + 2]), std::stof(argv[i + 3])};
                i += 3;
            } else if (arg == "--seed" && i + 1 < argc) {
                menu.seed = static_cast<std::uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--renderer" && i + 1 < argc) {
                const auto backend = ts2::render::backendNamed(argv[++i]);
                if (!backend || !ts2::render::available(*backend)) {
                    const bool both = ts2::render::available(ts2::render::Backend::SdlGpu) && ts2::render::available(ts2::render::Backend::OpenGL2);
                    std::fprintf(stderr, "Unknown renderer: use %s\n",
                                 both ? "sdlgpu or gl2" : ts2::render::available(ts2::render::Backend::OpenGL2) ? "gl2" : "sdlgpu");
                    return 1;
                }
                menu.backend = *backend;
            } else if (arg == "--click" && i + 2 < argc) {
                menu.clickX = std::stoi(argv[i + 1]);
                menu.clickY = std::stoi(argv[i + 2]);
                i += 2;
            } else if (arg == "--card" && i + 2 < argc) {
                menu.cardX = std::stoi(argv[i + 1]);
                menu.cardY = std::stoi(argv[i + 2]);
                i += 2;
            } else if (arg == "--ui-click" && i + 2 < argc) {
                menu.uiClicks.push_back({std::stoi(argv[i + 1]), std::stoi(argv[i + 2]), 0});
                i += 2;
            } else if (arg == "--ground-click" && i + 3 < argc) {
                menu.groundClick = std::array<int, 3>{std::stoi(argv[i + 1]), std::stoi(argv[i + 2]), std::stoi(argv[i + 3])};
                i += 3;
            } else if (arg == "--ui-hover" && i + 2 < argc) {
                menu.uiClicks.push_back({std::stoi(argv[i + 1]), std::stoi(argv[i + 2]), 2});
                i += 2;
            } else if (arg == "--ui-right-click" && i + 2 < argc) {
                menu.uiClicks.push_back({std::stoi(argv[i + 1]), std::stoi(argv[i + 2]), 1});
                i += 2;
            } else if (arg == "--user" && i + 1 < argc) {
                menu.user = utf8Path(argv[++i]);
            } else if (arg == "--add-neighborhoods") {
                menu.addNeighborhoods = true;
            } else if (arg == "--bumps" && i + 1 < argc && std::string_view(argv[i + 1]) == "off") {
                menu.bumpMapping = false;
                ++i;
            } else if (arg == "--wall-bumps" && i + 1 < argc && std::string_view(argv[i + 1]) == "off") {
                menu.wallBumps = false;
                ++i;
            } else if (arg == "--run" && i + 1 < argc) {
                menu.runSeconds = std::stod(argv[++i]);
            } else if (arg == "--unpaused") {
                menu.unpaused = true;
            } else if (arg == "--smooth-follow") {
                menu.smoothFollow = true;
            } else if (arg == "--timings") {
                menu.timings = true;
            } else if (arg == "--size" && i + 2 < argc) {
                menu.width = std::stoi(argv[i + 1]);
                menu.height = std::stoi(argv[i + 2]);
                i += 2;
            }
        }
        if (!ts2::platform::init()) {
            ts2::diag::error(std::string("SDL: ") + ts2::platform::lastError());
            return 1;
        }
        if (menu.user.empty()) {
            // The game keeps its user files in Documents.
            if (const auto documents = ts2::platform::documentsFolder(); !documents.empty()) {
                menu.user = documents / u8"EA Games" / u8"The Sims™ 2 Ultimate Collection";
            }
        }
        int result = 0;
        if (const std::string warning = checkInstallFiles(utf8Path(argv[1])); !warning.empty() && menu.shot.empty()) {
            app::showMessage(app::MessageKind::Warning, warning);
        }
        auto damageScan = std::make_shared<DamageScan>(utf8Path(argv[1]));
        menu.takeInstallWarning = [damageScan] { return damageScan->take(); };
        try {
            // Held until the game ends, so two NeoTS2s never play with the same save folder.
            std::optional<ts2::paths::SaveFolderLock> lock;
            if (!menu.user.empty()) {
                if (const std::string message = takeSaveFolder(utf8Path(argv[1]), menu.user, lock); !message.empty()) {
                    ts2::diag::error(message);
                    if (menu.shot.empty()) {
                        app::showMessage(app::MessageKind::Error, message);
                    }
                    ts2::platform::quit();
                    return 1;
                }
            }
            const auto fill = lock ? ts2::content::fillNewSaveFolder(utf8Path(argv[1]), *lock) : ts2::content::SaveFolderFill{};
            if (fill.filled) {
                ts2::diag::info(std::format("Filled the new save folder with the packs' content ({} files, {} MB)", fill.files,
                                            fill.bytes >> 20));
            } else if (!fill.problem.empty()) {
                ts2::diag::error("The new save folder wasn't filled: " + fill.problem);
                if (menu.shot.empty()) {
                    const std::string message = "This save folder has no neighborhoods yet, and NeoTS2 couldn't add them: " + fill.problem;
                    app::showMessage(app::MessageKind::Warning, message);
                }
            }
            if (lock && menu.addNeighborhoods) {
                const auto added = ts2::content::addShippedNeighborhoods(utf8Path(argv[1]), *lock);
                if (added.filled) {
                    ts2::diag::info(std::format("Added the install's neighborhoods to the save folder ({} files, {} MB)", added.files,
                                                added.bytes >> 20));
                } else if (!added.problem.empty()) {
                    ts2::diag::error("The neighborhoods weren't added: " + added.problem);
                    if (menu.shot.empty()) {
                        const std::string message = "NeoTS2 couldn't add the neighborhoods to this save folder: " + added.problem;
                        app::showMessage(app::MessageKind::Warning, message);
                    }
                }
            }
            result = runMenu(utf8Path(argv[1]), shaderFolder(), menu);
        } catch (const std::exception& ex) {
            ts2::diag::error(ex.what());
            result = 1;
            // Said to the player too, so the game never just disappears.
            if (menu.shot.empty()) {
                const std::string message = std::string("NeoTS2 had to close.\n\n") + ex.what() + "\n\nMore details are in the log in " +
                                            ts2::paths::utf8Text(ts2::diag::file().parent_path()) + ".";
                app::showMessage(app::MessageKind::Error, message);
            }
        }
        ts2::platform::quit();
        return result;
    }
    Options options;
    bool parsed = false;
    try {
        parsed = argc >= 3 && parseOptions(argc, argv, options);
    } catch (const std::logic_error&) {  // a number argument that isn't one, or out of range
    }
    if (!parsed) {
        std::fprintf(stderr, "Usage: neots2 <install folder> <lot.package> [--shot <out.png> [yaw pitch distance]]\n"
                             "              [--walls up|cutaway|down] [--level n] [--target x y z] [--roofs off] [--bench frames]\n"
                             "              [--light-state <name>]\n");
        return 1;
    }
    const bool shot = options.shot != nullptr;
    if (!ts2::platform::init()) {
        ts2::diag::error(std::string("SDL: ") + ts2::platform::lastError());
        return 1;
    }
    int result = 0;
    try {
        ts2::diag::info("loading the game's assets");
        ts2::scene::Assets assets(utf8Path(argv[1]));
        ts2::scene::SceneBuilder builder(assets);
        // A lot's Sims, run by their own scripts.
        std::unique_ptr<app::LotLife> life;
        // What the lot was lit with, for lighting it again as its clock moves on.
        std::shared_ptr<app::LotLightBase> lightBase;
        std::string lightState;
        // A neighborhood's folder shows the neighborhood, and a lot package the lot.
        const bool hood = std::filesystem::is_directory(utf8Path(argv[2]));
        ts2::scene::NeighborhoodSceneStats hoodStats;
        if (hood) {
            ts2::content::EffectsFiles effects(utf8Path(argv[1]));
            hoodStats = app::buildNeighborhood(builder, utf8Path(argv[1]), utf8Path(argv[2]), effects, options.seed);
            ts2::diag::info(std::format("neighborhood {} x {} tiles, {}, sea level {:.2f}, {} lots ({} imposters), {} plants ({} shown), {} decorations ({} shown)",
                                        hoodStats.width, hoodStats.height, hoodStats.terrainType, hoodStats.seaLevel, hoodStats.lots,
                                        hoodStats.imposters, hoodStats.plants, hoodStats.plantsShown, hoodStats.decorations,
                                        hoodStats.decorationsShown));
        } else {
            auto lot = ts2::content::loadLot(utf8Path(argv[2]));
            if (options.hour >= 0 && lot.globals.size() > ts2::sim::global::kMinute) {
                lot.globals[ts2::sim::global::kHour] = static_cast<std::int16_t>(options.hour);
                lot.globals[ts2::sim::global::kMinute] = static_cast<std::int16_t>(options.minute);
            }
            const auto stats = ts2::scene::addLot(builder, lot, utf8Path(argv[2]));
            lightState = options.lightState.empty() ? app::savedLightingState(assets.lighting(), lot) : options.lightState;
            lightBase = app::lightLot(assets, builder.scene(), lot, utf8Path(argv[2]), lightState);
            ts2::diag::info(std::format("{} objects, {} walls, {} roofs, {} terrain paints", stats.objects, stats.walls, stats.roofs,
                                        stats.terrainPaints));
            if (auto sim = app::loadSimulation(utf8Path(argv[1]), utf8Path(argv[2]))) {
                if (options.hour >= 0) {
                    sim->globals()[ts2::sim::global::kHour] = static_cast<std::int16_t>(options.hour);
                    sim->globals()[ts2::sim::global::kMinute] = static_cast<std::int16_t>(options.minute);
                    ts2::sim::updateTimeOfDay(sim->globals());
                }
                life = std::make_unique<app::LotLife>(std::move(sim), assets);
            }
        }
        auto& scene = builder.scene();
        const int top = ts2::scene::topLevel(scene);

        ts2::platform::WindowOptions windowOptions;
        windowOptions.title = "NeoTS2";
        windowOptions.width = 1280;
        windowOptions.height = 800;
        windowOptions.resizable = true;
        windowOptions.highDensity = true;
        windowOptions.hidden = shot;
        std::unique_ptr<ts2::platform::Window> window;
        const std::filesystem::path shaders = shaderFolder();
        {
            const auto rendererOwner = app::openRenderer(window, options.backend, windowOptions, shaders, shot);
            ts2::render::IRenderer& renderer = *rendererOwner;
            renderer.setBumpMapping(options.bumpMapping);
            renderer.setFloorAndWallNormalMapping(options.wallBumps);
            renderer.setNight(ts2::content::stateTime(lightState) == ts2::content::StateTime::Night);
            ts2::diag::info("drawing with " + renderer.device());
            renderer.upload(scene);
            ts2::scene::ScenePose pose;
            // A shot shows the lot `--time` seconds after it came to life.
            const std::vector<std::uint8_t> allShown;
            if (life) {
                life->traceIn(scene, pose, allShown);
                life->setPaused(false);
                if (options.choose && (*options.choose)[0] > 0 && (*options.choose)[1] >= 0) {
                    const auto object = static_cast<std::uint32_t>((*options.choose)[0]);
                    const auto choices = life->pieMenu(object);
                    if (const auto at = static_cast<std::size_t>((*options.choose)[1]); at < choices.size()) {
                        life->choose(object, choices[at]);
                    }
                }
            }
            std::unique_ptr<app::LotDaylight> daylight;
            if (life && lightBase) {
                const auto* state = ts2::content::findState(assets.lighting(), lightState);
                daylight = std::make_unique<app::LotDaylight>(lightBase, lightState, state ? state->environment : std::array<float, 3>{1, 1, 1},
                                                              ts2::scene::Transform{});
            }
            for (double t = 0; life && t < options.time * 1000; t += 100) {
                life->update(100, scene);
                if (daylight) {
                    daylight->update(0.1, life->simulation().globals(), assets, scene, renderer, *life);
                }
                if (!life->changedInstances().empty()) {
                    renderer.addMaterials(scene);
                    renderer.addMeshes(scene);
                    for (const std::size_t instance : life->changedInstances()) {
                        renderer.setInstance(scene, instance);
                    }
                }
                for (const auto& [instance, place] : life->movedObjects()) {
                    renderer.setPlace(instance, place);
                }
                for (const std::size_t instance : life->relitInstances()) {
                    renderer.setLights(scene, instance);
                }
                if (!life->changedMaterials().empty()) {
                    renderer.addMaterials(scene);
                    for (const auto& [instance, materials] : life->changedMaterials()) {
                        renderer.setMaterials(instance, materials);
                    }
                }
            }
            if (daylight) {
                daylight->settle(scene, renderer, *life);
                for (const std::size_t instance : life->relitInstances()) {
                    renderer.setLights(scene, instance);
                }
            }
            app::animateActors(scene, life ? life->seconds() : options.time, renderer, pose);
            for (const auto& actor : scene.actors) {
                if (actor.animation && actor.animation->animation) {
                    ts2::diag::info(std::format("Sim {} plays {}", actor.object, actor.animation->animation->name));
                }
            }
            const std::uint64_t started = ts2::platform::ticksNs();
            Orbit orbit;
            app::frameScene(scene, orbit);
            if (hood) {
                app::neighborhoodCamera(orbit, utf8Path(argv[1]), hoodStats);
            }
            ts2::scene::View view;
            view.walls = options.walls;
            view.roofs = options.roofs;
            // Storeys count from the ground floor, 1 up.
            const int ground = ts2::scene::groundLevel(scene);
            view.level = options.level < 1 ? top : std::clamp(ground + options.level - 1, ground, top);
            orbit.target[2] -= static_cast<float>(top - view.level) * kStorey;
            if (options.target) {
                orbit.target = options.at;
            }

            if (shot) {
                if (options.camera) {
                    orbit.yaw = options.yaw;
                    orbit.pitch = options.pitch;
                    orbit.distance = options.distance;
                }
                if (orbit.rig && options.fov <= 0) {
                    orbit.zoomTo(orbit.distance);
                }
                view.facing = orbit.facing();
                renderer.show(ts2::scene::visibleInstances(scene, view));
                auto camera = orbit.camera();
                if (options.fov > 0) {
                    camera.fovX = 0;
                    camera.fovY = options.fov * kPi / 180;
                }
                if (options.bench > 0) {
                    renderer.benchmark(camera, options.shotWidth, options.shotHeight, options.bench);
                    ts2::diag::info(std::format("{} frames timed:\n{}", options.bench, ts2::diag::takeTimings()));
                }
                if (!renderer.drawToFile(camera, options.shotWidth, options.shotHeight, utf8Path(options.shot))) {
                    throw std::runtime_error(std::string("couldn't save ") + options.shot);
                }
                ts2::diag::info(std::string("wrote ") + options.shot);
            }

            auto setTitle = [&] {
                const std::string title = "NeoTS2 - walls " + std::string(wallViewName(view.walls)) + ", storey " +
                                          std::to_string(view.level - ground + 1) + " of " + std::to_string(top - ground + 1) +
                                          (view.roofs ? "" : ", roofs off");
                window->setTitle(title);
            };
            setTitle();
            auto changeLevel = [&](int delta) {
                const int level = std::clamp(view.level + delta, ground, top);
                orbit.target[2] += static_cast<float>(level - view.level) * kStorey;
                view.level = level;
                setTitle();
            };

            bool running = !shot;
            std::uint64_t last = ts2::platform::ticksNs();
            while (running) {
                ts2::platform::Event e;
                while (ts2::platform::pollEvent(e)) {
                    using ts2::platform::Key;
                    if (e.type == ts2::platform::Event::Type::Quit || (e.type == ts2::platform::Event::Type::KeyDown && e.key == Key::Escape)) {
                        running = false;
                    } else if (e.type == ts2::platform::Event::Type::KeyDown) {
                        switch (e.key) {
                        case Key::Q:
                        case Key::Comma:
                            orbit.turn(-1);
                            break;
                        case Key::E:
                        case Key::Period:
                            orbit.turn(1);
                            break;
                        case Key::PageUp:
                            changeLevel(1);
                            break;
                        case Key::PageDown:
                            changeLevel(-1);
                            break;
                        case Key::Num1:
                        case Key::Num2:
                        case Key::Num3:
                            view.walls = static_cast<WallView>(static_cast<int>(e.key) - static_cast<int>(Key::Num1));
                            setTitle();
                            break;
                        case Key::Tab:
                            view.walls = static_cast<WallView>((static_cast<int>(view.walls) + 1) % 3);
                            setTitle();
                            break;
                        case Key::R:
                            view.roofs = !view.roofs;
                            setTitle();
                            break;
                        default:
                            break;
                        }
                    } else {
                        app::orbitEvent(orbit, e);
                    }
                }

                const std::uint64_t now = ts2::platform::ticksNs();
                const float dt = std::min(0.1f, static_cast<float>(now - last) / 1e9f);
                last = now;
                app::orbitStep(orbit, dt);

                view.facing = orbit.facing();
                renderer.show(ts2::scene::visibleInstances(scene, view));
                if (life) {
                    life->setCamera(orbit.camera().eye);
                    life->update(static_cast<double>(dt) * 1000, scene);
                }
                app::animateActors(scene, life ? life->seconds() : static_cast<double>(now - started) / 1e9, renderer, pose);
                renderer.setTime(static_cast<float>(static_cast<double>(now) / 1e9));
                if (!renderer.draw(orbit.camera())) {
                    ts2::platform::delayMs(16);
                }
            }
        }
    } catch (const std::exception& ex) {
        ts2::diag::error(ex.what());
        result = 1;
    }
    ts2::platform::quit();
    return result;
}
