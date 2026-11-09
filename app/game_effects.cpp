// The visual effects the game plays on the lot and on the neighborhood's decorations.
#include "game.hpp"

#include "diag/log.hpp"
#include "scene/effect_terrain.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace app {

// Starts the effects the neighborhood's decorations name at their slots, grown by their prewarm.
// Their ground decals are already in the scene.
void Game::startHoodEffects() {
    hoodEffectPlayer_.reset();
    hoodEffectImages_.clear();
    const ts2::effects::File* file = hood_.effects ? hood_.effects->all() : nullptr;
    if (!file || hood_.stats.slotEffects.empty()) {
        return;
    }
    hoodEffectPlayer_ = std::make_unique<ts2::scene::EffectPlayer>(*file, static_cast<std::uint32_t>(lastFrame_) | 1);
    if (hood_.scene.terrain) {
        hoodEffectTerrain_ = ts2::scene::effectTerrain(*hood_.scene.terrain);
        // A terrain that failed its checks has no height, and effects then play as if there were none.
        if (hoodEffectTerrain_.height) {
            ts2::scene::EffectConditions conditions = hoodEffectPlayer_->conditions();
            conditions.terrain = &hoodEffectTerrain_;
            hoodEffectPlayer_->setConditions(conditions);
        }
    }
    applyEffectOptions();
    updateHoodEffectState();
    hoodEffectPlayer_->update(0, 0);  // takes the state at once, so the effects are built for it
    for (const auto& slot : hood_.stats.slotEffects) {
        if (auto effect = hoodEffectPlayer_->create(slot.effect)) {
            const float c = std::cos(slot.turn), s = std::sin(slot.turn);
            ts2::scene::EmitterPlace place;
            place.rotation = {c, -s, 0, s, c, 0, 0, 0, 1};
            place.position = slot.position;
            effect->place(place);
            effect->start(true);
        }
    }
}

// The level of detail by the camera's zoom, and the state bits the effects ask for, which follow
// the Day/Night Toggle.
void Game::updateHoodEffectState() {
    if (hoodEffectPlayer_) {
        hoodEffectPlayer_->setDetail(orbit_.rig ? ts2::camera::effectDetailAt(*orbit_.rig, orbit_.distance) : 1.0f);
        hoodEffectPlayer_->setState(app::neighborhoodEffectState(hood_.times.atNight));
    }
}

// The decorations' effects play on in real time and hand their particles to the renderer.
void Game::updateHoodEffects(float realSeconds) {
    if (!hoodEffectPlayer_) {
        return;
    }
    const auto camera = orbit_.camera();
    const auto ahead = ts2::render::normalize(ts2::render::sub(camera.target, camera.eye));
    const auto beside = ts2::render::normalize(ts2::render::cross(ahead, {0, 0, 1}));
    const auto over = ts2::render::cross(beside, ahead);
    hoodEffectPlayer_->setCamera({camera.eye[0], camera.eye[1], camera.eye[2]}, {ahead[0], ahead[1], ahead[2]}, {over[0], over[1], over[2]});
    updateHoodEffectState();
    hoodEffectPlayer_->update(realSeconds, realSeconds);

    const auto view = ts2::render::FrameView::make(camera, width_, height_);
    ts2::scene::ParticleView sprites;
    sprites.forward = {ahead[0], ahead[1], ahead[2]};
    sprites.right = {beside[0], beside[1], beside[2]};
    sprites.up = {over[0], over[1], over[2]};
    std::copy(view.viewProjection.m.begin(), view.viewProjection.m.end(), sprites.clip.begin());
    effectVertices_.clear();
    effectBatches_.clear();
    bool newImages = false;
    ts2::scene::appendEffectSprites(*hoodEffectPlayer_, sprites, [&](const std::string& name) {
        const auto [known, isNew] = hoodEffectImages_.try_emplace(name, -1);
        if (isNew) {
            if (const auto image = assets_.get()->texture(name)) {
                known->second = static_cast<int>(hood_.scene.images.size());
                hood_.scene.images.push_back(*image);
                newImages = true;
            }
        }
        return known->second;
    }, effectVertices_, effectBatches_);
    if (newImages) {
        renderer_.addMaterials(hood_.scene);
    }
    renderer_.showParticles(effectVertices_, effectBatches_);
    placeEffectModels();
}

// The decorations' effects' models where they are now. Instances no model takes are hidden.
void Game::placeEffectModels() {
    const auto& models = hood_.stats.effectModels;
    if (models.empty()) {
        return;
    }
    models.place(hoodEffectPlayer_.get(), effectModelPlaces_);
    std::vector<std::uint8_t> shown(hood_.scene.instances.size(), 1);
    for (const auto& p : effectModelPlaces_) {
        if (p.instance >= shown.size()) {
            continue;
        }
        shown[p.instance] = p.shown ? 1 : 0;
        if (p.shown) {
            renderer_.setPlace(p.instance, p.transform);
            renderer_.setShapeColor(p.instance, p.shapeColor);
        }
    }
    renderer_.show(std::move(shown));
}

ts2::scene::ObjectEffects::PlaceOf Game::effectPlaces() {
    // Objects count levels from 1 at the ground storey.
    return [this](std::int16_t object, const std::string& bone) { return lotLife_->effectPlace(lot_, lotPose_, object, bone, lotView_.view.level - lotView_.ground + 1); };
}

// Scripts start effects from the simulation's first tick on.
void Game::startLotEffects() {
    if (!effectsFiles_) {
        effectsFiles_ = std::make_unique<ts2::content::EffectsFiles>(install_);
    }
    const ts2::effects::File* file = effectsFiles_->all();
    if (!file) {
        return;
    }
    lotEffectPlayer_ = std::make_unique<ts2::scene::EffectPlayer>(*file, static_cast<std::uint32_t>(lastFrame_) | 1);
    lotEffects_ = std::make_unique<ts2::scene::ObjectEffects>(*lotEffectPlayer_);
    applyEffectOptions();
    // The sea's look is chosen as a lot opens, so a change waits for the next lot.
    renderer_.setSeaReflections(lotUi_.graphicsOptions().propOn("EffectsQuality", "enableOceanReflection"));
    lotEffectsSeconds_ = lotLife_->seconds();
    updateLotEffectConditions();
    lotEffectPlayer_->update(0, 0);  // takes the state at once, so the first effects are built for it
    lotLife_->simulation().world().setEffectCommands([this](const ts2::sim::EffectCommand& c) { doEffectCommand(c); });
}

void Game::doEffectCommand(const ts2::sim::EffectCommand& c) {
    using Do = ts2::sim::EffectCommand::Do;
    const auto kind = static_cast<ts2::scene::ObjectEffects::Kind>(c.kind);
    switch (c.what) {
    case Do::Start: {
        std::string picture;
        if (c.pictureNumber) {
            picture = makeEffectScore(*c.pictureNumber);
        } else if (c.pictureWant) {
            picture = makeEffectWant(*c.pictureWant);
        } else if (!c.picture.empty() && (c.pictureGuid == 0 || makeEffectPicture(c))) {
            picture = c.picture;
        }
        if (c.queued) {
            const auto now = static_cast<std::int64_t>(lotLife_->simulation().world().ticks());
            const std::int64_t expires = std::max<std::int64_t>(now + c.queueWait, 0);
            lotEffects_->startQueued(c.object, c.name, c.bone, c.number, c.keep, c.hard, c.queuePriority, static_cast<std::uint64_t>(expires), picture, c.firstFrame, effectPlaces());
            break;
        }
        lotEffects_->start(c.object, c.name, c.bone, c.number, kind, c.keep, c.hard, effectPlaces());
        if (!picture.empty()) {
            lotEffects_->setPicture(c.object, c.name, c.bone, c.number, picture, c.firstFrame);
        }
        break;
    }
    case Do::StartOnce: lotEffects_->startOnce(c.object, c.name, c.bone, effectPlaces()); break;
    case Do::Stop: lotEffects_->stop(c.object, c.name, c.bone, c.number, c.hard); break;
    case Do::StopAll:
        lotEffects_->stopAll(c.object, kind, c.hard);
        if (c.clearsQueue) {
            lotEffects_->clearQueue(c.object);
        }
        break;
    }
}

// A score's picture is made the first time that score is shown, and kept under its text. Gives the picture's name.
std::string Game::makeEffectScore(int score) {
    constexpr const char* kFont = "HeadlineEffectTextureText";
    const std::string text = (score < 0 ? "-" : "+") + ts2::content::groupedNumber(std::abs(score));
    std::string name = "HeadlineEffectTextTexture_" + text;
    if (!effectImages_.contains(name)) {
        auto& fonts = screen_.fonts();
        const auto& style = fonts.style(kFont);
        auto picture = ts2::content::headlineTextPicture(fonts.draw(style, text).pixels, fonts.lineHeight(style));
        effectImages_[name] = static_cast<int>(lot_.images.size());
        lot_.images.push_back({picture.width, picture.height, std::move(picture.rgba)});
        renderer_.addMaterials(lot_);
    }
    return name;
}

// A want's picture is made the first time it is shown and kept under its name, as an object's is. A Sim's portrait
// comes later. Gives the picture's name, empty when the want shows nothing.
std::string Game::makeEffectWant(const ts2::swaf::Want& want) {
    auto shown = lotUi_.balloonWant(*lotLife_, want);
    if (shown.name.empty() || effectImages_.contains(shown.name)) {
        return shown.name;
    }
    if (shown.neighbor != 0) {
        effectPortraits_[shown.name] = {shown.neighbor, std::move(shown.badge)};
        return shown.name;
    }
    auto picture = ts2::content::wantBalloonPicture(shown.picture, shown.badge, app::LotUi::kBalloonPictureSide);
    effectImages_[shown.name] = static_cast<int>(lot_.images.size());
    lot_.images.push_back({picture.width, picture.height, std::move(picture.rgba)});
    renderer_.addMaterials(lot_);
    return shown.name;
}

// A picture of an object or a Sim gets its image the first time it is asked for, and keeps it under its name. A Sim's
// portrait comes later, once its character file is read. False when there is no picture, so the effect keeps its own.
bool Game::makeEffectPicture(const ts2::sim::EffectCommand& c) {
    const auto known = effectImages_.find(c.picture);
    if (known != effectImages_.end()) {
        return known->second >= 0 || effectPortraits_.contains(c.picture);
    }
    if (c.pictureNeighbor != 0) {
        effectPortraits_[c.picture] = {c.pictureNeighbor, {}};
        return true;
    }
    int& image = effectImages_[c.picture];
    image = -1;
    if (auto picture = c.pictureIsMemory ? lotUi_.balloonMemoryPicture(*lotLife_, c.pictureGuid) : lotUi_.balloonObjectPicture(*lotLife_, c.pictureGuid, c.pictureIndex)) {
        image = static_cast<int>(lot_.images.size());
        lot_.images.push_back({picture->width, picture->height, std::move(picture->rgba)});
        renderer_.addMaterials(lot_);
    }
    return image >= 0;
}

void Game::stopLotEffects() {
    effectPortraits_.clear();
    effectVertices_.clear();
    effectBatches_.clear();
    renderer_.showParticles(effectVertices_, effectBatches_);
    lotEffects_.reset();
    lotEffectPlayer_.reset();
    effectImages_.clear();
}

// The level of detail goes with the camera's zoom, and the state bits say the time of day (bits 0 and 1). Bit 11 is
// the camera being outside a room, which it is taken to be for now. Bit 13 is the season's. The bits of
// weather are not set yet, so none of its effects play.
void Game::updateLotEffectConditions() {
    lotEffectPlayer_->setDetail(!freeCam_ && orbit_.rig ? ts2::camera::effectDetailAt(*orbit_.rig, orbit_.distance) : 1.0f);
    constexpr std::uint32_t kAlways = 0x800 | 0x2000;
    std::uint32_t time = 0x1;
    if (lotDaylight_) {
        using ts2::content::StateTime;
        switch (ts2::content::stateTime(lotDaylight_->state())) {
        case StateTime::Morning: time = 0x2; break;
        case StateTime::Day: time = 0x1; break;
        case StateTime::Evening: time = 0x3; break;
        case StateTime::Night: time = 0; break;
        }
    }
    lotEffectPlayer_->setState(kAlways | time);
}

// Effects follow their objects, play on and hand their sprites to the renderer.
void Game::updateLotEffects(float realSeconds) {
    if (!lotEffectPlayer_ || !lotLife_) {
        return;
    }
    updateLotEffectConditions();
    {
        const auto camera = viewCamera();
        const auto ahead = ts2::render::normalize(ts2::render::sub(camera.target, camera.eye));
        const auto beside = ts2::render::normalize(ts2::render::cross(ahead, {0, 0, 1}));
        const auto over = ts2::render::cross(beside, ahead);
        lotEffectPlayer_->setCamera({camera.eye[0], camera.eye[1], camera.eye[2]}, {ahead[0], ahead[1], ahead[2]}, {over[0], over[1], over[2]});
    }
    lotLife_->clipEffects(lot_, [this](const ts2::sim::EffectCommand& c) { doEffectCommand(c); });
    if (!lotLife_->paused()) {
        lotEffects_->advanceQueues(lotLife_->simulation().world().ticks(), effectPlaces());
    }
    lotEffects_->follow(effectPlaces());
    const double seconds = lotLife_->seconds();
    lotEffectPlayer_->update(static_cast<float>(std::clamp(seconds - lotEffectsSeconds_, 0.0, 1.0)), realSeconds);
    lotEffectsSeconds_ = seconds;

    const auto camera = viewCamera();
    const auto view = ts2::render::FrameView::make(camera, width_, height_);
    ts2::scene::ParticleView sprites;
    const auto towards = ts2::render::normalize(ts2::render::sub(camera.target, camera.eye));
    const auto side = ts2::render::normalize(ts2::render::cross(towards, {0, 0, 1}));
    sprites.forward = {towards[0], towards[1], towards[2]};
    sprites.right = {side[0], side[1], side[2]};
    const auto top = ts2::render::cross(side, towards);
    sprites.up = {top[0], top[1], top[2]};
    std::copy(view.viewProjection.m.begin(), view.viewProjection.m.end(), sprites.clip.begin());

    effectVertices_.clear();
    effectBatches_.clear();
    bool newImages = false;
    ts2::scene::appendEffectSprites(*lotEffectPlayer_, sprites, [&](const std::string& name) {
        const auto [known, isNew] = effectImages_.try_emplace(name, -1);
        const auto portrait = effectPortraits_.find(name);
        if (portrait != effectPortraits_.end()) {
            if (auto picture = lotUi_.balloonPortrait(*lotLife_, portrait->second.neighbor)) {
                if (portrait->second.badge.width > 0) {
                    *picture = ts2::content::wantBalloonPicture(*picture, portrait->second.badge, app::LotUi::kBalloonPictureSide);
                }
                known->second = static_cast<int>(lot_.images.size());
                lot_.images.push_back({picture->width, picture->height, std::move(picture->rgba)});
                newImages = true;
                effectPortraits_.erase(portrait);
            }
        } else if (isNew) {
            if (const auto image = assets_.get()->texture(name)) {
                known->second = static_cast<int>(lot_.images.size());
                lot_.images.push_back(*image);
                newImages = true;
            }
        }
        return known->second;
    }, effectVertices_, effectBatches_);
    if (newImages) {
        renderer_.addMaterials(lot_);
    }
    renderer_.showParticles(effectVertices_, effectBatches_);
}

} // namespace app
