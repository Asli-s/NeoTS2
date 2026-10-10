#pragma once

#include "formats/camera_config.hpp"
#include "render/renderer.hpp"
#include "scene/neighborhood_scene.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <optional>

namespace app {

using ts2::render::Vec3;

constexpr float kPi = 3.14159265358979f;

// A camera circling a point: its distance, and its angles around and above the point. Quarter
// turns ease toward `yawGoal`.
struct Orbit {
    Vec3 target{0, 0, 0};
    float distance = 30, yaw = -kPi / 4, pitch = 0.7f;
    float yawGoal = -kPi / 4;
    float minDistance = 1, maxDistance = 300;  // how near and far the camera may zoom
    float minFar = 500;                       // the least distance it sees to
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
        c.farPlane = std::max(minFar, distance * 3);
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

// Turns, pans and zooms the camera for a mouse event: middle-drag turns, right-drag pans, the wheel
// zooms. Returns whether the event was used.
inline bool orbitEvent(Orbit& orbit, const SDL_Event& e) {
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        if (e.motion.state & SDL_BUTTON_MMASK) {
            orbit.yaw -= e.motion.xrel * 0.005f;
            orbit.yawGoal = orbit.yaw;
            if (!orbit.rig) {
                orbit.pitch = std::clamp(orbit.pitch + e.motion.yrel * 0.005f, 0.05f, 1.55f);
            }
            return true;
        }
        if (e.motion.state & SDL_BUTTON_RMASK) {
            // Drag the ground along, scaled to how far away the camera is.
            const float s = orbit.distance * 0.0015f;
            orbit.pan(-e.motion.xrel * s, e.motion.yrel * s);
            return true;
        }
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        orbit.zoomTo(orbit.distance * std::pow(0.9f, e.wheel.y));
        return true;
    }
    return false;
}

// Moves the camera by the keys held for `dt` seconds (WASD or arrows pan, Z/X or +/- zoom) and
// eases quarter turns along.
inline void orbitStep(Orbit& orbit, float dt) {
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const float step = orbit.distance * 0.8f * dt;
    orbit.pan(static_cast<float>((keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) - (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT])) * step,
              static_cast<float>((keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) - (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN])) * step);
    const int zoom = (keys[SDL_SCANCODE_X] || keys[SDL_SCANCODE_MINUS] || keys[SDL_SCANCODE_KP_MINUS]) -
                     (keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_EQUALS] || keys[SDL_SCANCODE_KP_PLUS]);
    if (zoom != 0) {
        orbit.zoomTo(orbit.distance * std::exp(static_cast<float>(zoom) * 1.5f * dt));
    }
    orbit.yaw += (orbit.yawGoal - orbit.yaw) * (1 - std::exp(-12 * dt));
}

// The game's neighborhood camera, starting where the neighborhood was saved (or over its middle
// at the rig's default zoom).
void neighborhoodCamera(Orbit& orbit, const std::filesystem::path& install, const ts2::scene::NeighborhoodSceneStats& stats);

} // namespace app
