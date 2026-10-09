// NeoTS2: opens a saved lot in a window.
//   neots2 <install folder> <lot.package> [--shot <out.png> [yaw pitch distance]]
// Left-drag orbits, right-drag pans, the wheel zooms. --shot draws one frame to a file and exits.
#include "render/renderer.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/lot_scene.hpp"

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

// SDL hands main its arguments in UTF-8.
std::filesystem::path utf8Path(const char* text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text)));
}

// A camera circling a point: its distance, and its angles around and above the point.
struct Orbit {
    Vec3 target{0, 0, 0};
    float distance = 30, yaw = -0.8f, pitch = 0.7f;

    ts2::render::Camera camera() const {
        ts2::render::Camera c;
        c.target = target;
        c.eye = {target[0] + distance * std::cos(pitch) * std::cos(yaw), target[1] + distance * std::cos(pitch) * std::sin(yaw),
                 target[2] + distance * std::sin(pitch)};
        return c;
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
        for (int k = 0; k < 3; ++k) {
            lo[static_cast<std::size_t>(k)] = std::min(lo[static_cast<std::size_t>(k)], at[static_cast<std::size_t>(k)]);
            hi[static_cast<std::size_t>(k)] = std::max(hi[static_cast<std::size_t>(k)], at[static_cast<std::size_t>(k)]);
        }
    }
    if (lo[0] > hi[0]) {
        return;
    }
    orbit.target = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
    orbit.distance = std::max(10.0f, 1.2f * std::max(hi[0] - lo[0], hi[1] - lo[1]));
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3 || (argc > 3 && std::string_view(argv[3]) == "--shot" && argc < 5)) {
        std::fprintf(stderr, "Usage: neots2 <install folder> <lot.package> [--shot <out.png> [yaw pitch distance]]\n");
        return 1;
    }
    const bool shot = argc > 4 && std::string_view(argv[3]) == "--shot";
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
        std::printf("%zu objects, %zu walls, %zu roofs\n", stats.objects, stats.walls, stats.roofs);

        SDL_Window* window = SDL_CreateWindow("NeoTS2", 1280, 800,
                                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (shot ? SDL_WINDOW_HIDDEN : 0));
        if (window == nullptr) {
            throw std::runtime_error(SDL_GetError());
        }
        const std::filesystem::path shaders = utf8Path(SDL_GetBasePath()) / "shaders";
        {
            ts2::render::Renderer renderer(window, shaders);
            std::printf("drawing with %s\n", renderer.backend().c_str());
            renderer.upload(builder.scene());
            Orbit orbit;
            frame(builder.scene(), orbit);
            if (shot) {
                if (argc > 7) {
                    orbit.yaw = std::stof(argv[5]);
                    orbit.pitch = std::stof(argv[6]);
                    orbit.distance = std::stof(argv[7]);
                }
                if (!renderer.drawToFile(orbit.camera(), 1280, 800, utf8Path(argv[4]))) {
                    throw std::runtime_error(std::string("couldn't save ") + argv[4]);
                }
                std::printf("wrote %s\n", argv[4]);
            }

            bool running = !shot;
            while (running) {
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) {
                        running = false;
                    } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
                        if (e.motion.state & SDL_BUTTON_LMASK) {
                            orbit.yaw -= e.motion.xrel * 0.005f;
                            orbit.pitch = std::clamp(orbit.pitch + e.motion.yrel * 0.005f, 0.05f, 1.55f);
                        } else if (e.motion.state & SDL_BUTTON_RMASK) {
                            // Pan along the ground, scaled to how far away the camera is.
                            const float s = orbit.distance * 0.0015f;
                            const float c = std::cos(orbit.yaw), n = std::sin(orbit.yaw);
                            orbit.target[0] += (n * e.motion.xrel - c * e.motion.yrel) * s;
                            orbit.target[1] += (-c * e.motion.xrel - n * e.motion.yrel) * s;
                        }
                    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                        orbit.distance = std::clamp(orbit.distance * std::pow(0.9f, e.wheel.y), 1.0f, 300.0f);
                    }
                }
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
