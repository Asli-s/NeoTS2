#include "scene/effect_player.hpp"

#include "diag/log.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ts2::scene {

namespace {

using effects::Component;
using effects::VisualEffect;

// Limits that keep a broken file from taking over the game. A chain of effects naming each other would
// otherwise grow without end. The game's own files stay far below both.
constexpr int kMostNesting = 16;
constexpr std::size_t kMostEffects = 20000;

// A change to these state bits starts the parts it brings with a prewarm, as they should look long started.
constexpr std::uint32_t kPrewarmedState = 0x80800;

// Effects placed by something this player doesn't do yet: cameraAttached, applyCursor and clampScreenSize.
constexpr std::uint32_t kNotPlaced = VisualEffect::kCameraAttached | VisualEffect::kApplyCursor | VisualEffect::kClampScreenSize;

// These are placed anew every update, as the camera moves.
constexpr std::uint32_t kFollowsCamera = VisualEffect::kViewRelative | VisualEffect::kCameraFacing;

constexpr std::array<float, 9> kNoTurn{1, 0, 0, 0, 1, 0, 0, 0, 1};

std::array<float, 9> product(const std::array<float, 9>& a, const std::array<float, 9>& b) {
    std::array<float, 9> out{};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
        }
    }
    return out;
}

// `own`, which is placed inside `parent`, in the parent's own space.
EmitterPlace inside(const EmitterPlace& parent, const EmitterPlace& own) {
    EmitterPlace out;
    const auto& r = parent.rotation;
    const float x = own.position[0] * parent.scale, y = own.position[1] * parent.scale, z = own.position[2] * parent.scale;
    out.position = {x * r[0] + y * r[1] + z * r[2] + parent.position[0], x * r[3] + y * r[4] + z * r[5] + parent.position[1],
                    x * r[6] + y * r[7] + z * r[8] + parent.position[2]};
    out.rotation = product(r, own.rotation);
    out.scale = parent.scale * own.scale;
    return out;
}

EmitterPlace placeOf(const Component& c) {
    EmitterPlace out;
    out.rotation = c.orientation;
    out.scale = c.scale;
    out.position = c.offset;
    return out;
}

float between(const std::array<float, 2>& range, float share) { return range[0] + (range[1] - range[0]) * share; }

// True when a part may play at this level of detail and in this state.
bool allowed(const Component& c, int detail, std::uint32_t state) {
    return detail >= c.lowestDetail && detail < c.detailEnd && (state & c.stateMask) == c.state;
}

} // namespace

PlayingEffect::PlayingEffect(EffectPlayer& player, std::string name, const effects::VisualEffect* record, int nesting)
    : player_(&player), name_(std::move(name)), record_(record), nesting_(nesting) {}

PlayingEffect::~PlayingEffect() = default;

void PlayingEffect::start(bool prewarm) {
    if (!record_) {
        return;
    }
    for (EffectPart& part : parts_) {
        part.start(prewarm);
    }
    running_ = true;
    stopping_ = false;
    placedSinceStart_ = false;
}

void PlayingEffect::stop(bool hard) {
    if (!record_) {
        return;
    }
    if (record_->flags & VisualEffect::kHardStop) {
        hard = true;
    }
    for (EffectPart& part : parts_) {
        part.stop(hard);
    }
    if (hard) {
        running_ = false;
        stopping_ = false;
    } else {
        stopping_ = true;
    }
}

void PlayingEffect::placeIn(const EmitterPlace& own, const EmitterPlace& parent) {
    if (!record_ || !player_) {
        return;
    }
    const std::uint32_t flags = record_->flags;
    if ((flags & VisualEffect::kDetach) && placedSinceStart_ && running_) {
        return;
    }
    ownGiven_ = own;
    parentGiven_ = parent;
    own_ = own;
    parent_ = parent;
    if (flags & VisualEffect::kIgnoreScale) {
        own_.scale = 1;
        parent_.scale = 1;
    }
    if (flags & VisualEffect::kIgnoreOrientation) {
        own_.rotation = kNoTurn;
        parent_.rotation = kNoTurn;
    }
    if (sizeByDetail_[0] >= 0) {
        parent_.scale *= between(sizeByDetail_, player_->detailShare_);
    }
    if (flags & VisualEffect::kOrientZPole) {
        // Keeps only its turn about the vertical.
        const float a = own_.rotation[0], b = own_.rotation[3];
        const float length = std::sqrt(a * a + b * b);
        own_.rotation = kNoTurn;
        if (length > 0) {
            own_.rotation[0] = own_.rotation[4] = a / length;
            own_.rotation[3] = b / length;
            own_.rotation[1] = -b / length;
        }
    }
    if (flags & VisualEffect::kRigid) {
        parent_ = inside(parent_, own_);
        own_ = EmitterPlace{};
    }
    if ((flags & VisualEffect::kViewRelative) && player_->hasCamera_) {
        // Turns the parent about where it stands. cameraFacing points x at the camera with z up, and
        // viewRelative alone turns about the vertical with the camera.
        std::array<float, 9> turn = kNoTurn;
        if (flags & VisualEffect::kCameraFacing) {
            std::array<float, 3> n{player_->eye_[0] - parent_.position[0], player_->eye_[1] - parent_.position[1], player_->eye_[2] - parent_.position[2]};
            const float nLength = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (nLength > 0) {
                for (float& v : n) {
                    v /= nLength;
                }
            }
            const auto& up = player_->up_;
            std::array<float, 3> side{up[1] * n[2] - up[2] * n[1], up[2] * n[0] - up[0] * n[2], up[0] * n[1] - up[1] * n[0]};
            const float length = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
            std::array<float, 3> top{0, 1, 0};
            if (length > 0) {
                for (float& v : side) {
                    v /= length;
                }
                top = {n[1] * side[2] - n[2] * side[1], n[2] * side[0] - n[0] * side[2], n[0] * side[1] - n[1] * side[0]};
            } else {
                side = {1, 0, 0};
            }
            turn = {n[0], side[0], top[0], n[1], side[1], top[1], n[2], side[2], top[2]};
        } else {
            const float angle = std::atan2(player_->forward_[1], player_->forward_[0]);
            turn[0] = turn[4] = std::cos(angle);
            turn[3] = std::sin(angle);
            turn[1] = -turn[3];
        }
        parent_.rotation = product(turn, parent_.rotation);
    }
    for (EffectPart& part : parts_) {
        part.setPlace(inside(own_, placeOf(*part.component)), parent_);
    }
    placedSinceStart_ = true;
}

void PlayingEffect::setHidden(bool hidden) {
    hidden_ = hidden;
    for (EffectPart& part : parts_) {
        if (part.child) {
            part.child->setHidden(hidden);
        } else if (part.meta) {
            part.meta->setHidden(hidden);
        } else if (part.sequence) {
            part.sequence->setHidden(hidden);
        }
    }
}

void EffectPart::setPicture(const std::string& name) {
    if (component->flags & Component::kIgnoreParams) {
        return;
    }
    if (child) {
        child->setPicture(name);
    } else if (emitter) {
        emitter->setPicture(name);
    } else if (meta) {
        meta->setPicture(name);
    }
}

void EffectPart::setFirstFrame(int frame) {
    if (component->flags & Component::kIgnoreParams) {
        return;
    }
    if (child) {
        child->setFirstFrame(frame);
    } else if (emitter) {
        emitter->setFirstFrame(frame);
    } else if (meta) {
        meta->setFirstFrame(frame);
    }
}

void PlayingEffect::setPicture(const std::string& name) {
    if (!record_ || (record_->flags & VisualEffect::kIgnoreParams)) {
        return;
    }
    picture_ = name;
    for (EffectPart& part : parts_) {
        part.setPicture(name);
    }
}

void PlayingEffect::setFirstFrame(int frame) {
    if (!record_ || (record_->flags & VisualEffect::kIgnoreParams)) {
        return;
    }
    firstFrame_ = frame;
    for (EffectPart& part : parts_) {
        part.setFirstFrame(frame);
    }
}

void PlayingEffect::setMessages(float alpha, const std::array<float, 3>& colour) {
    if (!record_ || (record_->flags & VisualEffect::kIgnoreParams)) {
        return;
    }
    messageAlpha_ = alpha;
    messageColour_ = colour;
    for (EffectPart& part : parts_) {
        if (part.component->flags & Component::kIgnoreParams) {
            continue;
        }
        if (part.child) {
            part.child->setMessages(alpha, colour);
        } else if (part.meta) {
            part.meta->setAlpha(alpha);
        } else if (part.model) {
            part.setModelMessages(alpha, colour);
        }
    }
}

void PlayingEffect::update(float seconds) {
    if (record_ && (record_->flags & kFollowsCamera)) {
        placeIn(ownGiven_, parentGiven_);
    }
    for (EffectPart& part : parts_) {
        if (!part.running()) {
            continue;
        }
        const float scaled = seconds * part.component->timeScale;
        if (part.child) {
            part.child->update(scaled);
        } else if (part.emitter) {
            const bool messages = !(part.component->flags & Component::kIgnoreParams);
            player_->updateParticles(part, scaled, messages ? messageAlpha_ : 1, messages ? messageColour_ : std::array<float, 3>{1, 1, 1});
        } else if (part.meta) {
            part.meta->update(scaled, player_->conditions_);
        } else if (part.sequence) {
            part.sequence->update(scaled);
        }
    }
}

// True when nothing is left to play. Parts with the ignoreLength keyword don't count, and an effect
// with the noAutoStop keyword goes on until it is told to stop.
bool PlayingEffect::finished() {
    if (!running_ || !record_) {
        return true;
    }
    if (!stopping_ && (record_->flags & VisualEffect::kNoAutoStop)) {
        return false;
    }
    for (const EffectPart& part : parts_) {
        if (!(part.component->flags & Component::kIgnoreLength) && part.running()) {
            return false;
        }
    }
    stop(true);
    return true;
}

void EffectPart::start(bool prewarm) {
    if (emitter) {
        emitter->start(prewarm);
    } else if (meta) {
        meta->start(prewarm);
    } else if (sequence) {
        sequence->start(prewarm);
    } else if (child) {
        child->start(prewarm);
    } else if (model) {
        modelShown = true;
    }
}

void EffectPart::stop(bool hard) {
    if (emitter) {
        emitter->stop(hard);
    } else if (meta) {
        meta->stop(hard);
    } else if (sequence) {
        sequence->stop(hard);
    } else if (child) {
        child->stop(hard);
    } else if (model) {
        // Soft or hard, a model goes at once.
        modelShown = false;
    }
}

bool EffectPart::running() const {
    if (emitter) {
        return emitter->running();
    }
    if (meta) {
        return meta->running();
    }
    if (sequence) {
        return sequence->running();
    }
    if (model) {
        return modelShown;
    }
    return child && child->running();
}

void EffectPart::setPlace(const EmitterPlace& own, const EmitterPlace& parentPlace) {
    if (emitter) {
        emitter->place(own);
        parent = parentPlace;
    } else if (meta) {
        meta->place(own, parentPlace);
    } else if (sequence) {
        sequence->place(own, parentPlace);
    } else if (child) {
        child->placeIn(own, parentPlace);
    } else if (model) {
        parent = inside(parentPlace, own);
    }
}

void EffectPart::setModelMessages(float messageAlpha, const std::array<float, 3>& colour) {
    modelColour = {model->colour[0] * colour[0], model->colour[1] * colour[1], model->colour[2] * colour[2], model->alpha * messageAlpha};
}

EffectSequence::EffectSequence(const effects::SequenceBlock& block, EffectPlayer& player, std::uint32_t seed, int nesting)
    : block_(block), player_(player), nesting_(nesting), randomState_(seed == 0 ? 1 : seed) {}

float EffectSequence::random(float low, float high) {
    randomState_ ^= randomState_ << 13;
    randomState_ ^= randomState_ >> 17;
    randomState_ ^= randomState_ << 5;
    return low + (high - low) * (static_cast<float>(randomState_ - 1) / 4294967296.0f);
}

void EffectSequence::start(bool prewarm) {
    if (block_.items.empty()) {
        running_ = false;
        return;
    }
    running_ = true;
    if (!prewarm || fresh_) {
        time_ = 0;
        item_ = 0;
        endTime_ = 0;
        untilEffectEnds_ = false;
    }
    fresh_ = false;
}

void EffectSequence::stop(bool hard) {
    if (current_) {
        current_->stop(hard);
        current_.reset();
    }
    if (hard) {
        for (const auto& effect : kept_) {
            if (effect->running()) {
                effect->stop(true);
            }
        }
        kept_.clear();
    }
    running_ = false;
}

void EffectSequence::setHidden(bool hidden) {
    hidden_ = hidden;
    if (current_) {
        current_->setHidden(hidden);
    }
    for (const auto& effect : kept_) {
        effect->setHidden(hidden);
    }
}

void EffectSequence::place(const EmitterPlace& own, const EmitterPlace& parent) {
    own_ = own;
    parent_ = parent;
    if (current_) {
        current_->placeIn(own, parent);
    }
}

// When an item's time is up the next one starts. An item with a negative range lasts until its effect ends.
void EffectSequence::update(float seconds) {
    const auto dropEnded = [this] { std::erase_if(kept_, [](const std::shared_ptr<PlayingEffect>& effect) { return !effect->running(); }); };
    if (!running_) {
        dropEnded();
        return;
    }
    time_ += seconds;
    if (untilEffectEnds_) {
        endTime_ = time_;
        if (current_ && current_->running()) {
            return;
        }
    } else if (time_ < endTime_) {
        return;
    }
    if (endTime_ > 0) {
        ++item_;
        if (item_ >= block_.items.size()) {
            if (!(block_.flags & effects::SequenceBlock::kLoops)) {
                stop((block_.flags & effects::SequenceBlock::kLetFinish) != 0);
                return;
            }
            item_ = 0;
        }
        if (current_ && current_->running()) {
            const bool keepPlaying = (block_.flags & effects::SequenceBlock::kKeepPlaying) != 0;
            if (!keepPlaying) {
                current_->stop(false);
            }
            if (keepPlaying || !(block_.flags & effects::SequenceBlock::kLetFinish)) {
                kept_.push_back(current_);
                dropEnded();
            }
            current_.reset();
        }
    }
    const effects::SequenceBlock::Item& item = block_.items[item_];
    if (item.range[0] < 0) {
        untilEffectEnds_ = true;
    } else {
        untilEffectEnds_ = false;
        endTime_ += random(item.range[0], item.range[1]);
    }
    if (!item.effect.empty()) {
        current_.reset();
        if (player_.knows(item.effect)) {
            current_ = player_.createInside(item.effect, nesting_ + 1);
        }
        if (current_) {
            current_->setHidden(hidden_);
            current_->placeIn(own_, parent_);
            current_->start((block_.flags & effects::SequenceBlock::kStartsPrewarmed) != 0);
        }
    }
}

EffectPlayer::EffectPlayer(const effects::File& file, std::uint32_t seed) : file_(file), randomState_(seed == 0 ? 1 : seed) {}

EffectPlayer::~EffectPlayer() {
    for (const auto& effect : effects_) {
        effect->player_ = nullptr;
    }
}

std::uint32_t EffectPlayer::randomBits() {
    randomState_ ^= randomState_ << 13;
    randomState_ ^= randomState_ >> 17;
    randomState_ ^= randomState_ << 5;
    return randomState_;
}

std::shared_ptr<PlayingEffect> EffectPlayer::make(std::string_view name, int nesting) {
    if (effectCount() >= kMostEffects) {
        diag::warnOnce("effects: reached the most effects that may play at once");
        return nullptr;
    }
    const VisualEffect* record = file_.find(name, level_);
    if (record && (record->flags & kNotPlaced)) {
        diag::warnOnce("effects: an effect placed by the camera or the cursor plays where it was put (" + std::string(name) + ")");
    }
    std::shared_ptr<PlayingEffect> effect(new PlayingEffect(*this, std::string(name), record, nesting));
    effects_.push_back(effect);
    rebuild(*effect, true, 0);
    return effect;
}

std::shared_ptr<PlayingEffect> EffectPlayer::create(std::string_view name) {
    if (!knows(name)) {
        return nullptr;
    }
    return make(name, 0);
}

std::shared_ptr<PlayingEffect> EffectPlayer::createInside(std::string_view name, int nesting) {
    if (nesting > kMostNesting) {
        diag::warnOnce("effects: effects inside effects go too deep (" + std::string(name) + ")");
        return nullptr;
    }
    return make(name, nesting);
}

void EffectPlayer::setLevel(std::uint16_t level) {
    if (level == level_) {
        return;
    }
    level_ = level;
    levelChanging_ = true;
    rebuildAll();
    levelChanging_ = false;
}

void EffectPlayer::setDetail(float zoom) {
    const float clamped = std::clamp(zoom, 1.0f, 4.9999f);
    if (clamped == zoom_) {
        return;
    }
    detailBefore_ = detail_;
    zoomBefore_ = zoom_;
    zoom_ = clamped;
    detail_ = static_cast<int>(clamped);
    detailShare_ = clamped - static_cast<float>(detail_);
    if (detail_ != detailBefore_) {
        rebuildAll();
    }
}

void EffectPlayer::setCamera(const std::array<float, 3>& eye, const std::array<float, 3>& forward, const std::array<float, 3>& up) {
    hasCamera_ = true;
    eye_ = eye;
    forward_ = forward;
    up_ = up;
}

void EffectPlayer::rebuildAll() {
    // Rebuilding can make more effects, which join the end of the list.
    for (std::size_t i = 0; i < effects_.size(); ++i) {
        rebuild(*effects_[i], false, 0);
    }
}

// Each part keeps its rate, size and alpha at this level of detail and the next, as shares of the way through
// its range. A part for one level of detail, or two next to each other, has its pairs as they stand.
void EffectPlayer::setPartDetail(EffectPart& part) const {
    const Component& c = *part.component;
    if (c.lowestDetail < c.detailEnd - 1) {
        const float step = 1.0f / static_cast<float>(c.detailEnd - c.lowestDetail);
        const float low = static_cast<float>(detail_ - c.lowestDetail) * step;
        const float high = static_cast<float>(detail_ - c.lowestDetail + 1) * step;
        part.rate = {between(c.rateByDetail, low), between(c.rateByDetail, high)};
        part.size = {between(c.sizeByDetail, low), between(c.sizeByDetail, high)};
        part.alpha = {between(c.alphaByDetail, low), between(c.alphaByDetail, high)};
    } else {
        part.rate = c.rateByDetail;
        part.size = c.sizeByDetail;
        part.alpha = c.alphaByDetail;
    }
    if (part.child) {
        part.child->sizeByDetail_ = part.size;
    } else if (part.meta) {
        part.meta->setDetailScales(part.rate[0], part.size[0]);
    }
}

// Makes one allowed part, placed inside the effect, and starts it if the effect is playing.
void EffectPlayer::makePart(PlayingEffect& effect, const Component& c, bool force, bool prewarm) {
    EffectPart part;
    part.component = &c;
    if (c.type == 0) {
        if (effect.nesting_ >= kMostNesting) {
            diag::warnOnce("effects: effects inside effects go too deep (" + effect.name_ + ")");
            return;
        }
        part.child = make(c.name, effect.nesting_ + 1);
        if (!part.child) {
            return;
        }
        part.child->nested_ = true;
        part.child->setHidden(effect.hidden_);
    } else if (c.type == static_cast<std::uint8_t>(effects::BlockType::Particles)) {
        if (static_cast<std::size_t>(c.index) >= file_.particles.size()) {
            return;
        }
        part.emitter = std::make_unique<ParticleEmitter>(file_.particles[static_cast<std::size_t>(c.index)], randomBits());
    } else if (c.type == static_cast<std::uint8_t>(effects::BlockType::MetaParticles)) {
        if (static_cast<std::size_t>(c.index) >= file_.metaParticles.size()) {
            return;
        }
        part.meta = std::make_unique<MetaParticleEmitter>(file_.metaParticles[static_cast<std::size_t>(c.index)], *this, randomBits(), effect.nesting_);
        part.meta->setHidden(effect.hidden_);
    } else if (c.type == static_cast<std::uint8_t>(effects::BlockType::Sequence)) {
        if (static_cast<std::size_t>(c.index) >= file_.sequences.size()) {
            return;
        }
        part.sequence = std::make_unique<EffectSequence>(file_.sequences[static_cast<std::size_t>(c.index)], *this, randomBits(), effect.nesting_);
        part.sequence->setHidden(effect.hidden_);
    } else if (c.type == static_cast<std::uint8_t>(effects::BlockType::Model)) {
        if (static_cast<std::size_t>(c.index) >= file_.models.size()) {
            return;
        }
        const effects::ModelBlock& block = file_.models[static_cast<std::size_t>(c.index)];
        if (block.size != 1) {
            diag::warnOnce("effects: a model of another size isn't played yet (" + block.model + ")");
            return;
        }
        part.model = &block;
        if (c.flags & Component::kIgnoreParams) {
            part.setModelMessages(1, {1, 1, 1});
        } else {
            part.setModelMessages(effect.messageAlpha_, effect.messageColour_);
        }
    } else {
        diag::warnOnce("effects: a part of block type " + std::to_string(c.type) + " isn't played yet (" + effect.name_ + ")");
        return;
    }
    if (!effect.picture_.empty()) {
        part.setPicture(effect.picture_);
    }
    if (effect.firstFrame_ >= 0) {
        part.setFirstFrame(effect.firstFrame_);
    }
    setPartDetail(part);
    part.setPlace(inside(effect.own_, placeOf(c)), effect.parent_);
    if (effect.running_ && !effect.stopping_) {
        if (force) {
            part.start(false);
        } else if (levelChanging_) {
            part.start(true);
        } else {
            part.start(prewarm);
        }
    }
    effect.parts_.push_back(std::move(part));
}

// Brings an effect's parts in line with the level of detail and the state. `force` makes the parts of a new
// effect, however the state and level of detail were before.
void EffectPlayer::rebuild(PlayingEffect& effect, bool force, std::uint32_t changedState) {
    if (levelChanging_) {
        for (EffectPart& part : effect.parts_) {
            part.stop(true);
        }
        effect.parts_.clear();
        effect.record_ = file_.find(effect.name_, level_);
    }
    if (!effect.record_) {
        return;
    }
    const bool prewarm = (changedState & kPrewarmedState) != 0;

    // Parts no longer allowed stop. With a prewarm they go at once, otherwise they finish first.
    std::erase_if(effect.parts_, [&](EffectPart& part) {
        if (!allowed(*part.component, detail_, state_)) {
            part.stop(prewarm);
            if (prewarm || !part.running()) {
                return true;
            }
        }
        if (zoom_ != zoomBefore_) {
            setPartDetail(part);
        }
        return false;
    });

    // A group of components plays one of them, by a roll against their running total of weights. After the
    // pick the roll is out of reach. A state list is a run of components that one part can switch between:
    // only its first is made.
    std::uint16_t group = 0;
    std::uint32_t roll = 0, total = 0;
    bool inStateList = false;
    const auto& components = effect.record_->components;
    for (std::size_t i = 0; i < components.size(); ++i) {
        const Component& c = components[i];
        const bool now = allowed(c, detail_, state_);
        const bool before = allowed(c, detailBefore_, stateBefore_);
        if (c.index < 0 || !((!before && now) || force || levelChanging_)) {
            continue;
        }
        if (c.group == 0) {
            group = 0;
        } else {
            if (c.group != group) {
                roll = randomBits() & 0xFFFF;
                total = 0;
            }
            group = c.group;
            total += c.weight;
            if (total < roll) {
                continue;
            }
            roll = 0x10000;
        }
        if (!now) {
            continue;
        }
        if (!inStateList) {
            makePart(effect, c, force, prewarm);
        }
        if ((c.flags & Component::kStateList) && !effect.parts_.empty()) {
            inStateList = !(c.flags & Component::kEndsStateList);
        }
    }
}

// Moves one part's particles on, and plays its death effect where any of them died.
void EffectPlayer::updateParticles(EffectPart& part, float seconds, float alpha, const std::array<float, 3>& colour) {
    ParticleEmitter& emitter = *part.emitter;
    EmitterScales scales;
    scales.rate = between(part.rate, detailShare_);
    scales.size = between(part.size, detailShare_);
    scales.alpha = between(part.alpha, detailShare_) * alpha;
    scales.colour = colour;
    emitter.setScales(scales);
    emitter.update(seconds, conditions_);
    const std::string& deathEffect = file_.particles[static_cast<std::size_t>(part.component->index)].deathEffect;
    for (const auto& where : emitter.takeDeaths()) {
        if (const auto effect = file_.knows(deathEffect) ? make(deathEffect, 0) : nullptr) {
            EmitterPlace place;
            place.position = where;
            effect->placeIn(place, part.parent);
            effect->start(false);
        }
    }
}

void EffectPlayer::update(float gameSeconds, float realSeconds) {
    if (const std::uint32_t changed = state_ ^ nextState_) {
        stateBefore_ = state_;
        state_ = nextState_;
        for (std::size_t i = 0; i < effects_.size(); ++i) {
            if (effects_[i]->record_ && (effects_[i]->record_->stateBits & changed)) {
                rebuild(*effects_[i], false, changed);
            }
        }
        stateBefore_ = state_;
    }
    for (std::size_t i = 0; i < effects_.size(); ++i) {
        PlayingEffect& effect = *effects_[i];
        if (effect.nested_ || !effect.running_ || !effect.record_) {
            continue;
        }
        effect.update((effect.record_->flags & VisualEffect::kRealTime) ? realSeconds : gameSeconds);
    }
    const auto count = static_cast<float>(particleCount());
    const auto target = static_cast<float>(particleTarget_);
    if (count > target) {
        conditions_.density *= target / count;
    } else if (conditions_.density < 1) {
        const float grown = target * conditions_.density;
        conditions_.density = grown < count ? grown / count : 1;
    }
    std::erase_if(effects_, [](const std::shared_ptr<PlayingEffect>& effect) { return effect->finished() && effect.use_count() == 1; });
}

std::size_t EffectPlayer::particleCount() const {
    std::size_t count = 0;
    for (const auto& effect : effects_) {
        for (const EffectPart& part : effect->parts_) {
            count += part.emitter ? part.emitter->particles().size() : 0;
        }
    }
    return count;
}

void EffectPlayer::forEachParticleSystem(const ParticleVisitor& visit) const {
    for (const auto& effect : effects_) {
        if (effect->hidden_) {
            continue;
        }
        for (const EffectPart& part : effect->parts_) {
            if (part.emitter) {
                visit(file_.particles[static_cast<std::size_t>(part.component->index)], *part.emitter, part.parent);
            }
        }
    }
}

void EffectPlayer::forEachModel(const ModelVisitor& visit) const {
    for (const auto& effect : effects_) {
        if (effect->hidden_) {
            continue;
        }
        for (const EffectPart& part : effect->parts_) {
            if (part.model && part.modelShown) {
                visit(*part.model, part.parent, part.modelColour);
            }
        }
    }
}

} // namespace ts2::scene
