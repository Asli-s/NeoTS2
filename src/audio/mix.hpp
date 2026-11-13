#pragma once

// The game's rules for how loud a sound is and which ear it is in, as pure functions.

#include <array>
#include <cstdint>

namespace ts2::audio {

using Vec3 = std::array<float, 3>;

// The distance drop: 1 up to `minDistance`, 0 past `maxDistance`, else
// 1 / ((d / min - 1) * rolloff + 1). A rolloff of 0 means no drop.
float distanceGain(float distance, float minDistance, float maxDistance, float rolloff);

// Where the listener hears from. Rows of the frame are its side (screen right), forward and up.
struct Listener {
    Vec3 position{};   // the attenuation point
    Vec3 ears{};       // where the direction to a sound is taken from
    Vec3 side{1, 0, 0};
    Vec3 forward{0, 1, 0};
    Vec3 up{0, 0, 1};
};

// The listener for a camera: above the camera's target by `minHeight`, moved back along the view
// by `toCameraOffset`, with the ears `positionsScalar` of the way from the eye to it. Its frame is
// the camera's (side = forward x up).
Listener cameraListener(const Vec3& eye, const Vec3& target, const Vec3& up, float minHeight, float toCameraOffset,
                        float positionsScalar);

// The drop each listener axis gives, multiplied: each axis takes the distance curve on its own
// part of the offset, with rolloff `rolloff` times (scale, scale, scale * aspectRatio).
float axisGain(const Listener& listener, const Vec3& source, float minDistance, float maxDistance, float rolloff,
               float scale, float aspectRatio);

// The direction of a sound in radians from straight ahead, positive to the right.
float azimuth(const Listener& listener, const Vec3& source);

// Equal power between the ears: right sqrt((1 + sin a) / 2), left sqrt((1 - sin a) / 2).
struct EarGains {
    float left = 0;
    float right = 0;
};
EarGains panGains(float azimuthRadians);

// The left and right gains of each channel of a source at `azimuthRadians`. A stereo source's
// channels sit a quarter turn to each side of it (left at -90 degrees), so it never pans.
std::array<EarGains, 2> channelGains(float azimuthRadians, int channels);

// A voice's priority as the steal rule weighs it: its track priority times its distance gain,
// cut down over its last 250 ms.
float effectivePriority(float priority, float gain, double msLeft);

// The game's play chance roll from a draw of 0 to 32767: 0 to 100.
int chanceRoll(int draw);
// Whether a track with that chance (in percent) plays for a roll. A chance of 0 always plays.
bool chancePlays(std::uint32_t chance, int roll);

// The random draws the play chance and sample choice take: 0 to 32767, the same series for the
// same seed. NeoTS2's own, as the game's seed is not known.
class AudioRandom {
public:
    explicit AudioRandom(std::uint32_t seed = 1) : state_(seed) {}
    int next();

private:
    std::uint32_t state_;
};

} // namespace ts2::audio
