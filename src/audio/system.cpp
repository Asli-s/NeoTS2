#include "audio/system.hpp"

#include "audio/bank.hpp"
#include "audio/xa.hpp"
#include "diag/log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <format>
#include <future>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <vector>

namespace ts2::audio {

namespace {

constexpr int kOutputRate = 48000;
constexpr int kOutputChannels = 2;
constexpr std::size_t kMaxVoices = 32;
constexpr std::size_t kMaxCacheBytes = 64u << 20;

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// The vox suffix the game appends: age letter, and for children up the voice, for teens and older
// the sex too. Ghosts speak with the ghost voice.
std::string voxSuffix(const Context& context) {
    const int age = context.age;
    char letter = 'a';
    if (age < 2) {
        letter = 'b';
    } else if (age == 2) {
        letter = 'p';
    } else if (age < 16) {
        letter = 'c';
    } else if (age < 19) {
        letter = 't';
    } else if (age > 50) {
        letter = 'e';
    }
    if (age < 3) {
        return {letter};
    }
    const int voiceCount = age >= 19 && age <= 50 ? 2 : 1;
    char voice = (context.ghost & 1) != 0 ? 'g' : static_cast<char>('a' + std::max<int>(0, context.voice) % voiceCount);
    if (age < 19) {
        return {letter, voice};
    }
    return {letter, context.gender == 1 ? 'f' : 'm', voice};
}

} // namespace

struct System::Impl {
    struct Voice {
        std::shared_ptr<const Pcm> pcm;
        std::string name;      // the event's own name, before modifiers
        std::string resolved;  // the track it was played as
        std::int16_t source = 0;
        double position = 0;  // in source frames
        double step = 1;
        bool loop = false;
        std::optional<std::array<float, 3>> at;  // lot position; none: centred
    };

    Bank bank;
    bool installStarted = false;
    std::future<void> installing;
    std::atomic<bool> ready{false};
    SDL_AudioStream* stream = nullptr;
    std::mutex mutex;
    std::vector<Voice> voices;
    std::map<std::string, std::shared_ptr<const Pcm>> cache;
    std::size_t cacheBytes = 0;
    std::mt19937 random{0x5132};
    std::set<std::string> missed;
    std::map<std::string, std::string> aliases;
    std::array<float, 3> listener{};
    std::array<float, 3> right{1, 0, 0};
    bool paused = false;

    [[nodiscard]] bool openDevice();
    void mix(int frames, std::int16_t* out);
    static void SDLCALL feed(void* userdata, SDL_AudioStream* stream, int additional, int total);

    [[nodiscard]] std::shared_ptr<const Pcm> load(const SampleLocation& location);
    [[nodiscard]] double playResolved(const std::string& resolved, std::string_view name, std::int16_t source,
                                      std::optional<std::array<float, 3>> at = std::nullopt, bool quiet = false);
};

bool System::Impl::openDevice() {
    if (stream != nullptr) {
        return true;
    }
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        ts2::diag::warnOnce(std::string("no audio device: ") + SDL_GetError());
        return false;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, kOutputChannels, kOutputRate};
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Impl::feed, this);
    if (stream == nullptr) {
        ts2::diag::warnOnce(std::string("no audio device: ") + SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream);
    return true;
}

void System::Impl::feed(void* userdata, SDL_AudioStream* stream, int additional, int) {
    if (additional <= 0) {
        return;
    }
    auto* self = static_cast<Impl*>(userdata);
    // The callback asks for a block; anything beyond one mix pass is fed as silence rather than
    // allocating in the audio thread.
    std::vector<std::int16_t> buffer(static_cast<std::size_t>(additional) / sizeof(std::int16_t));
    const int frames = additional / (kOutputChannels * static_cast<int>(sizeof(std::int16_t)));
    if (frames > 0) {
        self->mix(frames, buffer.data());
    }
    SDL_PutAudioStreamData(stream, buffer.data(), additional);
}

void System::Impl::mix(int frames, std::int16_t* out) {
    std::lock_guard lock(mutex);
    std::fill_n(out, static_cast<std::size_t>(frames) * kOutputChannels, std::int16_t{0});
    for (auto it = voices.begin(); it != voices.end();) {
        Voice& voice = *it;
        const Pcm& pcm = *voice.pcm;
        const std::size_t pcmFrames = pcm.samples.size() / pcm.channels;
        if (pcmFrames == 0) {
            it = voices.erase(it);
            continue;
        }
        if (paused && voice.loop) {
            ++it;
            continue;
        }
        // Distance and pan from the listener (the camera's target): full volume within five tiles,
        // then (5/d)^1.2, and pan by the side of the camera the source is on.
        float gain = 1, pan = 0;
        if (voice.at) {
            const float dx = (*voice.at)[0] - listener[0];
            const float dy = (*voice.at)[1] - listener[1];
            const float dz = (*voice.at)[2] - listener[2];
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            constexpr float kMinDistance = 5;
            constexpr float kRolloff = 1.2f;
            gain = distance <= kMinDistance ? 1 : std::pow(kMinDistance / distance, kRolloff);
            const float flat = std::hypot(dx, dy);
            const float side = flat > 1e-4f ? (dx * right[0] + dy * right[1]) / flat : 0;
            pan = side * std::min(1.0f, flat / kMinDistance) * 0.8f;
        }
        // Loops sit under the one-shots: an aquarium running all day must not drown the toilet.
        const float base = voice.loop ? 0.4f : 0.7f;
        const float leftGain = base * gain * (1 - std::max(0.0f, pan));
        const float rightGain = base * gain * (1 + std::min(0.0f, pan));
        for (int i = 0; i < frames; ++i) {
            const auto pos = static_cast<std::size_t>(voice.position);
            if (pos >= pcmFrames) {
                if (!voice.loop) {
                    break;
                }
                voice.position = std::fmod(voice.position, static_cast<double>(pcmFrames));
            }
            const auto a = static_cast<std::size_t>(voice.position);
            const auto b = a + 1 < pcmFrames ? a + 1 : (voice.loop ? 0 : a);
            const float t = static_cast<float>(voice.position - static_cast<double>(a));
            const float leftA = pcm.samples[a * pcm.channels];
            const float rightA = pcm.channels > 1 ? pcm.samples[a * pcm.channels + 1] : leftA;
            const float leftB = pcm.samples[b * pcm.channels];
            const float rightB = pcm.channels > 1 ? pcm.samples[b * pcm.channels + 1] : leftB;
            const float leftSample = leftA + (leftB - leftA) * t;
            const float rightSample = rightA + (rightB - rightA) * t;
            auto& l = out[i * kOutputChannels];
            auto& r = out[i * kOutputChannels + 1];
            l = static_cast<std::int16_t>(std::clamp<int>(l + static_cast<int>(leftSample * leftGain), -32768, 32767));
            r = static_cast<std::int16_t>(std::clamp<int>(r + static_cast<int>(rightSample * rightGain), -32768, 32767));
            voice.position += voice.step;
        }
        if (!voice.loop && voice.position >= static_cast<double>(pcmFrames)) {
            it = voices.erase(it);
        } else {
            ++it;
        }
    }
}

std::shared_ptr<const Pcm> System::Impl::load(const SampleLocation& location) {
    const std::string key = std::to_string(location.instance) + ":" + std::to_string(location.instanceHigh);
    if (const auto it = cache.find(key); it != cache.end()) {
        return it->second;
    }
    const auto bytes = bank.read(location);
    if (bytes.empty()) {
        return nullptr;
    }
    if (bytes.size() < 2 || bytes[0] != 'X' || bytes[1] != 'A') {
        ts2::diag::warnOnce("sound sample format not supported yet");
        return nullptr;
    }
    try {
        auto pcm = std::make_shared<const Pcm>(decodeXa(bytes));
        if (pcm->samples.empty()) {
            return nullptr;
        }
        if (cacheBytes + pcm->samples.size() * sizeof(std::int16_t) > kMaxCacheBytes) {
            cache.clear();
            cacheBytes = 0;
        }
        cacheBytes += pcm->samples.size() * sizeof(std::int16_t);
        cache.emplace(key, pcm);
        return pcm;
    } catch (const std::exception& ex) {
        ts2::diag::warnOnce(std::string("sound sample: ") + ex.what());
        return nullptr;
    }
}

double System::Impl::playResolved(const std::string& resolved, std::string_view name, std::int16_t source,
                                  std::optional<std::array<float, 3>> at, bool quiet) {
    const bool loop = bank.loops(resolved);
    if (loop) {
        // One loop per object and track: a script that asks again while it plays is not a second
        // voice, even when a different event name resolves to the same track.
        std::lock_guard lock(mutex);
        for (const Voice& voice : voices) {
            if (voice.loop && voice.source == source && voice.resolved == resolved) {
                return 0;
            }
        }
    }
    const auto locations = bank.samplesOf(resolved);
    if (locations.empty()) {
        return 0;
    }
    const auto pcm = load(locations[random() % locations.size()]);
    if (!pcm) {
        return 0;
    }
    if (!quiet) {
        const std::string where = at ? std::format(" at {:.2f},{:.2f},{:.2f}", (*at)[0], (*at)[1], (*at)[2])
                                     : std::string(" centred");
        ts2::diag::info(std::format("audio: playing {} ({:.2f} s, {} Hz){} on object {}{}", resolved, pcm->seconds(),
                                    pcm->sampleRate, bank.loops(resolved) ? ", looping" : "", source, where));
    }
    Voice voice;
    voice.pcm = pcm;
    voice.name = std::string(name);
    voice.resolved = resolved;
    voice.source = source;
    voice.step = static_cast<double>(pcm->sampleRate) / kOutputRate;
    voice.loop = loop;
    voice.at = at;
    std::lock_guard lock(mutex);
    if (voices.size() >= kMaxVoices) {
        voices.erase(voices.begin());
    }
    voices.push_back(std::move(voice));
    return pcm->seconds();
}

System::System() : impl_(std::make_unique<Impl>()) {}

System::~System() {
    if (impl_->installing.valid()) {
        impl_->installing.wait();
    }
    if (impl_->stream != nullptr) {
        SDL_DestroyAudioStream(impl_->stream);
    }
}

void System::addInstall(const std::filesystem::path& installRoot) {
    if (impl_->installStarted) {
        return;
    }
    impl_->installStarted = true;
    // Indexing the sound packages takes a while (250 packages, every index read), so it runs beside
    // the menu. Events before it finishes are dropped; --play-sound waits.
    impl_->installing = std::async(std::launch::async, [impl = impl_.get(), installRoot] {
        impl->bank.addInstall(installRoot);
        for (const auto& [key, value] : impl->bank.propertySet("aliases")) {
            impl->aliases[lower(key)] = value;
        }
        impl->ready = true;
        ts2::diag::info("audio: indexed the install's sound packages");
    });
}

std::string System::resolve(std::string_view name, const Context& context) {
    if (name.empty() || !impl_->ready) {
        return {};
    }
    const std::string base = lower(name);
    std::vector<std::string> candidates;
    const auto add = [&](std::string suffix) {
        std::string candidate = base;
        if (!suffix.empty()) {
            candidate += "_";
            candidate += suffix;
        }
        if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end()) {
            candidates.push_back(std::move(candidate));
        }
    };
    // Most specific first: the game appends every modifier it knows, then drops from the end until
    // a track exists.
    std::string quality = context.quality, dest = context.destSurface, source = context.sourceSurface, shoe = context.shoe;
    std::string full;
    if (!quality.empty()) full += "_" + quality;
    if (!dest.empty()) full += "_" + dest;
    if (!source.empty()) full += "_" + source;
    if (!shoe.empty()) full += "_" + shoe;
    if (!full.empty()) add(full.substr(1));
    if (!quality.empty() && !dest.empty()) add(quality + "_" + dest);
    if (!dest.empty() && !quality.empty()) add(dest + "_" + quality);
    if (!dest.empty() && !source.empty()) add(dest + "_" + source);
    if (!quality.empty()) add(quality);
    if (!dest.empty()) add(dest);
    if (!source.empty()) add(source);
    if (base.starts_with("vox") && context.person) add(voxSuffix(context));
    add({});

    for (const auto& candidate : candidates) {
        if (const auto alias = impl_->aliases.find(candidate); alias != impl_->aliases.end()) {
            if (impl_->bank.hasTrack(alias->second)) {
                return alias->second;
            }
        }
        if (impl_->bank.hasTrack(candidate)) {
            return candidate;
        }
    }
    return {};
}

void System::play(std::string_view name, std::int16_t source, bool stop, const Context& context,
                  std::optional<std::array<float, 3>> position) {
    if (!impl_->ready) {
        return;
    }
    if (stop) {
        std::lock_guard lock(impl_->mutex);
        std::erase_if(impl_->voices, [&](const Impl::Voice& voice) {
            return voice.source == source && (voice.name == name || voice.resolved == name);
        });
        return;
    }
    if (!impl_->openDevice()) {
        return;
    }
    const std::string resolved = resolve(name, context);
    if (resolved.empty()) {
        if (impl_->missed.insert(std::string(name)).second) {
            ts2::diag::warning("audio: no track for sound event " + std::string(name));
        }
        return;
    }
    (void)impl_->playResolved(resolved, name, source, std::move(position));
}

void System::setListener(std::array<float, 3> at, std::array<float, 3> right) {
    const float length = std::hypot(right[0], right[1]);
    std::lock_guard lock(impl_->mutex);
    impl_->listener = at;
    if (length > 1e-6f) {
        impl_->right = {right[0] / length, right[1] / length, right[2] / length};
    }
}

void System::stopAll() {
    std::lock_guard lock(impl_->mutex);
    impl_->voices.clear();
}

void System::stopSource(std::int16_t source) {
    std::lock_guard lock(impl_->mutex);
    std::erase_if(impl_->voices, [source](const Impl::Voice& voice) { return voice.source == source; });
}

void System::setPaused(bool paused) {
    std::lock_guard lock(impl_->mutex);
    impl_->paused = paused;
}

void System::pruneSources(const std::function<bool(std::int16_t)>& alive) {
    std::lock_guard lock(impl_->mutex);
    std::erase_if(impl_->voices, [&](const Impl::Voice& voice) { return voice.source != 0 && !alive(voice.source); });
}

void System::updatePositions(const std::function<std::optional<std::array<float, 3>>(std::int16_t)>& positionOf) {
    std::lock_guard lock(impl_->mutex);
    std::map<std::int16_t, std::optional<std::array<float, 3>>> seen;
    for (auto& voice : impl_->voices) {
        if (voice.source == 0) {
            continue;
        }
        const auto [it, inserted] = seen.try_emplace(voice.source, std::nullopt);
        if (inserted) {
            it->second = positionOf(voice.source);
        }
        if (it->second) {
            voice.at = it->second;
        }
    }
}

double System::playName(std::string_view name) {
    if (impl_->installing.valid()) {
        impl_->installing.wait();
    }
    if (!impl_->openDevice()) {
        return 0;
    }
    const std::string resolved = resolve(name, Context{});
    if (resolved.empty()) {
        ts2::diag::warning("audio: no track named " + std::string(name));
        return 0;
    }
    return impl_->playResolved(resolved, name, 0);
}

bool System::playUi(std::string_view name) {
    if (!impl_->ready) {
        return false;
    }
    const std::string resolved = resolve(name, Context{});
    if (resolved.empty()) {
        return false;
    }
    return impl_->playResolved(resolved, name, 0, std::nullopt, true) > 0;
}

} // namespace ts2::audio
