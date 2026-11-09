#include "sim/vm_execution.hpp"

#include <limits>

namespace ts2::sim::vm_detail {

namespace {

// How far a fade of `fadeMs` has got `ms` in (0-1), whole when there is no fade.
float fadeShare(double ms, float fadeMs) {
    return fadeMs > 0 ? static_cast<float>(std::clamp(ms / fadeMs, 0.0, 1.0)) : 1.0f;
}

// Stops an overlay at `now`: it falls from the weight its fades give it then to none over `blendMs`,
// as Animate Stop and a put's end of the carry poses both fade it.
void stopOverlay(PlayingOverlay& overlay, double now, float blendMs) {
    const double played = std::max(0.0, now - overlay.startMs);
    const float fadedIn = overlay.blendsIn ? fadeShare(played, overlay.blendInMs.value_or(kOverlayBlendMs)) : 1.0f;
    const float fadedOut = overlay.blendsOut && overlay.plays >= 0
                               ? fadeShare(overlay.lengthMs - played, overlay.blendOutMs.value_or(kOverlayBlendMs))
                               : 1.0f;
    overlay.stoppedAt = fadedIn * fadedOut;
    overlay.stopped = true;
    overlay.plays = 1;
    overlay.lengthMs = static_cast<float>(played + blendMs);
    overlay.blendOutMs = blendMs;
}

} // namespace

// The animation entry [entry] of string list [list] names, from the frame's code owner, its
// semi-global code or the global code, the first that has the entry, nullopt when that entry is
// empty or (unless `orMissing`) names an animation the game doesn't have.
std::optional<std::string> Execution::animationEntry(const Frame& f, std::uint16_t list, std::uint16_t entry, bool orMissing,
                                          std::uint8_t* category) {
    for (const auto group : {f.codeOwner, code_.semiGlobalGroup(f.codeOwner), kGlobalGroup}) {
        const auto text = code_.string(group, list, entry);
        if (!text || text->empty()) {
            continue;
        }
        std::string name = content::animationName(*text);
        if (category != nullptr) {
            *category = content::animationCategory(*text);
        }
        const auto& lengths = world().animationLengths();
        if (name.empty() || (!orMissing && lengths && !lengths(name))) {
            return std::nullopt;
        }
        return name;
    }
    return std::nullopt;
}

// A social animation: entry [entry] of list 0x83 from the code owner or its semi-global code,
// named for me and the Sim my interaction is with ("a2t-" + entry). A teen or elder plays the
// adult's animation when the game has none of its own: tried in turn as me, then the partner,
// then both. Pets are not done yet. nullopt for an empty entry.
std::optional<std::string> Execution::socialAnimation(const Frame& f, const SimObject& sim, std::uint16_t entry) {
    std::optional<std::string> text;
    for (const auto group : {f.codeOwner, code_.semiGlobalGroup(f.codeOwner)}) {
        text = code_.string(group, content::animation_list::kSocial, entry);
        if (text && !text->empty()) {
            break;
        }
    }
    if (!text || text->empty()) {
        return std::nullopt;
    }
    const SimObject* partner = world().find(sim.interactionPartner);
    if (partner == nullptr || !partner->isPerson()) {
        throw Stop{Outcome::Error, "Trying to perform an animation from the Social Interaction list against a non person.", ""};
    }
    if (sim.personData[kSpeciesPersonData] != 0 || partner->personData[kSpeciesPersonData] != 0) {
        throw Stop{Outcome::Unsupported, "social animations with pets not implemented", ""};
    }
    auto asAdult = [](std::int16_t age) { return age > 15 ? std::int16_t{19} : age; };  // teens and elders
    const std::int16_t mine = sim.personData[kLifeStagePersonData], theirs = partner->personData[kLifeStagePersonData];
    const std::string clip = content::animationName(*text);
    const auto& lengths = world().animationLengths();
    for (const auto& [actor, other] : {std::pair{mine, theirs}, {asAdult(mine), theirs}, {mine, asAdult(theirs)},
                                       {asAdult(mine), asAdult(theirs)}}) {
        std::string name = content::socialPrefix(actor, other) + clip;
        if (!lengths || lengths(name)) {
            return name;
        }
    }
    throw Stop{Outcome::Error, "Invalid animation type specified in animation string", content::socialPrefix(mine, theirs) + clip};
}

// Animate Sim (operands: see AnimateOperands). Starts the animation on me and waits, a tick at a
// time, until it begins fading out (by the world's clock), then is true. The clip plays on to its
// end as it fades, under whatever the script starts next. The animation is entry [entry]
// of my age's string list for [list] (see content::ageAnimationList), else of [list] itself.
// List 0x80 names the clip as Animate Overlay does (specialAnimationName), list 0x83 by the social prefixes.
// The clip's SimAntics events run the event tree in bytes 4-5 (see tellAnimateSimEvents).
// Byte 2 flag 0x80 turns its fade in off, 0x40 its fade out (see animateFades).
std::optional<bool> Execution::animateSim(const bhav::Instruction& in, Frame& f) {
    SimObject& sim = me();
    const std::size_t frame = thread_.frames.size() - 1;
    const double now = world().clockMs();
    if (sim.animation && sim.animation->frame == frame && sim.animation->group == f.group && sim.animation->tree == f.instance &&
        sim.animation->pc == f.pc) {
        if (!sim.animation->fadingOut(now)) {
            return std::nullopt;
        }
        if (sim.animation->blendsOut) {
            sim.ending = std::move(sim.animation);
        }
        sim.animation.reset();
        return true;
    }
    const AnimateOperands a = animateOperands(in.operands);
    const std::int16_t entry = a.entryFromParam ? slotAt(f.params, a.entry, "param") : static_cast<std::int16_t>(a.entry);
    std::optional<std::string> name;
    std::uint8_t category = content::animation_category::kNoCategory;
    if (a.list == content::animation_list::kSocial) {
        name = socialAnimation(f, sim, static_cast<std::uint16_t>(entry));
    } else if (a.list == kSpecialAnimationList) {
        name = specialAnimationName(sim, static_cast<std::uint16_t>(entry), &category);
    } else {
        const std::uint8_t ageList = content::ageAnimationList(a.list, sim.personData[kLifeStagePersonData]);
        name = animationEntry(f, ageList, static_cast<std::uint16_t>(entry), ageList == a.list, &category);
        if (!name && ageList != a.list) {
            name = animationEntry(f, a.list, static_cast<std::uint16_t>(entry), true, &category);
        }
    }
    if (!name) {
        throw Stop{Outcome::Error, "Animation name is empty string.", hex(a.list, 2) + " " + std::to_string(entry)};
    }
    // The clip the lot was saved playing carries on at its saved rate from where it got to. The temps
    // that gave its speed may have changed since it started.
    auto resumed = std::exchange(sim.resumedClip, std::nullopt);
    if (resumed && !std::ranges::equal(resumed->name, *name, [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
        })) {
        resumed.reset();
    }
    const std::int16_t speed = a.speedFromTemp ? thread_.temps[2] : a.speed;
    if (speed == 0 && !resumed) {
        throw Stop{Outcome::Error, "Animation speed cannot be zero", ""};
    }
    // A name the game has no clip for plays nothing and the primitive is true at once. Hang Out
    // names "-" for a pose change that has no clip.
    if (const auto& lengths = world().animationLengths(); lengths && !lengths(*name)) {
        return true;
    }
    PlayingAnimation play;
    play.name = *name;
    play.startMs = resumed ? now - resumed->playedMs : now;
    play.speed = resumed ? resumed->speed : static_cast<float>(speed) / 32.0f;
    play.flipped = a.flipFromTemp ? thread_.temps[3] != 0 : a.flipped;
    play.blendsOut = (in.operands[2] & 0x40) == 0;
    play.blendsIn = (in.operands[2] & 0x80) == 0;
    play.layer = 2 * a.priority;
    play.category = entryCategory(play.name, category);
    play.frame = frame;
    play.group = f.group;
    play.tree = f.instance;
    play.pc = f.pc;
    play.eventTree = a.eventTree;
    play.eventCodeOwner = f.codeOwner;
    play.eventStackObject = stackObject();
    if (a.ikOwner != 0 || a.ikData != 0) {
        play.ikTarget = read(a.ikOwner, a.ikData, f);
    }
    if (const auto& lengths = world().animationLengths()) {
        if (const auto ms = lengths(play.name)) {
            play.lengthMs = *ms / std::abs(play.speed);
        }
    }
    const AnimateFades fades = animateFades(play.lengthMs, play.blendsIn, play.blendsOut);
    play.fades = true;
    play.fadeInMs = fades.inMs;
    play.fadeOutMs = fades.outMs;
    // Its events before the save were told then.
    if (resumed && world().animationEvents()) {
        for (const AnimationEvent& e : world().animationEvents()(play.name)) {
            if (e.timeMs / std::abs(play.speed) <= resumed->playedMs) {
                ++play.eventsTold;
            }
        }
    }
    sim.animation = std::move(play);
    return std::nullopt;
}

// List 0x80: entry [entry] of my age's list of adult animations in the global group only, or of
// the adult list itself when my age's clip doesn't exist. A person is needed (error 0x4A).
// An entry without a kind is a carry pose when its clip has a target of type 2, otherwise a
// normal animation, as the game sorts it (AN-20).
std::uint8_t Execution::entryCategory(const std::string& name, std::uint8_t category) {
    namespace cat = content::animation_category;
    if (category != cat::kNoCategory || !world().animationCarries()) {
        return category;
    }
    return world().animationCarries()(name) ? cat::kCarry : cat::kNormal;
}

std::optional<std::string> Execution::specialAnimationName(const SimObject& sim, std::uint16_t entry, std::uint8_t* category) {
    if (!sim.isPerson()) {
        throw Stop{Outcome::Error, "not a person", "animation list 0x80"};
    }
    const std::uint8_t ageList = content::ageAnimationList(content::animation_list::kAdult, sim.personData[kLifeStagePersonData]);
    auto named = [&](std::uint8_t list) -> std::optional<std::string> {
        const auto text = code_.string(kGlobalGroup, list, entry);
        if (!text || text->empty()) {
            return std::nullopt;
        }
        if (category != nullptr) {
            *category = content::animationCategory(*text);
        }
        return content::animationName(*text);
    };
    const auto& lengths = world().animationLengths();
    std::optional<std::string> name = named(ageList);
    if (ageList != content::animation_list::kAdult && (!name || (lengths && !lengths(*name)))) {
        if (auto adult = named(content::animation_list::kAdult); adult && (!lengths || lengths(*adult))) {
            return adult;
        }
    }
    return name;
}

// Animate Overlay (operands: see PlayingOverlay): lays the animation over the object the operands
// name and goes on at once. The entry is looked up for me (the caller), not that object. A Sim
// reads its age's list first, for the object list 0x86 the one Animate Object reads (see
// content::objectAnimationList), and the given list when that entry names no animation. Which
// Sim's age picks the list when it plays on another Sim is not known yet.
bool Execution::animateOverlay(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    SimObject& sim = me();
    const std::int16_t targetId = read(o[6], static_cast<std::uint16_t>(o[7] | o[8] << 8), f);
    SimObject* target = world().find(targetId);
    if (target == nullptr) {
        throw Stop{Outcome::Error, "could not find object reference", std::to_string(targetId)};
    }
    const auto entry = static_cast<std::uint16_t>(o[0] | o[1] << 8);
    const std::int16_t index = (o[2] & 0x04) ? slotAt(f.params, entry, "param") : static_cast<std::int16_t>(entry);
    const std::uint8_t list = o[9];
    std::optional<std::string> name;
    std::uint8_t category = content::animation_category::kNoCategory;
    // A clip laid on another Sim is named for that Sim: its age's list, and for social animations
    // that Sim and the one its interaction is with.
    const SimObject& named = target->isPerson() ? *target : sim;
    if (list == kSpecialAnimationList) {
        name = specialAnimationName(sim, static_cast<std::uint16_t>(index), &category);
    } else if (list == content::animation_list::kSocial && named.isPerson()) {
        name = socialAnimation(f, named, static_cast<std::uint16_t>(index));
    } else {
        const std::int16_t age = named.personData[kLifeStagePersonData];
        std::uint16_t ageList = list;
        if (named.isPerson()) {
            ageList = list == content::animation_list::kObject ? content::objectAnimationList(named.personData[kSpeciesPersonData], age)
                                                               : content::ageAnimationList(list, age);
        }
        name = animationEntry(f, ageList, static_cast<std::uint16_t>(index), ageList == list, &category);
        if (!name && ageList != list) {
            name = animationEntry(f, list, static_cast<std::uint16_t>(index), true, &category);
        }
    }
    if (!name) {
        throw Stop{Outcome::Error, "Animation name is empty string.", hex(list, 2) + " " + std::to_string(index)};
    }
    const auto speed = static_cast<std::int8_t>(o[3]);
    if (speed == 0) {
        throw Stop{Outcome::Error, "Animation speed cannot be zero", ""};
    }
    PlayingOverlay overlay;
    overlay.plays = (o[2] & 0x20) ? thread_.temps[1] : 1;
    if (overlay.plays == 0 || overlay.plays < -1) {
        throw Stop{Outcome::Error, "Animate Overlay play count out of range", std::to_string(overlay.plays)};
    }
    const float weight = static_cast<float>(static_cast<std::int8_t>(o[10])) / 100.0f;
    overlay.weight = weight < 0 || weight > 1 ? 1.0f : weight;
    overlay.priority = std::min<std::uint8_t>(o[11], 4);
    overlay.blendsIn = (o[2] & 0x80) == 0;
    overlay.blendsOut = (o[2] & 0x40) == 0;
    overlay.name = *name;
    overlay.startMs = world().eventClockMs();
    overlay.speed = static_cast<float>(speed) / 32.0f;
    overlay.flipped = (o[15] & 0x01) ? thread_.temps[3] != 0 : (o[2] & 0x01) != 0;
    overlay.category = entryCategory(overlay.name, category);
    overlay.eventTree = static_cast<std::uint16_t>(o[4] | o[5] << 8);
    overlay.codeOwner = f.codeOwner;
    overlay.stackObject = f.stackObjectId;
    if (o[15] & 0x08) {
        overlay.ikTarget = thread_.temps[6];
    }
    if (const auto& lengths = world().animationLengths()) {
        const auto ms = lengths(overlay.name);
        if (!ms) {
            // The game logs a missing animation and goes on, playing nothing.
            return true;
        }
        overlay.lengthMs = *ms / std::abs(overlay.speed) * std::max<std::int16_t>(overlay.plays, 1);
        // It fades as an Animate Sim clip does, the fades shrunk to fit only when it plays a set number
        // of times.
        const AnimateFades fades = overlay.plays < 0 ? AnimateFades{overlay.blendsIn ? kAnimateFadeMs : 0.0f, overlay.blendsOut ? kAnimateFadeMs : 0.0f}
                                                     : animateFades(overlay.lengthMs, overlay.blendsIn, overlay.blendsOut);
        overlay.blendInMs = fades.inMs;
        overlay.blendOutMs = fades.outMs;
    }
    layOverlay(*target, std::move(overlay));
    return true;
}

void Execution::layOverlay(SimObject& sim, PlayingOverlay overlay) {
    constexpr std::size_t kMaxOverlays = 8;
    if (sim.overlays.size() >= kMaxOverlays) {
        diag::warnOnce("animation: more than 8 overlays at once drops the oldest");
        sim.overlays.erase(sim.overlays.begin());
    }
    overlay.serial = 0;
    sim.overlays.push_back(std::move(overlay));
}

// Animate Object's entry text from the code of `owner`, empty when there is none. With a Sim, the
// list for its age and species (see content::objectAnimationList) is tried in the object's own code
// and then its semi-global code, a non-human young or elder Sim's also as an adult's. Last comes the
// object list 0x86 of both.
std::string Execution::objectAnimationText(std::uint32_t owner, const SimObject* sim, std::uint16_t entry) {
    constexpr std::int16_t kAdultAge = 50, kOldestYoung = 18;
    const std::uint32_t semi = code_.semiGlobalGroup(owner);
    const std::int16_t species = sim ? sim->personData[kSpeciesPersonData] : std::int16_t{0};
    const std::int16_t age = sim ? sim->personData[kLifeStagePersonData] : kAdultAge;
    const bool asAdultToo = sim != nullptr && species != 0 && (age > kAdultAge || age <= kOldestYoung);
    std::vector<std::pair<std::uint32_t, std::uint16_t>> tries;
    for (const std::uint32_t group : {owner, semi}) {
        if (sim != nullptr) {
            tries.emplace_back(group, content::objectAnimationList(species, age));
            if (asAdultToo) {
                tries.emplace_back(group, content::objectAnimationList(species, kAdultAge));
            }
        }
    }
    tries.emplace_back(owner, content::animation_list::kObject);
    tries.emplace_back(semi, content::animation_list::kObject);
    for (const auto& [group, list] : tries) {
        if (group == 0) {
            continue;
        }
        if (auto text = code_.string(group, list, entry); text && !text->empty()) {
            return *text;
        }
    }
    return {};
}

// Animate Object. Operands:
//   bytes 0-1  animation entry (byte 2 bit 0x04: the param it names)
//   byte 2     flags: 0x01 flipped, 0x02 speed from temp 2, 0x04 entry in a param, 0x20 play count
//              from temp 1, 0x40 no fade out, 0x80 no fade in
//   byte 3     speed, in 32nds (32 = as authored)
//   bytes 4-5  the tree told of the clip's events (0: none), from the running tree's code, run on
//              the object that plays it (see tellAnimateSimEvents)
//   byte 6, 7-8  the object that plays it (owner, data)
//   byte 10    more flags: 0x01 flipped if temp 3, 0x02 the entry is for the Sim in temp 5 (see
//              objectAnimationText), 0x20 the lists are the running tree's code, not the object's
// It plays once, or temp 1 times (-1: until Animate Stop, 0 or below -1 is an error), carries on at
// once and leaves the last pose. A missing clip plays nothing. Sims and other byte 10 flags show
// nothing yet (AN-23).
bool Execution::animateObject(const bhav::Instruction& in, Frame& f) {
    constexpr std::uint8_t kFlipFromTemp3 = 0x01, kForSimInTemp5 = 0x02, kRefresh = 0x10, kRunningTreeCode = 0x20,
                           kPlaysFromTemp1 = 0x20;
    constexpr std::uint8_t kKnownFlags = kFlipFromTemp3 | kForSimInTemp5 | kRefresh | kRunningTreeCode;
    const auto& o = in.operands;
    const std::int16_t targetId = read(o[6], static_cast<std::uint16_t>(o[7] | o[8] << 8), f);
    SimObject* target = world().find(targetId);
    if (target == nullptr) {
        throw Stop{Outcome::Error, "could not find object reference", std::to_string(targetId)};
    }
    if ((o[10] & ~kKnownFlags) != 0 || target->isPerson()) {
        return true;
    }
    const auto entry = static_cast<std::uint16_t>(o[0] | o[1] << 8);
    const std::int16_t index = (o[2] & 0x04) ? slotAt(f.params, entry, "param") : static_cast<std::int16_t>(entry);
    const SimObject* sim = nullptr;
    if (o[10] & kForSimInTemp5) {
        sim = world().find(thread_.temps[5]);
        if (sim == nullptr) {
            throw Stop{Outcome::Error, "could not find object reference", std::to_string(thread_.temps[5])};
        }
        if (!sim->isPerson()) {
            throw Stop{Outcome::Error, "Object referred to must be a person.", std::to_string(sim->id)};
        }
    }
    const std::string text = objectAnimationText((o[10] & kRunningTreeCode) ? f.codeOwner : target->group, sim,
                                                 static_cast<std::uint16_t>(index));
    if (content::animationCategory(text) == content::animation_category::kUnknownCategory) {
        throw Stop{Outcome::Error, "Invalid animation type specified in animation string", text};
    }
    const std::string name = content::animationName(text);
    if (name.empty()) {
        throw Stop{Outcome::Error, "Animation name is empty string.", std::to_string(index)};
    }
    const std::int16_t plays = (o[2] & kPlaysFromTemp1) ? thread_.temps[1] : std::int16_t{1};
    if (plays == 0 || plays < -1) {
        throw Stop{Outcome::Error, "Invalid number of loops", std::to_string(plays)};
    }
    const auto& lengths = world().animationLengths();
    if (lengths && !lengths(name)) {
        return true;
    }
    // Speed 0 holds the clip's first pose.
    const auto speed = (o[2] & 0x02) ? thread_.temps[2] : static_cast<std::int8_t>(o[3]);
    PlayingAnimation play;
    play.name = name;
    play.startMs = world().eventClockMs();
    play.speed = static_cast<float>(speed) / 32.0f;
    play.flipped = (o[10] & kFlipFromTemp3) ? thread_.temps[3] != 0 : (o[2] & 0x01) != 0;
    play.loops = plays != 1;
    play.blendsIn = (o[2] & 0x80) == 0;
    play.blendsOut = (o[2] & 0x40) == 0;
    // Counted plays end after the last one. One played until Animate Stop ends when that gives it the
    // time it played.
    play.lengthMs = std::numeric_limits<float>::infinity();
    if (plays > 0 && speed != 0 && lengths) {
        if (const auto length = lengths(name)) {
            play.lengthMs = static_cast<float>(plays) * *length / std::abs(play.speed);
        }
    }
    // It fades in and out as an Animate Sim clip does. One played until Animate Stop fades in once.
    const AnimateFades fades = plays < 0 ? AnimateFades{play.blendsIn ? kAnimateFadeMs : 0.0f, play.blendsOut ? kAnimateFadeMs : 0.0f}
                                         : animateFades(play.lengthMs, play.blendsIn, play.blendsOut);
    play.fades = true;
    play.fadeInMs = fades.inMs;
    play.fadeOutMs = fades.outMs;
    play.eventTree = static_cast<std::uint16_t>(o[4] | o[5] << 8);
    play.eventCodeOwner = f.codeOwner;
    play.eventStackObject = stackObject();
    play.tellsEnd = true;
    // The clip it replaces plays on under it until it ends or the new one crossfades it.
    if (target->animation) {
        target->ending = std::move(target->animation);
    }
    target->animation = std::move(play);
    return true;
}

// Animate Stop. Operands:
//   bytes 0-1  animation entry (byte 2 bit 0x04: the param it names), for stopping by name
//   byte 2     flags: 0x01 flipped, 0x02 blend out (233 ms), 0x20 short blend out (70 ms),
//              0x04 entry in a param, 0x08 flipped if temp 3, 0x40 flipped or not
//   byte 3, 4-5  the object whose animations stop (owner, data)
//   byte 6     list, for stopping by name (0x80 named as Animate Overlay names it)
//   byte 7     what stops: 0 by name, 1 overlays, 2 full-body animations, 3 all, 4-11 by category
//              (carry, idle, gesture, reaction, normal, facial, facial idle, receptivity, see
//              content::animationCategory and entryCategory)
//   byte 8     priority: 0 any, else layers 2p-2 and 2p-1
// Animations stop or start fading out at once, and the primitive carries on.
bool Execution::animateStop(const bhav::Instruction& in, Frame& f) {
    constexpr float kBlendMs = 233.33f, kShortBlendMs = 70.0f;
    const auto& o = in.operands;
    const std::int16_t targetId = read(o[3], static_cast<std::uint16_t>(o[4] | o[5] << 8), f);
    SimObject* target = world().find(targetId);
    if (target == nullptr) {
        throw Stop{Outcome::Error, "could not find object reference", std::to_string(targetId)};
    }
    const bool flipped = (o[2] & 0x08) ? thread_.temps[3] != 0 : (o[2] & 0x01) != 0;
    const bool eitherFlip = (o[2] & 0x40) != 0;
    const float blendMs = (o[2] & 0x20) ? kShortBlendMs : (o[2] & 0x02) ? kBlendMs : 0.0f;
    const std::uint8_t kind = o[7];
    const std::uint8_t priority = o[8];
    std::optional<std::string> name;
    if (kind == 0) {
        const auto entry = static_cast<std::uint16_t>(o[0] | o[1] << 8);
        const std::int16_t index = (o[2] & 0x04) ? slotAt(f.params, entry, "param") : static_cast<std::int16_t>(entry);
        const std::uint8_t list = o[6];
        if (list == kSpecialAnimationList) {
            // Named as Animate Overlay names it, for me. Only a Sim's animations stop this way.
            if (!target->isPerson()) {
                throw Stop{Outcome::Error, "not a person", "animation list 0x80"};
            }
            name = specialAnimationName(me(), static_cast<std::uint16_t>(index), nullptr);
        } else if (list == content::animation_list::kSocial && target->isPerson()) {
            // Named as the start names it, for the Sim whose clips stop and its interaction's partner.
            name = socialAnimation(f, *target, static_cast<std::uint16_t>(index));
        } else {
            const std::uint8_t ageList =
                target->isPerson() ? content::ageAnimationList(list, target->personData[kLifeStagePersonData]) : list;
            // As for the start, an age's entry naming a clip the game doesn't have falls back to the
            // adult list, so a teen stops the adult clip it was given.
            name = animationEntry(f, ageList, static_cast<std::uint16_t>(index), ageList == list);
            if (!name && ageList != list) {
                name = animationEntry(f, list, static_cast<std::uint16_t>(index), true);
            }
        }
        if (!name) {
            return true;
        }
    }
    namespace cat = content::animation_category;
    constexpr std::uint8_t kCategories[] = {cat::kCarry, cat::kIdle, cat::kGesture, cat::kReaction, cat::kNormal,
                                            cat::kFacial, cat::kFacialIdle, cat::kReceptivity};
    constexpr std::uint8_t kFirstCategoryKind = 4;
    if (kind >= kFirstCategoryKind + std::size(kCategories)) {
        return true;
    }
    const std::uint8_t wanted = kind >= kFirstCategoryKind ? kCategories[kind - kFirstCategoryKind] : 0;
    const auto matches = [&](const std::string& animation, bool animationFlipped, int layer, std::uint8_t category) {
        if (kind >= kFirstCategoryKind && category != wanted) {
            return false;
        }
        if (priority != 0 && (layer & ~1) != 2 * priority - 2) {
            return false;
        }
        if (!eitherFlip && animationFlipped != flipped) {
            return false;
        }
        return kind != 0 || std::ranges::equal(animation, *name, [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    };
    const double now = world().eventClockMs();
    if (kind == 0 || kind == 1 || kind == 3 || kind >= kFirstCategoryKind) {
        constexpr int kOverlayLayers[] = {1, 3, 5, 7, 0};
        for (auto it = target->overlays.begin(); it != target->overlays.end();) {
            if (it->stopped || !matches(it->name, it->flipped, kOverlayLayers[std::min<std::uint8_t>(it->priority, 4)], it->category)) {
                ++it;
                continue;
            }
            if (blendMs <= 0) {
                it = target->overlays.erase(it);
                continue;
            }
            stopOverlay(*it, now, blendMs);
            ++it;
        }
    }
    if ((kind == 0 || kind == 2 || kind == 3 || kind >= kFirstCategoryKind) && target->animation &&
        matches(target->animation->name, target->animation->flipped, target->animation->layer, target->animation->category)) {
        target->animation->lengthMs = static_cast<float>(std::max(0.0, now - target->animation->startMs));
        target->animation->endEventsTold = 2;
    }
    if ((kind == 0 || kind == 2 || kind == 3 || kind >= kFirstCategoryKind) && blendMs <= 0 && target->ending &&
        matches(target->ending->name, target->ending->flipped, target->ending->layer, target->ending->category)) {
        target->ending.reset();
    }
    return true;
}

// Reach/Put (Sims only). Operands:
//   byte 3, 4-5  the object (owner, data): what to grab, or a put's target
//   byte 6       a put's target slot, or with flag bit 0 my temp 0
//   byte 9       flags: bit 0 see above, bit 1 the hand from temp 3 (non-zero: hand 1), bit 0x10
//                the object's clip is "o-generic-resetToZero-pose" instead of bytes 13-14
//   byte 10      0 grab into my hand, 1 put what I hold into the target's slot, 2 put it on the floor
//   bytes 13-14  the entry of the moved object's animation list (0x86) it plays as it changes hands
// Grabbing what that hand holds is true at once, putting with an empty hand false. Otherwise I play
// STR# 0x92's clip for the height (floor, coffee table, table, counter), reach (over 0.8 tiles) and
// grab or put, the object changes hands at event 0 (see runHandOffTrees), and it is true when the
// clip ends. Bytes 11-12 name the grasp and carry poses laid over it (see playReachStages).
// Not yet: byte 9 bit 0x04 (where the object's clip is looked up first) and a floor put's placement
// test (PR-29).
Execution::ReachStep Execution::reachPut(const bhav::Instruction& in, Frame& f) {
    constexpr std::size_t kContainerField = 2, kSlotField = 3;
    constexpr std::uint8_t kReachList = 0x92;
    constexpr float kFarReach = 0.8f;
    SimObject& sim = me();
    if (!sim.isPerson()) {
        throw Stop{Outcome::Error, "Reach/Put on an object that is not a person", ""};
    }
    const std::size_t frame = thread_.frames.size() - 1;
    const double now = world().clockMs();
    if (sim.reaching && !(sim.reaching->frame == frame && sim.reaching->group == f.group && sim.reaching->tree == f.instance &&
                          sim.reaching->pc == f.pc)) {
        sim.reaching.reset();
    }
    if (sim.reaching) {
        Reaching& reach = *sim.reaching;
        if (!reach.transferred && now >= reach.transferMs) {
            reach.transferred = true;
            const Reaching handOff = reach;
            const bool moved = reachTransfer(sim, handOff);
            runHandOffTrees(handOff, moved);
            if (!moved) {
                sim.reaching.reset();
                return ReachStep{ReachStep::Kind::Done, false};
            }
            playHandOffClip(handOff);
            if (!sim.reaching) {
                return ReachStep{ReachStep::Kind::Done, true};
            }
        }
        if (now < reach.endMs) {
            return ReachStep{ReachStep::Kind::Wait};
        }
        sim.reaching.reset();
        return ReachStep{ReachStep::Kind::Done, true};
    }
    const auto& o = in.operands;
    const std::uint8_t flags = o[9];
    const auto mode = static_cast<Reaching::Mode>(o[10] <= 2 ? o[10] : 0);
    const std::int16_t hand = (flags & 0x02) ? (thread_.temps[3] != 0 ? 1 : 0) : 0;
    const std::int16_t id = read(o[3], static_cast<std::uint16_t>(o[4] | o[5] << 8), f);
    SimObject* target = world().find(id);
    if (mode != Reaching::Mode::PutOnFloor && (target == nullptr || target->removed)) {
        throw Stop{Outcome::Error, "could not find object reference", std::to_string(id)};
    }
    if (static_cast<std::size_t>(hand) >= sim.slots.size()) {
        throw Stop{Outcome::Error, "the Sim has no such hand slot", std::to_string(hand)};
    }
    Reaching reach{frame, f.group, f.instance, f.pc, mode};
    reach.stackObject = f.stackObjectId;
    // The reach is measured from me to where the object is, or to the slot it goes in: a glass on a
    // tray on a bar is as high as the bar's slot holds the tray. Where the game measures a floor put
    // to is not known yet, so it is measured to my own place.
    const HeldPlace mine = heldPlace(world(), code_, sim);
    HeldPlace there = mine;
    std::uint32_t clipGroup = kGlobalGroup;
    if (mode == Reaching::Mode::Grab) {
        if (target->data[kContainerField] == sim.id && target->data[kSlotField] == hand) {
            return ReachStep{ReachStep::Kind::Done, true};
        }
        reach.object = target->id;
        reach.slot = hand;
        reach.source = std::max<std::int16_t>(target->data[kContainerField], 0);
        there = heldPlace(world(), code_, *target);
        clipGroup = target->group;
    } else {
        reach.object = sim.slots[static_cast<std::size_t>(hand)];
        if (reach.object == 0 || world().find(reach.object) == nullptr) {
            return ReachStep{ReachStep::Kind::Done, false};
        }
        if (mode == Reaching::Mode::Put) {
            reach.container = target->id;
            reach.slot = (flags & 0x01) ? thread_.temps[0] : static_cast<std::int16_t>(o[6]);
            if (reach.slot < 0 || static_cast<std::size_t>(reach.slot) >= target->slots.size()) {
                throw Stop{Outcome::Error, "bad slot number", std::to_string(reach.slot)};
            }
            there = slotPlace(world(), code_, *target, reach.slot);
            clipGroup = target->group;
        }
    }
    const float height = there.height - mine.height;
    const float distance = std::hypot(static_cast<float>(there.x - mine.x) / 65536.0f, static_cast<float>(there.y - mine.y) / 65536.0f);
    const int band = height < 0.4f ? 0 : height < 0.7f ? 1 : height < 0.9f ? 2 : 3;
    const auto entry = static_cast<std::uint16_t>(band + (distance > kFarReach ? 4 : 0) + (mode != Reaching::Mode::Grab ? 8 : 0));
    std::optional<std::string> clip;
    for (const auto group : {clipGroup, code_.semiGlobalGroup(clipGroup), kGlobalGroup}) {
        if (const auto text = code_.string(group, kReachList, entry); text && !text->empty()) {
            clip = content::animationName(*text);
            break;
        }
    }
    const auto& lengths = world().animationLengths();
    const auto& events = world().animationEventTime();
    if (clip) {
        clip = childClip(sim, *clip);
    }
    const auto length = clip && lengths ? lengths(*clip) : std::nullopt;
    if (!length) {
        throw Stop{Outcome::Error, "Missing reach animation", clip.value_or(std::to_string(entry))};
    }
    const auto event = events ? events(*clip, 0) : std::nullopt;
    if (!event) {
        throw Stop{Outcome::Error, "Reach animation does not contain event 0", *clip};
    }
    reach.transferMs = now + *event;
    reach.endMs = now + *length;
    // The object's clip is named now and played at the hand-off: a tray set down drops its carry pose.
    constexpr std::uint8_t kResetClipFlag = 0x10;
    if (const SimObject* moved = world().find(reach.object); moved != nullptr) {
        const std::string text = (flags & kResetClipFlag) ? std::string("o-generic-resettozero-pose")
                                                          : objectAnimationText(moved->group, nullptr, static_cast<std::uint16_t>(o[13] | o[14] << 8));
        reach.objectClip = content::animationName(text);
    }
    // It plays as a Sim animation of medium priority, fading in and out over 233.33 ms each, or over
    // half its length each when it is shorter than both.
    constexpr float kReachFadeMs = 233.33f;
    constexpr int kReachLayer = 2;
    PlayingAnimation play;
    play.name = *clip;
    play.startMs = now;
    play.lengthMs = *length;
    play.layer = kReachLayer;
    play.fades = true;
    play.fadeInMs = play.fadeOutMs = std::min(kReachFadeMs, *length / 2);
    play.frame = frame;
    play.group = f.group;
    play.tree = f.instance;
    play.pc = f.pc;
    // The hand reaches for the object grabbed, or for where the held one goes: the target's slot,
    // or the floor spot reachTransfer puts it on. The hand's target doesn't turn with that spot. It
    // faces the way I face now, turned by the reach clip's root turn (see PlayingAnimation::IkObject).
    PlayingAnimation::IkObject ik;
    ik.heading = sim.heading ? *sim.heading : compassHeading(sim.data[kDirectionField]);
    ik.level = static_cast<std::int16_t>(sim.level);
    ik.height = there.height;
    if (mode == Reaching::Mode::PutOnFloor) {
        const FloorSpot spot = floorSpot(sim, *world().find(reach.object));
        ik.x = spot.x;
        ik.y = spot.y;
    } else {
        ik.x = static_cast<float>(there.x) / 65536.0f;
        ik.y = static_cast<float>(there.y) / 65536.0f;
        ik.level = static_cast<std::int16_t>(target->level);
    }
    play.ikObject = ik;
    sim.animation = std::move(play);
    sim.reaching = reach;
    const SimObject* named = mode == Reaching::Mode::Grab ? target : world().find(reach.object);
    if (named != nullptr) {
        playReachStages(sim, *named, mode == Reaching::Mode::Grab, hand != 0, static_cast<std::uint16_t>(o[11] | o[12] << 8), *length, *event, now);
    }
    return ReachStep{ReachStep::Kind::Wait};
}

// Right after the hand-off at the reach clip's event 0, worked or not, the game runs two trees by
// name at once on my stack, each in its own object's code with that object as stack object. First
// "CT - Reach - Events" of the container (the one grabbed from or put into, none on the floor),
// then the moved object's "CT - Grab" after a grab that worked, else its "CT - Put". Each gets the
// event (0) and the stack object of the tree that ran Reach/Put. A name the code doesn't have runs
// nothing. This is how a serving bowl's hidden portion shows as a Sim takes it.
// Not known yet: params 2-4 (no shipped tree of these names reads them, so they stay 0) and what
// the game does with a tree that waits or fails (the result is not used).
void Execution::runHandOffTrees(const Reaching& reach, bool moved) {
    constexpr std::size_t kSavedTemps = 8;
    const std::int16_t container = reach.mode == Reaching::Mode::Grab ? reach.source : reach.container;
    const std::array<std::int16_t, 5> args{0, reach.stackObject, 0, 0, 0};
    const auto run = [&](std::int16_t id, const char* name) {
        const SimObject* owner = id > 0 ? world().find(id) : nullptr;
        if (owner == nullptr || owner->removed) {
            return;
        }
        const std::uint32_t group = owner->group;
        for (const std::uint32_t inGroup : {group, code_.semiGlobalGroup(group), kGlobalGroup}) {
            const auto tree = inGroup != 0 ? code_.behaviorByName(inGroup, name) : std::nullopt;
            if (!tree) {
                continue;
            }
            // The game keeps my first eight temps across the run.
            std::array<std::int16_t, kSavedTemps> temps{};
            std::copy_n(thread_.temps.begin(), kSavedTemps, temps.begin());
            const RunResult r = runOnMe(group, *tree, id, args);
            std::copy_n(temps.begin(), kSavedTemps, thread_.temps.begin());
            if (r.outcome == Outcome::Unsupported || r.outcome == Outcome::MissingCode) {
                throw Stop{r.outcome, std::string(name) + ": " + r.reason, r.detail};
            }
            return;
        }
    };
    run(container, "CT - Reach - Events");
    run(reach.object, moved && reach.mode == Reaching::Mode::Grab ? "CT - Grab" : "CT - Put");
}

// The clip a reach names for the object it moves starts on that object as it changes hands and
// replaces the object's own clip. How the game blends it is not known yet, so it plays once with
// Animate Object's usual fades. A clip the game doesn't have plays nothing.
void Execution::playHandOffClip(const Reaching& reach) {
    SimObject* object = world().find(reach.object);
    const auto& lengths = world().animationLengths();
    const auto length = lengths && !reach.objectClip.empty() ? lengths(reach.objectClip) : std::nullopt;
    if (object == nullptr || !length) {
        return;
    }
    PlayingAnimation play;
    play.name = reach.objectClip;
    play.startMs = world().eventClockMs();
    play.speed = 1;
    play.lengthMs = *length;
    const AnimateFades fades = animateFades(play.lengthMs, play.blendsIn, play.blendsOut);
    play.fades = true;
    play.fadeInMs = fades.inMs;
    play.fadeOutMs = fades.outMs;
    if (object->animation) {
        object->ending = std::move(object->animation);
    }
    object->animation = std::move(play);
}

// Children take the children's clips ("c2o-" for "a2o-", assumed from the game's age prefixes).
std::string Execution::childClip(const SimObject& sim, const std::string& clip) {
    const std::int16_t stage = sim.personData[kLifeStagePersonData];
    const auto& lengths = world().animationLengths();
    if (stage >= 3 && stage <= 15 && clip.rfind("a2o-", 0) == 0 && lengths && lengths("c" + clip.substr(1))) {
        return "c" + clip.substr(1);
    }
    return clip;
}

// The overlays Reach/Put lays over its reach clip (`length` ms, event at `event` ms), from the
// object's STR# 0x81 entry bytes 11-12 pick: "grasp,carry". A grab fades the grasp in towards the
// event, then crossfades it into the carry pose, which stays until Animate Stop or a put. A put plays
// the grasp around the event and fades out that hand's carry poses. The second hand is mirrored.
// Not yet: the grasp's "-iktransjig" clip, whose timing hangs on a value not known yet.
void Execution::playReachStages(SimObject& sim, const SimObject& object, bool grab, bool flipped, std::uint16_t entry, float length,
                     float event, double now) {
    constexpr std::uint8_t kStageList = 0x81;
    constexpr float kBlendMs = 233.33f;
    namespace cat = content::animation_category;
    if (!grab) {
        for (auto& overlay : sim.overlays) {
            if (!overlay.stopped && overlay.category == cat::kCarry && overlay.flipped == flipped) {
                stopOverlay(overlay, now, kBlendMs);
            }
        }
    }
    std::optional<std::string> text;
    const std::uint8_t ageList = content::ageAnimationList(kStageList, sim.personData[kLifeStagePersonData]);
    for (const auto group : {object.group, code_.semiGlobalGroup(object.group)}) {
        for (const auto list : {ageList, kStageList}) {
            if (!text || text->empty()) {
                text = code_.string(group, list, entry);
            }
        }
    }
    if (!text || text->empty()) {
        return;
    }
    const auto comma = text->find(',');
    const std::string first = text->substr(0, comma);
    const std::string second = comma == std::string::npos ? std::string() : text->substr(comma + 1);
    const auto& lengths = world().animationLengths();
    const auto lay = [&](const std::string& entryText, std::uint8_t category, std::uint8_t priority, float startMs, float inMs,
                         float outMs, std::optional<float> envelopeMs) {
        const std::string name = childClip(sim, content::animationName(entryText));
        if (name.empty() || (lengths && !lengths(name))) {
            return;  // the game skips a stage whose clip is missing
        }
        PlayingOverlay overlay;
        overlay.name = name;
        overlay.startMs = now + startMs;
        overlay.plays = envelopeMs ? 1 : -1;
        overlay.lengthMs = envelopeMs.value_or(0.0f);
        overlay.priority = priority;
        overlay.flipped = flipped;
        overlay.category = entryCategory(name, category);
        overlay.blendInMs = std::max(0.0f, inMs);
        overlay.blendOutMs = std::max(0.0f, outMs);
        overlay.blendsOut = envelopeMs.has_value();
        layOverlay(sim, std::move(overlay));
    };
    const std::uint8_t graspCategory = content::animationCategory(first);
    if (grab) {
        // The grasp from 0.6 of the way to the event until the carry has taken over.
        const bool carries = !second.empty();
        const float start = 0.6f * event, in = 0.4f * event - 50.0f;
        const float envelope = length - start - (carries ? kBlendMs : 0.0f);
        const float out = in + kBlendMs + 50.0f > envelope ? envelope - in - 25.0f : kBlendMs;
        if (envelope > 0) {
            lay(first, graspCategory, 1, start, in, out, envelope);
        }
        if (carries) {
            const std::uint8_t tagged = content::animationCategory(second);
            lay(second, tagged == cat::kNoCategory ? cat::kCarry : tagged, 2, length - 2 * kBlendMs, kBlendMs, 0, std::nullopt);
        }
    } else {
        // The grasp from the start until a while after the event.
        float out = 0.23f * (length - event), in = kBlendMs;
        const float envelope = event + out;
        if (out + kBlendMs > envelope) {
            in = out = envelope / 2;
        }
        if (envelope > 0) {
            lay(first, graspCategory, 1, 0, in, out, envelope);
        }
    }
}

// Where Reach/Put puts a held object on the floor: the middle of the tile 0.7 tiles in front of
// the Sim, facing back at it. Facing rule 0x17 n above 0 turns it one step on when the direction
// isn't a multiple of n.
Execution::FloorSpot Execution::floorSpot(const SimObject& sim, const SimObject& object) {
    const float heading = sim.heading ? *sim.heading : compassHeading(sim.data[kDirectionField]);
    FloorSpot spot;
    spot.x = std::floor(static_cast<float>(sim.x) / 65536.0f + 0.7f * std::cos(heading)) + 0.5f;
    spot.y = std::floor(static_cast<float>(sim.y) / 65536.0f - 0.7f * std::sin(heading)) + 0.5f;
    int facing = compassDirection(heading) - 4;
    const std::int16_t rule = object.data[kFacingRuleField];
    if (rule != 0 && facing % rule != 0) {
        ++facing;
    }
    spot.facing = static_cast<std::int16_t>(facing & 7);
    return spot;
}

// The object changes hands at the reach clip's event 0. False when it can't (it is gone, or
// the slot it should go into is taken).
bool Execution::reachTransfer(SimObject& sim, const Reaching& reach) {
    SimObject* object = world().find(reach.object);
    if (object == nullptr || object->removed) {
        return false;
    }
    if (reach.mode == Reaching::Mode::PutOnFloor) {
        // Only that one spot is tried. When the placement test fails, the object stays in hand.
        const FloorSpot spot = floorSpot(sim, *object);
        const float x = spot.x, y = spot.y;
        if (!(x >= 0 && y >= 0 && x < 256 && y < 256)) {
            return false;
        }
        if (const auto& fits = world().objectFits(); fits && !fits(*object, x, y, sim.level, spot.facing)) {
            return false;
        }
        takeOutOfSlot(*object);
        object->data[kDirectionField] = spot.facing;
        object->x = static_cast<std::int32_t>(std::lround(x * 65536.0f));
        object->y = static_cast<std::int32_t>(std::lround(y * 65536.0f));
        object->level = sim.level;
        object->tile = Tile{static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y), static_cast<std::uint8_t>(sim.level)};
        world().updateRoom(*object);
        world().markFootprintChanged(object->id);
        return true;
    }
    SimObject* container = reach.mode == Reaching::Mode::Grab ? &sim : world().find(reach.container);
    if (container == nullptr || container->removed || static_cast<std::size_t>(reach.slot) >= container->slots.size()) {
        return false;
    }
    const std::int16_t held = container->slots[static_cast<std::size_t>(reach.slot)];
    if (held != 0 && held != object->id) {
        return false;
    }
    putInSlot(*object, *container, reach.slot);
    return true;
}

// Look At (0x6E) turns the Sim's head toward an object, a slot of it or the camera, or with STOP
// turns it back. It carries on at once (see LookController). STOP never reads its target, so a
// STOP may name a variable that doesn't exist, as the chessboard's Practice/Join does with local 3
// in a tree of 2 locals.
bool Execution::lookAt(const bhav::Instruction& in, Frame& f) {
    constexpr float kRadiansPerDegree = 3.14159265358979f / 180;
    // What the latest looks look at, enough for the ones the controller may still tell of.
    constexpr std::size_t kKeptLooks = 8;
    const LookAtOperands o = lookAtOperands(in.operands);
    SimObject& sim = me();
    const bool camera = (o.flags & 0x80) != 0;
    std::int16_t target = -1;
    if (!camera && !(o.flags & 0x01)) {
        target = read(o.owner, o.data, f);
        if (world().find(target) == nullptr) {
            throw Stop{Outcome::Error, "could not find object reference", std::to_string(target)};
        }
    }
    const float towards = (o.flags & 0x08) ? static_cast<float>(thread_.temps[1]) : static_cast<float>(o.towards) * 2;
    if (!(towards > 0)) {
        throw Stop{Outcome::Error, "Look At turn speed must be above 0", std::to_string(towards)};
    }
    const float away = (o.flags2 & 0x02)   ? static_cast<float>(thread_.temps[1])
                       : (o.flags2 & 0x01) ? static_cast<float>(thread_.temps[2])
                                           : static_cast<float>(o.away) * 2;
    if (!(away > 0)) {
        throw Stop{Outcome::Error, "Look At turn speed must be above 0", std::to_string(away)};
    }
    if (!sim.isPerson()) {
        throw Stop{Outcome::Error, "not a person", "Look At"};
    }
    const double now = world().clockMs();
    if (o.flags & 0x01) {
        sim.look.stop(now, away * kRadiansPerDegree);
        return true;
    }
    LookRequest r;
    r.id = ++sim.looksStarted;
    r.startMs = now;
    r.towards = towards * kRadiansPerDegree;
    r.away = away * kRadiansPerDegree;
    const std::int16_t duration = (o.flags & 0x10) ? thread_.temps[0] : o.duration;
    r.forever = duration == -1;
    r.remainingMs = r.forever ? 0.0f : static_cast<float>(duration);
    r.noEarlyExit = (o.flags & 0x02) != 0;
    sim.look.begin(r);
    sim.lookTargets.push_back({r.id, target, o.pointKind, o.slot, (o.flags & 0x20) == 0, o.eventTree, f.codeOwner, f.stackObjectId});
    if (sim.lookTargets.size() > kKeptLooks) {
        sim.lookTargets.erase(sim.lookTargets.begin());
    }
    sim.personData[kLookTargetPersonData] = target;
    sim.personData[kLookPointPersonData] = static_cast<std::int16_t>(o.slot | o.pointKind << 12);
    sim.personData[kLookStatePersonData] = static_cast<std::int16_t>(kLookTurningTo);
    sim.personData[kLookHoldPersonData] = duration;
    return true;
}

} // namespace ts2::sim::vm_detail
