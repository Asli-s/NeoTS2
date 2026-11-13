#include "audio/mix.hpp"

#include <cmath>
#include <numbers>

namespace ts2::audio {

namespace {

Vec3 sub(const Vec3& a, const Vec3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

float dot(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

// The unit vector along `v`, or `fallback` when `v` has no length.
Vec3 normalized(const Vec3& v, const Vec3& fallback) {
    const float length = std::sqrt(dot(v, v));
    return length > 1e-6f ? Vec3{v[0] / length, v[1] / length, v[2] / length} : fallback;
}

} // namespace

float distanceGain(float distance, float minDistance, float maxDistance, float rolloff) {
    if (distance < minDistance || rolloff == 0) {
        return 1;
    }
    if (distance > maxDistance) {
        return 0;
    }
    // A minimum of 0 (a broken file) gives silence away from the listener, as the curve's division
    // does, and full volume on it.
    if (minDistance <= 0) {
        return distance > 0 ? 0.0f : 1.0f;
    }
    return 1 / ((distance / minDistance - 1) * rolloff + 1);
}

Listener cameraListener(const Vec3& eye, const Vec3& target, const Vec3& up, float minHeight, float toCameraOffset,
                        float positionsScalar) {
    Listener out;
    out.forward = normalized(sub(target, eye), {0, 1, 0});
    out.side = normalized(cross(out.forward, up), {1, 0, 0});
    out.up = cross(out.side, out.forward);
    out.position = {target[0] - out.forward[0] * toCameraOffset, target[1] - out.forward[1] * toCameraOffset,
                    target[2] - out.forward[2] * toCameraOffset + minHeight};
    out.ears = {eye[0] + (out.position[0] - eye[0]) * positionsScalar, eye[1] + (out.position[1] - eye[1]) * positionsScalar,
                eye[2] + (out.position[2] - eye[2]) * positionsScalar};
    return out;
}

float axisGain(const Listener& listener, const Vec3& source, float minDistance, float maxDistance, float rolloff,
               float scale, float aspectRatio) {
    const Vec3 offset = sub(source, listener.position);
    const float parts[3] = {dot(listener.side, offset), dot(listener.forward, offset), dot(listener.up, offset)};
    const float rolloffs[3] = {rolloff * scale, rolloff * scale, rolloff * scale * aspectRatio};
    float gain = 1;
    for (int i = 0; i < 3; ++i) {
        gain *= distanceGain(std::fabs(parts[i]), minDistance, maxDistance, rolloffs[i]);
    }
    return gain;
}

float azimuth(const Listener& listener, const Vec3& source) {
    const Vec3 offset = sub(source, listener.ears);
    return std::atan2(dot(listener.side, offset), dot(listener.forward, offset));
}

EarGains panGains(float azimuthRadians) {
    const float s = std::sin(azimuthRadians);
    return {std::sqrt(std::fmax(0.0f, (1 - s) / 2)), std::sqrt(std::fmax(0.0f, (1 + s) / 2))};
}

std::array<EarGains, 2> channelGains(float azimuthRadians, int channels) {
    constexpr float kQuarter = std::numbers::pi_v<float> / 2;
    if (channels < 2) {
        return {panGains(azimuthRadians), EarGains{}};
    }
    return {panGains(azimuthRadians - kQuarter), panGains(azimuthRadians + kQuarter)};
}

float effectivePriority(float priority, float gain, double msLeft) {
    constexpr double kFadeMs = 250;
    const double tail = msLeft < kFadeMs ? std::fmax(0.0, msLeft) * 0.004 : 1;
    return static_cast<float>(priority * gain * tail);
}

int chanceRoll(int draw) {
    return static_cast<std::int16_t>(draw * 100 / 0x7FFF);
}

bool chancePlays(std::uint32_t chance, int roll) {
    return chance == 0 || chance >= static_cast<std::uint32_t>(roll);
}

int AudioRandom::next() {
    state_ = state_ * 214013u + 2531011u;
    return static_cast<int>((state_ >> 16) & 0x7FFF);
}

} // namespace ts2::audio
