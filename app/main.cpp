// NeoTS2: opens a saved lot in a window.
//   neots2 <install folder> <lot.package> [--shot <out.png> [yaw pitch distance]]
//          [--walls up|cutaway|down] [--level n (1: the ground floor)] [--target x y z]
// Mouse: left-drag orbits, right-drag pans, the wheel zooms. Keys: WASD or arrows pan, Q/E or
// comma/period turn a quarter, Z/X or +/- zoom, Page Up/Down change storey, 1/2/3 or Tab walls
// up/cutaway/down. --shot draws one frame to a file and exits.
#include "render/renderer.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/lot_scene.hpp"
#include "scene/view.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
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

    ts2::render::Camera camera() const {
        ts2::render::Camera c;
        c.target = target;
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
    Options options;
    if (argc < 3 || !parseOptions(argc, argv, options)) {
        std::fprintf(stderr, "Usage: neots2 <install folder> <lot.package> [--shot <out.png> [yaw pitch distance]]\n"
                             "              [--walls up|cutaway|down] [--level n] [--target x y z]\n");
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
        const auto stats = ts2::scene::addLot(builder, utf8Path(argv[2]));
        std::printf("%zu objects, %zu walls, %zu roofs, %zu terrain paints\n", stats.objects, stats.walls, stats.roofs,
                    stats.terrainPaints);
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
            ts2::scene::View view;
            view.walls = options.walls;
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
                view.facing = orbit.facing();
                renderer.show(ts2::scene::visibleInstances(scene, view));
                if (!renderer.drawToFile(orbit.camera(), 1280, 800, utf8Path(options.shot))) {
                    throw std::runtime_error(std::string("couldn't save ") + options.shot);
                }
                std::printf("wrote %s\n", options.shot);
            }

            auto setTitle = [&] {
                const std::string title = "NeoTS2 - walls " + std::string(wallViewName(view.walls)) + ", storey " +
                                          std::to_string(view.level - ground + 1) + " of " + std::to_string(top - ground + 1);
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
                        default:
                            break;
                        }
                    } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
                        if (e.motion.state & SDL_BUTTON_LMASK) {
                            orbit.yaw -= e.motion.xrel * 0.005f;
                            orbit.yawGoal = orbit.yaw;
                            orbit.pitch = std::clamp(orbit.pitch + e.motion.yrel * 0.005f, 0.05f, 1.55f);
                        } else if (e.motion.state & SDL_BUTTON_RMASK) {
                            // Drag the ground along, scaled to how far away the camera is.
                            const float s = orbit.distance * 0.0015f;
                            orbit.pan(-e.motion.xrel * s, e.motion.yrel * s);
                        }
                    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                        orbit.distance = std::clamp(orbit.distance * std::pow(0.9f, e.wheel.y), 1.0f, 300.0f);
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
                orbit.distance = std::clamp(orbit.distance * std::exp(static_cast<float>(zoom) * 1.5f * dt), 1.0f, 300.0f);
                orbit.yaw += (orbit.yawGoal - orbit.yaw) * (1 - std::exp(-12 * dt));

                view.facing = orbit.facing();
                renderer.show(ts2::scene::visibleInstances(scene, view));
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
