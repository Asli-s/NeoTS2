#pragma once

#include "formats/camera_config.hpp"
#include "render/renderer.hpp"
#include "scene/hood_drift.hpp"
#include "scene/neighborhood_scene.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>

namespace app {

using ts2::render::Vec3;

constexpr float kPi = 3.14159265358979f;

// Eases `value` toward `goal` for `ms` milliseconds, as the game eases its camera's values: each
// step is 0.02/`period` of the way left per millisecond, at least `least` units (the length of
// the step for several values), and lands on the goal once it would reach or pass it.
template <std::size_t N>
void easeToward(std::array<float, N>& value, const std::array<float, N>& goal, unsigned ms, float period, float least) {
    std::array<float, N> left{}, step{};
    float leftSq = 0, stepSq = 0;
    const float f = static_cast<float>(ms) * 0.02f / period;
    for (std::size_t k = 0; k < N; ++k) {
        left[k] = goal[k] - value[k];
        step[k] = left[k] * f;
        leftSq += left[k] * left[k];
        stepSq += step[k] * step[k];
    }
    if (stepSq >= least * least ? leftSq < stepSq : leftSq <= least * least) {
        value = goal;
        return;
    }
    if (stepSq < least * least) {
        const float scale = least / std::sqrt(leftSq);
        for (std::size_t k = 0; k < N; ++k) {
            step[k] = left[k] * scale;
        }
    }
    for (std::size_t k = 0; k < N; ++k) {
        value[k] += step[k];
    }
}

// Eases the scalar `value` toward `goal` the same way.
inline void easeToward(float& value, float goal, unsigned ms, float period, float least) {
    const float left = goal - value;
    const float step = static_cast<float>(ms) * left * 0.02f / period;
    if (step * step < least * least) {
        value = left * left <= least * least ? goal : value + (left > 0 ? least : left < 0 ? -least : 0.0f);
    } else {
        value = left * left < step * step ? goal : value + step;
    }
}

// An angle in radians brought into [0, 2 pi). An angle that isn't finite gives 0.
inline float wrapAngle(float a) {
    if (!std::isfinite(a)) {
        return 0;
    }
    a = std::fmod(a, 2 * kPi);
    if (a < 0) {
        a += 2 * kPi;
    }
    return a < 2 * kPi ? a : 0.0f;
}

// Eases the angle `value` toward `goal` the same way, the short way round.
inline void easeAngleToward(float& value, float goal, unsigned ms, float period, float least) {
    value = wrapAngle(value);
    const float left = std::remainder(wrapAngle(goal) - value, 2 * kPi);
    float eased = value;
    easeToward(eased, value + left, ms, period, least);
    value = eased;
}

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
    // Where the target is easing to, for a camera that eases its moves as the lot camera does.
    // Without one, moves take the target at once.
    std::optional<Vec3> goal;
    double easeCarry = 0;  // milliseconds not yet eased
    // The scroll speed along the ground, in units, from a right-button drag or the scroll keys.
    // The goal is set this far ahead of the target five times over each frame.
    std::array<float, 2> dragSpeed{0, 0};
    // The scroll keys' screen offset in pixels (y down), easing toward 50 pixels each way a key
    // is held. While it is not zero it sets the scroll speed each frame as a drag would, and the
    // step it reaches zero stops the scroll.
    Vec3 keyOffset{0, 0, 0};
    bool keyScrolling = false;
    bool keysOn = true;  // off while a menu over the view takes the keys
    // Where the pitch sits between the rig's minimum and maximum pitch at this zoom (0 to 1), and
    // where it is easing to.
    float pitchBlend = 0, pitchBlendGoal = 0;
    // How far above the target the camera aims and circles (the camera file's
    // pivotheightabovefloor), so the target sits below the middle of the screen.
    float pivotHeight = 0;
    // A middle-button drag's mouse movement since the last frame, in pixels, from the press point
    // the pointer is put back to after each move. Moves between frames are added up, as Windows
    // merges them before the game sees one.
    std::array<float, 2> turnMove{0, 0};
    // Added to the yaw where the camera stands, by the neighborhood camera's drift.
    float yawOffset = 0;
    // Where the zoom distance is easing to, for a camera with a goal.
    float distanceGoal = 30;
    // The least step of each eased value (the camera file's defaultminchange), and the scroll keys'
    // ease period, least step and offset in pixels (keyboardtranslationlerpsteps, -lerpminchange
    // and -speed). The defaults are the Live camera's.
    float minChange = 0.02f;
    float keyPeriod = 1, keyMinChange = 0.001f, keySpeed = 50;
    // Keeps a scrolled goal where the camera may go (the lot camera: near the lot).
    std::function<void(Vec3&)> limitGoal;
    // A camera with a goal moves in steps 120 times a second, as the game's camera steps, so it
    // moves at the same speed at any frame rate. It is drawn between the step before (`previous`)
    // and the last one, `between` (0 to 1) of the way. `stepTime` is the time not yet stepped.
    struct Pose {
        Vec3 target{0, 0, 0};
        float yaw = 0, pitch = 0, distance = 0;
    };
    std::optional<Pose> previous;
    float between = 1, stepTime = 0;
    // What the camera tracks (a Sim's root_trans in the world), none while it tracks nothing.
    // Scrolling ends the tracking, turning and zooming don't.
    std::function<std::optional<Vec3>()> subject;

    // Takes the easing settings from the camera file.
    void useRigEasing() {
        if (rig) {
            // A negative least step would ease away from the goal and never settle.
            minChange = std::max(0.0f, rig->param("defaultminchange", minChange));
            keyPeriod = std::max(1.0f, rig->param("keyboardtranslationlerpsteps", keyPeriod));
            keyMinChange = std::max(0.0f, rig->param("keyboardtranslationlerpminchange", keyMinChange));
            keySpeed = rig->param("keyboardtranslationspeed", keySpeed);
        }
    }

    void zoomTo(float d) {
        distance = std::clamp(d, minDistance, maxDistance);
        if (rig) {
            pitch = ts2::camera::pitchAt(*rig, distance, pitchBlend) * kPi / 180;
        }
    }

    ts2::render::Camera camera() const {
        ts2::render::Camera c;
        Vec3 at = target;
        float turn = yaw + yawOffset, tilt = pitch, away = distance;
        if (previous && between < 1) {
            const auto& p = *previous;
            const float t = between;
            for (std::size_t k = 0; k < 3; ++k) {
                at[k] = p.target[k] + (at[k] - p.target[k]) * t;
            }
            turn = p.yaw + yawOffset + std::remainder(turn - p.yaw - yawOffset, 2 * kPi) * t;
            tilt = p.pitch + (tilt - p.pitch) * t;
            away = p.distance + (away - p.distance) * t;
        }
        c.target = {at[0], at[1], at[2] + pivotHeight};
        c.farPlane = std::max(minFar, away * 3);
        if (rig && !rig->levels.empty()) {
            const auto& level = rig->levels.front();
            c.fovX = level.fov * kPi / 180;
            // The near clip shrinks with the zoom (1 m far out, 0.1 m closest in LiveCamera.txt).
            c.nearPlane = ts2::camera::nearClipAt(*rig, away);
            c.farPlane = level.farClip;
        }
        c.eye = {c.target[0] + away * std::cos(tilt) * std::cos(turn), c.target[1] + away * std::cos(tilt) * std::sin(turn),
                 c.target[2] + away * std::sin(tilt)};
        return c;
    }

    // The way the camera looks, along the ground.
    std::array<float, 2> facing() const { return {-std::cos(yaw), -std::sin(yaw)}; }

    // Moves the target along the ground: `right` and `forward` as seen on screen.
    void pan(float right, float forward) {
        const float c = std::cos(yaw), s = std::sin(yaw);
        Vec3& moved = goal ? *goal : target;
        moved[0] += -s * right - c * forward;
        moved[1] += c * right - s * forward;
    }

    // Sets the scroll speed for a screen offset of `dx`, `dy` pixels (y down) on a `width` x
    // `height` view: that offset in ground units at the target's distance. A drag passes a
    // quarter of the mouse's offset from where it started.
    void scrollBy(float dx, float dy, int width, int height) {
        const auto c = camera();
        const float aspect = static_cast<float>(width) / static_cast<float>(std::max(height, 1));
        const float fovY = c.fovX > 0 ? 2 * std::atan(std::tan(c.fovX / 2) / aspect) : c.fovY;
        const float perPixel = 2 * distance * std::tan(fovY / 2) / static_cast<float>(std::max(height, 1));
        const float right = dx * perPixel, forward = -dy * perPixel;
        const float co = std::cos(yaw), si = std::sin(yaw);
        dragSpeed = {-si * right - co * forward, co * right - si * forward};
        subject = nullptr;
    }

    // Ends any tracking and eases the target to `point`, kept where the camera may go.
    void centreOn(const Vec3& point) {
        subject = nullptr;
        Vec3& moved = goal ? *goal : target;
        moved = point;
        if (goal && limitGoal) {
            limitGoal(*goal);
        }
    }

    // The nearest view at 45 degrees to the lot's grid, `quarters` turns on.
    void turn(int quarters) {
        yawGoal = std::round((yawGoal + kPi / 4) / (kPi / 2)) * (kPi / 2) - kPi / 4 + static_cast<float>(quarters) * kPi / 2;
    }
};

// Where a point of the world shows on a `width` x `height` view, in pixels. A point behind the
// camera gives (-1, -1).
inline std::array<float, 2> screenPoint(const ts2::render::Camera& c, int width, int height, const Vec3& p) {
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float tx = c.fovX > 0 ? std::tan(c.fovX / 2) : std::tan(c.fovY / 2) * aspect;
    const float ty = tx / aspect;
    const Vec3 f = ts2::render::normalize(ts2::render::sub(c.target, c.eye));
    const Vec3 s = ts2::render::normalize(ts2::render::cross(f, {0, 0, 1}));
    const Vec3 u = ts2::render::cross(s, f);
    const Vec3 d = ts2::render::sub(p, c.eye);
    const float depth = ts2::render::dot(d, f);
    if (depth <= 1e-6f) {
        return {-1, -1};
    }
    const float nx = ts2::render::dot(d, s) / depth / tx, ny = ts2::render::dot(d, u) / depth / ty;
    return {(nx + 1) / 2 * static_cast<float>(width), (1 - ny) / 2 * static_cast<float>(height)};
}

// One step of the camera's tracking, as the game's lot camera tracks a Sim: once the subject's
// floor point (its root_trans less the pivot height) shows more than 30 pixels from the target's
// goal on screen, the goal moves under it.
inline void followSubject(Orbit& orbit, int width, int height) {
    constexpr float kDeadZone = 30;
    const auto at = orbit.subject();
    if (!at || !orbit.goal || width <= 0 || height <= 0) {
        return;
    }
    const Vec3 floor = {(*at)[0], (*at)[1], (*at)[2] - orbit.pivotHeight};
    const auto camera = orbit.camera();
    const auto goal = screenPoint(camera, width, height, *orbit.goal);
    const auto subject = screenPoint(camera, width, height, floor);
    if (std::hypot(subject[0] - goal[0], subject[1] - goal[1]) > kDeadZone) {
        (*orbit.goal)[0] = floor[0];
        (*orbit.goal)[1] = floor[1];
        if (orbit.limitGoal) {
            orbit.limitGoal(*orbit.goal);
        }
    }
}

// Turns, pans and zooms the camera for a mouse event: middle-drag turns, right-drag pans (a camera
// with a goal scrolls by its drag speed instead), the wheel zooms. Returns whether the event was used.
inline bool orbitEvent(Orbit& orbit, const SDL_Event& e) {
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        if ((e.motion.state & SDL_BUTTON_MMASK) && orbit.goal) {
            return true;  // the drag's movement is added to turnMove by whoever holds the pointer
        }
        if (e.motion.state & SDL_BUTTON_MMASK) {
            orbit.yaw -= e.motion.xrel * 0.005f;
            orbit.yawGoal = orbit.yaw;
            if (!orbit.rig) {
                orbit.pitch = std::clamp(orbit.pitch + e.motion.yrel * 0.005f, 0.05f, 1.55f);
            }
            return true;
        }
        if ((e.motion.state & SDL_BUTTON_RMASK) && !orbit.goal) {
            // Drag the ground along, scaled to how far away the camera is.
            const float s = orbit.distance * 0.0015f;
            orbit.pan(-e.motion.xrel * s, e.motion.yrel * s);
            return true;
        }
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL && orbit.goal) {
        // As the game's lot camera: each notch moves the zoom goal by a tenth of the distance plus
        // 10 (the camera file's wheel scaling, 5, times 0.02), within the zoom levels.
        const float z = orbit.distanceGoal;
        orbit.distanceGoal = std::clamp(z - (z + 10) * e.wheel.y * 5 * 0.02f, orbit.minDistance, orbit.maxDistance);
        return true;
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        orbit.zoomTo(orbit.distance * std::pow(0.9f, e.wheel.y));
        return true;
    }
    return false;
}

// One step of a camera with a goal, `ms` milliseconds long, as the game's lot camera steps: the
// keys, the drag's turn, then everything easing toward its goal.
inline void orbitGameStep(Orbit& orbit, unsigned ms, int width, int height) {
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const auto held = [&](SDL_Scancode a, SDL_Scancode b) { return orbit.keysOn && (keys[a] || keys[b]); };
    const int right = held(SDL_SCANCODE_D, SDL_SCANCODE_RIGHT) - held(SDL_SCANCODE_A, SDL_SCANCODE_LEFT);
    const int down = held(SDL_SCANCODE_S, SDL_SCANCODE_DOWN) - held(SDL_SCANCODE_W, SDL_SCANCODE_UP);
    easeToward(orbit.keyOffset, {static_cast<float>(right) * orbit.keySpeed, static_cast<float>(down) * orbit.keySpeed, 0}, ms, orbit.keyPeriod,
               orbit.keyMinChange);
    if (orbit.keyOffset[0] != 0 || orbit.keyOffset[1] != 0) {
        orbit.scrollBy(orbit.keyOffset[0], orbit.keyOffset[1], width, height);
        orbit.keyScrolling = true;
    } else if (orbit.keyScrolling) {
        orbit.dragSpeed = {0, 0};
        orbit.keyScrolling = false;
    }
    const int zoom = !orbit.keysOn ? 0 : (keys[SDL_SCANCODE_X] || keys[SDL_SCANCODE_MINUS] || keys[SDL_SCANCODE_KP_MINUS]) -
                     (keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_EQUALS] || keys[SDL_SCANCODE_KP_PLUS]);
    if (zoom != 0) {
        // The game's zoom keys are not known yet. These move the goal the eased zoom follows.
        orbit.distanceGoal = std::clamp(orbit.distanceGoal * std::exp(static_cast<float>(zoom) * 1.5f * static_cast<float>(ms) / 1000),
                                        orbit.minDistance, orbit.maxDistance);
    }
    if (orbit.turnMove[0] != 0 || orbit.turnMove[1] != 0) {
        // As the game's lot camera: the larger of the two moves wins, 0.003 radians of turn (at
        // most 3) and 0.005 of the pitch range a pixel.
        float dx = std::round(orbit.turnMove[0]), dy = std::round(orbit.turnMove[1]);
        orbit.turnMove = {0, 0};
        if (dx != 0 && dy != 0) {
            const float ratio = std::abs(dx / dy);
            if (ratio > 1) {
                dy = 0;
            } else if (ratio < 1) {
                dx = 0;
            }
        }
        if (dx != 0) {
            // Our yaw (where the eye stands) turns the other way to the game's camera angle.
            orbit.yawGoal = wrapAngle(orbit.yaw + std::clamp(dx * 0.003f, -3.0f, 3.0f));
        }
        if (dy != 0) {
            orbit.pitchBlendGoal = std::clamp(orbit.pitchBlend - dy * 0.005f, 0.0f, 1.0f);
        }
    }
    easeAngleToward(orbit.yaw, orbit.yawGoal, ms, 8, orbit.minChange);
    easeToward(orbit.pitchBlend, orbit.pitchBlendGoal, ms, 7, orbit.minChange);
    float distance = orbit.distance;
    easeToward(distance, orbit.distanceGoal, ms, 5, orbit.minChange);
    orbit.zoomTo(distance);
    if (std::hypot(orbit.dragSpeed[0], orbit.dragSpeed[1]) > 0.001f) {
        (*orbit.goal)[0] = orbit.target[0] + orbit.dragSpeed[0] * 5;
        (*orbit.goal)[1] = orbit.target[1] + orbit.dragSpeed[1] * 5;
        if (orbit.limitGoal) {
            orbit.limitGoal(*orbit.goal);
        }
    }
    if (orbit.subject) {
        followSubject(orbit, width, height);
    }
    easeToward(orbit.target, *orbit.goal, ms, 7, orbit.minChange);
}

// Moves the camera by the keys held for `dt` seconds (WASD or arrows pan, Z/X or +/- zoom) and
// eases quarter turns along. A camera with a goal moves as the game's lot camera does, in steps
// 120 times a second on a `width` x `height` view, `beforeStep` running before each.
inline void orbitStep(Orbit& orbit, float dt, int width = 0, int height = 0, const std::function<void()>& beforeStep = {}) {
    if (orbit.goal) {
        constexpr float kStep = 1.0f / 120;
        orbit.stepTime += dt;
        while (orbit.stepTime >= kStep) {
            orbit.stepTime -= kStep;
            if (beforeStep) {
                beforeStep();
            }
            orbit.previous = Orbit::Pose{orbit.target, orbit.yaw, orbit.pitch, orbit.distance};
            // The game eases by whole milliseconds. The rest carries to the next step.
            orbit.easeCarry += 1000.0 / 120;
            const auto ms = static_cast<unsigned>(std::floor(orbit.easeCarry));
            orbit.easeCarry -= ms;
            orbitGameStep(orbit, ms, width, height);
        }
        orbit.between = orbit.stepTime / kStep;
        return;
    }
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const float step = orbit.distance * 0.8f * dt;
    const int right = (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) - (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]);
    const int up = (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) - (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]);
    orbit.pan(static_cast<float>(right) * step, static_cast<float>(up) * step);
    const int zoom = (keys[SDL_SCANCODE_X] || keys[SDL_SCANCODE_MINUS] || keys[SDL_SCANCODE_KP_MINUS]) -
                     (keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_EQUALS] || keys[SDL_SCANCODE_KP_PLUS]);
    if (zoom != 0) {
        orbit.zoomTo(orbit.distance * std::exp(static_cast<float>(zoom) * 1.5f * dt));
    }
    orbit.yaw += (orbit.yawGoal - orbit.yaw) * (1 - std::exp(-12 * dt));
}

// Applies one step of the neighborhood camera's drift: the sway folded into the yaw when a new
// drift begins, the new sway, and the new goal kept where the camera may go.
inline void applyDrift(Orbit& orbit, const ts2::scene::HoodDrift::Step& step) {
    if (step.foldSway) {
        orbit.yaw = wrapAngle(orbit.yaw + orbit.yawOffset);
        orbit.yawGoal = wrapAngle(orbit.yawGoal + orbit.yawOffset);
        orbit.yawOffset = 0;
    }
    if (step.sway) {
        orbit.yawOffset = *step.sway;
    }
    if (step.goal && orbit.goal) {
        Vec3 goal{(*step.goal)[0], (*step.goal)[1], (*step.goal)[2]};
        if (orbit.limitGoal) {
            orbit.limitGoal(goal);
        }
        *orbit.goal = goal;
    }
}

// The game's neighborhood camera, easing as the lot camera does with its own file's settings and
// keeping its target inside the file's margins. It starts where the neighborhood was saved (or
// over its middle at the rig's default zoom).
void neighborhoodCamera(Orbit& orbit, const std::filesystem::path& install, const ts2::scene::NeighborhoodSceneStats& stats);

} // namespace app
