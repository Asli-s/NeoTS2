#include "game_sound.hpp"

#include "content/user_props.hpp"
#include "diag/log.hpp"
#include "platform/platform.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace app {

namespace {

using ts2::audio::Category;
using ts2::render::Vec3;

constexpr int kOutputRate = 44100;           // the game's DefaultOutputRate
constexpr float kMusicVolume = 0.7f * 0.75f;  // defaultmusicvolume times 2dMusicVolumeScalar
constexpr float kSongFadeSeconds = 3;         // trackfadeouttime
// The listener sits over the camera's target, a fifth of the camera's height above it, at least 1
// (listenerheightscale, listenerminheight).
constexpr float kListenerHeightScale = 0.2f, kListenerMinHeight = 1;

Vec3 minus(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
float length(const Vec3& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

float percent(const ts2::content::UserProps& props, const char* key, float fallback) {
    const auto found = props.values.find(key);
    if (found == props.values.end()) {
        return fallback;
    }
    int value = 0;
    const auto& text = found->second;
    if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{}) {
        return fallback;
    }
    return static_cast<float>(std::clamp(value, 0, 100)) / 100.0f;
}

// A track's pitch over its clip's own rate, with its random pitch in semitones.
double pitchOf(const ts2::audio::PickedSound& s, std::mt19937& random) {
    double pitch = s.track.pitchHz > 0 && s.pcm->rate > 0 ? static_cast<double>(s.track.pitchHz) / s.pcm->rate : 1.0;
    const int low = std::min(s.track.pitchRandomMin, s.track.pitchRandomMax), high = std::max(s.track.pitchRandomMin, s.track.pitchRandomMax);
    if (high > low || low != 0) {
        std::uniform_real_distribution<double> semitones(low, high);
        pitch *= std::pow(2.0, semitones(random) / 12.0);
    }
    return std::clamp(pitch, 0.05, 8.0);
}

} // namespace

GameSound::GameSound(const std::filesystem::path& install, const std::filesystem::path& userFolder) {
    mixer_ = std::make_unique<ts2::audio::Mixer>(kOutputRate);
    readOptions(userFolder);
    started_ = ts2::platform::startAudio(kOutputRate, &GameSound::fill, this);
    if (!started_) {
        ts2::diag::warning("sound: no sound out: " + ts2::platform::lastError());
        return;
    }
    bank_ = std::async(std::launch::async, [install]() -> std::shared_ptr<ts2::audio::SoundBank> {
                try {
                    return std::make_shared<ts2::audio::SoundBank>(install);
                } catch (const std::exception& e) {
                    ts2::diag::warning(std::string("sound: the sound packages can't be read: ") + e.what());
                    return nullptr;
                }
            }).share();
}

GameSound::~GameSound() {
    if (started_) {
        ts2::platform::stopAudio();  // the device's thread stops calling fill before the mixer goes
    }
}

void GameSound::fill(void* self, float* out, std::size_t frames) {
    static_cast<GameSound*>(self)->mixer_->mix(out, frames);
}

void GameSound::readOptions(const std::filesystem::path& userFolder) {
    const auto props = ts2::content::readUserProps(userFolder);
    const auto on = [&](const char* key) { return props.flag(key).value_or(true); };
    mixer_->setCategoryVolume(Category::Effects, on("SoundMasterGroupFXEnabled") ? percent(props, "FXVolume", 1) : 0);
    mixer_->setCategoryVolume(Category::Voice, on("SoundMasterGroupVoxEnabled") ? percent(props, "VOXVolume", 1) : 0);
    mixer_->setCategoryVolume(Category::Music, on("SoundMasterGroupMusicEnabled") ? percent(props, "MusicVolume", 0.4f) : 0);
    mixer_->setCategoryVolume(Category::Ambience, on("SoundMasterGroupAmbienceEnabled") ? percent(props, "AmbienceVolume", 1) : 0);
}

void GameSound::tellOnce(const std::string& text) {
    // Many of the names scripts use have no sound in the game; each is told once, and only so many.
    if (told_.size() < 200 && told_.insert(text).second) {
        ts2::diag::info(text);
    }
}

ts2::audio::SoundBank* GameSound::bank() {
    if (!started_ || !bank_.valid() || bank_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return nullptr;
    }
    return bank_.get().get();
}

void GameSound::music(std::string_view station) {
    if (station == station_) {
        return;
    }
    station_ = std::string(station);
    songs_.reset();
    if (song_ != 0) {
        mixer_->stop(song_);  // fades out over kSongFadeSeconds
        song_ = 0;
    }
    nextSong_ = {};
}

void GameSound::update() {
    ts2::audio::SoundBank* sounds = bank();
    if (sounds == nullptr || station_.empty()) {
        return;
    }
    if (!songs_) {
        auto list = sounds->station(station_);
        if (list.empty()) {
            tellOnce(std::format("sound: no songs for the station {}", station_));
            station_.clear();
            return;
        }
        songs_ = std::make_unique<ts2::audio::SoundBank::HitList>(std::move(list));
    }
    if (song_ != 0 && mixer_->playing(song_)) {
        return;
    }
    song_ = 0;
    if (!nextSong_.valid()) {
        const auto key = songs_->choose(random_);
        auto shared = bank_;
        nextSong_ = std::async(std::launch::async, [shared, key] { return shared.get()->clip(key); });  // a song takes a moment to decode
        return;
    }
    if (nextSong_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }
    auto pcm = nextSong_.get();
    if (!pcm) {
        return;  // the next update picks another
    }
    ts2::audio::PlayRequest r;
    r.pcm = std::move(pcm);
    r.category = Category::Music;
    r.gain = kMusicVolume;
    r.fadeInSeconds = 1;
    r.fadeOutSeconds = kSongFadeSeconds;
    r.priority = 127;
    song_ = mixer_->play(r);
    ts2::diag::info(std::format("sound: a {} song starts ({:.0f} s)", station_, static_cast<double>(r.pcm->frames()) / r.pcm->rate));
}

void GameSound::lotSound(const std::string& name, std::int16_t source, bool stop) {
    if (stop) {
        for (const LotVoice& v : lotVoices_) {
            if (v.source == source && v.name == name) {
                mixer_->stop(v.id);
            }
        }
        std::erase_if(lotVoices_, [&](const LotVoice& v) { return v.source == source && v.name == name; });
        return;
    }
    ts2::audio::SoundBank* sounds = bank();
    if (sounds == nullptr) {
        return;
    }
    // A loop asked for again while it plays goes on, as a script repeats its start each time round.
    std::erase_if(lotVoices_, [&](const LotVoice& v) { return !mixer_->playing(v.id); });
    if (std::any_of(lotVoices_.begin(), lotVoices_.end(), [&](const LotVoice& v) { return v.source == source && v.name == name && v.track.looped; })) {
        return;
    }
    std::string heard = name;
    if (ts2::audio::isVoiceName(name)) {
        const auto voice = voiceOf_ ? voiceOf_(source) : std::nullopt;
        if (!voice) {
            tellOnce("sound: a voice line from an object that isn't a Sim: " + name);
            return;
        }
        const auto names = ts2::audio::voiceNames(name, *voice);
        const auto found = std::find_if(names.begin(), names.end(), [&](const std::string& n) { return sounds->has(n); });
        if (found == names.end()) {
            tellOnce("sound: the game has no voice for " + names.front());
            return;
        }
        heard = *found;
    }
    auto picked = sounds->pick(heard);
    if (!picked) {
        tellOnce("sound: the game has no sound named " + heard);
        return;
    }
    ts2::audio::PlayRequest r;
    r.pcm = picked->pcm;
    r.category = picked->category;
    r.gain = picked->track.volume;
    r.pan = picked->track.pan;
    r.pitch = pitchOf(*picked, random_);
    r.loop = picked->track.looped;
    r.fadeInSeconds = static_cast<float>(picked->track.fadeInMs) / 1000.0f;
    r.fadeOutSeconds = static_cast<float>(picked->track.fadeOutMs) / 1000.0f;
    r.priority = picked->track.priority;
    if (picked->track.is3d) {
        r.gain = 0;  // placed on the next frame, before it is heard
        r.fadeInSeconds = std::max(r.fadeInSeconds, 0.02f);
    }
    const auto id = mixer_->play(r);
    if (id != 0) {
        lotVoices_.push_back({id, source, name, picked->track, false});
        tellOnce(std::format("sound: plays {}{}", heard, picked->track.is3d ? " (3D)" : ""));
    }
}

void GameSound::lotFrame(const ts2::render::Camera& camera, const PlaceOf& placeOf, bool paused) {
    mixer_->setPaused(Category::Effects, paused);
    mixer_->setPaused(Category::Voice, paused);
    mixer_->setPaused(Category::Ambience, paused);
    const Vec3 ahead = minus(camera.target, camera.eye);
    const float height = std::max(kListenerMinHeight, (camera.eye[2] - camera.target[2]) * kListenerHeightScale);
    const Vec3 listener{camera.target[0], camera.target[1], camera.target[2] + height};
    Vec3 right{ahead[1], -ahead[0], 0};  // ahead turned a quarter to the right, level
    const float r = length(right);
    right = r > 0 ? Vec3{right[0] / r, right[1] / r, 0} : Vec3{1, 0, 0};
    std::erase_if(lotVoices_, [&](LotVoice& v) {
        if (!mixer_->playing(v.id)) {
            return true;
        }
        if (!v.track.is3d) {
            return false;
        }
        const auto place = placeOf(v.source);
        if (!place) {
            if (v.placed) {  // its object went: so does its sound
                mixer_->stop(v.id);
                return true;
            }
            return false;
        }
        v.placed = true;
        const Vec3 to = minus(*place, listener);
        const float distance = length(to);
        const float side = distance > 0 ? (to[0] * right[0] + to[1] * right[1]) / distance : 0;
        const auto s = ts2::audio::spatial(distance, side, v.track.minDistance, v.track.maxDistance, v.track.rolloff);
        mixer_->set(v.id, s.gain * v.track.volume, s.pan);
        return false;
    });
}

void GameSound::leaveLot() {
    for (const LotVoice& v : lotVoices_) {
        mixer_->stop(v.id);
    }
    lotVoices_.clear();
    mixer_->setPaused(Category::Effects, false);
    mixer_->setPaused(Category::Voice, false);
    mixer_->setPaused(Category::Ambience, false);
}

} // namespace app
