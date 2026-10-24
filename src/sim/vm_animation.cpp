#include "sim/vm_execution.hpp"

#include <limits>

namespace ts2::sim::vm_detail {

// The animation entry [entry] of string list [list] names, from the frame's code owner, its
// semi-global code or the global code, the first that has the entry, nullopt when that entry is
// empty or (unless `orMissing`) names an animation the game doesn't have.
std::optional<std::string> Execution::animationEntry(const Frame& f, std::uint8_t list, std::uint16_t entry, bool orMissing,
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
// time, until it has played (by the world's clock), then is true. The animation is entry [entry]
// of my age's string list for [list] (see content::ageAnimationList), else of [list] itself.
// List 0x80 names the clip as Animate Overlay does (specialAnimationName), list 0x83 by the social prefixes.
// The clip's SimAntics events run the event tree in bytes 4-5 (see tellAnimateSimEvents).
// Not yet: blending, flipping, IK objects.
std::optional<bool> Execution::animateSim(const bhav::Instruction& in, Frame& f) {
    SimObject& sim = me();
    const std::size_t frame = thread_.frames.size() - 1;
    const double now = world().clockMs();
    if (sim.animation && sim.animation->frame == frame && sim.animation->group == f.group && sim.animation->tree == f.instance &&
        sim.animation->pc == f.pc) {
        if (!sim.animation->finished(now)) {
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
        name = animationEntry(f, ageList, static_cast<std::uint16_t>(entry), false, &category);
        if (!name && ageList != a.list) {
            name = animationEntry(f, a.list, static_cast<std::uint16_t>(entry), false, &category);
        }
    }
    if (!name) {
        throw Stop{Outcome::Error, "Animation name is empty string.", hex(a.list, 2) + " " + std::to_string(entry)};
    }
    const std::int16_t speed = a.speedFromTemp ? thread_.temps[2] : a.speed;
    if (speed == 0) {
        throw Stop{Outcome::Error, "Animation speed cannot be zero", ""};
    }
    PlayingAnimation play;
    play.name = *name;
    play.startMs = now;
    play.speed = static_cast<float>(speed) / 32.0f;
    play.flipped = a.flipFromTemp ? thread_.temps[3] != 0 : a.flipped;
    play.blendsOut = (in.operands[2] & 0x40) == 0;
    play.layer = 2 * a.priority;
    play.category = entryCategory(play.name, category);
    play.frame = frame;
    play.group = f.group;
    play.tree = f.instance;
    play.pc = f.pc;
    play.eventTree = a.eventTree;
    play.eventCodeOwner = f.codeOwner;
    play.eventStackObject = stackObject();
    if (const auto& lengths = world().animationLengths()) {
        if (const auto ms = lengths(play.name)) {
            play.lengthMs = *ms / std::abs(play.speed);
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

// Animate Overlay (operands: see PlayingOverlay): lays the animation over me and goes on at once.
bool Execution::animateOverlay(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    SimObject& sim = me();
    const auto entry = static_cast<std::uint16_t>(o[0] | o[1] << 8);
    const std::int16_t index = (o[2] & 0x04) ? slotAt(f.params, entry, "param") : static_cast<std::int16_t>(entry);
    const std::uint8_t list = o[9];
    std::optional<std::string> name;
    std::uint8_t category = content::animation_category::kNoCategory;
    if (list == kSpecialAnimationList) {
        name = specialAnimationName(sim, static_cast<std::uint16_t>(index), &category);
    } else {
        const std::uint8_t ageList = content::ageAnimationList(list, sim.personData[kLifeStagePersonData]);
        name = animationEntry(f, ageList, static_cast<std::uint16_t>(index), true, &category);
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
    overlay.flipped = (o[2] & 0x01) != 0;
    overlay.category = entryCategory(overlay.name, category);
    overlay.eventTree = static_cast<std::uint16_t>(o[4] | o[5] << 8);
    overlay.codeOwner = f.codeOwner;
    overlay.stackObject = f.stackObjectId;
    if (const auto& lengths = world().animationLengths()) {
        const auto ms = lengths(overlay.name);
        if (!ms) {
            // The game logs a missing animation and goes on, playing nothing.
            return true;
        }
        overlay.lengthMs = *ms / std::abs(overlay.speed) * std::max<std::int16_t>(overlay.plays, 1);
    }
    layOverlay(sim, std::move(overlay));
    return true;
}

void Execution::layOverlay(SimObject& sim, PlayingOverlay overlay) {
    constexpr std::size_t kMaxOverlays = 8;
    if (sim.overlays.size() >= kMaxOverlays) {
        diag::warnOnce("animation: more than 8 overlays at once drops the oldest");
        sim.overlays.erase(sim.overlays.begin());
    }
    sim.overlays.push_back(std::move(overlay));
}

// Animate Object. Operands:
//   bytes 0-1  animation entry of the object list 0x86 (byte 2 bit 0x04: the param it names)
//   byte 2     flags: 0x02 speed from temp 2, 0x04 entry in a param
//   byte 3     speed, in 32nds (32 = as authored)
//   byte 6, 7-8  the object that plays it (owner, data)
//   byte 9     2: plays until stopped (the plumbob's spin, water)
//   byte 10    more flags (0x02 and 0x20 pick other sources, not read yet)
// The animation starts and the primitive carries on at once. Only the clips that play until stopped
// are kept, with plain sources. What the other values of byte 9 do is not known yet (AN-23), so
// those calls show nothing, as before.
bool Execution::animateObject(const bhav::Instruction& in, Frame& f) {
    constexpr std::uint8_t kObjectAnimations = 0x86, kUntilStopped = 2, kOtherSources = 0x22;
    const auto& o = in.operands;
    const std::int16_t targetId = read(o[6], static_cast<std::uint16_t>(o[7] | o[8] << 8), f);
    SimObject* target = world().find(targetId);
    if (target == nullptr) {
        throw Stop{Outcome::Error, "could not find object reference", std::to_string(targetId)};
    }
    if (o[9] != kUntilStopped || (o[10] & kOtherSources) != 0 || target->isPerson()) {
        return true;
    }
    const auto entry = static_cast<std::uint16_t>(o[0] | o[1] << 8);
    const std::int16_t index = (o[2] & 0x04) ? slotAt(f.params, entry, "param") : static_cast<std::int16_t>(entry);
    const auto name = animationEntry(f, kObjectAnimations, static_cast<std::uint16_t>(index), false);
    if (!name) {
        // The game logs a missing animation and goes on, playing nothing.
        return true;
    }
    const auto speed = (o[2] & 0x02) ? thread_.temps[2] : static_cast<std::int8_t>(o[3]);
    if (speed == 0) {
        throw Stop{Outcome::Error, "Animation speed cannot be zero", ""};
    }
    PlayingAnimation play;
    play.name = *name;
    play.startMs = world().eventClockMs();
    play.speed = static_cast<float>(speed) / 32.0f;
    play.loops = true;
    // Until Animate Stop gives it the time it played.
    play.lengthMs = std::numeric_limits<float>::infinity();
    target->animation = std::move(play);
    return true;
}

// Animate Stop. Operands:
//   bytes 0-1  animation entry (byte 2 bit 0x04: the param it names), for stopping by name
//   byte 2     flags: 0x01 flipped, 0x02 blend out (233 ms), 0x20 short blend out (70 ms),
//              0x04 entry in a param, 0x08 flipped if temp 3, 0x40 flipped or not
//   byte 3, 4-5  the object whose animations stop (owner, data)
//   byte 6     list, for stopping by name
//   byte 7     what stops: 0 by name, 1 overlays, 2 full-body animations, 3 all, 4-11 by category
//              (carry, idle, gesture, reaction, normal, facial, facial idle, receptivity, see
//              content::animationCategory). An entry without a kind is a carry pose or a normal
//              animation by its clip, see entryCategory.
//   byte 8     priority: 0 any, else layers 2p-2 and 2p-1
// Animations stop at once (or start fading out) and the primitive carries on.
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
        const std::uint8_t ageList = target->isPerson() ? content::ageAnimationList(list, target->personData[kLifeStagePersonData]) : list;
        name = animationEntry(f, ageList, static_cast<std::uint16_t>(index), true);
        if (!name && ageList != list) {
            name = animationEntry(f, list, static_cast<std::uint16_t>(index), true);
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
            it->stopped = true;
            it->plays = 1;
            it->lengthMs = static_cast<float>(std::max(0.0, now - it->startMs) + blendMs);
            it->blendOutMs = blendMs;
            ++it;
        }
    }
    if ((kind == 0 || kind == 2 || kind == 3 || kind >= kFirstCategoryKind) && target->animation &&
        matches(target->animation->name, target->animation->flipped, target->animation->layer, target->animation->category)) {
        target->animation->lengthMs = static_cast<float>(std::max(0.0, now - target->animation->startMs));
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
//   byte 9       flags: bit 0 see above, bit 1 the hand from temp 3 (non-zero: hand 1)
//   byte 10      0 grab into my hand, 1 put what I hold into the target's slot, 2 put it on the floor
// Grabbing what I already hold in that hand is true at once. Putting with an empty hand is false.
// Otherwise I play the reach clip of STR# 0x92 for the height (floor, coffee table, table,
// counter), reach (more than 0.8 tiles away) and grab or put, and the object changes hands at
// its event 0. The primitive waits and is true when the clip ends. Bytes 11-12 name the grasp and
// carry poses laid over it (see playReachStages). Not yet: the object's animation of bytes 13-14,
// the trees the game runs on the events, and the placement test of a floor put (PR-29).
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
            if (!reachTransfer(sim, reach)) {
                sim.reaching.reset();
                return ReachStep{ReachStep::Kind::Done, false};
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
    float height = 0;
    float x = static_cast<float>(sim.x) / 65536.0f, y = static_cast<float>(sim.y) / 65536.0f;
    const auto slotHeight = [&](const SimObject& container, std::int16_t slot) {
        const ObjectType* type = code_.objectType(container.definition.guid());
        return type && slot >= 0 && static_cast<std::size_t>(slot) < type->containerSlotHeights.size()
                   ? type->containerSlotHeights[static_cast<std::size_t>(slot)] : 0.0f;
    };
    std::uint32_t clipGroup = kGlobalGroup;
    if (mode == Reaching::Mode::Grab) {
        if (target->data[kContainerField] == sim.id && target->data[kSlotField] == hand) {
            return ReachStep{ReachStep::Kind::Done, true};
        }
        reach.object = target->id;
        reach.slot = hand;
        if (const SimObject* under = target->data[kContainerField] > 0 ? world().find(target->data[kContainerField]) : nullptr) {
            height = slotHeight(*under, target->data[kSlotField]);
        }
        x = static_cast<float>(target->x) / 65536.0f;
        y = static_cast<float>(target->y) / 65536.0f;
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
            height = slotHeight(*target, reach.slot);
            x = static_cast<float>(target->x) / 65536.0f;
            y = static_cast<float>(target->y) / 65536.0f;
            clipGroup = target->group;
        }
    }
    const float distance = std::hypot(x - static_cast<float>(sim.x) / 65536.0f, y - static_cast<float>(sim.y) / 65536.0f);
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
    PlayingAnimation play;
    play.name = *clip;
    play.startMs = now;
    play.lengthMs = *length;
    play.frame = frame;
    play.group = f.group;
    play.tree = f.instance;
    play.pc = f.pc;
    // The hand reaches for the object grabbed, or for where the held one goes: the target's slot,
    // or the floor spot reachTransfer puts it on, facing as it will.
    PlayingAnimation::IkObject ik;
    ik.level = static_cast<std::int16_t>(sim.level);
    ik.height = height;
    if (mode == Reaching::Mode::PutOnFloor) {
        const FloorSpot spot = floorSpot(sim, *world().find(reach.object));
        ik.x = spot.x;
        ik.y = spot.y;
        ik.heading = compassHeading(spot.facing);
    } else {
        ik.x = x;
        ik.y = y;
        ik.level = static_cast<std::int16_t>(target->level);
        ik.heading = compassHeading(target->data[kDirectionField]);
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

// Children take the children's clips ("c2o-" for "a2o-", assumed from the game's age prefixes).
std::string Execution::childClip(const SimObject& sim, const std::string& clip) {
    const std::int16_t stage = sim.personData[kLifeStagePersonData];
    const auto& lengths = world().animationLengths();
    if (stage >= 3 && stage <= 15 && clip.rfind("a2o-", 0) == 0 && lengths && lengths("c" + clip.substr(1))) {
        return "c" + clip.substr(1);
    }
    return clip;
}

// The overlays Reach/Put lays over its reach clip (`length` ms, its event at `event` ms), named by
// the entry of the object's STR# 0x81 that bytes 11-12 pick: "grasp,carry". A grab fades the grasp
// in towards the event and, as the reach clip ends, crossfades it into the carry pose, which stays
// until something stops it (Animate Stop of carry poses, or a put). A put plays the grasp around
// the event and fades out every carry pose of that hand. Both are mirrored for the second hand.
// Not yet: the grasp's "-iktransjig" clip the game also plays around the event, because its timing
// depends on a value of that clip not known yet.
void Execution::playReachStages(SimObject& sim, const SimObject& object, bool grab, bool flipped, std::uint16_t entry, float length,
                     float event, double now) {
    constexpr std::uint8_t kStageList = 0x81;
    constexpr float kBlendMs = 233.33f;
    namespace cat = content::animation_category;
    if (!grab) {
        for (auto& overlay : sim.overlays) {
            if (!overlay.stopped && overlay.category == cat::kCarry && overlay.flipped == flipped) {
                overlay.stopped = true;
                overlay.plays = 1;
                overlay.lengthMs = static_cast<float>(std::max(0.0, now - overlay.startMs) + kBlendMs);
                overlay.blendOutMs = kBlendMs;
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

} // namespace ts2::sim::vm_detail
