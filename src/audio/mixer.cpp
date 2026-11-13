#include "audio/mixer.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::audio {

namespace {

// Equal-power pan: the middle is 0.707 a side, a hard side is 1 there and 0 on the other.
void panGains(float pan, float& left, float& right) {
    constexpr float kQuarterPi = 0.78539816f;
    const float angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * kQuarterPi;
    left = std::cos(angle);
    right = std::sin(angle);
}

} // namespace

Spatial spatial(float distance, float side, float minDistance, float maxDistance, float rolloff) {
    Spatial s;
    const float d = std::clamp(distance, minDistance, std::max(minDistance, maxDistance));
    s.gain = minDistance / (minDistance + std::max(0.0f, rolloff) * (d - minDistance));
    // Nearby sounds are heard more in the middle, as a source beside the listener is heard by both ears.
    const float spread = std::clamp(distance / std::max(minDistance, 0.001f), 0.0f, 1.0f);
    s.pan = std::clamp(side, -1.0f, 1.0f) * spread;
    return s;
}

Mixer::Mixer(int outputRate, std::size_t maxVoices) : outputRate_(std::max(outputRate, 8000)), maxVoices_(std::max<std::size_t>(maxVoices, 1)) {}

Mixer::VoiceId Mixer::play(const PlayRequest& request) {
    if (!request.pcm || request.pcm->frames() == 0 || request.pcm->rate <= 0) {
        return 0;
    }
    const std::lock_guard lock(mutex_);
    if (voices_.size() >= maxVoices_) {
        // Make way: a stopping voice first, else the lowest priority below this one's.
        auto victim = std::find_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.stopping; });
        if (victim == voices_.end()) {
            victim = std::min_element(voices_.begin(), voices_.end(),
                                      [](const Voice& a, const Voice& b) { return a.request.priority < b.request.priority; });
            if (victim->request.priority > request.priority) {
                return 0;
            }
        }
        voices_.erase(victim);
    }
    Voice v;
    v.id = next_++;
    v.request = request;
    v.gain = request.gain;
    v.pan = request.pan;
    if (request.fadeInSeconds > 0) {
        v.fade = 0;
        v.fadeStep = 1.0f / (request.fadeInSeconds * static_cast<float>(outputRate_));
    } else {
        v.fade = 1;
    }
    voices_.push_back(std::move(v));
    return voices_.back().id;
}

void Mixer::set(VoiceId id, float gain, float pan) {
    const std::lock_guard lock(mutex_);
    for (Voice& v : voices_) {
        if (v.id == id) {
            v.gain = std::max(0.0f, gain);
            v.pan = std::clamp(pan, -1.0f, 1.0f);
        }
    }
}

void Mixer::setPitch(VoiceId id, double pitch) {
    const std::lock_guard lock(mutex_);
    for (Voice& v : voices_) {
        if (v.id == id) {
            v.request.pitch = std::clamp(pitch, 0.05, 8.0);
        }
    }
}

void Mixer::stop(VoiceId id, bool fade) {
    const std::lock_guard lock(mutex_);
    for (auto it = voices_.begin(); it != voices_.end(); ++it) {
        if (it->id != id) {
            continue;
        }
        if (!fade || it->request.fadeOutSeconds <= 0) {
            voices_.erase(it);
        } else if (!it->stopping) {
            it->stopping = true;
            it->fadeStep = -1.0f / (it->request.fadeOutSeconds * static_cast<float>(outputRate_));
        }
        return;
    }
}

void Mixer::stopAll(bool fade) {
    const std::lock_guard lock(mutex_);
    if (!fade) {
        voices_.clear();
        return;
    }
    for (Voice& v : voices_) {
        v.stopping = true;
        v.fadeStep = -1.0f / (std::max(v.request.fadeOutSeconds, 0.05f) * static_cast<float>(outputRate_));
    }
}

bool Mixer::playing(VoiceId id) const {
    const std::lock_guard lock(mutex_);
    return std::any_of(voices_.begin(), voices_.end(), [&](const Voice& v) { return v.id == id && !v.stopping; });
}

std::size_t Mixer::voicesPlaying() const {
    const std::lock_guard lock(mutex_);
    return voices_.size();
}

void Mixer::setCategoryVolume(Category category, float volume) {
    const std::lock_guard lock(mutex_);
    categoryVolume_.at(static_cast<std::size_t>(category)) = std::clamp(volume, 0.0f, 1.0f);
}

void Mixer::setMasterVolume(float volume) {
    const std::lock_guard lock(mutex_);
    master_ = std::clamp(volume, 0.0f, 1.0f);
}

void Mixer::setPaused(Category category, bool paused) {
    const std::lock_guard lock(mutex_);
    paused_.at(static_cast<std::size_t>(category)) = paused;
}

void Mixer::mix(float* out, std::size_t frames) {
    std::fill(out, out + frames * 2, 0.0f);
    const std::lock_guard lock(mutex_);
    for (Voice& v : voices_) {
        const auto category = static_cast<std::size_t>(v.request.category);
        if (paused_.at(category)) {
            continue;
        }
        const Pcm& pcm = *v.request.pcm;
        const std::size_t length = pcm.frames();
        const auto channels = static_cast<std::size_t>(pcm.channels);
        const double step = static_cast<double>(pcm.rate) * v.request.pitch / outputRate_;
        float left = 0, right = 0;
        panGains(v.pan, left, right);
        const float level = v.gain * categoryVolume_.at(category) * master_;
        left *= level;
        right *= level;
        const float fromLeft = v.appliedLeft < 0 ? left : v.appliedLeft;
        const float fromRight = v.appliedRight < 0 ? right : v.appliedRight;
        const float frameCount = static_cast<float>(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            if (v.position >= static_cast<double>(length)) {
                if (!v.request.loop) {
                    break;
                }
                v.position = std::fmod(v.position, static_cast<double>(length));
            }
            const auto at = static_cast<std::size_t>(v.position);
            const auto t = static_cast<float>(v.position - static_cast<double>(at));
            std::size_t next = at + 1;
            if (next >= length) {
                next = v.request.loop ? 0 : at;
            }
            const auto sample = [&](std::size_t frame, std::size_t channel) {
                return static_cast<float>(pcm.samples[frame * channels + std::min(channel, channels - 1)]) * (1.0f / 32768.0f);
            };
            const float l = sample(at, 0) + (sample(next, 0) - sample(at, 0)) * t;
            const float r = sample(at, 1) + (sample(next, 1) - sample(at, 1)) * t;
            const float blend = static_cast<float>(i) / frameCount;
            v.fade = std::clamp(v.fade + v.fadeStep, 0.0f, 1.0f);
            out[2 * i] += l * (fromLeft + (left - fromLeft) * blend) * v.fade;
            out[2 * i + 1] += r * (fromRight + (right - fromRight) * blend) * v.fade;
            v.position += step;
        }
        v.appliedLeft = left;
        v.appliedRight = right;
    }
    std::erase_if(voices_, [](const Voice& v) {
        const bool ended = !v.request.loop && v.position >= static_cast<double>(v.request.pcm->frames());
        return ended || (v.stopping && v.fade <= 0);
    });
    for (std::size_t i = 0; i < frames * 2; ++i) {
        out[i] = std::clamp(out[i], -1.0f, 1.0f);
    }
}

} // namespace ts2::audio
