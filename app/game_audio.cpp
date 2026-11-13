// The app's end of the sound system: one system for the whole game, fed by the lot's events.
#include "game.hpp"

#include <array>
#include <cstdint>

namespace app {

namespace {

// What the object's soundinfo (STR# 0x94) says, when it has one.
std::string soundInfo(ts2::sim::CodeSource& code, std::uint32_t group, std::size_t index) {
    const auto value = code.string(group, 0x94, index);
    return value ? *value : std::string();
}

} // namespace

void Game::ensureAudio() {
    if (audio_) {
        return;
    }
    audio_ = std::make_unique<ts2::audio::System>();
    audio_->addInstall(install_);
}

void Game::playUiClick(int window) {
    if (!audio_ || window < 0) {
        return;
    }
    const std::string sound = screen_.clickSound(window);
    audio_->playUi(sound.empty() ? "ui_primary_button" : sound);
}

void Game::playUiAction(app::LotUi::Action action) {
    if (!audio_) {
        return;
    }
    using Action = app::LotUi::Action;
    const char* name = "ui_primary_button";
    switch (action) {
    case Action::Pause: name = "ui_button_pause"; break;
    case Action::Normal: name = "ui_button_play"; break;
    case Action::High: name = "ui_button_fw"; break;
    case Action::Ultra: name = "ui_button_ffw"; break;
    default: break;
    }
    (void)audio_->playUi(name);
}

void Game::onSoundEvent(const ts2::sim::SoundEvent& event) {
    if (!audio_ || !lotLife_) {
        return;
    }
    auto& simulation = lotLife_->simulation();
    ts2::audio::Context context;
    // A multi-tile part sounds as its lead: one aquarium is one source, not one per part.
    std::int16_t source = event.source;
    if (const ts2::sim::SimObject* object = simulation.world().find(event.source)) {
        const std::int16_t lead = object->data[0x13];  // object data 0x13: lead tile
        if (lead != 0) {
            source = lead;
        }
        context.person = object->isPerson();
        if (context.person) {
            // Person data 58 age, 65 sex, 68 ghost flags, 88 voice.
            context.age = object->personData[58];
            context.gender = object->personData[65];
            context.ghost = object->personData[68];
            context.voice = object->personData[88];
        }
        context.destSurface = soundInfo(simulation.code(), object->group, 0);
        context.sourceSurface = soundInfo(simulation.code(), object->group, 1);
        context.quality = soundInfo(simulation.code(), object->group, 2);
    }
    std::optional<std::array<float, 3>> position;
    if (const auto root = simRoot(source)) {
        position = std::array<float, 3>{(*root)[0], (*root)[1], (*root)[2]};
    } else if (const auto place = objectRoot(source)) {
        position = std::array<float, 3>{(*place)[0], (*place)[1], (*place)[2]};
    }
    audio_->play(event.name, source, event.stop, context, position);
}

} // namespace app
