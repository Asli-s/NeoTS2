#include "sim/look_at.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::sim {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDegree = kPi / 180;

using Vec = std::array<float, 3>;

Vec sub(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec scaled(const Vec& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec cross(const Vec& a, const Vec& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }

} // namespace

LookAngles lookAngles(const Vec& v) {
    return {std::atan2(-v[1], std::sqrt(v[0] * v[0] + v[2] * v[2])), std::atan2(v[0], v[2]), 0};
}

namespace {

using Quat = std::array<float, 4>;

Quat product(const Quat& a, const Quat& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
Quat inverse(const Quat& q) { return {-q[0], -q[1], -q[2], q[3]}; }
Vec rotate(const Quat& q, const Vec& v) {
    const Vec u{q[0], q[1], q[2]};
    const Vec a = cross(u, v);
    const Vec b = cross(u, a);
    return {v[0] + 2 * (q[3] * a[0] + b[0]), v[1] + 2 * (q[3] * a[1] + b[1]), v[2] + 2 * (q[3] * a[2] + b[2])};
}
Vec added(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }

Quat slerped(const Quat& a, Quat b, float t) {
    float c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (c < 0) {
        b = {-b[0], -b[1], -b[2], -b[3]};
        c = -c;
    }
    float wa = 1 - t, wb = t;
    if (c < 0.9995f) {
        const float angle = std::acos(c), s = std::sin(angle);
        wa = std::sin((1 - t) * angle) / s;
        wb = std::sin(t * angle) / s;
    }
    Quat r{wa * a[0] + wb * b[0], wa * a[1] + wb * b[1], wa * a[2] + wb * b[2], wa * a[3] + wb * b[3]};
    const float l = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
    return l > 0 ? Quat{r[0] / l, r[1] / l, r[2] / l, r[3] / l} : a;
}

// The turn by Euler angles (x, y, z): about x first, then y, then z.
Quat fromEuler(const Vec& e) {
    const float cx = std::cos(e[0] / 2), sx = std::sin(e[0] / 2), cy = std::cos(e[1] / 2), sy = std::sin(e[1] / 2);
    const float cz = std::cos(e[2] / 2), sz = std::sin(e[2] / 2);
    return {cz * cy * sx - sz * sy * cx, cz * sy * cx + sz * cy * sx, sz * cy * cx - cz * sy * sx, cz * cy * cx + sz * sy * sx};
}
Vec eulerOf(const Quat& q) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float r00 = 1 - 2 * (y * y + z * z), r10 = 2 * (x * y + z * w), r20 = 2 * (x * z - y * w);
    const float r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y);
    return {std::atan2(r21, r22), std::asin(std::clamp(-r20, -1.0f, 1.0f)), std::atan2(r10, r00)};
}
// A look as Euler angles from its angles: x the pitch, y the yaw, z the roll.
Vec eulerOf(const LookAngles& a) { return {a.pitch, a.yaw, a.roll}; }
LookAngles anglesOf(const Vec& e) { return {e[0], e[1], e[2]}; }

// The angle between two looks' forward (+Z) directions, 0 when the cosine isn't strictly inside -1..1
// as the game takes it.
float apart(const Quat& a, const Quat& b) {
    const float c = dot(rotate(a, {0, 0, 1}), rotate(b, {0, 0, 1}));
    return c > -1 && c < 1 ? std::acos(c) : 0.0f;
}

// The ease of a turn, as the game steps it: from the speed it had, up to `cruise` within the first
// `rampAngle` of the turn, steady, then easing down to 0.3 of `cruise` in the last `slowAngle`.
// `speed` is left as reached. Returns the mean speed over the `ms` stepped.
float easedSpeed(float& speed, float cruise, float total, float rem, float ramp, float rampAngle, float slowAngle, float ms) {
    const float acc = cruise / ramp * 0.7f;
    float mean = speed, left = ms;
    if (std::abs(total - rem) < rampAngle) {
        const float f = std::abs(total - rem) / rampAngle;
        const float rising = std::min((1 - f) * ramp, left);
        speed = std::min(rising * acc + (0.3f + 0.7f * f) * cruise, cruise);
        mean = (mean + speed) / 2;
        left -= rising;
    }
    if (left > 0) {
        if (rem > slowAngle) {
            const float steady = std::min((rem - slowAngle) / (cruise * 0.001f), left);
            speed = cruise;
            mean = (mean + cruise) / 2;
            left -= steady;
        }
        if (left > 0) {
            const float g = std::min(rem / slowAngle, 1.0f);
            speed = std::max((0.3f + 0.7f * g) * cruise - left * acc, 0.3f * cruise);
            mean = (mean + speed) / 2;
        }
    }
    return mean;
}

constexpr float kThirtyDegrees = 30 * kDegree;
constexpr float kNearestTarget = 0.3f;     // closer than this the look ends
constexpr float kLeastLookWeight = 0.001f;

} // namespace

float LookEnvelope::at(float ms) const {
    if (ms <= begin) {
        return from;
    }
    if (ms >= begin + length || length <= 0) {
        return to;
    }
    return from + (to - from) * (ms - begin) / length;
}

float LookRequest::weightAt(double nowMs) const {
    return envelope ? envelope->at(static_cast<float>(nowMs - startMs)) : weight;
}

void LookController::lose(double nowMs) {
    if (request_ && request_->state < kLookEndedEarly) {
        setState(*request_, kLookEndedEarly, nowMs, stopChanges_);
    }
}

int LookController::following() const {
    if (incoming_ && incoming_->state < kLookDone) {
        return incoming_->id;
    }
    return request_ && request_->state < kLookDone ? request_->id : 0;
}

const LookRequest* LookController::current() const {
    return request_ && request_->state < kLookDone ? &*request_ : nullptr;
}

void LookController::setState(LookRequest& r, int state, double nowMs, std::vector<Change>& changes) {
    if (state == r.state || (state > kLookTurningTo && state < r.state)) {
        return;
    }
    if (r.state <= kLookHolding && state > kLookHolding) {
        r.envelope.reset();
        r.weight = 1;
    }
    if (state == kLookTurningTo) {
        scaleTo_ = 1;
        speed_ = r.towards * 0.3f;
        start_ = look_;
    } else if (state == kLookTimeUp || state == kLookEndedEarly) {
        scaleAway_ = 1;
        speed_ = r.away * 0.3f;
        start_ = look_;
    }
    const auto elapsed = static_cast<float>(nowMs - r.startMs);
    const float progress = elapsed + estimate_ != 0 ? elapsed / (elapsed + estimate_) : 0.0f;
    if (progress >= 1 || state >= kLookTimeUp) {
        r.remainingMs = 0;
        r.untilStopped = false;
    }
    r.state = state;
    const int percent = r.forever ? 0 : static_cast<int>(std::lround(progress * 100));
    changes.push_back({r.id, state, nowMs, percent, r.untilStopped ? -1 : static_cast<int>(r.remainingMs)});
}

void LookController::begin(const LookRequest& request) {
    if (request.towards > 0 && request.away > 0 && std::isfinite(request.towards) && std::isfinite(request.away)) {
        incoming_ = request;
        incoming_->untilStopped = request.forever;
    }
}

void LookController::stop(double nowMs, float away) {
    if (!(away > 0) || !std::isfinite(away)) {
        return;
    }
    for (auto* r : {&request_, &incoming_}) {
        if (*r && (*r)->state < kLookEndedEarly) {
            (*r)->away = away;
            setState(**r, (*r)->state == kLookNone ? kLookCancelled : kLookEndedEarly, nowMs, stopChanges_);
        }
    }
}

void LookController::update(double nowMs, const LookPose& pose, std::vector<Change>& changes) {
    changes.insert(changes.end(), stopChanges_.begin(), stopChanges_.end());
    stopChanges_.clear();
    const bool never = !updated_;
    const double last = lastMs_;
    lastMs_ = nowMs;
    updated_ = true;
    // A newer request replaces the one followed, and the head starts from the old one's weight.
    std::optional<float> previousWeight;
    bool first = false;
    if (incoming_ && incoming_->state < kLookDone) {
        if (request_ && request_->state < kLookDone) {
            setState(*request_, kLookCancelled, nowMs, changes);
        }
        if (request_ && started_) {
            previousWeight = request_->weightAt(nowMs);
        }
        request_ = incoming_;
        first = true;
        estimate_ = 2147483648.0f;
    }
    incoming_.reset();
    if (!request_ || request_->state >= kLookDone) {
        request_.reset();
        started_ = false;
        return;
    }
    LookRequest& r = *request_;
    const double dt = nowMs - (never ? r.startMs : std::max(r.startMs, last));
    float fromWeight = kLeastLookWeight;
    if (!started_) {
        // A look from idle starts where the head is turned now.
        rest_ = pose.headRest;
        look_ = product(inverse(rest_), pose.headTurn);
        euler_ = eulerOf(look_);
        insideCone_ = false;
        started_ = true;
    } else {
        fromWeight = previousWeight ? *previousWeight : r.weightAt(nowMs);
    }
    if (first) {
        setState(r, kLookTurningTo, nowMs, changes);
    }
    const float elapsed = static_cast<float>(nowMs - r.startMs);
    float cruiseTo = r.towards * scaleTo_, cruiseAway = r.away * scaleAway_;
    // The look's frame: the neck's turn with the head's rest turn, from the eye.
    const Quat frame = product(pose.neck, rest_);
    const Vec origin = added(pose.head, rotate(frame, pose.eye));
    const Vec toTarget = rotate(inverse(frame), sub(pose.target, origin));
    const float distance = std::sqrt(dot(toTarget, toTarget));
    LookAngles wanted = lookAngles(toTarget);
    const bool clamped = clampLook(wanted);
    const Quat desired = fromEuler(eulerOf(wanted));
    if (LookAngles own = anglesOf(euler_); !clampLook(own)) {
        insideCone_ = true;
    }
    double t = dt;
    if (first && t <= 0) {
        t = 1;
    }
    float remTo = 0;
    const Quat rest{0, 0, 0, 1};
    while (t > 0) {
        const auto ms = static_cast<float>(t);
        if (r.state == kLookTurningTo || r.state == kLookHolding) {
            if (distance < kNearestTarget) {
                r.away = 200 * kDegree;
                cruiseAway = r.away * scaleAway_;
                setState(r, kLookEndedEarly, nowMs, changes);
                if (r.state != kLookEndedEarly) {
                    break;
                }
                continue;
            }
            const float rem = apart(look_, desired), total = apart(start_, desired);
            if (first) {
                const float s = std::clamp(total / kThirtyDegrees, 0.3f, 1.0f);
                cruiseTo = cruiseTo / scaleTo_ * s;
                speed_ *= s;
                cruiseAway = cruiseAway / scaleAway_ * s;
                scaleAway_ = scaleTo_ = s;
                ramp_ = std::clamp(total * 0.25f / (cruiseTo * 0.001f), 25.0f, 300.0f);
                rampAngle_ = cruiseTo * 0.001f * ramp_ * 0.5f;
            }
            float mean = cruiseTo;
            if (r.state == kLookHolding) {
                speed_ = cruiseTo;
            } else {
                mean = easedSpeed(speed_, cruiseTo, total, rem, ramp_, rampAngle_, 0.5f * rampAngle_, ms);
            }
            const float step = ms * mean * 0.001f;
            if (!r.envelope && step <= rem) {
                // The head IK comes in over half the time a turn this far takes at cruising speed.
                r.envelope = LookEnvelope{fromWeight, 1, 0, rem / cruiseTo * 500};
                r.weight = r.envelope->at(elapsed);
            }
            if (step < rem) {
                look_ = slerped(look_, desired, step / rem);
                euler_ = eulerOf(look_);
                remTo = rem;
                if (r.state == kLookHolding && !r.untilStopped) {
                    r.remainingMs = std::max(r.remainingMs - ms, 0.0f);
                    if (r.remainingMs <= 0) {
                        setState(r, kLookTimeUp, nowMs, changes);
                    }
                }
                break;
            }
            look_ = desired;
            euler_ = eulerOf(wanted);
            insideCone_ = true;
            if (r.state == kLookTurningTo) {
                // The game carries on with the time it took to arrive, not the time left after.
                const double past = step > 0 ? std::round(t * (1 - rem / step)) : 0.0;
                setState(r, clamped && !r.noEarlyExit ? kLookEndedEarly : kLookHolding, nowMs, changes);
                t -= past;
                continue;
            }
            if (!r.untilStopped) {
                r.remainingMs = std::max(r.remainingMs - ms, 0.0f);
            }
            if (clamped && !r.noEarlyExit) {
                setState(r, kLookEndedEarly, nowMs, changes);
            } else if (!r.untilStopped && r.remainingMs <= 0) {
                setState(r, kLookTimeUp, nowMs, changes);
            }
            break;
        }
        if (r.state == kLookTimeUp || r.state == kLookEndedEarly) {
            const float rem = apart(look_, rest), total = apart(start_, rest);
            if (!r.envelope) {
                const float s = std::clamp(total / kThirtyDegrees, 0.3f, 1.0f);
                cruiseAway = cruiseAway / scaleAway_ * s;
                scaleAway_ = s;
                speed_ *= s;
                ramp_ = std::clamp(total * 0.25f / (cruiseAway * 0.001f), 25.0f, 300.0f);
                rampAngle_ = cruiseAway * 0.001f * ramp_ * 0.5f;
            }
            const float step = easedSpeed(speed_, cruiseAway, total, rem, ramp_, rampAngle_, 3 * rampAngle_, ms) * ms * 0.001f;
            if (step >= rem) {
                look_ = rest;
                euler_ = {};
                setState(r, r.state + 2, nowMs, changes);
            } else {
                look_ = slerped(look_, rest, step / rem);
                euler_ = eulerOf(lookAngles(rotate(look_, {0, 0, 1})));
                if (!r.envelope) {
                    // The head IK goes out over the time the turn back takes at cruising speed.
                    r.envelope = LookEnvelope{1, kLeastLookWeight, elapsed, total / cruiseAway * 1000};
                    r.weight = 1;
                } else if (r.envelope->end() <= elapsed) {
                    setState(r, r.state + 2, nowMs, changes);
                }
            }
            break;
        }
        r.envelope.reset();
        r.weight = kLeastLookWeight;
        break;
    }
    if (insideCone_) {
        if (LookAngles own = anglesOf(euler_); clampLook(own)) {
            euler_ = eulerOf(own);
            look_ = fromEuler(euler_);
        }
    }
    if (r.state < kLookDone) {
        float progress = 0;
        if (r.untilStopped) {
            estimate_ = 2147483392.0f;
        } else {
            const float remRest = apart(look_, rest);
            float estimate = r.remainingMs + (remTo > 0 ? remTo / cruiseTo * 1000 : 0.0f);
            estimate += r.envelope ? std::max(r.envelope->end() - elapsed, 0.0f) : remRest / cruiseAway * 1000;
            estimate_ = std::round(estimate);
            progress = elapsed + estimate_ != 0 ? elapsed / (elapsed + estimate_) : 0.0f;
        }
        if (progress >= 1) {
            setState(r, r.state == kLookEndedEarly ? kLookCancelled : kLookDone, nowMs, changes);
        }
    }
    if (r.state < kLookDone) {
        const Vec ahead = rotate(look_, {0, 0, distance});
        target_ = added(origin, rotate(frame, ahead));
        pole_ = added(origin, rotate(frame, {0, 1, 0}));
    }
}

bool clampLook(LookAngles& a) {
    const auto clampTo = [](float& v, float lo, float hi) {
        const float was = v;
        v = std::clamp(v, lo, hi);
        return v != was;
    };
    const bool pitch = clampTo(a.pitch, -51 * kDegree, 36 * kDegree);
    const bool yaw = clampTo(a.yaw, -70 * kDegree, 70 * kDegree);
    const bool roll = clampTo(a.roll, -40 * kDegree, 40 * kDegree);
    return pitch || yaw || roll;
}

} // namespace ts2::sim

namespace ts2::sim {

LookAtOperands lookAtOperands(std::span<const std::uint8_t> o) {
    LookAtOperands a;
    if (o.size() < 16) {
        return a;
    }
    a.flags = o[0];
    a.owner = o[1];
    a.data = static_cast<std::uint16_t>(o[2] | o[3] << 8);
    a.towards = o[4];
    a.away = o[5];
    a.duration = static_cast<std::int16_t>(o[6] | o[7] << 8);
    a.slot = o[8];
    a.eventTree = static_cast<std::uint16_t>(o[9] | o[10] << 8);
    a.pointKind = o[14];
    a.flags2 = o[15];
    return a;
}

} // namespace ts2::sim
