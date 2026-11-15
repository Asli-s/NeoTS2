// The app's end of the sound system: one system for the whole game, fed by the lot's events, the
// UI's buttons and the live-mode keys.
#include "game.hpp"

#include <array>
#include <cmath>
#include <cstdint>

namespace app {

namespace {

constexpr std::uint16_t kSoundInfo = 0x94;  // STR# 0x94, the object's sound strings
constexpr std::size_t kHiddenFlags = 0x22;  // object data 0x22, bit 0x02 silences the object

std::optional<std::string> soundInfo(ts2::sim::CodeSource& code, std::uint32_t group, std::size_t index) {
    auto value = code.string(group, kSoundInfo, index);
    return value && !value->empty() ? value : std::nullopt;
}

// What the name modifiers read from the event's object.
ts2::audio::NameInputs nameInputs(ts2::sim::LotSimulation& simulation, std::int16_t id) {
    ts2::audio::NameInputs inputs;
    const ts2::sim::SimObject* object = simulation.world().find(id);
    if (object == nullptr) {
        return inputs;
    }
    // Which object gives the destination surface (for Sims and objects not in a slot) is not known
    // yet, so that modifier leaves the name as it is.
    inputs.sourceSurface = soundInfo(simulation.code(), object->group, 1);
    inputs.quality = soundInfo(simulation.code(), object->group, 2);
    if (object->isPerson()) {
        const auto& pd = object->personData;
        inputs.speaker = ts2::audio::Speaker{pd[0x3A], pd[0x41], pd[0x44], pd[0x58], pd[0xAF], pd[0xBA]};
    }
    return inputs;
}

} // namespace

void Game::ensureAudio() {
    if (audio_ || options_.noAudio) {
        return;
    }
    audio_ = std::make_unique<ts2::audio::System>();
    audio_->setPerformanceLevel(lotUi_.gameOptions().audioPerformance());
    audio_->addInstall(install_);
    // --mute leaves the mixer running, so the options' levels still apply behind it.
    audio_->setMuted(options_.mute);
    applyAudioOptions();
}

void Game::applyAudioOptions() {
    if (!audio_) {
        return;
    }
    const app::AudioOptionsPanel& panel = openAudioOptions();
    const auto volumes = panel.volumes();
    const auto mutes = panel.mutes();
    constexpr ts2::audio::Group kGroups[] = {ts2::audio::Group::Voice, ts2::audio::Group::Sfx, ts2::audio::Group::Music,
                                             ts2::audio::Group::Ambience};
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        audio_->setGroupVolume(kGroups[i], volumes[i]);
        audio_->setGroupMuted(kGroups[i], mutes[i]);
    }
}

std::optional<ts2::audio::Vec3> Game::soundPlace(std::int16_t id) const {
    if (!lotLife_) {
        return std::nullopt;
    }
    const ts2::sim::SimObject* object = lotLife_->simulation().world().find(id);
    if (object == nullptr) {
        return std::nullopt;
    }
    if (object->isPerson()) {
        if (const auto root = simRoot(id, "root_rot")) {
            return ts2::audio::Vec3{(*root)[0], (*root)[1], (*root)[2]};
        }
        return std::nullopt;
    }
    if (object->x == ts2::sim::kOffLot || !lot_.floorHeight) {
        return std::nullopt;
    }
    // The object's own place (in 1/65536 tiles) at its storey's floor, not its lead tile's.
    const float x = static_cast<float>(object->x) / 65536.0f;
    const float y = static_cast<float>(object->y) / 65536.0f;
    float at[3] = {x, y, lot_.floorHeight(object->level, x, y)};
    lotPlace_.apply(at, true);
    return ts2::audio::Vec3{at[0], at[1], at[2]};
}

void Game::onSoundEvent(const ts2::sim::SoundEvent& event) {
    if (!audio_ || !lotLife_) {
        return;
    }
    ts2::audio::Event sound;
    sound.name = event.name;
    sound.object = event.source;
    sound.stop = event.stop;
    if (!event.stop) {
        sound.inputs = nameInputs(lotLife_->simulation(), event.source);
        sound.position = soundPlace(event.source);
    }
    audio_->play(sound);
}

void Game::onAnimationSound(std::int16_t object, const std::string& name) {
    if (!lotLife_) {
        return;
    }
    if (const ts2::sim::SimObject* found = lotLife_->simulation().world().find(object);
        found != nullptr && (found->data[kHiddenFlags] & 0x02) != 0) {
        return;
    }
    onSoundEvent({name, object, false});
}

void Game::playPressSound(int window) {
    if (audio_) {
        (void)audio_->playUiId(screen_.clickSound(window));
    }
}

void Game::playKeySound(app::LotUi::Action action, bool wasPaused) {
    if (!audio_ || !lotLife_) {
        return;
    }
    using Action = app::LotUi::Action;
    const auto speedSound = [this] {
        switch (lotLife_->speed()) {
        case ts2::sim::Speed::High: return "ui_button_fw";
        case ts2::sim::Speed::Ultra: return "ui_button_ffw";
        default: return "ui_button_play";
        }
    };
    const bool paused = lotLife_->paused();
    if (action == Action::Pause) {
        // Only a change makes a sound: pausing plays the pause sound, going on the speed's.
        if (paused != wasPaused) {
            (void)audio_->playUi(paused ? "ui_button_pause" : speedSound());
        }
    } else if ((action == Action::Normal || action == Action::High || action == Action::Ultra) && !paused) {
        (void)audio_->playUi(speedSound());
    }
}

void Game::playPuckSounds(bool zoomIn, bool zoomOut, bool turning) {
    if (!audio_) {
        return;
    }
    if (zoomIn && !puckZoomIn_) {
        (void)audio_->playUi("ui_button_zoom_in");
    }
    if (zoomOut && !puckZoomOut_) {
        (void)audio_->playUi("ui_button_zoom_out");
    }
    puckZoomIn_ = zoomIn;
    puckZoomOut_ = zoomOut;
    if (turning && puckTurnSound_ == 0) {
        puckTurnSound_ = audio_->playUi("ui_button_rotate_lp");
    } else if (!turning && puckTurnSound_ != 0) {
        audio_->stop(puckTurnSound_);
        puckTurnSound_ = 0;
    }
}

void Game::updateAudio() {
    if (!audio_) {
        return;
    }
    if (inLot_ && lotLife_) {
        const auto camera = viewCamera();
        audio_->setListener(camera.eye, camera.target, camera.up);
        audio_->setPaused(lotLife_->paused());
        audio_->setSpeed(static_cast<int>(lotLife_->speed()));
        const auto& world = lotLife_->simulation().world();
        audio_->updatePositions([this, &world](std::int16_t id) -> std::optional<ts2::audio::Vec3> {
            // A removed object's sounds end with it.
            if (world.find(id) == nullptr) {
                audio_->stopObject(id);
                return std::nullopt;
            }
            return soundPlace(id);
        });
        const auto globals = lotLife_->simulation().globals();
        ts2::audio::AmbienceState state;
        state.onLot = true;
        state.hour = globals[ts2::sim::global::kHour] + globals[ts2::sim::global::kMinute] / 60.0;
        const int cx = static_cast<int>(std::floor(camera.target[0]));
        const int cy = static_cast<int>(std::floor(camera.target[1]));
        // Radius in tiles, 20 far out to 8 close in (Misc.package's LotAmbienceZoomRadiusCurve).
        const float distance = std::sqrt((camera.eye[0] - camera.target[0]) * (camera.eye[0] - camera.target[0]) +
                                         (camera.eye[1] - camera.target[1]) * (camera.eye[1] - camera.target[1]) +
                                         (camera.eye[2] - camera.target[2]) * (camera.eye[2] - camera.target[2]));
        state.zoom = 1 - std::clamp(distance / 40.0f, 0.0f, 1.0f);
        const int radius = static_cast<int>(std::lround(20.0f - 12.0f * state.zoom));
        int open = 0, total = 0;
        bool water = false;
        for (int y = cy - radius; y <= cy + radius; ++y) {
            for (int x = cx - radius; x <= cx + radius; ++x) {
                for (int q = 0; q < 4; ++q) {
                    const float qx = x + ((q == 0 || q == 3) ? 0.25f : 0.75f);
                    const float qy = y + ((q == 0 || q == 1) ? 0.25f : 0.75f);
                    if (const auto isOpen = world.outside().open(0, qx, qy)) {
                        ++total;
                        open += *isOpen ? 1 : 0;
                    }
                }
                water = water || world.outside().pool(0, x, y, 0) || world.outside().pool(0, x, y, 1) ||
                        world.outside().pool(0, x, y, 2) || world.outside().pool(0, x, y, 3);
            }
        }
        state.outside = total > 0 ? static_cast<float>(open) / static_cast<float>(total) : 1;
        state.water = water;
        audio_->setAmbience(state);
    } else {
        // The neighborhood has no night and its ground is all open.
        ts2::audio::AmbienceState state;
        state.inNeighborhood = inNeighborhood_;
        state.hour = 12;
        audio_->setAmbience(state);
    }
    audio_->update();
}

} // namespace app
