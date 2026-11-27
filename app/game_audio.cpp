// The app's end of the sound system: one system for the whole game, fed by the lot's events, the
// UI's buttons and the live-mode keys.
#include "game.hpp"

#include "placement_errors.hpp"
#include "sim/clock.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

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
    audio_->addInstall(install_);
    audio_->setMusicFolder(options_.user / "Music");
    lotUi_.audioOptions().setMusic(audio_.get());
    hoodUi_.audioOptions().setMusic(audio_.get());
    // --mute leaves the mixer running, so the options' levels still apply behind it.
    audio_->setMuted(options_.mute);
    applyAudioOptions();
}

void Game::waitForAudioIndex() {
    // About 30 s at most, so a broken install still gets its picture.
    for (int i = 0; audio_ && !audio_->ready() && i < 1500; ++i) {
        audio_->update();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void Game::applyAudioOptions() {
    if (!audio_) {
        return;
    }
    using ts2::audio::Group;
    const app::AudioOptionsPanel& panel = openAudioOptions();
    const auto volumes = panel.volumes();
    const auto mutes = panel.mutes();
    audio_->setPerformanceLevel(panel.quality() - 1);
    // Each option covers more groups than its name says, as in the game. The effects level and box
    // also set UI, TV and Stitcher, and music also sets Stereo. The voices box also mutes Cinematic,
    // but the voices level leaves it alone.
    const std::array<std::vector<Group>, 4> levels{{{Group::Voice},
                                                    {Group::Sfx, Group::Ui, Group::Television, Group::Stitcher},
                                                    {Group::Music, Group::Stereo},
                                                    {Group::Ambience}}};
    const std::array<std::vector<Group>, 4> boxes{{{Group::Voice, Group::Cinematic},
                                                   {Group::Sfx, Group::Ui, Group::Television, Group::Stitcher},
                                                   {Group::Music, Group::Stereo},
                                                   {Group::Ambience}}};
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        for (const Group group : levels[i]) {
            audio_->setGroupVolume(group, volumes[i]);
        }
        for (const Group group : boxes[i]) {
            audio_->setGroupMuted(group, mutes[i]);
        }
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

// The money text starts at the mouse as the sale is booked. Selling for nothing shows no text and
// makes no sound.
void Game::sellHeld() {
    if (!lotLife_) {
        return;
    }
    if (const auto sold = buyHand_.sell(*lotLife_); sold && *sold != 0) {
        recutPending_ = true;
        if (audio_) {
            (void)audio_->playUi("ui_object_sell");
        }
        moneyText_.show(screen_, -*sold, lotMouseUi_[0], lotMouseUi_[1]);
    }
    if (!buyHand_.carrying()) {
        cursors_.show(app::Cursors::kArrow);
    }
}

void Game::undoHand(bool redo) {
    if (!lotLife_) {
        return;
    }
    if (redo ? buyHand_.redo(*lotLife_) : buyHand_.undo(*lotLife_)) {
        recutPending_ = true;
        if (audio_) {
            (void)audio_->playUi(redo ? "ui_redo" : "ui_undo");
        }
    }
}

void Game::paidHand(const app::BuyHand::Paid& paid) {
    using What = app::BuyHand::Paid::What;
    if (paid.what == What::PutDown || paid.what == What::Bought) {
        recutPending_ = true;
    }
    if (paid.what == What::Refused) {
        refusedHand(paid);
    } else if (paid.what == What::PutDown) {
        if (audio_) {
            (void)audio_->playUiId(paid.sound);
        }
    } else if (paid.what == What::Lifted) {
        if (audio_) {
            (void)audio_->playUi("ui_object_pickup");
        }
    } else if (paid.what == What::CantAfford) {
        if (audio_) {
            (void)audio_->playUi("ui_error");
        }
    } else if (paid.what == What::Bought) {
        if (audio_) {
            (void)audio_->playUiId(paid.sound);
        }
    }
    if (paid.what == What::Bought && paid.cost != 0) {
        if (audio_) {
            (void)audio_->playUi("ui_object_buy");
        }
        moneyText_.show(screen_, paid.cost, lotMouseUi_[0], lotMouseUi_[1]);
    }
}

// A refused lift or put-down says why in a balloon, with the error sound. A lift's points at the
// object, a put-down's at the spot the test refused.
void Game::refusedHand(const app::BuyHand::Paid& paid) {
    constexpr std::uint32_t kUiText = 0x7FA7DC7C;  // string id, leakcheck: ok
    if (audio_) {
        (void)audio_->playUi("ui_error");
    }
    if (!lotLife_) {
        return;
    }
    const auto flags = lotLife_->handPlacementFlags(paid.refusedAt);
    const std::string text = app::placementErrorText(
        [&](std::uint32_t table, std::uint32_t line) { return screen_.data().text(kUiText, table << 16 | line); }, paid.refusal, flags[0], flags[1]);
    const auto at = paid.spot ? std::optional(lotLife_->scenePointAt(paid.spot->x, paid.spot->y, paid.spot->level, lot_))
                              : lotLife_->handScenePoint(paid.refusedAt, lot_);
    if (at && !text.empty()) {
        showWorldTip(text, *at);
    }
}

void Game::settleHand(bool keepHistory, bool asEscape) {
    if (!lotLife_) {
        return;
    }
    if (buyHand_.carrying()) {
        if (asEscape || buyHand_.badSpot() || buyHand_.buying()) {
            buyHand_.cancel(*lotLife_);
        } else {
            buyHand_.click(*lotLife_, 0);
        }
    }
    if (!keepHistory) {
        buyHand_.clearHistory(*lotLife_);
    }
}

void Game::pickBuyTool(app::LotUi::Tool tool) {
    if (!lotLife_ || (buyHand_.carrying() && !buyHand_.buying())) {
        return;
    }
    if (buyHand_.buying()) {
        buyHand_.cancel(*lotLife_);
    }
    buyTool_ = tool;
    cursors_.show(buyToolCursor());
}

std::uint32_t Game::buyToolCursor() const {
    using Tool = app::LotUi::Tool;
    switch (buyTool_) {
    case Tool::Design: return app::Cursors::kDesignTool;
    case Tool::Eyedropper: return app::Cursors::kEyedropper;
    case Tool::Sledgehammer: return app::Cursors::kSledgehammer;
    case Tool::Hand:
    case Tool::Wall: break;  // the wall tool shows its marker at the corner, with the arrow
    case Tool::Floor: break;  // the game's floor tool cursor is not known yet
    case Tool::WallPaint: break;  // nor the wall paint tool's
    }
    return app::Cursors::kArrow;
}

void Game::toggleDayNight() {
    using ts2::sim::TimeOfDay;
    if (!lotLife_) {
        return;
    }
    const auto globals = lotLife_->simulation().globals();
    if (globals.size() <= ts2::sim::global::kTimeOfDay) {
        return;
    }
    auto& timeOfDay = globals[ts2::sim::global::kTimeOfDay];
    const auto now = static_cast<TimeOfDay>(timeOfDay);
    if (now == TimeOfDay::Evening || now == TimeOfDay::Night) {
        timeOfDay = static_cast<std::int16_t>(TimeOfDay::Day);
    } else if (now == TimeOfDay::Day || now == TimeOfDay::Morning) {
        timeOfDay = static_cast<std::int16_t>(TimeOfDay::Night);
    }
}

void Game::setLotCamera(LotCamera camera) {
    if (camera == lotCamera_ || !orbit_.rig) {
        return;
    }
    auto& left = lotCameras_[static_cast<std::size_t>(lotCamera_)];
    left.rig = orbit_.rig;
    left.distance = orbit_.distance;
    left.distanceGoal = orbit_.distanceGoal;
    left.pitchBlend = orbit_.pitchBlend;
    left.pitchBlendGoal = orbit_.pitchBlendGoal;
    left.minDistance = orbit_.minDistance;
    left.maxDistance = orbit_.maxDistance;
    left.pivotHeight = orbit_.pivotHeight;
    left.used = true;
    auto& next = lotCameras_[static_cast<std::size_t>(camera)];
    if (!next.used) {
        // The first time on, a camera starts at its first zoom level and its least pitch.
        next.rig = ts2::content::findCamera(install_, camera == LotCamera::TopDown ? "Build" : "Ceiling");
        if (!next.rig || next.rig->levels.empty()) {
            return;
        }
        next.maxDistance = next.rig->levels.front().distance;
        next.minDistance = next.rig->levels.back().distance;
        next.pivotHeight = next.rig->param("pivotheightabovefloor", 0);
        next.distance = next.distanceGoal = next.maxDistance;
        next.used = true;
    }
    if (!next.rig || next.rig->levels.empty()) {
        return;
    }
    if (camera == LotCamera::TopDown) {
        next.distanceGoal = next.rig->levels.front().distance;
    }
    orbit_.rig = next.rig;
    orbit_.minDistance = next.minDistance;
    orbit_.maxDistance = next.maxDistance;
    orbit_.pivotHeight = next.pivotHeight;
    orbit_.pitchBlend = next.pitchBlend;
    orbit_.pitchBlendGoal = next.pitchBlendGoal;
    orbit_.zoomTo(next.distance);
    orbit_.distanceGoal = next.distanceGoal;
    orbit_.freeZoom = false;
    orbit_.useRigEasing();
    orbit_.limitGoal = keepNearLot(orbit_.rig->param("constraintwidth", 0));
    lotCamera_ = camera;
}

void Game::toggleLotCamera(LotCamera camera) {
    const LotCamera to = lotCamera_ == camera ? LotCamera::Live : camera;
    setLotCamera(to);
    if (lotCamera_ == to) {
        buyCamera_ = to;
    }
}

void Game::useBuyTool(std::uint32_t object) {
    using Tool = app::LotUi::Tool;
    if (!lotLife_ || object == 0 || !lotLife_->toolTakes(object, buyTool_ == Tool::Sledgehammer)) {
        return;
    }
    if (buyTool_ == Tool::Eyedropper) {
        // The hand takes a copy of the object's catalog item, with the object's colours, as if picked
        // from the catalog.
        if (const auto guid = lotLife_->eyedropGuid(object)) {
            buyHand_.buy(*lotLife_, *guid, lotLife_->eyedropColours(object));
            // A Build catalog item (one with a build type, OBJD 0x45) opens Build mode on it, any
            // other Buy mode. Going between the two keeps the copy in the hand.
            constexpr std::size_t kBuildType = 0x45;
            const auto* type = lotLife_->simulation().code().objectType(*guid);
            lotUi_.eyedropItem(*guid, type != nullptr && type->definition.field(kBuildType) != 0);
            handMode_ = lotUi_.mode();
            buyTool_ = Tool::Hand;
            lotLife_->setDesignHovered(0);
            cursors_.show(buyHand_.carrying() ? app::Cursors::kHandHolding : app::Cursors::kArrow);
        }
    } else if (buyTool_ == Tool::Sledgehammer) {
        smashObject(object);
    }
}

void Game::smashObject(std::uint32_t object) {
    // A Sim in its slots keeps an object from being smashed (unless the moveObjects cheat is on,
    // which isn't ported).
    if (lotLife_->simInSlots(object)) {
        constexpr int kCantDelete = 0x24;  // placement error code
        refusedHand({app::BuyHand::Paid::What::Refused, 0, kCantDelete, lotLife_->handLead(object)});
        return;
    }
    if (const auto sold = buyHand_.smash(*lotLife_, object)) {
        // A door or window smashed leaves no hole in its wall.
        recutPending_ = true;
        if (audio_) {
            (void)audio_->playUi("ui_object_sell");
        }
        moneyText_.show(screen_, -*sold, lotMouseUi_[0], lotMouseUi_[1]);
    }
}

void Game::showWorldTip(const std::string& text, const std::array<float, 3>& point) {
    hideWorldTip();
    worldTipPoint_ = point;
    worldTipShown_ = ts2::platform::ticksNs();
    screen_.setWorldTip(text);
}

void Game::hideWorldTip() {
    worldTipShown_.reset();
    screen_.setWorldTip({});
}

void Game::answerDesign(bool accepted) {
    const auto colours = lotUi_.designedColours(), replaced = lotUi_.designedColours(true);
    lotUi_.designDialog().close(screen_);
    // The colours tried on the object go, and Accept gives it the ones picked. Accepting the colours
    // the object has already changes nothing and costs nothing.
    if (lotLife_) {
        lotLife_->designRestore();
    }
    if (accepted && lotLife_) {
        if (!colours.empty()) {
            lotLife_->handRecolour(designObject_, colours);
            lotLife_->handPay(kDesignCost);
            buyHand_.recordRecolour(designObject_, replaced, colours, kDesignCost);
            if (audio_) {
                (void)audio_->playUi("ui_object_buy");
            }
            moneyText_.show(screen_, kDesignCost, lotMouseUi_[0], lotMouseUi_[1]);
        }
    }
    designObject_ = 0;
    designPressed_ = 0;
    cursors_.show(buyToolCursor());
}

void Game::previewDesign() {
    if (lotLife_) {
        lotLife_->designRestore();
        lotLife_->handRecolour(designObject_, lotUi_.designedColours());
    }
}

void Game::dragTurnHand(float x, float y) {
    static constexpr std::uint32_t kCursors[4] = {app::Cursors::kRotateNorthEast, app::Cursors::kRotateNorthWest, app::Cursors::kRotateSouthWest,
                                                  app::Cursors::kRotateSouthEast};
    if (!buyDragFrom_ || !lotLife_ || !buyHand_.carrying()) {
        return;
    }
    // A facing's front as the simulation steps it (see the front tile of Create New Object Instance),
    // seen from the camera at the held object.
    const auto front = [&](std::int16_t facing) -> std::array<float, 2> {
        const float angle = static_cast<float>(2 - facing) * kPi / 4;
        const auto d = lotLife_->sceneDirection(std::cos(angle), -std::sin(angle));
        const auto& c = orbit_.camera();
        const auto a = app::screenPoint(c, width_, height_, buyFloor_);
        const auto b = app::screenPoint(c, width_, height_, {buyFloor_[0] + d[0], buyFloor_[1] + d[1], buyFloor_[2] + d[2]});
        return {b[0] - a[0], b[1] - a[1]};
    };
    const auto turn = app::BuyHand::dragTurn(static_cast<int>(x - (*buyDragFrom_)[0]), static_cast<int>(y - (*buyDragFrom_)[1]), front);
    if (!turn) {
        return;
    }
    buyHand_.face(*lotLife_, turn->facing);
    buyDragCursor_ = kCursors[turn->quarter];
    buyDragLifted_ = false;
    cursors_.show(*buyDragCursor_);
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
    }
    audio_->update();
}

} // namespace app
