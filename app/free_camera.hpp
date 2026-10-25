#pragma once

#include "orbit.hpp"

#include "platform/platform.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

namespace app {

using Quat = std::array<float, 4>;  // w, x, y, z

// The game's free camera ("CameraMan" in FirstPersonCameras.txt), entered with Tab in a lot: the
// mouse looks round without a button, W/S/A/D move along the ground, Q/E down and up, Z/X step
// through the field of view list. It keeps between its least and most height above the ground or
// roof under it, and moves faster the higher it is.
class FreeCamera {
public:
    // The camera file's settings, with the game's own values for those it leaves out.
    explicit FreeCamera(const ts2::camera::Config& rig) {
        constexpr float kDegrees = kPi / 180;
        for (const auto& level : rig.levels) {
            if (std::isfinite(level.fov) && level.fov > 0 && level.fov < 180) {
                fovs_.push_back(level.fov * kDegrees);
            }
        }
        if (fovs_.empty()) {
            fovs_.push_back(60 * kDegrees);
        }
        if (rig.clip) {
            nearClip_ = std::max((*rig.clip)[0], 0.001f);
            farClip_ = std::max((*rig.clip)[1], nearClip_ * 2);
        }
        headingSensitivity_ = rig.param("mouseheadingsensitivity", 0.0045f);
        pitchSensitivity_ = rig.param("mousepitchsensitivity", 0.001f);
        // A limit of 0 is none.
        upperPitch_ = rig.param("upperpitchlimit", 30) * kDegrees;
        lowerPitch_ = rig.param("lowerpitchlimit", 30) * kDegrees;
        minHeight_ = rig.param("mincameraheight", 1);
        maxHeight_ = std::max(rig.param("maxcameraheight", 100), minHeight_);
        maxVelocity_ = rig.param("maxvelocity", 10);
        slowest_ = rig.param("velocitysensitivitymin", 0.25f);
        tickLimit_ = std::max(rig.param("ticklimit", 50), 1.0f);
        constraintWidth_ = rig.param("constraintwidth", 0);
        // Each eased value's steps and least change, the file's pair only when it gives both.
        const float least = rig.param("defaultminchange", 0.5f);
        auto ease = [&](const char* steps, const char* change, Ease otherwise) {
            otherwise.least = least;
            const std::string s = steps, c = change;
            if (rig.params.contains(s) && rig.params.contains(c)) {
                otherwise.steps = std::max(static_cast<float>(static_cast<int>(rig.param(s, 1))), 1.0f);
                otherwise.least = rig.param(c, least);
            }
            return otherwise;
        };
        move_ = ease("translationsteps", "translationminchange", {100, least});
        turn_ = ease("orientationsteps", "orientationminchange", {10, least});
        speed_ = ease("velocitysteps", "velocityminchange", {5, least});
        zoom_ = ease("fovsteps", "fovminchange", {30, least});
    }

    // How far past the lot's edges it may go (negative: past them), in tiles.
    float constraintWidth() const { return constraintWidth_; }

    // Takes over from the lot camera where it stands: its eye, its direction and its field of view
    // (the list's nearest narrower step chosen for Z and X).
    void enter(const Orbit& orbit) {
        const auto c = orbit.camera();
        at_ = goal_ = c.eye;
        const float fov = c.fovX > 0 ? c.fovX : c.fovY;
        fovIndex_ = 0;
        for (std::size_t i = 0; i < fovs_.size(); ++i) {
            if (fovs_[i] <= fov) {
                fovIndex_ = i;
            }
        }
        fov_ = fovGoal_ = fov;
        // Forward is (-sin heading, cos heading) along the ground, so the lot camera's yaw (where its
        // eye stands from its target) is a quarter turn less.
        heading_ = orbit.yaw + kPi / 2;
        pitch_ = limitPitch(-orbit.pitch);
        turnNow_ = turnGoal_ = turnOf(heading_, pitch_);
        velocity_ = velocityGoal_ = move = {0, 0, 0};
        cornersAt_.fill(-200);
        cornersGoal_.fill(-200);
        previous_.reset();
    }

    // A key going down or up. The movement keys set their axis while down and clear it when either of
    // its keys comes up. Z and X step the field of view. Returns whether the key is the camera's.
    bool key(ts2::platform::Key code, bool down) {
        using ts2::platform::Key;
        auto axis = [&](std::size_t k, float value) {
            move[k] = down ? value : 0;
            return true;
        };
        switch (code) {
        case Key::A: return axis(0, -1);
        case Key::D: return axis(0, 1);
        case Key::W: return axis(1, 1);
        case Key::S: return axis(1, -1);
        case Key::Q: return axis(2, -1);
        case Key::E: return axis(2, 1);
        case Key::Z:
        case Key::Equals:
        case Key::PadPlus:
            if (down && fovIndex_ > 0) {
                fovGoal_ = fovs_[--fovIndex_];
            }
            return true;
        case Key::X:
        case Key::Minus:
        case Key::PadMinus:
            if (down && fovIndex_ + 1 < fovs_.size()) {
                fovGoal_ = fovs_[++fovIndex_];
            }
            return true;
        default: return false;
        }
    }

    // The mouse `dx`, `dy` pixels from the middle of the window: right turns right, down looks down.
    void look(float dx, float dy) {
        heading_ -= dx * headingSensitivity_;
        pitch_ = limitPitch(pitch_ - dy * pitchSensitivity_);
        turnGoal_ = turnOf(heading_, pitch_);
    }

    // Runs `dt` seconds in steps 120 times a second, as the lot camera does. `ground` gives the world
    // height of the ground or roof under a world position, sampling the next corner (see
    // CameraHeight::groundBelow), and `keepInside` keeps a goal near the lot.
    void run(float dt, const std::function<float(float, float, std::array<float, 4>&, unsigned&)>& ground,
             const std::function<void(Vec3&)>& keepInside) {
        constexpr float kStep = 1.0f / 120;
        stepTime_ += dt;
        while (stepTime_ >= kStep) {
            stepTime_ -= kStep;
            previous_ = Pose{at_, turnNow_, fov_};
            easeCarry_ += 1000.0 / 120;
            const auto ms = static_cast<unsigned>(std::floor(easeCarry_));
            easeCarry_ -= ms;
            step(std::min(ms, static_cast<unsigned>(tickLimit_)), ground, keepInside);
        }
        between_ = stepTime_ / kStep;
    }

    // The view, drawn between the last two steps.
    ts2::render::Camera camera() const {
        Vec3 eye = at_;
        Quat turn = turnNow_;
        float fov = fov_;
        if (previous_ && between_ < 1) {
            const float t = between_;
            for (std::size_t k = 0; k < 3; ++k) {
                eye[k] = previous_->at[k] + (at_[k] - previous_->at[k]) * t;
            }
            for (std::size_t k = 0; k < 4; ++k) {
                turn[k] = previous_->turn[k] + (turnNow_[k] - previous_->turn[k]) * t;
            }
            turn = normalised(turn);
            fov = previous_->fov + (fov_ - previous_->fov) * t;
        }
        const Vec3 f = rotate(turn, {0, 1, 0});
        ts2::render::Camera c;
        c.eye = eye;
        c.target = {eye[0] + f[0], eye[1] + f[1], eye[2] + f[2]};
        c.fovX = fov;
        c.nearPlane = nearClip_;
        c.farPlane = farClip_;
        return c;
    }

    // The way it looks, along the ground.
    std::array<float, 2> facing() const {
        const Vec3 f = rotate(turnNow_, {0, 1, 0});
        const float length = std::hypot(f[0], f[1]);
        return length > 0 ? std::array<float, 2>{f[0] / length, f[1] / length} : std::array<float, 2>{-std::sin(heading_), std::cos(heading_)};
    }

    // Hands back to the lot camera, which the game sets from this view at once: it looks down at the
    // view's pitch (held within the closest zoom's pitch range), from as far as puts the eye at its
    // height above the pivot, at the point on the lot's floor that far along the view, facing the
    // same way. `toLot` and `fromLot` move points between the world and the lot's coordinates.
    void leave(Orbit& orbit, const std::function<Vec3(const Vec3&)>& toLot, const std::function<Vec3(const Vec3&)>& fromLot) const {
        if (!orbit.rig || orbit.rig->levels.empty()) {
            return;
        }
        constexpr float kDegrees = kPi / 180;
        const auto& closest = orbit.rig->levels.back();
        const Vec3 eye = toLot(at_);
        const float down = std::clamp(-pitch_ / kDegrees, closest.pitchMin, closest.pitchMax);
        const float sine = std::sin(down * kDegrees);
        const float distance = std::clamp(sine > 0 ? (eye[2] - orbit.pivotHeight) / sine : orbit.maxDistance, orbit.minDistance, orbit.maxDistance);
        const Vec3 lookWorld = rotate(turnNow_, {0, 1, 0});
        const Vec3 eyePlusLook = toLot({at_[0] + lookWorld[0], at_[1] + lookWorld[1], at_[2] + lookWorld[2]});
        const Vec3 look = {eyePlusLook[0] - eye[0], eyePlusLook[1] - eye[1], eyePlusLook[2] - eye[2]};
        const Vec3 target = fromLot({eye[0] + look[0] * distance, eye[1] + look[1] * distance, 0});
        const float yaw = wrapAngle(heading_ - kPi / 2);
        const float least = ts2::camera::pitchAt(*orbit.rig, distance, 0), most = ts2::camera::pitchAt(*orbit.rig, distance, 1);
        orbit.pitchBlend = orbit.pitchBlendGoal = most > least ? (std::clamp(down, least, most) - least) / (most - least) : 0;
        orbit.target = target;
        orbit.goal = target;
        orbit.yaw = orbit.yawGoal = yaw;
        orbit.zoomTo(distance);
        orbit.distanceGoal = orbit.distance;
        orbit.dragSpeed = {0, 0};
        orbit.previous.reset();
    }

    // The movement keys held: right, forward and up, each -1, 0 or 1.
    Vec3 move{0, 0, 0};

private:
    struct Ease {
        float steps = 1, least = 0;
    };
    struct Pose {
        Vec3 at;
        Quat turn;
        float fov;
    };

    std::vector<float> fovs_;  // radians across the screen
    std::size_t fovIndex_ = 0;
    float nearClip_ = 0.15f, farClip_ = 2000;
    float headingSensitivity_ = 0, pitchSensitivity_ = 0, upperPitch_ = 0, lowerPitch_ = 0;
    float minHeight_ = 1, maxHeight_ = 100, maxVelocity_ = 10, slowest_ = 0.25f, tickLimit_ = 50, constraintWidth_ = 0;
    Ease move_, turn_, speed_, zoom_;

    Vec3 at_{0, 0, 0}, goal_{0, 0, 0}, velocity_{0, 0, 0}, velocityGoal_{0, 0, 0};
    Quat turnNow_{1, 0, 0, 0}, turnGoal_{1, 0, 0, 0};
    float heading_ = 0, pitch_ = 0, fov_ = 1, fovGoal_ = 1;
    // How high it is between its least and most height (0 to 1), which sets its speed.
    float heightShare_ = 1;
    // The ground's corner heights kept under the eye and under the goal, and the corner sampled next.
    std::array<float, 4> cornersAt_{}, cornersGoal_{};
    unsigned corner_ = 0;
    std::optional<Pose> previous_;
    float stepTime_ = 0, between_ = 1;
    double easeCarry_ = 0;

    float limitPitch(float pitch) const {
        if (upperPitch_ != 0) {
            pitch = std::min(pitch, upperPitch_);
        }
        if (lowerPitch_ != 0) {
            pitch = std::max(pitch, lowerPitch_);
        }
        return pitch;
    }

    // Turned by `heading` about the up axis after tilting by `pitch` about the side axis.
    static Quat turnOf(float heading, float pitch) {
        const float cz = std::cos(heading / 2), sz = std::sin(heading / 2), cx = std::cos(pitch / 2), sx = std::sin(pitch / 2);
        return {cz * cx, cz * sx, sz * sx, sz * cx};
    }

    static Quat normalised(const Quat& q) {
        const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        return length > 0 ? Quat{q[0] / length, q[1] / length, q[2] / length, q[3] / length} : Quat{1, 0, 0, 0};
    }

    static Vec3 rotate(const Quat& q, const Vec3& v) {
        const Vec3 u = {q[1], q[2], q[3]};
        const Vec3 t = {2 * (u[1] * v[2] - u[2] * v[1]), 2 * (u[2] * v[0] - u[0] * v[2]), 2 * (u[0] * v[1] - u[1] * v[0])};
        return {v[0] + q[0] * t[0] + (u[1] * t[2] - u[2] * t[1]), v[1] + q[0] * t[1] + (u[2] * t[0] - u[0] * t[2]),
                v[2] + q[0] * t[2] + (u[0] * t[1] - u[1] * t[0])};
    }

    // One step `ms` milliseconds long, as the game's: set the goal ahead by the eased speed along the
    // keys' directions (forward along the ground, slower the more it looks down), ease everything,
    // then keep the eye and the goal within their heights above the ground under them.
    void step(unsigned ms, const std::function<float(float, float, std::array<float, 4>&, unsigned&)>& ground, const std::function<void(Vec3&)>& keepInside) {
        const float scale = slowest_ + (1 - slowest_) * heightShare_;
        velocityGoal_ = {move[0] * maxVelocity_ * scale, move[1] * maxVelocity_ * scale, move[2] * maxVelocity_ * scale};
        if (velocityGoal_[0] != 0 || velocityGoal_[1] != 0 || velocityGoal_[2] != 0) {
            const Vec3 f = rotate(turnNow_, {0, 1, 0});
            const float fx = f[0], fy = f[1];
            goal_ = {at_[0] + fy * velocity_[0] + fx * velocity_[1], at_[1] - fx * velocity_[0] + fy * velocity_[1], at_[2] + velocity_[2]};
            if (keepInside) {
                keepInside(goal_);
            }
        }
        easeToward(at_, goal_, ms, move_.steps, move_.least);
        Quat turn = turnNow_;
        easeToward(turn, turnGoal_, ms, turn_.steps, turn_.least);
        turnNow_ = normalised(turn);
        easeToward(velocity_, velocityGoal_, ms, speed_.steps, speed_.least);
        easeToward(fov_, fovGoal_, ms, zoom_.steps, zoom_.least);
        const float under = ground(at_[0], at_[1], cornersAt_, corner_);
        at_[2] = std::clamp(at_[2], under + minHeight_, under + maxHeight_);
        if (maxHeight_ > minHeight_) {
            const float share = (at_[2] - under - minHeight_) / (maxHeight_ - minHeight_);
            if (share >= 0 && share <= 1) {
                heightShare_ = share;
            }
        }
        const float underGoal = ground(goal_[0], goal_[1], cornersGoal_, corner_);
        goal_[2] = std::clamp(goal_[2], underGoal + minHeight_, underGoal + maxHeight_);
    }
};

} // namespace app
