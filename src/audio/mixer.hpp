#pragma once

#include "audio/decode.hpp"
#include "audio/sound_bank.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace ts2::audio {

// A sound to start.
struct PlayRequest {
    std::shared_ptr<const Pcm> pcm;
    Category category = Category::Effects;
    float gain = 1;        // the track's own volume
    float pan = 0;         // -1 left to 1 right
    double pitch = 1;      // playback speed: 1 is the clip's own rate
    bool loop = false;
    float fadeInSeconds = 0, fadeOutSeconds = 0.1f;
    int priority = 32;     // with every voice busy, the lowest priority playing makes way
};

// Mixes what plays into the output, interleaved stereo floats. play, set and stop come from the game's
// thread and mix from the audio thread; a mutex keeps them apart. Volumes and pans move to new values
// over one mix call, so changes don't click.
class Mixer {
public:
    using VoiceId = std::uint64_t;

    explicit Mixer(int outputRate, std::size_t maxVoices = 24);

    // 0 when the request has no clip, or every voice is busy with sounds of higher priority.
    VoiceId play(const PlayRequest& request);
    // A 3D sound's volume and pan as its source and the listener move.
    void set(VoiceId id, float gain, float pan);
    void setPitch(VoiceId id, double pitch);
    // Fades it out over its fade-out time (or at once), then lets the voice go.
    void stop(VoiceId id, bool fade = true);
    void stopAll(bool fade = true);
    [[nodiscard]] bool playing(VoiceId id) const;

    // The option volumes, 0 to 1, and the master volume over them.
    void setCategoryVolume(Category category, float volume);
    void setMasterVolume(float volume);
    // Paused, everything holds its place (the lot's sounds while the game is paused).
    void setPaused(Category category, bool paused);

    // Fills `frames` stereo frames. Called by the audio device's thread.
    void mix(float* out, std::size_t frames);

    [[nodiscard]] int outputRate() const { return outputRate_; }
    [[nodiscard]] std::size_t voicesPlaying() const;

private:
    struct Voice {
        VoiceId id = 0;
        PlayRequest request;
        double position = 0;  // in the clip's frames
        float gain = 1, pan = 0;
        float appliedLeft = -1, appliedRight = -1;  // last mix's gains, -1 before the first
        float fade = 0;       // 0 to 1
        float fadeStep = 0;   // per output frame
        bool stopping = false;
    };

    int outputRate_;
    std::size_t maxVoices_;
    mutable std::mutex mutex_;
    std::vector<Voice> voices_;
    VoiceId next_ = 1;
    std::array<float, 4> categoryVolume_{1, 1, 1, 1};
    std::array<bool, 4> paused_{};
    float master_ = 1;
};

// The gain and pan of a 3D sound at `distance` from the listener, the game's DirectSound model: full
// volume to `minDistance`, then minDistance / (minDistance + rolloff * (distance - minDistance)), held
// past `maxDistance`. `side` is where the source lies across the listener's view, -1 left to 1 right.
struct Spatial {
    float gain = 1, pan = 0;
};
Spatial spatial(float distance, float side, float minDistance, float maxDistance, float rolloff);

} // namespace ts2::audio
