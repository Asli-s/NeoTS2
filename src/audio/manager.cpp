#include "audio/manager.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ts2::audio {

namespace {

bool isAny(Group group, std::initializer_list<Group> groups) {
    return std::find(groups.begin(), groups.end(), group) != groups.end();
}

// Groups the pause leaves playing.
bool pausesWithGame(Group group) {
    return !isAny(group, {Group::Music, Group::Ui, Group::Cinematic});
}

// Sounds dropped, not started paused, while their group is paused: one-shots of most groups.
bool droppedWhilePaused(Group group, const TrackSettings& settings) {
    return !settings.looping && !isAny(group, {Group::Stereo, Group::Music, Group::Television, Group::Stitcher});
}

// Sounds the voice limit may stop for a new one.
bool stealable(Group group, const TrackSettings& settings) {
    return !settings.looping && !settings.keepPlaying &&
           !isAny(group, {Group::Stereo, Group::Music, Group::Cinematic, Group::Television, Group::Stitcher});
}

// Speed changes the pitch and volume of these groups only.
bool followsSpeed(Group group) {
    return group == Group::Sfx || group == Group::Voice;
}

float length(const Vec3& v) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

} // namespace

void Manager::setPerformanceLevel(int level) {
    level_ = std::clamp(level, 0, 2);
}

void Manager::setGamePaused(bool paused) {
    gamePaused_ = paused;
    for (Instance& instance : instances_) {
        if (pausesWithGame(instance.request.group)) {
            instance.gamePaused = paused;
        }
    }
}

bool Manager::groupPaused(Group group) const {
    return gamePaused_ && pausesWithGame(group);
}

bool Manager::playsFor(const Instance& instance, std::int16_t object) const {
    return !instance.stopped && instance.request.object == object;
}

float Manager::distanceToListener(const Instance& instance) const {
    const Vec3& at = *instance.request.position;
    return length({at[0] - listener_.position[0], at[1] - listener_.position[1], at[2] - listener_.position[2]});
}

float Manager::gainAt(const Request& request, const TrackSettings& settings) const {
    if (!settings.positional || !request.position) {
        return 1;
    }
    const Vec3& at = *request.position;
    const float d = length({at[0] - listener_.position[0], at[1] - listener_.position[1], at[2] - listener_.position[2]});
    return distanceGain(d, settings.minDistance, settings.maxDistance, tuning_.rolloff);
}

bool Manager::makeRoom(Group group, float priority) {
    const auto groupIndex = static_cast<std::size_t>(group);
    int inGroup = 0, total = 0;
    for (const Instance& instance : instances_) {
        if (!instance.stopped) {
            ++total;
            inGroup += instance.request.group == group ? 1 : 0;
        }
    }
    // The counts are taken before the new sound, so one more than the limit can play. A limit of 0
    // or -1 is no limit: the base game's own data sets Sfx to 0, and several Ambience sounds play at once.
    const int groupMax = tuning_.maxSoundsByGroup[groupIndex][static_cast<std::size_t>(level_)];
    const int totalMax = tuning_.maxSounds[static_cast<std::size_t>(level_)];
    const bool groupFull = groupMax > 0 && inGroup > groupMax;
    const bool allFull = !groupFull && group != Group::Stitcher && totalMax > 0 && total > totalMax;
    if (!groupFull && !allFull) {
        return true;
    }
    Instance* victim = nullptr;
    float lowest = std::numeric_limits<float>::infinity();
    for (Instance& instance : instances_) {
        if (instance.stopped || instance.gamePaused || instance.outOfRange || !stealable(instance.request.group, instance.settings) ||
            (groupFull && instance.request.group != group)) {
            continue;
        }
        const double left = instance.lengthMs > 0 ? instance.lengthMs - instance.playedMs : std::numeric_limits<double>::infinity();
        const float effective = effectivePriority(instance.settings.priority, instance.primaryGain, left);
        if (effective < lowest) {
            lowest = effective;
            victim = &instance;
        }
    }
    if (victim == nullptr || !(lowest < priority)) {
        return false;
    }
    victim->stopped = true;
    return true;
}

std::optional<std::uint32_t> Manager::start(const Request& request, const TrackSettings& settings, std::span<const SampleKey> samples,
                                            double nowMs, Dropped* why) {
    const auto drop = [&](Dropped reason) -> std::optional<std::uint32_t> {
        if (why != nullptr) {
            *why = reason;
        }
        return std::nullopt;
    };
    // At fast speeds a Sim's new voice line waits for none: it is dropped while anything else of
    // the same object plays.
    if (speedIndex_ > 2 && request.group == Group::Voice &&
        std::any_of(instances_.begin(), instances_.end(), [&](const Instance& i) { return playsFor(i, request.object); })) {
        return drop(Dropped::VoiceAtSpeed);
    }
    if (settings.instanceMode == 2 && std::any_of(instances_.begin(), instances_.end(), [&](const Instance& i) {
            return playsFor(i, request.object) && i.request.track == request.track;
        })) {
        return drop(Dropped::AlreadyPlaying);
    }
    if (settings.playChance != 0 && !chancePlays(settings.playChance, chanceRoll(random_.next()))) {
        return drop(Dropped::Chance);
    }
    const bool paused = groupPaused(request.group);
    if (paused && droppedWhilePaused(request.group, settings)) {
        return drop(Dropped::GroupPaused);
    }
    const float primaryGain = gainAt(request, settings);
    if (!makeRoom(request.group, effectivePriority(settings.priority, primaryGain, std::numeric_limits<double>::infinity()))) {
        return drop(Dropped::NoVoice);
    }
    if (samples.empty()) {
        return drop(Dropped::NoSample);
    }
    Instance instance;
    instance.id = nextId_++;
    if (nextId_ == 0) {
        nextId_ = 1;
    }
    instance.request = request;
    instance.settings = settings;
    instance.sample = samples[static_cast<std::size_t>(random_.next()) % samples.size()];
    instance.startMs = nowMs;
    instance.gamePaused = paused;
    instance.fade = paused ? 0.0f : 1.0f;
    instance.primaryGain = primaryGain;
    instances_.push_back(instance);
    if (why != nullptr) {
        *why = Dropped::None;
    }
    return instance.id;
}

void Manager::stopEvent(SoundKey event, std::int16_t object) {
    for (Instance& instance : instances_) {
        if (instance.request.event == event && instance.request.object == object) {
            instance.stopped = true;
        }
    }
}

void Manager::stopObject(std::int16_t object) {
    for (Instance& instance : instances_) {
        if (instance.request.object == object) {
            instance.stopped = true;
        }
    }
}

void Manager::stop(std::uint32_t id) {
    for (Instance& instance : instances_) {
        if (instance.id == id) {
            instance.stopped = true;
        }
    }
}

void Manager::stopAll() {
    instances_.clear();
}

void Manager::setLength(std::uint32_t id, double lengthMs) {
    for (Instance& instance : instances_) {
        if (instance.id == id) {
            instance.lengthMs = lengthMs;
        }
    }
}

void Manager::setPlayed(std::uint32_t id, double playedMs) {
    for (Instance& instance : instances_) {
        if (instance.id == id) {
            instance.playedMs = playedMs;
        }
    }
}

void Manager::moveObject(std::int16_t object, const Vec3& position) {
    for (Instance& instance : instances_) {
        if (object != 0 && instance.request.object == object && instance.request.position) {
            instance.request.position = position;
        }
    }
}

const Manager::Instance* Manager::find(std::uint32_t id) const {
    const auto it = std::find_if(instances_.begin(), instances_.end(), [id](const Instance& i) { return i.id == id; });
    return it == instances_.end() ? nullptr : &*it;
}

Manager::Output Manager::output(Instance& instance, double elapsedMs) {
    const TrackSettings& s = instance.settings;
    const bool positional = s.positional && instance.request.position;
    float volume = s.volume;
    float azimuthRadians = 0;
    instance.primaryGain = 1;
    if (positional) {
        const Vec3& at = *instance.request.position;
        const float d = distanceToListener(instance);
        instance.primaryGain = distanceGain(d, s.minDistance, s.maxDistance, tuning_.rolloff);
        // Out of range a one-shot ends, a loop waits until its object is near again.
        const bool exempt = s.keepPlaying || isAny(instance.request.group, {Group::Stereo, Group::Stitcher});
        instance.outOfRange = s.looping && d >= s.maxDistance;
        if (!s.looping && !exempt && d >= s.maxDistance) {
            instance.stopped = true;
        }
        // The second listener stands at the first, so it only softens the drop.
        const float secondary = 1 - (1 - instance.primaryGain) * tuning_.secondaryListenerInfluence;
        volume *= secondary * axisGain(listener_, at, s.minDistance, s.maxDistance, tuning_.rolloff, s.rolloffScale, tuning_.rolloffAspectRatio);
        azimuthRadians = azimuth(listener_, at);
    } else {
        instance.outOfRange = false;
    }
    // The pause fades over the track's fade-out time, and comes back over the same time.
    const float target = instance.gamePaused || instance.outOfRange ? 0.0f : 1.0f;
    const double fadeMs = instance.outOfRange ? 0 : s.fadeOutMs;
    if (fadeMs <= 0) {
        instance.fade = target;
    } else {
        const float step = static_cast<float>(elapsedMs / fadeMs);
        instance.fade = target < instance.fade ? std::max(target, instance.fade - step) : std::min(target, instance.fade + step);
    }
    volume *= instance.fade;
    const auto speed = static_cast<std::size_t>(speedIndex_ >= 2 && speedIndex_ <= 4 ? speedIndex_ : 2);
    Output out;
    out.id = instance.id;
    if (followsSpeed(instance.request.group)) {
        volume *= tuning_.speedVolume[speed];
        out.pitch = tuning_.speedPitch[speed];
    }
    out.advancing = instance.fade > 0 || target > 0;
    out.mono = channelGains(azimuthRadians, 1)[0];
    out.stereo = channelGains(azimuthRadians, 2);
    for (EarGains* gains : {&out.mono, &out.stereo[0], &out.stereo[1]}) {
        gains->left *= volume;
        gains->right *= volume;
    }
    return out;
}

std::vector<Manager::Output> Manager::update(double elapsedMs) {
    std::erase_if(instances_, [](const Instance& i) { return i.stopped; });
    std::vector<Output> out;
    out.reserve(instances_.size());
    for (Instance& instance : instances_) {
        out.push_back(output(instance, elapsedMs));
    }
    std::erase_if(instances_, [](const Instance& i) { return i.stopped; });
    std::erase_if(out, [this](const Output& o) { return find(o.id) == nullptr; });
    return out;
}

} // namespace ts2::audio
