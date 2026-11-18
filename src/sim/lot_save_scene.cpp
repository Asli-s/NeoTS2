#include "sim/lot_save_detail.hpp"
#include "sim/turn.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <optional>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ts2::sim {

using namespace lot_save_detail;

namespace {

// Whether the entry holds the clip that gives a Sim its pose, which its type names ("sil" low,
// "sim" medium, "sih" high, "siu" underlay).
bool isSimClip(const cobject::Animation& animation) {
    return animation.kind == cobject::Animation::Kind::Clip && animation.request.type.size() == 3 &&
           animation.request.type[0] == 's' && animation.request.type[1] == 'i';
}

// An animation to build another from, since most of what an entry holds is not known yet. One of
// the same clip is best, then one of the same type, then any clip, then anything else the object
// has, so that what it holds of its own is used before another object's.
const cobject::Animation* templateFor(const std::vector<cobject::Animation>& animations, const std::string& name,
                                      const std::string& type, const cobject::Animation* otherwise) {
    const cobject::Animation* any = nullptr;
    const cobject::Animation* anyKind = nullptr;
    const cobject::Animation* sameType = nullptr;
    for (const cobject::Animation& a : animations) {
        if (a.kind != cobject::Animation::Kind::Clip) {
            if (anyKind == nullptr) {
                anyKind = &a;
            }
            continue;
        }
        if (a.request.name == name) {
            return &a;
        }
        if (sameType == nullptr && a.request.type == type) {
            sameType = &a;
        }
        if (any == nullptr) {
            any = &a;
        }
    }
    if (sameType != nullptr) {
        return sameType;
    }
    if (any != nullptr) {
        return any;
    }
    return anyKind != nullptr ? anyKind : otherwise;
}

// One animation the object is playing, as the record names it.
struct Playing {
    const std::string* name = nullptr;
    std::string type;
    std::int32_t layer = 0;
    float rate = 1;
    float weight = 1;
    std::int32_t plays = 1;
    double playedMs = 0;
    double lengthMs = 0;  // one play, 0 or infinite when not known
    bool flipped = false;
    std::int32_t category = 1;
    std::int16_t eventObject = 0;
    std::uint16_t eventTree = 0;
    std::uint32_t eventObjectGuid = 0;
    std::int16_t ikObject = 0;
    AnimationAnchor anchor;
    // How long it fades in and out, when the engine keeps its fades (see blendCurve).
    std::optional<std::pair<float, float>> fades;
    // The kind Animate Stop stops by: 3 for an Animate Sim or Animate Object clip and 4 for an
    // overlay, each 2 more for a clip that moves a skeleton. 0 when not known.
    std::int32_t kind = 0;
};

// What an animation started at `startMs` hangs from: the anchor a save gave it, else the Sim's
// container when the Sim was already in it as the animation started, else nothing.
AnimationAnchor anchorOf(const SimObject& object, const std::optional<AnimationAnchor>& saved, double startMs) {
    constexpr std::size_t kContainerField = 2;
    if (saved) {
        return *saved;
    }
    const bool inIt = object.isPerson() && object.data[kContainerField] > 0 && object.data[kContainerField] == object.anchor.object;
    return inIt && startMs >= object.anchoredAtMs ? object.anchor : AnimationAnchor{};
}

// Whether a clip played once, `playedMs` into it, has played to its end. One restored from a save
// doesn't know its length (`lengthMs` not finite), which the world's clip lengths then give.
bool clipEnded(const std::string& name, float lengthMs, float speed, double playedMs, const World& world) {
    if (!std::isfinite(lengthMs) && world.animationLengths()) {
        const auto ms = world.animationLengths()(name);
        lengthMs = ms && speed != 0 ? *ms / std::abs(speed) : lengthMs;
    }
    return std::isfinite(lengthMs) && playedMs >= lengthMs;
}

// A looping clip ends only where its length says: the plays it was given, or the time it played
// once Animate Stop stopped it. One played until stopped has no end before then.
bool objectClipEnded(const PlayingAnimation& clip, const World& world, double nowMs) {
    if (clip.loops) {
        return std::isfinite(clip.lengthMs) && nowMs - clip.startMs >= clip.lengthMs;
    }
    return clipEnded(clip.name, clip.lengthMs, clip.speed, nowMs - clip.startMs, world);
}

// What the object is playing now. A walk or a turn, which plays on layer 0, is left out: the game's
// own saves hold none. So is a clip that has ended, which the game's saves don't hold either (a chair
// a Sim sat down on keeps no clip), and which a load would play again: a Sim's clip carried over from
// a save its script never came back to held the Sim in its last pose, so its next walk slid. The overlays'
// types and layers go together as every saved lot has them.
std::vector<Playing> playingNow(const SimObject& object, const World& world, double nowMs, const std::vector<cobject::Animation>& animations,
                                const cobject::Animation* otherwise) {
    constexpr std::array<std::pair<const char*, std::int32_t>, 5> kOverlayLayers{
        {{"ovl", 1}, {"ovm", 3}, {"ovh", 5}, {"ovu", 7}, {"osl", 0}}};
    std::vector<Playing> out;
    if (object.animation && (object.animation->layer != 0 || !object.isPerson()) && !objectClipEnded(*object.animation, world, nowMs)) {
        Playing p;
        p.name = &object.animation->name;
        p.rate = object.animation->speed;
        p.playedMs = nowMs - object.animation->startMs;
        p.lengthMs = object.animation->lengthMs;
        p.flipped = object.animation->flipped;
        p.plays = object.animation->loops ? -1 : 1;
        p.category = object.animation->category;
        p.eventTree = object.animation->eventTree;
        p.eventObject = object.animation->eventStackObject;
        p.ikObject = object.animation->ikTarget;
        if (object.animation->fades) {
            p.fades = std::pair{object.animation->fadeInMs, object.animation->fadeOutMs};
        }
        p.anchor = anchorOf(object, object.animation->savedAnchor, object.animation->startMs);
        // An object's own clip has a type of its own. A Sim's names the layer of its priority, as the
        // game's saves pair them: "sil" 0, "sim" 2, "sih" 4 and "siu" 6.
        constexpr std::array<const char*, 4> kSimClipTypes{"sil", "sim", "sih", "siu"};
        const int simLayer = std::clamp(object.animation->layer, 0, 6) & ~1;
        p.type = object.isPerson() ? kSimClipTypes[static_cast<std::size_t>(simLayer / 2)] : "obj";
        p.layer = object.isPerson() ? simLayer : 2;
        out.push_back(p);
    } else if (object.resumedClip &&
               !clipEnded(object.resumedClip->name, std::numeric_limits<float>::infinity(), object.resumedClip->speed, object.resumedClip->playedMs, world)) {
        Playing p;
        p.name = &object.resumedClip->name;
        p.rate = object.resumedClip->speed;
        p.playedMs = object.resumedClip->playedMs;
        p.anchor = object.resumedClip->anchor.value_or(AnimationAnchor{});
        const cobject::Animation* was = templateFor(animations, *p.name, "sim", otherwise);
        p.type = was != nullptr && isSimClip(*was) ? was->request.type : "sim";
        p.layer = was != nullptr && isSimClip(*was) ? was->instance.layer : 2;
        out.push_back(p);
    }
    for (const PlayingOverlay& overlay : object.overlays) {
        const auto at = std::min<std::size_t>(overlay.priority, kOverlayLayers.size() - 1);
        Playing p;
        p.name = &overlay.name;
        p.type = kOverlayLayers[at].first;
        p.layer = kOverlayLayers[at].second;
        p.rate = overlay.speed;
        p.weight = overlay.weight;
        p.plays = overlay.plays;
        p.playedMs = nowMs - overlay.startMs;
        p.lengthMs = overlay.plays > 0 ? overlay.lengthMs / overlay.plays : overlay.lengthMs;
        p.flipped = overlay.flipped;
        p.category = overlay.category;
        p.eventTree = overlay.eventTree;
        p.eventObject = overlay.stackObject;
        p.ikObject = overlay.ikTarget;
        p.fades = std::pair{overlay.blendsIn ? overlay.blendInMs.value_or(kOverlayBlendMs) : 0.0f,
                            overlay.blendsOut ? overlay.blendOutMs.value_or(kOverlayBlendMs) : 0.0f};
        p.anchor = anchorOf(object, overlay.savedAnchor, overlay.startMs);
        out.push_back(p);
    }
    return out;
}

bool sameName(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

// The handle of a clip a save writes (see ClipHandles). A Sim clip carried on from the entry of the
// same clip keeps its own. An object's Animate Object clip ("obj") has the handle every such clip of
// the game's saves has. Another object clip keeps the one of the entry it starts from, as how the
// game numbers those is not known yet.
cobject::Word clipHandle(const cobject::Animation& was, const Playing& playing, ClipHandles* handles) {
    constexpr cobject::Word kClassBits = 0xE0000000, kOverlayClass = 0x60000000, kSimClipClass = 0x40000000, kObjectClipClass = 0x20000000,
                            kMirrored = 1;
    constexpr int kSerialShift = 21;
    const cobject::Word mirrored = playing.flipped ? kMirrored : 0;
    const bool overlay = playing.type.starts_with("ov") || playing.type == "osl";
    const bool simClip = playing.type.starts_with("si");
    if (handles != nullptr && overlay) {
        return kOverlayClass | mirrored;
    }
    if (handles != nullptr && playing.type == "obj") {
        return kObjectClipClass | mirrored;
    }
    if (handles != nullptr && simClip) {
        if (was.kind == cobject::Animation::Kind::Clip && sameName(was.request.name, *playing.name) && (was.request.flags & kClassBits) == kSimClipClass) {
            return (was.request.flags & ~kMirrored) | mirrored;
        }
        return kSimClipClass | ((++handles->last & 0xFF) << kSerialShift) | mirrored;
    }
    return (was.request.flags & ~kMirrored) | mirrored;
}

// The blend curve the game saves with a clip: rising from 0.001 to its weight over its fade in,
// holding the weight, then falling to 0.001 over its fade out, the three as long as all its plays.
// One that plays until stopped holds for the game's "until stopped" length, the float just under
// 2^31. A load takes the clip's length and fades from it, so a curve another clip's entry held ended
// this one at once.
cobject::Curve blendCurve(const Playing& playing, float fadeIn, float fadeOut) {
    const float kUntilStopped = std::bit_cast<float>(cobject::Word{0x4EFFFFFE});
    constexpr float kFloor = 0.001f;
    const auto word = [](float v) { return std::bit_cast<cobject::Word>(v); };
    float hold = kUntilStopped;
    if (playing.plays >= 0) {
        const double all = std::isfinite(playing.lengthMs) ? playing.lengthMs * std::max(playing.plays, 1) : 0.0;
        hold = static_cast<float>(std::max(0.0, all - fadeIn - fadeOut));
    }
    const float weight = playing.weight;
    cobject::Curve curve;
    curve.value = 1;
    curve.segments = {{0, {word(kFloor), word(weight)}, word(fadeIn)},
                      {1, {word(weight), word(weight)}, word(hold)},
                      {2, {word(weight), word(kFloor)}, word(fadeOut)}};
    return curve;
}

// A clip entry as the game makes a new one, before it plays: the request and the clip at their
// starting values, its three places at no move and no turn (each written with a scale of 1), and
// "never" for its ticks to come. Then the two flags every playing clip of the game's saves has: the
// clip has started, and the word after its resource name.
cobject::Animation newClipEntry() {
    const auto real = [](float v) { return std::bit_cast<cobject::Word>(v); };
    constexpr cobject::Word kNeverTick = 0x7FFFFF00, kUntilStopped = 0x4EFFFFFE;
    cobject::Animation out;
    out.kind = cobject::Animation::Kind::Clip;
    out.request.words1 = {real(-1), real(-1), 1, 1, kNeverTick, 0, 0, 0, 0, 0, 0, 0, real(1), 0xFFFFFFFF, 1};
    out.request.category = 1;
    out.instance.words2 = {0, real(1), 5};  // the third, 5, is how the clip decides it has ended
    out.instance.weight = real(1);
    out.instance.plays = 1;
    out.instance.words4 = {real(1), 0};
    for (std::size_t place = 0; place < 3; ++place) {
        out.instance.words4[2 + place * 8 + 6] = real(1);  // the rotation's w
        out.instance.words4[2 + place * 8 + 7] = real(1);  // the scale written after each place
    }
    out.instance.words5 = {1, 0, kNeverTick, kUntilStopped, 1};
    out.word = 1;
    return out;
}

// `was` with one animation the object is playing in it. Only the fields whose meaning is known are
// set, since the rest of an entry is not read yet and taking them from an animation of the object's
// own is the nearest the save can get.
cobject::Animation playedAnimation(const cobject::Animation& was, const Playing& playing, ClipHandles* handles) {
    // An empty entry or an attached one holds nothing of a clip's, so a new clip starts as the game
    // makes one. So does one an older NeoTS2 save built from either, which the game never writes: it
    // writes a scale of 1 after each place, and a playing clip's word after its resource name is
    // never 0. A load shows no clip from an entry of 0s, so the Sim playing it is invisible.
    constexpr std::size_t kFirstScale = 9;
    const bool clip = was.kind == cobject::Animation::Kind::Clip && was.instance.words4[kFirstScale] == std::bit_cast<cobject::Word>(1.0f) &&
                      was.word != 0;
    cobject::Animation out = clip ? was : newClipEntry();
    // A clip's resource is its animation's, named as the clip with "_anim" after it. One started from
    // the entry of the same clip keeps the name's letter case as that entry has it.
    if (!clip || !sameName(was.request.name, *playing.name)) {
        out.resource = *playing.name + "_anim";
        // The kind Animate Stop goes by (see Playing::kind), and for an Animate Sim or Animate
        // Object clip how it ends: 3, at its end, for one that doesn't fade out, else 5, as its
        // weight falls away. The game's own entry of the same clip keeps its own.
        if (playing.kind != 0) {
            out.request.value1 = playing.kind;
        }
        constexpr std::size_t kEndMode = 2;
        if (playing.kind == 3 || playing.kind == 5) {
            out.instance.words2[kEndMode] = playing.fades && playing.fades->second == 0 ? 3 : 5;
        }
    } else if (playing.kind != 0) {
        // An entry of the same clip from an older NeoTS2 save can hold a kind of the other class
        // (a full-body clip's on an overlay). The game's own entries keep theirs.
        const std::int32_t sameClass = playing.kind <= 4 ? playing.kind + 2 : playing.kind - 2;
        if (out.request.value1 != playing.kind && out.request.value1 != sameClass) {
            out.request.value1 = playing.kind;
        }
    }
    out.request.name = *playing.name;
    out.request.type = playing.type;
    out.request.flags = clipHandle(was, playing, handles);
    out.request.rate = std::bit_cast<cobject::Word>(playing.rate);
    out.request.category = playing.category;
    out.request.eventTree = playing.eventTree;
    out.request.eventObject = playing.eventObject;
    out.request.eventObjectGuid = playing.eventObjectGuid;
    // The entry it starts from is another clip's, so these are set for this one: the object its IK
    // reaches for, and what it hangs from.
    out.request.words1[5] = static_cast<cobject::Word>(static_cast<std::int32_t>(playing.ikObject));
    out.request.words3 = {static_cast<cobject::Word>(playing.anchor.object), playing.anchor.name};
    out.instance.state = 1;
    out.instance.layer = playing.layer;
    out.instance.plays = playing.plays;
    out.instance.weight = std::bit_cast<cobject::Word>(playing.weight);
    // An object's clip carried on from the game's entry of the same clip keeps the curve the game wrote.
    const bool objectClipCarriedOn = playing.type == "obj" && clip && sameName(was.request.name, *playing.name);
    if (playing.fades && !objectClipCarriedOn) {
        out.instance.curve = blendCurve(playing, playing.fades->first, playing.fades->second);
    }
    // A load moves every tick of an entry onto its own clock by one amount, so they go together: the
    // tick the lot's clips were saved at for an entry built from an empty one, and a stop tick of
    // "never", as every playing entry of the game's saves has. A stop tick of 0 stops the clip as the
    // lot loads.
    constexpr cobject::Word kNeverTick = 0x7FFFFF00;
    if (handles != nullptr && out.instance.savedTick == 0) {
        out.instance.savedTick = handles->savedTick;
    }
    out.instance.stopTick = kNeverTick;
    // Ticks are whole milliseconds, and so is the time into a clip, as the game's saves hold it.
    const double played = std::floor(std::clamp(playing.playedMs, 0.0, 1.0e9));
    out.instance.startTick = out.instance.savedTick - static_cast<std::int32_t>(played);
    // The time into the clip: into its current play for one that repeats, held at its end for one
    // that plays once, and 0 when its length isn't known (an object's clip of unknown length plays
    // on without end). A load wraps the time by the clip's length and never finishes when the
    // length is 0 and the time isn't.
    double time = 0;
    if (std::isfinite(playing.lengthMs) && playing.lengthMs > 0) {
        time = std::floor(playing.plays == 1 ? std::min(played, playing.lengthMs) : std::fmod(played, playing.lengthMs));
    }
    out.instance.time = std::bit_cast<cobject::Word>(static_cast<float>(time));
    return out;
}

} // namespace

cobject::Place standingScenePlace(const SimObject& sim, float floorHeight) {
    // The game writes the angle from 0 up to a whole turn.
    constexpr float kTurn = 6.28318530718f;
    float angle = wrapAngle(drawnHeading(sim.heading ? *sim.heading : compassHeading(sim.data[1])));
    if (angle < 0) {
        angle += kTurn;
    }
    const auto word = [](float v) { return std::bit_cast<cobject::Word>(v); };
    return {word(static_cast<float>(sim.y) / 65536.0f), word(static_cast<float>(sim.x) / 65536.0f), word(floorHeight), word(0.0f), word(0.0f),
            word(std::sin(angle / 2)), word(std::cos(angle / 2))};
}

cobject::Place simScenePlace(const SimObject& sim, const std::function<float(std::int32_t, float, float)>& floorHeight) {
    constexpr std::size_t kContainerField = 2;
    if (sim.data[kContainerField] > 0) {
        return {0, 0, 0, 0, 0, 0, std::bit_cast<cobject::Word>(1.0f)};
    }
    const float x = static_cast<float>(sim.y) / 65536.0f, y = static_cast<float>(sim.x) / 65536.0f;
    return standingScenePlace(sim, floorHeight ? floorHeight(sim.level, x, y) : 0.0f);
}

namespace lot_save_detail {

cobject::Record sceneRecord(const cobject::Record& loaded, const SimObject& object, const World& world, double nowMs,
                            const cobject::Animation* otherwise, ClipHandles* handles) {
    cobject::Record record = loaded;
    std::vector<cobject::Animation>& animations = record.animatable.animations;
    std::vector<Playing> playing = playingNow(object, world, nowMs, animations, otherwise);
    // Each animation names the object its events go to and that object's type by GUID, which is
    // where a load looks the event tree up. One with an event tree and no object given names the
    // object itself. One with neither keeps 0 with the object's own GUID, as the game's saves hold it.
    for (Playing& p : playing) {
        if (p.eventObject == 0 && p.eventTree != 0) {
            p.eventObject = object.id;
        }
        // The time saved into a clip goes by the clip's own length where the world knows it: an
        // overlay held until stopped keeps another length, and a load wraps the time by the clip's
        // length without end when that is 0 and the time isn't (a carry pose).
        if (const auto& lengths = world.animationLengths(); lengths && p.name != nullptr) {
            if (const auto ms = lengths(*p.name)) {
                p.lengthMs = p.rate != 0 ? *ms / std::abs(p.rate) : 0.0;
            }
        }
        const SimObject* target = p.eventObject != 0 ? world.find(p.eventObject) : &object;
        p.eventObjectGuid = target != nullptr ? target->definition.guid() : 0;
        const bool overlay = p.type.starts_with("ov") || p.type == "osl";
        const bool fullBody = p.type.starts_with("si") || p.type == "obj";
        if (const auto& moves = world.animationMovesSkeleton(); moves && p.name != nullptr && (overlay || fullBody)) {
            p.kind = (overlay ? 4 : 3) + (moves(*p.name) ? 2 : 0);
        }
    }
    std::vector<cobject::Animation> out;
    for (const cobject::Animation& a : animations) {
        if (a.kind == cobject::Animation::Kind::Attached) {
            out.push_back(a);
        }
    }
    for (const Playing& p : playing) {
        if (const cobject::Animation* was = templateFor(animations, *p.name, p.type, otherwise)) {
            out.push_back(playedAnimation(*was, p, handles));
        }
    }
    // The game writes an empty entry for an animation it does not keep, so the list keeps its length.
    while (out.size() < animations.size()) {
        out.push_back({});
    }
    animations = std::move(out);
    if (handles != nullptr && object.isPerson()) {
        record.animatable.settings[0] = handles->saved;
    }
    return record;
}

std::optional<std::array<cobject::Word, 14>> simPlacePair(const SimObject& sim, const cobject::Place& place, const TypeOwners& owners) {
    constexpr std::size_t kContainerField = 2;
    cobject::Place world = place;
    if (sim.data[kContainerField] > 0) {
        const auto drawn = owners.drawnPlace ? owners.drawnPlace(static_cast<std::int16_t>(sim.id)) : std::nullopt;
        if (!drawn) {
            return std::nullopt;
        }
        world = *drawn;
    }
    std::array<cobject::Word, 14> pair{};
    std::copy(place.begin(), place.end(), pair.begin());
    std::copy(world.begin(), world.end(), pair.begin() + 7);
    return pair;
}

cobject::Record newSimSceneRecord(const cobject::Record& another, const SimObject& sim, const World& world, double nowMs,
                                  const cobject::Animation* otherwise, float floorHeight, const TypeOwners& owners,
                                  ClipHandles* handles) {
    cobject::Record start;
    start.chain = cobject::Chain::Person;
    start.animatable.scale = std::bit_cast<cobject::Word>(1.0f);
    start.animatable.settings = another.animatable.settings;
    start.locomotable.value = another.locomotable.value;
    const cobject::Place place = simScenePlace(sim, [floorHeight](std::int32_t, float, float) { return floorHeight; });
    start.object.place = place;
    start.animatable.place = place;
    start.animatable.places = simPlacePair(sim, place, owners);
    return sceneRecord(start, sim, world, nowMs, otherwise, handles);
}

} // namespace lot_save_detail

} // namespace ts2::sim
