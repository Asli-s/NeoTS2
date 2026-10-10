// NeoTS2: opens a saved lot in a window, or shows the game's main menu.
//   neots2 <install folder> --menu [--user <user folder>] [--size w h] [--preview n] [--play n [--bin families|lots] [--lot id] [--click x y]]
//          [--hover x y]
//          [--shot <out.png>]
//   neots2 <install folder> <lot.package> [--shot <out.png> [yaw pitch distance]]
//          [--walls up|cutaway|down] [--level n (1: the ground floor)] [--target x y z] [--roofs off]
// Mouse: middle-drag turns, right-drag pans, the wheel zooms. Keys: WASD or arrows pan, Q/E or
// comma/period turn a quarter, Z/X or +/- zoom, Page Up/Down change storey, 1/2/3 or Tab walls
// up/cutaway/down, R roofs on/off. --shot draws one frame to a file and exits.
#include "content/cameras.hpp"
#include "content/install.hpp"
#include "content/lighting.hpp"
#include "content/material_scripts.hpp"
#include "formats/nhood_paint_rules.hpp"
#include "menu.hpp"
#include "neighborhood.hpp"
#include "lot_view.hpp"
#include "orbit.hpp"
#include "render/renderer.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/lot_scene.hpp"
#include "scene/neighborhood_scene.hpp"
#include "scene/view.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using app::kPi;
using app::kStorey;
using app::Orbit;
using ts2::render::Vec3;
using ts2::scene::WallView;

// SDL hands main its arguments in UTF-8.
std::filesystem::path utf8Path(const char* text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text)));
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
    int level = 0;  // counted from 1 for the ground floor; 0: the top storey
    bool roofs = true;
    float fov = 0;  // degrees, up and down; 0 keeps the default
    int shotWidth = 1280, shotHeight = 800;
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
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
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
            } else if (arg == "--click" && i + 2 < argc) {
                menu.clickX = std::stoi(argv[i + 1]);
                menu.clickY = std::stoi(argv[i + 2]);
                i += 2;
            } else if (arg == "--user" && i + 1 < argc) {
                menu.user = utf8Path(argv[++i]);
            } else if (arg == "--size" && i + 2 < argc) {
                menu.width = std::stoi(argv[i + 1]);
                menu.height = std::stoi(argv[i + 2]);
                i += 2;
            }
        }
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
            return 1;
        }
        if (menu.user.empty()) {
            // The game keeps its user files in Documents.
            if (const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS)) {
                menu.user = utf8Path(documents) / u8"EA Games" / u8"The Sims™ 2 Ultimate Collection";
            }
        }
        int result = 0;
        try {
            result = runMenu(utf8Path(argv[1]), utf8Path(SDL_GetBasePath()) / "shaders", menu);
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "error: %s\n", ex.what());
            result = 1;
        }
        SDL_Quit();
        return result;
    }
    Options options;
    if (argc < 3 || !parseOptions(argc, argv, options)) {
        std::fprintf(stderr, "Usage: neots2 <install folder> <lot.package> [--shot <out.png> [yaw pitch distance]]\n"
                             "              [--walls up|cutaway|down] [--level n] [--target x y z] [--roofs off]\n");
        return 1;
    }
    const bool shot = options.shot != nullptr;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    int result = 0;
    try {
        std::printf("loading the game's assets...\n");
        ts2::scene::Assets assets(utf8Path(argv[1]));
        ts2::scene::SceneBuilder builder(assets);
        // A neighborhood's folder shows the neighborhood; a lot package, the lot.
        const bool hood = std::filesystem::is_directory(utf8Path(argv[2]));
        ts2::scene::NeighborhoodSceneStats hoodStats;
        if (hood) {
            hoodStats = app::buildNeighborhood(builder, utf8Path(argv[1]), utf8Path(argv[2]));
            std::printf("neighborhood %u x %u tiles, %s, sea level %.2f, %zu lots (%zu imposters), %zu plants (%zu shown)\n",
                        hoodStats.width, hoodStats.height, hoodStats.terrainType.c_str(), hoodStats.seaLevel, hoodStats.lots,
                        hoodStats.imposters, hoodStats.plants, hoodStats.plantsShown);
        } else {
            const auto stats = ts2::scene::addLot(builder, utf8Path(argv[2]));
            std::printf("%zu objects, %zu walls, %zu roofs, %zu terrain paints\n", stats.objects, stats.walls, stats.roofs,
                        stats.terrainPaints);
        }
        const auto& scene = builder.scene();
        const int top = ts2::scene::topLevel(scene);

        SDL_Window* window = SDL_CreateWindow("NeoTS2", 1280, 800,
                                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (shot ? SDL_WINDOW_HIDDEN : 0));
        if (window == nullptr) {
            throw std::runtime_error(SDL_GetError());
        }
        const std::filesystem::path shaders = utf8Path(SDL_GetBasePath()) / "shaders";
        {
            ts2::render::Renderer renderer(window, shaders);
            std::printf("drawing with %s\n", renderer.backend().c_str());
            renderer.upload(scene);
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
                if (!renderer.drawToFile(camera, options.shotWidth, options.shotHeight, utf8Path(options.shot))) {
                    throw std::runtime_error(std::string("couldn't save ") + options.shot);
                }
                std::printf("wrote %s\n", options.shot);
            }

            auto setTitle = [&] {
                const std::string title = "NeoTS2 - walls " + std::string(wallViewName(view.walls)) + ", storey " +
                                          std::to_string(view.level - ground + 1) + " of " + std::to_string(top - ground + 1) +
                                          (view.roofs ? "" : ", roofs off");
                SDL_SetWindowTitle(window, title.c_str());
            };
            setTitle();
            auto changeLevel = [&](int delta) {
                const int level = std::clamp(view.level + delta, ground, top);
                orbit.target[2] += static_cast<float>(level - view.level) * kStorey;
                view.level = level;
                setTitle();
            };

            bool running = !shot;
            Uint64 last = SDL_GetTicksNS();
            while (running) {
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) {
                        running = false;
                    } else if (e.type == SDL_EVENT_KEY_DOWN) {
                        switch (e.key.key) {
                        case SDLK_Q:
                        case SDLK_COMMA:
                            orbit.turn(-1);
                            break;
                        case SDLK_E:
                        case SDLK_PERIOD:
                            orbit.turn(1);
                            break;
                        case SDLK_PAGEUP:
                            changeLevel(1);
                            break;
                        case SDLK_PAGEDOWN:
                            changeLevel(-1);
                            break;
                        case SDLK_1:
                        case SDLK_2:
                        case SDLK_3:
                            view.walls = static_cast<WallView>(e.key.key - SDLK_1);
                            setTitle();
                            break;
                        case SDLK_TAB:
                            view.walls = static_cast<WallView>((static_cast<int>(view.walls) + 1) % 3);
                            setTitle();
                            break;
                        case SDLK_R:
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

                const Uint64 now = SDL_GetTicksNS();
                const float dt = std::min(0.1f, static_cast<float>(now - last) / 1e9f);
                last = now;
                app::orbitStep(orbit, dt);

                view.facing = orbit.facing();
                renderer.show(ts2::scene::visibleInstances(scene, view));
                renderer.setTime(static_cast<float>(static_cast<double>(now) / 1e9));
                if (!renderer.draw(orbit.camera())) {
                    SDL_Delay(16);
                }
            }
        }
        SDL_DestroyWindow(window);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        result = 1;
    }
    SDL_Quit();
    return result;
}
