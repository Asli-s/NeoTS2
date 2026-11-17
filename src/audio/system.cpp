#include "audio/system.hpp"

#include "audio/bank.hpp"
#include "audio/manager.hpp"
#include "audio/mp3.hpp"
#include "audio/music.hpp"
#include "audio/song_settings.hpp"
#include "audio/spx.hpp"
#include "audio/xa.hpp"
#include "diag/log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ts2::audio {

namespace {

constexpr int kOutputRate = 48000;
constexpr int kOutputChannels = 2;
// The mixer's voices. The data's limits keep far fewer playing, this only guards a broken file.
constexpr std::size_t kMaxVoices = 64;
// Frames mixed per pass of the device callback.
constexpr std::size_t kMixFrames = 1024;
constexpr std::size_t kMaxCacheBytes = 64u << 20;
constexpr std::size_t kMaxDecodeJobs = 256;

std::uint64_t sampleId(SampleKey key) {
    return mapKey(key.instance, key.instanceHigh);
}

std::chrono::steady_clock::time_point now() {
    return std::chrono::steady_clock::now();
}

// The sound group an event plays in, by its name.
Group eventGroup(std::string_view name) {
    return name.starts_with("vox") ? Group::Voice : name.starts_with("cine") ? Group::Cinematic : Group::Sfx;
}

// One voice as the device callback sees it. Gains move from the last pass's to this one's across a
// pass, so a change is never a click.
struct MixVoice {
    const Pcm* pcm = nullptr;
    std::uint32_t id = 0;
    double position = 0;  // in sample frames
    double step = 1;
    bool loop = false;
    bool advancing = true;
    bool ended = false;
    std::array<EarGains, 3> gains{};  // mono, stereo left channel, stereo right channel
    std::array<EarGains, 3> lastGains{};
    bool filtered = false;
    std::array<Biquad, 2> lowpass, highpass;  // by source channel
};

EarGains lerp(const EarGains& a, const EarGains& b, float t) {
    return {a.left + (b.left - a.left) * t, a.right + (b.right - a.right) * t};
}

// Mixes `frames` of one voice into `out` (interleaved left, right). Marks it ended at its end.
void mixVoice(MixVoice& v, float* out, std::size_t frames) {
    const Pcm& pcm = *v.pcm;
    const std::size_t channels = pcm.channels;
    const std::size_t length = pcm.samples.size() / channels;
    if (length == 0) {
        v.ended = true;
        return;
    }
    for (std::size_t i = 0; i < frames; ++i) {
        if (v.position >= static_cast<double>(length)) {
            if (!v.loop) {
                v.ended = true;
                break;
            }
            v.position = std::fmod(v.position, static_cast<double>(length));
        }
        const auto a = static_cast<std::size_t>(v.position);
        const std::size_t b = a + 1 < length ? a + 1 : v.loop ? 0 : a;
        const auto t = static_cast<float>(v.position - static_cast<double>(a));
        const float ramp = static_cast<float>(i) / static_cast<float>(frames);
        const auto sample = [&](std::size_t frame, std::size_t channel) {
            return static_cast<float>(pcm.samples[frame * channels + channel]);
        };
        if (channels == 1) {
            float s = sample(a, 0) + (sample(b, 0) - sample(a, 0)) * t;
            if (v.filtered) {
                s = v.lowpass[0].process(v.highpass[0].process(s));
            }
            const EarGains g = lerp(v.lastGains[0], v.gains[0], ramp);
            out[i * 2] += s * g.left;
            out[i * 2 + 1] += s * g.right;
        } else {
            float l = sample(a, 0) + (sample(b, 0) - sample(a, 0)) * t;
            float r = sample(a, 1) + (sample(b, 1) - sample(a, 1)) * t;
            if (v.filtered) {
                l = v.lowpass[0].process(v.highpass[0].process(l));
                r = v.lowpass[1].process(v.highpass[1].process(r));
            }
            const EarGains gl = lerp(v.lastGains[1], v.gains[1], ramp);
            const EarGains gr = lerp(v.lastGains[2], v.gains[2], ramp);
            out[i * 2] += l * gl.left + r * gr.left;
            out[i * 2 + 1] += l * gl.right + r * gr.right;
        }
        v.position += v.step;
    }
    v.lastGains = v.gains;
}

} // namespace

struct System::Impl {
    Bank bank;
    Manager manager;
    std::thread loader;
    std::atomic<bool> loaded{false};
    bool started = false;  // set on the main thread once loading is seen done
    int performanceLevel = 1;

    // The decoder thread: samples asked for, and samples done (empty for one that failed).
    std::thread decoder;
    std::mutex decodeMutex;
    std::condition_variable decodeWake;
    std::deque<SampleKey> jobs;
    std::vector<std::pair<std::uint64_t, std::shared_ptr<const Pcm>>> decoded;
    bool quit = false;

    // Main thread only.
    std::unordered_map<std::uint64_t, std::shared_ptr<const Pcm>> cache;
    std::size_t cacheBytes = 0;
    std::unordered_set<std::uint64_t> pending;
    std::unordered_set<std::uint64_t> failed;
    std::array<std::shared_ptr<const Pcm>, kMaxVoices> slotPcm;
    std::unordered_map<std::uint32_t, std::size_t> slotOf;
    std::set<std::string> missed;
    // The music states by category name: whether it is playing, its shuffled songs and its current song.
    struct MusicState {
        bool active = false;
        std::optional<Playlist> playlist;
        std::uint32_t id = 0;
    };
    std::map<std::string, MusicState> music;
    // The songs that are off, by station name in lowercase, read from the station's settings file when first needed.
    std::filesystem::path musicFolder;
    std::map<std::string, SongSettings> songSettings;
    SongSettings& settingsOf(const std::string& station);
    std::vector<SampleKey> playableSongs(std::string_view station);
    void saveSettings(const std::string& station);
    // The stereos playing a station, one per object: each plays its station's songs one after another.
    struct StereoPlay {
        Request request;
        Playlist playlist;
        std::uint32_t id = 0;
        double retryAtMs = 0;
    };
    std::vector<StereoPlay> stereos;
    std::chrono::steady_clock::time_point lastUpdate = now();
    std::chrono::steady_clock::time_point startTime = now();

    // Shared with the device callback.
    SDL_AudioStream* stream = nullptr;
    bool deviceFailed = false;
    std::mutex mixMutex;
    std::array<MixVoice, kMaxVoices> voices;
    std::array<float, kMixFrames * kOutputChannels> mixBuffer{};
    std::array<std::int16_t, kMixFrames * kOutputChannels> outBuffer{};

    ~Impl();
    double nowMs() const { return std::chrono::duration<double, std::milli>(now() - startTime).count(); }
    void openDevice();
    void startDecoder();
    void decodeLoop();
    void askDecode(SampleKey key);
    void takeDecoded();
    void trimCache();
    void syncMixer(const std::vector<Manager::Output>& outputs);
    void updateMusic();
    void updateStereos();
    void startStereo(const Event& event);
    std::uint32_t startTrack(const Request& request, std::string_view name);
    static void SDLCALL feed(void* userdata, SDL_AudioStream* stream, int additional, int total);
};

System::Impl::~Impl() {
    if (loader.joinable()) {
        loader.join();
    }
    {
        std::lock_guard lock(decodeMutex);
        quit = true;
    }
    decodeWake.notify_all();
    if (decoder.joinable()) {
        decoder.join();
    }
    if (stream != nullptr) {
        SDL_DestroyAudioStream(stream);
    }
}

void System::Impl::openDevice() {
    if (stream != nullptr || deviceFailed) {
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        diag::warnOnce(std::string("no audio device: ") + SDL_GetError());
        deviceFailed = true;
        return;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, kOutputChannels, kOutputRate};
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Impl::feed, this);
    if (stream == nullptr) {
        diag::warnOnce(std::string("no audio device: ") + SDL_GetError());
        deviceFailed = true;
        return;
    }
    SDL_ResumeAudioStreamDevice(stream);
}

void System::Impl::feed(void* userdata, SDL_AudioStream* stream, int additional, int) {
    auto* self = static_cast<Impl*>(userdata);
    std::lock_guard lock(self->mixMutex);
    auto frames = static_cast<std::size_t>(std::max(additional, 0)) / (kOutputChannels * sizeof(std::int16_t));
    while (frames > 0) {
        const std::size_t pass = std::min(frames, kMixFrames);
        std::fill_n(self->mixBuffer.begin(), pass * kOutputChannels, 0.0f);
        for (MixVoice& voice : self->voices) {
            if (voice.pcm != nullptr && !voice.ended && voice.advancing) {
                mixVoice(voice, self->mixBuffer.data(), pass);
            }
        }
        for (std::size_t i = 0; i < pass * kOutputChannels; ++i) {
            self->outBuffer[i] = static_cast<std::int16_t>(std::clamp(self->mixBuffer[i], -32768.0f, 32767.0f));
        }
        SDL_PutAudioStreamData(stream, self->outBuffer.data(), static_cast<int>(pass * kOutputChannels * sizeof(std::int16_t)));
        frames -= pass;
    }
}

void System::Impl::startDecoder() {
    decoder = std::thread([this] { decodeLoop(); });
}

void System::Impl::decodeLoop() {
    for (;;) {
        SampleKey key;
        {
            std::unique_lock lock(decodeMutex);
            decodeWake.wait(lock, [this] { return quit || !jobs.empty(); });
            if (quit) {
                return;
            }
            key = jobs.front();
            jobs.pop_front();
        }
        std::shared_ptr<const Pcm> pcm;
        const auto bytes = bank.readSample(key);
        if (bytes.size() >= 2 && bytes[0] == 'X' && bytes[1] == 'A') {
            try {
                pcm = std::make_shared<const Pcm>(decodeXa(bytes));
            } catch (const std::exception& ex) {
                diag::warnOnce(std::string("sound sample: ") + ex.what());
            }
        } else if (bytes.size() >= 4 && bytes[0] == 'S' && bytes[1] == 'P' && bytes[2] == 'X' && bytes[3] == '1') {
            try {
                pcm = std::make_shared<const Pcm>(decodeSpx(bytes));
            } catch (const std::exception& ex) {
                diag::warnOnce(std::string("sound sample: ") + ex.what());
            }
        } else if ((bytes.size() >= 3 && bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3') ||
                   (bytes.size() >= 2 && bytes[0] == 0xFF && (bytes[1] & 0xE0) == 0xE0)) {
            try {
                pcm = std::make_shared<const Pcm>(decodeMp3(bytes));
            } catch (const std::exception& ex) {
                diag::warnOnce(std::string("sound sample: ") + ex.what());
            }
        } else if (!bytes.empty()) {
            diag::warnOnce("sound sample format not supported yet");
        }
        std::lock_guard lock(decodeMutex);
        decoded.emplace_back(sampleId(key), std::move(pcm));
    }
}

void System::Impl::askDecode(SampleKey key) {
    const std::uint64_t id = sampleId(key);
    if (cache.contains(id) || failed.contains(id) || !pending.insert(id).second) {
        return;
    }
    {
        std::lock_guard lock(decodeMutex);
        if (jobs.size() >= kMaxDecodeJobs) {
            pending.erase(id);
            return;
        }
        jobs.push_back(key);
    }
    decodeWake.notify_one();
}

void System::Impl::takeDecoded() {
    std::vector<std::pair<std::uint64_t, std::shared_ptr<const Pcm>>> done;
    {
        std::lock_guard lock(decodeMutex);
        done.swap(decoded);
    }
    for (auto& [id, pcm] : done) {
        pending.erase(id);
        if (!pcm || pcm->samples.empty() || pcm->channels == 0) {
            failed.insert(id);
            diag::info("audio: a sample could not be decoded");
            continue;
        }
        if (pcm->seconds() > 30) {
            diag::info("audio: a song of " + std::to_string(static_cast<int>(pcm->seconds())) + " s is decoded");
        }
        cacheBytes += pcm->samples.size() * sizeof(std::int16_t);
        cache[id] = std::move(pcm);
    }
    trimCache();
}

void System::Impl::trimCache() {
    // Samples no voice plays go first, the cache is only kept to start a sound again at once.
    for (auto it = cache.begin(); it != cache.end() && cacheBytes > kMaxCacheBytes;) {
        if (it->second.use_count() == 1) {
            cacheBytes -= it->second->samples.size() * sizeof(std::int16_t);
            it = cache.erase(it);
        } else {
            ++it;
        }
    }
}

void System::Impl::syncMixer(const std::vector<Manager::Output>& outputs) {
    std::unordered_map<std::uint32_t, const Manager::Output*> byId;
    for (const auto& output : outputs) {
        byId.emplace(output.id, &output);
    }
    std::vector<std::shared_ptr<const Pcm>> released;
    std::vector<std::uint32_t> ended;
    std::vector<std::pair<std::uint32_t, double>> played;
    {
        std::lock_guard lock(mixMutex);
        for (std::size_t slot = 0; slot < kMaxVoices; ++slot) {
            MixVoice& voice = voices[slot];
            if (voice.pcm == nullptr) {
                continue;
            }
            const auto it = byId.find(voice.id);
            if (it == byId.end() || voice.ended) {
                if (voice.ended) {
                    ended.push_back(voice.id);
                }
                slotOf.erase(voice.id);
                voice = MixVoice{};
                released.push_back(std::move(slotPcm[slot]));
                continue;
            }
            const Manager::Output& output = *it->second;
            voice.gains = {output.mono, output.stereo[0], output.stereo[1]};
            voice.advancing = output.advancing;
            voice.step = static_cast<double>(voice.pcm->sampleRate) / kOutputRate * output.pitch;
            played.emplace_back(voice.id, voice.position * 1000.0 / voice.pcm->sampleRate);
        }
        // New voices whose sample is decoded.
        for (const auto& output : outputs) {
            // A sound that just ended is still among the outputs until the manager hears of it.
            if (slotOf.contains(output.id) || std::find(ended.begin(), ended.end(), output.id) != ended.end()) {
                continue;
            }
            const Manager::Instance* instance = manager.find(output.id);
            const auto cached = instance != nullptr ? cache.find(sampleId(instance->sample)) : cache.end();
            if (cached == cache.end()) {
                continue;
            }
            const auto free = std::find_if(voices.begin(), voices.end(), [](const MixVoice& v) { return v.pcm == nullptr; });
            if (free == voices.end()) {
                ended.push_back(output.id);
                continue;
            }
            const auto slot = static_cast<std::size_t>(free - voices.begin());
            slotPcm[slot] = cached->second;
            MixVoice& voice = *free;
            voice.pcm = slotPcm[slot].get();
            voice.id = output.id;
            voice.loop = instance->settings.looping;
            voice.filtered = instance->request.lopass > 0 || instance->request.hipass > 0;
            for (std::size_t channel = 0; channel < 2; ++channel) {
                voice.lowpass[channel] = Biquad::lowpass(instance->request.lopass, kOutputRate);
                voice.highpass[channel] = Biquad::highpass(instance->request.hipass, kOutputRate);
            }
            voice.gains = voice.lastGains = {output.mono, output.stereo[0], output.stereo[1]};
            voice.advancing = output.advancing;
            voice.step = static_cast<double>(voice.pcm->sampleRate) / kOutputRate * output.pitch;
            slotOf.emplace(output.id, slot);
        }
    }
    for (const auto& [id, ms] : played) {
        manager.setPlayed(id, ms);
    }
    for (const std::uint32_t id : ended) {
        manager.stop(id);
    }
}

std::uint32_t System::Impl::startTrack(const Request& request, std::string_view name) {
    const Bank::Track* track = bank.track(request.track);
    if (track == nullptr) {
        if (missed.size() < 4096 && missed.insert(std::string(name)).second) {
            diag::info("audio: no track for " + std::string(name));
        }
        return 0;
    }
    const auto id = manager.start(request, track->settings, bank.samples(*track), nowMs());
    if (!id) {
        return 0;
    }
    const Manager::Instance* instance = manager.find(*id);
    askDecode(instance->sample);
    return *id;
}

namespace {

std::string lowerCase(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

constexpr std::uintmax_t kMaxSettingsBytes = 1u << 20;

} // namespace

SongSettings& System::Impl::settingsOf(const std::string& station) {
    const auto found = songSettings.find(station);
    if (found != songSettings.end()) {
        return found->second;
    }
    SongSettings settings;
    std::error_code error;
    const std::filesystem::path file = musicFolder / station / "settings";
    if (!musicFolder.empty() && std::filesystem::exists(file, error) && std::filesystem::file_size(file, error) <= kMaxSettingsBytes) {
        std::ifstream in(file, std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (auto read = parseSongSettings(bytes)) {
            settings = std::move(*read);
        } else {
            diag::warnOnce("audio: the settings file of station " + station + " is not readable, so all its songs play");
        }
    }
    return songSettings.emplace(station, std::move(settings)).first->second;
}

std::vector<SampleKey> System::Impl::playableSongs(std::string_view station) {
    const SongSettings& settings = settingsOf(lowerCase(station));
    std::vector<SampleKey> out;
    for (const SampleKey& song : bank.songs(station)) {
        if (settings.enabled(bank.songId(song))) {
            out.push_back(song);
        }
    }
    return out;
}

// The file lists every song of the station, so a song a pack adds later shows up as on.
void System::Impl::saveSettings(const std::string& station) {
    if (musicFolder.empty()) {
        return;
    }
    SongSettings& settings = settingsOf(station);
    for (const SampleKey& song : bank.songs(station)) {
        settings.songs.emplace(bank.songId(song), true);
    }
    std::error_code error;
    std::filesystem::create_directories(musicFolder / station, error);
    std::ofstream out(musicFolder / station / "settings", std::ios::binary | std::ios::trunc);
    const auto bytes = writeSongSettings(settings);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        diag::warnOnce("audio: the settings file of station " + station + " cannot be saved");
    }
}

void System::Impl::updateMusic() {
    for (auto& [category, state] : music) {
        if (state.id != 0 && !manager.playing(state.id)) {
            state.id = 0;
        }
        if (!state.active || state.id != 0) {
            continue;
        }
        if (!state.playlist) {
            state.playlist.emplace(playableSongs(category), static_cast<std::uint32_t>(std::hash<std::string>{}(category)));
            if (state.playlist->empty()) {
                diag::warnOnce("audio: no songs to play for music category " + category);
            }
        }
        if (state.playlist->empty()) {
            continue;
        }
        const Tuning& tuning = bank.tuning();
        TrackSettings settings;
        settings.volume = tuning.musicVolume * tuning.music2dScalar;
        settings.priority = tuning.musicPriority;
        settings.fadeOutMs = tuning.musicFadeOutMs;
        settings.keepPlaying = true;
        Request request;
        request.track = soundKey("music:" + category);
        request.event = request.track;
        request.group = Group::Music;
        const SampleKey song = state.playlist->take();
        if (const auto id = manager.start(request, settings, std::span<const SampleKey>(&song, 1), nowMs())) {
            state.id = *id;
            diag::info("audio: music " + category + " plays one of " + std::to_string(bank.songs(category).size()) + " songs");
            askDecode(song);
            // The next song decodes while this one plays.
            askDecode(state.playlist->upcoming());
        }
    }
}

void System::Impl::startStereo(const Event& event) {
    // The station is what follows "stereo_". The wall speakers take the nearest stereo's station
    // and are not played yet.
    const std::string station = event.name.substr(7);
    const std::vector<SampleKey> songs = playableSongs(station);
    if (songs.empty()) {
        diag::warnOnce("audio: no songs to play for station " + station);
        return;
    }
    // A stereo plays one station: choosing another replaces it.
    std::erase_if(stereos, [&](const StereoPlay& play) {
        if (play.request.object != event.object) {
            return false;
        }
        manager.stop(play.id);
        return true;
    });
    Request request;
    request.track = soundKey(event.name);
    request.event = request.track;
    request.object = event.object;
    request.group = Group::Stereo;
    request.position = event.position;
    // A cheap stereo plays through a narrow band, as the game's does (inferred: the game's quality
    // names are "cheap" and "exp", and an object's quality string is "chp" or "exp").
    if (event.inputs.quality && (*event.inputs.quality == "chp" || *event.inputs.quality == "cheap")) {
        request.lopass = 8500;
        request.hipass = 3500;
    }
    const std::uint32_t seed = static_cast<std::uint32_t>(std::hash<std::string>{}(station)) ^ static_cast<std::uint32_t>(event.object);
    stereos.push_back({request, Playlist(songs, seed), 0, 0});
}

void System::Impl::updateStereos() {
    const Tuning& tuning = bank.tuning();
    TrackSettings settings;
    settings.positional = true;
    settings.minDistance = tuning.musicMinDistance;
    settings.maxDistance = tuning.musicMaxDistance;
    settings.volume = tuning.musicVolume;
    settings.priority = tuning.musicPriority;
    settings.fadeOutMs = tuning.musicFadeOutMs;
    settings.keepPlaying = true;
    for (StereoPlay& play : stereos) {
        if (play.id != 0 && !manager.playing(play.id)) {
            diag::info("audio: stereo on object " + std::to_string(play.request.object) + " has stopped its song");
            play.id = 0;
        }
        if (play.id != 0 || nowMs() < play.retryAtMs) {
            continue;
        }
        const SampleKey song = play.playlist.take();
        if (const auto id = manager.start(play.request, settings, std::span<const SampleKey>(&song, 1), nowMs())) {
            play.id = *id;
            diag::info("audio: stereo on object " + std::to_string(play.request.object) + " plays a song" +
                       (play.request.lopass > 0 ? " (cheap)" : ""));
            askDecode(song);
            askDecode(play.playlist.upcoming());
        } else {
            // Dropped (no voice, say): try again in a second, not on every frame.
            play.retryAtMs = nowMs() + 1000;
        }
    }
}

System::System() : impl_(std::make_unique<Impl>()) {}
System::~System() = default;

void System::addInstall(const std::filesystem::path& installRoot) {
    if (impl_->loader.joinable() || impl_->loaded) {
        return;
    }
    // Indexing reads about 40,000 small resources, so it runs beside the menu at low priority.
    impl_->loader = std::thread([impl = impl_.get(), installRoot] {
        SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
        try {
            impl->bank.load(installRoot);
        } catch (const std::exception& ex) {
            diag::warnOnce(std::string("sound packages: ") + ex.what());
        }
        impl->loaded = true;
    });
}

void System::setMusicFolder(const std::filesystem::path& folder) {
    impl_->musicFolder = folder;
    impl_->songSettings.clear();
}

std::vector<std::string> System::musicStations(std::string_view category) {
    std::vector<std::string> out;
    if (!impl_->started) {
        return out;
    }
    const std::string wanted = lowerCase(category);
    for (const auto& [name, pool] : impl_->bank.categories()) {
        if (wanted.empty() ? pool.size() == 1 && lowerCase(pool[0]) == lowerCase(name) : lowerCase(name) == wanted) {
            out.insert(out.end(), pool.begin(), pool.end());
        }
    }
    return out;
}

std::string System::musicTitle(std::string_view station) {
    return impl_->started ? impl_->bank.songTitle(soundKey(station).instance, 1) : std::string();
}

std::vector<MusicSong> System::musicSongs(std::string_view station) {
    std::vector<MusicSong> out;
    if (!impl_->started) {
        return out;
    }
    const SongSettings& settings = impl_->settingsOf(lowerCase(station));
    for (const SampleKey& song : impl_->bank.songs(station)) {
        const std::uint32_t id = impl_->bank.songId(song);
        out.push_back({id, impl_->bank.songTitle(id, 1), settings.enabled(id)});
    }
    return out;
}

void System::setSongOn(std::string_view station, std::uint32_t id, bool on) {
    if (!impl_->started) {
        return;
    }
    const std::string name = lowerCase(station);
    impl_->settingsOf(name).songs[id] = on;
    impl_->saveSettings(name);
    // The station's next song comes from the new list.
    for (auto& [category, state] : impl_->music) {
        if (lowerCase(category) == name) {
            state.playlist.reset();
        }
    }
}

bool System::ready() const {
    return impl_->started;
}

void System::setPerformanceLevel(int level) {
    impl_->performanceLevel = level;
    impl_->manager.setPerformanceLevel(level);
}

void System::play(const Event& event) {
    if (!impl_->started || event.name.empty()) {
        return;
    }
    const std::string_view name = event.name;
    // A start of "stop_<name>" stops, under the full name as written.
    if (event.stop || name.starts_with("stop_")) {
        std::erase_if(impl_->stereos, [&](const Impl::StereoPlay& play) {
            return play.request.event == soundKey(name) && play.request.object == event.object;
        });
        impl_->manager.stopEvent(soundKey(name), event.object);
        return;
    }
    // The game takes these two events and does nothing with them. Its dip starts and ends with the
    // cinematic camera instead.
    if (name == "startmusicdipping" || name == "stopmusicdipping") {
        return;
    }
    if (name == "stopallsounds") {
        impl_->stereos.clear();
        impl_->manager.stopAll();
        return;
    }
    if (name.starts_with("stereo_") && name != "stereo_wall_speaker") {
        impl_->startStereo(event);
        return;
    }
    const std::uint32_t mask = eventMask(name, impl_->bank.listedMask(name));
    const std::string track = trackName(name, mask, event.inputs, impl_->bank.aliases());
    Request request;
    request.track = soundKey(track);
    request.event = soundKey(name);
    request.object = event.object;
    request.group = eventGroup(name);
    request.position = event.position;
    (void)impl_->startTrack(request, track);
}

void System::stopObject(std::int16_t object) {
    std::erase_if(impl_->stereos, [object](const Impl::StereoPlay& play) { return play.request.object == object; });
    impl_->manager.stopObject(object);
}

void System::stopAll() {
    impl_->stereos.clear();
    impl_->manager.stopAll();
}

void System::setPaused(bool paused) {
    impl_->manager.setGamePaused(paused);
}

void System::setSpeed(int index) {
    impl_->manager.setSpeedIndex(index);
}

void System::setGroupVolume(Group group, float volume) {
    impl_->manager.setGroupVolume(group, volume);
}

void System::setGroupMuted(Group group, bool muted) {
    impl_->manager.setGroupMuted(group, muted);
}

void System::setMuted(bool muted) {
    impl_->manager.setMuted(muted);
}

void System::setListener(const Vec3& eye, const Vec3& target, const Vec3& up) {
    if (!impl_->started) {
        return;
    }
    const Tuning& tuning = impl_->bank.tuning();
    impl_->manager.setListener(cameraListener(eye, target, up, tuning.listenerMinHeight, tuning.listenerToCameraOffset,
                                              tuning.attenuationPositionsScalar));
}

void System::updatePositions(const std::function<std::optional<Vec3>(std::int16_t)>& positionOf) {
    std::set<std::int16_t> objects;
    for (const auto& instance : impl_->manager.instances()) {
        if (instance.request.object != 0 && instance.request.position) {
            objects.insert(instance.request.object);
        }
    }
    for (const std::int16_t object : objects) {
        if (const auto at = positionOf(object)) {
            impl_->manager.moveObject(object, *at);
        }
    }
}

void System::update() {
    Impl& impl = *impl_;
    if (!impl.started) {
        if (!impl.loaded) {
            return;
        }
        impl.loader.join();
        impl.manager.setTuning(impl.bank.tuning());
        impl.manager.setPerformanceLevel(impl.performanceLevel);
        impl.startDecoder();
        impl.openDevice();
        impl.started = true;
        diag::info("audio: indexed " + std::to_string(impl.bank.trackCount()) + " sound tracks");
    }
    const auto at = now();
    const double elapsed = std::chrono::duration<double, std::milli>(at - impl.lastUpdate).count();
    impl.lastUpdate = at;
    impl.takeDecoded();
    // Sounds whose sample cannot be decoded never start.
    for (const auto& instance : impl.manager.instances()) {
        if (impl.failed.contains(sampleId(instance.sample))) {
            impl.manager.stop(instance.id);
        } else if (instance.lengthMs == 0) {
            if (const auto it = impl.cache.find(sampleId(instance.sample)); it != impl.cache.end()) {
                impl.manager.setLength(instance.id, it->second->seconds() * 1000);
            } else {
                impl.askDecode(instance.sample);
            }
        }
    }
    impl.updateMusic();
    impl.updateStereos();
    impl.syncMixer(impl.manager.update(elapsed));
}

void System::startMusic(std::string_view category) {
    auto& state = impl_->music[std::string(category)];
    state.active = true;
    if (state.id != 0) {
        impl_->manager.pause(state.id, false, 0);
    }
}

void System::pauseMusic(std::string_view category) {
    auto& state = impl_->music[std::string(category)];
    state.active = false;
    if (state.id != 0) {
        impl_->manager.pause(state.id, true, 0);
    }
}

void System::fadeOutMusic(std::string_view category, double ms) {
    auto& state = impl_->music[std::string(category)];
    state.active = false;
    if (state.id != 0) {
        impl_->manager.fadeOut(state.id, ms);
        state.id = 0;
    }
}

std::uint32_t System::playUi(std::string_view name) {
    if (!impl_->started) {
        return 0;
    }
    Request request;
    request.track = soundKey(name);
    request.event = request.track;
    request.group = Group::Ui;
    return impl_->startTrack(request, name);
}

std::uint32_t System::playUiId(std::uint32_t id) {
    if (!impl_->started || id == 0) {
        return 0;
    }
    Request request;
    request.track = {id, 0};
    request.event = request.track;
    request.group = Group::Ui;
    return impl_->startTrack(request, "UI sound " + std::to_string(id));
}

void System::stop(std::uint32_t id) {
    impl_->manager.stop(id);
}

double System::playName(std::string_view name) {
    while (!impl_->started && (impl_->loader.joinable() || impl_->loaded)) {
        update();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const std::uint32_t id = playUi(name);
    double seconds = 0;
    // A loop plays for 10 s, then stops.
    const auto until = now() + std::chrono::seconds(10);
    while (id != 0 && impl_->manager.find(id) != nullptr && now() < until) {
        update();
        if (const auto* instance = impl_->manager.find(id); instance != nullptr && instance->lengthMs > 0) {
            seconds = instance->lengthMs / 1000;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    stop(id);
    update();
    return seconds;
}

} // namespace ts2::audio
