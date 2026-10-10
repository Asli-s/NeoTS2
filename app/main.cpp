// NeoTS2: opens a saved lot in a window, or shows the game's main menu.
//   neots2 <install folder> --menu [--user <user folder>] [--size w h] [--preview n] [--hover x y] [--shot <out.png>]
//   neots2 <install folder> <lot.package> [--shot <out.png> [yaw pitch distance]]
//          [--walls up|cutaway|down] [--level n (1: the ground floor)] [--target x y z] [--roofs off]
// Mouse: left-drag orbits, right-drag pans, the wheel zooms. Keys: WASD or arrows pan, Q/E or
// comma/period turn a quarter, Z/X or +/- zoom, Page Up/Down change storey, 1/2/3 or Tab walls
// up/cutaway/down, R roofs on/off. --shot draws one frame to a file and exits.
#include "content/cameras.hpp"
#include "content/install.hpp"
#include "content/lighting.hpp"
#include "formats/nhood_paint_rules.hpp"
#include "menu.hpp"
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

using ts2::render::Vec3;
using ts2::scene::WallView;

constexpr float kPi = 3.14159265358979f;
constexpr float kStorey = 3.0f;  // how far the camera rises per storey

// SDL hands main its arguments in UTF-8.
std::filesystem::path utf8Path(const char* text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text)));
}

// A camera circling a point: its distance, and its angles around and above the point. Quarter
// turns ease toward `yawGoal`.
struct Orbit {
    Vec3 target{0, 0, 0};
    float distance = 30, yaw = -kPi / 4, pitch = 0.7f;
    float yawGoal = -kPi / 4;
    float minDistance = 1, maxDistance = 300;  // how near and far the camera may zoom
    // A camera from the game's camera files: when set, it gives the field of view and clipping,
    // and the pitch follows the distance.
    std::optional<ts2::camera::Config> rig;

    void zoomTo(float d) {
        distance = std::clamp(d, minDistance, maxDistance);
        if (rig) {
            pitch = ts2::camera::pitchAt(*rig, distance) * kPi / 180;
        }
    }

    ts2::render::Camera camera() const {
        ts2::render::Camera c;
        c.target = target;
        c.farPlane = std::max(500.0f, distance * 3);
        if (rig && !rig->levels.empty()) {
            const auto& level = rig->levels.front();
            c.fovX = level.fov * kPi / 180;
            c.nearPlane = level.nearClip;
            c.farPlane = level.farClip;
        }
        c.eye = {target[0] + distance * std::cos(pitch) * std::cos(yaw), target[1] + distance * std::cos(pitch) * std::sin(yaw),
                 target[2] + distance * std::sin(pitch)};
        return c;
    }

    // The way the camera looks, along the ground.
    std::array<float, 2> facing() const { return {-std::cos(yaw), -std::sin(yaw)}; }

    // Moves the target along the ground: `right` and `forward` as seen on screen.
    void pan(float right, float forward) {
        const float c = std::cos(yaw), s = std::sin(yaw);
        target[0] += -s * right - c * forward;
        target[1] += c * right - s * forward;
    }

    // The nearest view at 45 degrees to the lot's grid, `quarters` turns on.
    void turn(int quarters) {
        yawGoal = std::round((yawGoal + kPi / 4) / (kPi / 2)) * (kPi / 2) - kPi / 4 + static_cast<float>(quarters) * kPi / 2;
    }
};

// The middle and size of everything placed in the scene.
void frame(const ts2::scene::Scene& scene, Orbit& orbit) {
    Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    for (const auto& instance : scene.instances) {
        if (instance.mesh < 0 || scene.meshes[static_cast<std::size_t>(instance.mesh)].name == "terrain") {
            continue;
        }
        const auto& t = instance.transform.m;
        const Vec3 at{t[3], t[7], t[11]};
        for (std::size_t k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], at[k]);
            hi[k] = std::max(hi[k], at[k]);
        }
    }
    if (lo[0] > hi[0]) {
        return;
    }
    orbit.target = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
    orbit.distance = std::max(10.0f, 1.2f * std::max(hi[0] - lo[0], hi[1] - lo[1]));
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
            // The paint rules, from the newest product's NeighborhoodTerrain.ini.
            const auto ini = ts2::content::newestDataFile(ts2::content::findProducts(utf8Path(argv[1])), "Sys/NeighborhoodTerrain.ini");
            if (!ini) {
                throw std::runtime_error("NeighborhoodTerrain.ini was not found");
            }
            std::ifstream in(*ini, std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            // The light map's settings: the "neighbourhood" lighting state and the height map shadow tunables.
            const auto products = ts2::content::findProducts(utf8Path(argv[1]));
            const auto lighting = ts2::content::readLighting(products);
            ts2::scene::NeighborhoodLight light;
            light.shadowStrength = lighting.number("hmNHoodShadowStrength", light.shadowStrength);
            light.boxWidening = lighting.number("hmBlobWidthModifier", light.boxWidening);
            light.boxHeightScale = lighting.number("hmBlobHeightModifier", light.boxHeightScale);
            if (const auto state = lighting.states.find("neighbourhood"); state != lighting.states.end()) {
                if (const auto sun = state->second.lights.find("sun"); sun != state->second.lights.end()) {
                    const auto& d = sun->second.direction;
                    const double len = std::sqrt(double(d[0]) * d[0] + double(d[1]) * d[1] + double(d[2]) * d[2]);
                    light.sun = {float(d[0] / len), float(d[1] / len), float(d[2] / len)};
                }
                light.tint = state->second.lightMapTint;
                if (state->second.lightMap) {
                    if (auto image = ts2::content::lightingImage(products, *state->second.lightMap)) {
                        light.lookup = {image->width, image->height, std::move(image->rgba)};
                    }
                }
            }
            if (light.lookup.width == 0) {
                throw std::runtime_error("the neighborhood light map image was not found");
            }
            hoodStats = ts2::scene::addNeighborhood(builder, utf8Path(argv[2]), ts2::nhood::parsePaintRules(text), light);
            std::printf("neighborhood %u x %u tiles, %s, sea level %.2f\n", hoodStats.width, hoodStats.height,
                        hoodStats.terrainType.c_str(), hoodStats.seaLevel);
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
            frame(scene, orbit);
            if (hood) {
                // The game's neighborhood camera, starting where the neighborhood was saved.
                orbit.rig = ts2::content::findCamera(utf8Path(argv[1]), "Neighborhood");
                if (!orbit.rig || orbit.rig->levels.empty()) {
                    throw std::runtime_error("the game's Neighborhood camera was not found");
                }
                const auto& levels = orbit.rig->levels;
                orbit.maxDistance = levels.front().distance;
                orbit.minDistance = levels.back().distance;
                if (hoodStats.camera) {
                    const auto& saved = *hoodStats.camera;
                    orbit.target = {saved.target[0], saved.target[1], saved.target[2]};
                    orbit.yaw = orbit.yawGoal = saved.yaw;
                    orbit.zoomTo(saved.distance);
                } else {
                    orbit.target = {static_cast<float>(hoodStats.width) * 5, static_cast<float>(hoodStats.height) * 5, hoodStats.seaLevel};
                    orbit.zoomTo(levels[static_cast<std::size_t>(std::clamp(orbit.rig->defaultZoom, 0, static_cast<int>(levels.size()) - 1))].distance);
                }
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
                    } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
                        if (e.motion.state & SDL_BUTTON_LMASK) {
                            orbit.yaw -= e.motion.xrel * 0.005f;
                            orbit.yawGoal = orbit.yaw;
                            if (!orbit.rig) {
                                orbit.pitch = std::clamp(orbit.pitch + e.motion.yrel * 0.005f, 0.05f, 1.55f);
                            }
                        } else if (e.motion.state & SDL_BUTTON_RMASK) {
                            // Drag the ground along, scaled to how far away the camera is.
                            const float s = orbit.distance * 0.0015f;
                            orbit.pan(-e.motion.xrel * s, e.motion.yrel * s);
                        }
                    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                        orbit.zoomTo(orbit.distance * std::pow(0.9f, e.wheel.y));
                    }
                }

                const Uint64 now = SDL_GetTicksNS();
                const float dt = std::min(0.1f, static_cast<float>(now - last) / 1e9f);
                last = now;
                const bool* keys = SDL_GetKeyboardState(nullptr);
                const float step = orbit.distance * 0.8f * dt;
                orbit.pan(((keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) - (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT])) * step,
                          ((keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) - (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN])) * step);
                const int zoom = (keys[SDL_SCANCODE_X] || keys[SDL_SCANCODE_MINUS] || keys[SDL_SCANCODE_KP_MINUS]) -
                                 (keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_EQUALS] || keys[SDL_SCANCODE_KP_PLUS]);
                if (zoom != 0) {
                    orbit.zoomTo(orbit.distance * std::exp(static_cast<float>(zoom) * 1.5f * dt));
                }
                orbit.yaw += (orbit.yawGoal - orbit.yaw) * (1 - std::exp(-12 * dt));

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
