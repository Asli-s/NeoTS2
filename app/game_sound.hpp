#pragma once

#include "audio/mixer.hpp"
#include "audio/sound_bank.hpp"
#include "render/types.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace app {

// The game's sound: its sound effects, voices and music, from the install's sound packages, mixed and
// sent to the system's playback device. Sounds asked for before the packages are indexed (which
// happens in the background as the game starts) are skipped. Without a playback device everything
// is quietly skipped.
class GameSound {
public:
    GameSound(const std::filesystem::path& install, const std::filesystem::path& userFolder);
    ~GameSound();
    GameSound(const GameSound&) = delete;
    GameSound& operator=(const GameSound&) = delete;

    // The volume options from the user folder's userProps.xml: FXVolume, VOXVolume, MusicVolume and
    // AmbienceVolume (0-100), and the SoundMasterGroup...Enabled switches.
    void readOptions(const std::filesystem::path& userFolder);

    // Plays a music station's songs one after another in the game's order (a random one of those not
    // played lately), fading between them. An empty name fades the music out. "splash" is the main
    // menu's, "nhood" the neighborhood's.
    void music(std::string_view station);

    // A sound a lot's script or animation asked for, from object `source` (stop: the sound it started).
    // A Sim's voice line ("vox...") plays the version for that Sim's age, sex and voice.
    void lotSound(const std::string& name, std::int16_t source, bool stop);
    // Whose voice a lot object has, or nothing when it isn't a Sim.
    using VoiceOf = std::function<std::optional<ts2::audio::SimVoice>(std::int16_t object)>;
    void setVoiceOf(VoiceOf voiceOf) { voiceOf_ = std::move(voiceOf); }
    // Where a lot object is now, in world units, or nothing when it's gone.
    using PlaceOf = std::function<std::optional<ts2::render::Vec3>(std::int16_t object)>;
    // Moves the lot's 3D sounds as the camera and their objects move, and holds them while paused.
    void lotFrame(const ts2::render::Camera& camera, const PlaceOf& placeOf, bool paused);
    // Stops every lot sound, as the lot closes.
    void leaveLot();

    // Each frame: starts the next song when one ends or is ready.
    void update();

    [[nodiscard]] bool running() const { return started_; }

private:
    struct LotVoice {
        ts2::audio::Mixer::VoiceId id = 0;
        std::int16_t source = 0;
        std::string name;
        ts2::audio::TrackSettings track;
        bool placed = false;  // a 3D sound whose source has had a place
    };

    ts2::audio::SoundBank* bank();  // null until indexed
    void tellOnce(const std::string& text);
    static void fill(void* self, float* out, std::size_t frames);

    std::shared_future<std::shared_ptr<ts2::audio::SoundBank>> bank_;
    std::unique_ptr<ts2::audio::Mixer> mixer_;
    bool started_ = false;
    std::vector<LotVoice> lotVoices_;
    std::set<std::string> told_;
    VoiceOf voiceOf_;

    // Music.
    std::string station_;
    std::unique_ptr<ts2::audio::SoundBank::HitList> songs_;
    ts2::audio::Mixer::VoiceId song_ = 0;
    std::future<std::shared_ptr<const ts2::audio::Pcm>> nextSong_;
    std::mt19937 random_{std::random_device{}()};
};

} // namespace app
