#include "sim/lot_world.hpp"

#include "sim/object_reset.hpp"
#include "sim/relationship_table.hpp"
#include "sim/type_signature.hpp"
#include "sim/vm_execution.hpp"

#include "content/animation_lists.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <set>

namespace ts2::sim {

namespace {

// Whether a saved position fits the engine's 16.16 fixed point.
bool fitsFixedPoint(float v) { return std::isfinite(v) && std::fabs(v) < 32767.0f; }

std::string hex(std::uint32_t value) {
    char buffer[11];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
    return buffer;
}

template <std::size_t N>
bool copyInto(std::array<std::int16_t, N>& to, const std::vector<std::int16_t>& from) {
    std::copy_n(from.begin(), std::min(N, from.size()), to.begin());
    return from.size() <= N;
}

template <std::size_t N>
bool copyInto(LazyArray<std::int16_t, N>& to, const std::vector<std::int16_t>& from) {
    std::copy_n(from.begin(), std::min(N, from.size()), to.begin());
    return from.size() <= N;
}

// A Sim's saved runs (the trees started on its stack as runs of their own) and whether its running
// interaction was a blank one, as the game loads them with the stack.
void restoreRuns(const xobj::PersonState& saved, SimObject& object) {
    for (const auto& run : saved.runs) {
        // Only a broken save holds a negative depth. That run is left out.
        if (run.depth >= 0) {
            object.runDepths.push_back(static_cast<std::size_t>(run.depth));
        }
    }
    object.currentInteraction =
        xobj::isBlank(saved.running) ? SimObject::CurrentInteraction::Blank : SimObject::CurrentInteraction::Started;
    if (xobj::isBlank(saved.running)) {
        object.blankId = static_cast<std::uint16_t>(saved.running.id);
    }
    // The motive changes it was saved with go on (a Sim on the toilet keeps filling its bladder). A
    // broken save's change of a motive the Sim doesn't have, or of a value that isn't a number, is left
    // out, and at most 64 are kept, as Set Motive Change keeps.
    constexpr std::size_t kMaxMotiveChanges = 64;
    for (const auto& entry : saved.entries) {
        if (entry.key < 0 || static_cast<std::size_t>(entry.key) >= object.motives.size() || !std::isfinite(entry.value1) ||
            !std::isfinite(entry.value2) || object.motiveChanges.size() >= kMaxMotiveChanges) {
            continue;
        }
        object.motiveChanges.push_back({static_cast<std::uint8_t>(entry.key), entry.value1, entry.value2});
    }
}

// An interaction a Sim was saved with, as the lot gives it back. The fields the save keeps that the
// engine does not work from yet are left out, so a restored interaction shows and links as the game
// had it. Nothing for a blank one, which stands for a Sim with nothing to do.
std::optional<QueuedInteraction> restoredInteraction(const xobj::SavedInteraction& saved, const World& world) {
    if (xobj::isBlank(saved)) {
        return std::nullopt;
    }
    QueuedInteraction out;
    out.object = saved.target;
    if (const SimObject* object = world.find(saved.target)) {
        out.group = object->group;
    }
    // A line of -1 means the interaction runs a tree of its own rather than one of the object's lines.
    out.index = saved.index >= 0 ? static_cast<std::uint16_t>(saved.index) : 0;
    out.ownTree = saved.index < 0;
    out.action = saved.tree;
    out.priority = static_cast<std::int16_t>(std::clamp(saved.priority, static_cast<std::int32_t>(INT16_MIN),
                                                        static_cast<std::int32_t>(INT16_MAX)));
    for (std::size_t i = 0; i < out.args.size(); ++i) {
        out.args[i] = static_cast<std::int16_t>(saved.values[i]);
    }
    out.name = saved.name;
    out.iconObject = saved.iconObject > 0 ? saved.iconObject : 0;
    out.noIconObject = saved.iconObject < 0;
    // The icon object is the interaction's second object, the Sim a social is with.
    out.partner = out.iconObject;
    out.iconGuid = static_cast<std::uint32_t>(saved.iconGuid);
    out.iconIndex = static_cast<std::uint32_t>(saved.iconIndex);
    out.nested = saved.lineFlag2 != 0;
    // The save gives an interaction its own number and the number of the one it is linked with, so
    // a social's two halves still drop together.
    out.linkId = static_cast<std::uint16_t>(saved.id);
    out.savedId = static_cast<std::uint16_t>(saved.id);
    out.linkedId = static_cast<std::uint16_t>(saved.linkId);
    constexpr std::int32_t kMarkedAutonomous = 0x400;
    out.markedAutonomous = (saved.flags & kMarkedAutonomous) != 0;
    constexpr std::int32_t kCancelled = 0x100;
    out.cancelled = (saved.flags & kCancelled) != 0;
    return out;
}

// The interactions a Sim was saved running and waiting to run, given back to it.
void restoreQueue(const xobj::PersonState& saved, SimObject& object, World& world) {
    const auto take = [&](const xobj::SavedInteraction& from) -> Boxed<QueuedInteraction> {
        auto one = restoredInteraction(from, world);
        if (!one) {
            return {};
        }
        one->id = ++object.nextInteractionId;
        return Boxed<QueuedInteraction>(*one);
    };
    object.running = take(saved.running);
    if (saved.runningSub) {
        object.runningSub = take(*saved.runningSub);
    }
    for (const auto& from : saved.queue) {
        if (auto one = restoredInteraction(from, world)) {
            one->id = ++object.nextInteractionId;
            object.queue.push_back(std::move(*one));
        }
    }
    // The game keeps nested interactions waiting in a queue of their own.
    for (const auto& from : saved.subQueue) {
        if (auto one = restoredInteraction(from, world)) {
            one->id = ++object.nextInteractionId;
            one->nested = true;
            object.queue.push_back(std::move(*one));
        }
    }
    // A frame an Idle for Input started runs the interaction or sub-interaction, which ends when it
    // returns, as one started after loading does.
    for (Frame& f : object.thread.frames) {
        if (f.runsInteraction) {
            const Boxed<QueuedInteraction>& runs = f.callerContinuesTrue ? object.runningSub : object.running;
            f.interactionId = runs ? runs->id : 0;
        }
    }
    // A social animation is named for the second object of the running sub-interaction, else of the
    // running interaction, so a social saved halfway through plays on with the same Sim.
    if (const Boxed<QueuedInteraction>& with = object.runningSub ? object.runningSub : object.running) {
        object.interactionPartner = with->partner != 0 ? with->partner : with->object;
    }
    if (object.running) {
        // The queue shows the interaction it runs by the Sim's own action picture, which a script
        // can change while it runs, so the one the save held is put back there too.
        object.actionString = object.running->name;
        object.actionIconObject = object.running->icon();
        object.actionIconGuid = object.running->iconGuid;
        object.actionIconIndex = object.running->iconIndex;
    }
}

std::optional<std::string> restoreStack(const content::Lot& lot, const content::LotObject& saved,
                                        SimObject& object, CodeSource& code) {
    if (!saved.state.tail) {
        return std::nullopt;
    }
    std::vector<Frame> frames;
    // The frame below is an Idle for Input that started this one: 1 from the queue, 2 an immediate one.
    int startedBelow = 0;
    for (const auto& s : saved.state.tail->stack) {
        const auto* ownerEntry = lot.types.find(static_cast<std::uint16_t>(s.codeOwner));
        const auto* owner = ownerEntry ? code.objectType(ownerEntry->guid) : nullptr;
        if (!owner) {
            return "stack frame's code owner has no definition";
        }
        const auto tree = static_cast<std::uint16_t>(s.treeId);
        const std::uint32_t group = tree < 0x1000   ? kGlobalGroup
                                    : tree < 0x2000 ? owner->group
                                                    : code.semiGlobalGroup(owner->group);
        auto bhav = code.behavior(group, tree);
        if (!bhav) {
            return "stack frame's tree not found: " + hex(group) + " " + std::to_string(tree);
        }
        if (s.node >= bhav->instructions.size()) {
            return "stack frame's node is past the end of its tree";
        }
        Frame f;
        f.group = group;
        f.codeOwner = owner->group;
        f.instance = tree;
        f.pc = s.node;
        f.params = s.params;
        f.params.resize(std::max<std::size_t>({4, bhav->argCount, f.params.size()}), 0);
        f.locals = s.locals;
        f.locals.resize(std::max<std::size_t>(bhav->localCount, f.locals.size()), 0);
        f.stackObjectId = s.objectId;
        f.runningOnId = s.value2 != 0 ? s.value2 : static_cast<std::int16_t>(object.id);
        // The value after the locals is the frame's waiting flag: 1 when the primitive at its node
        // has pushed the frame above it and runs again once that returns. For Idle for Input that
        // frame is the interaction it started. An immediate Idle for Input started a sub-interaction,
        // after which it is true.
        if (startedBelow == 1) {
            f.callerIdlesAgain = true;
        } else if (startedBelow == 2) {
            f.callerContinuesTrue = true;
        }
        f.runsInteraction = startedBelow != 0;
        startedBelow = 0;
        const auto& at = bhav->instructions[f.pc];
        if (s.value3 == 1 && at.opcode == vm_detail::kIdleForInputOpcode) {
            startedBelow = at.operands[4] & 0x01 ? 2 : 1;
            f.idleStarted = startedBelow == 1;
        }
        f.code = std::move(bhav);
        frames.push_back(std::move(f));
    }
    if (!frames.empty()) {
        // A Sleep saved part way through goes on for the ticks the object map kept for it, then is true.
        // It doesn't start over.
        const auto& waits = lot.objectMap.waitTicks;
        const auto wait = static_cast<std::size_t>(object.id) - 1;
        const Frame& top = frames.back();
        if (wait < waits.size() && waits[wait] > 0 && top.code->instructions[top.pc].opcode == vm_detail::kSleepOpcode) {
            object.thread.pendingResult = true;
            object.thread.sleepTicks = waits[wait] + 1;
        }
        object.thread.frames = std::move(frames);
        object.thread.stackObjectId = saved.state.tail->stack.front().objectId;
    }
    return std::nullopt;
}

// The overlays an object was playing when the lot was saved, carrying on from where they were. Only
// playing ("state 1") overlays come back. A Sim's Animate Sim clip comes back by restoreSimClip and
// an object's Animate Object clip by restoreObjectClip. Attached animations are not restored yet.
void restoreOverlays(const content::Lot& lot, std::uint32_t id, SimObject& object, CodeSource& code, double nowMs) {
    const auto saved = lot.savedAnimations.find(id);
    if (saved == lot.savedAnimations.end()) {
        return;
    }
    constexpr std::array<int, 5> kPriorityLayers{1, 3, 5, 7, 0};
    for (const auto& a : saved->second) {
        const auto layer = std::find(kPriorityLayers.begin(), kPriorityLayers.end(), a.layer);
        // An Animate Object clip ("obj") is restoreObjectClip's, on whatever layer a broken save puts it.
        if (a.kind != cobject::SavedAnimation::Kind::Clip || a.state != 1 || layer == kPriorityLayers.end() ||
            a.type.size() != 3 || a.type[0] != 'o' || a.type == "obj") {
            continue;
        }
        PlayingOverlay o;
        o.name = a.name;
        o.flipped = a.flipped;
        // Saved values out of range are read as Animate Overlay reads its own (a weight outside 0-1 is 1).
        const double played = a.startedMsAgo;
        o.startMs = nowMs - played;
        o.restoredAtMs = nowMs;
        o.lengthMs = a.lengthMs && !std::isnan(*a.lengthMs) ? std::max(*a.lengthMs, 0.0f) : 0.0f;
        o.plays = static_cast<std::int16_t>(std::clamp(a.plays, -1, 0x7FFF));
        o.weight = a.weight >= 0 && a.weight <= 1 ? a.weight : 1.0f;
        o.priority = static_cast<std::uint8_t>(layer - kPriorityLayers.begin());
        // The saved kind lets Animate Stop by kind stop it, so a saved idle stops when the Sim walks.
        if (a.category && *a.category >= 0 && *a.category <= content::animation_category::kReceptivity) {
            o.category = static_cast<std::uint8_t>(*a.category);
        }
        o.stackObject = a.eventObject;
        o.ikTarget = a.ikObject > 0 ? a.ikObject : 0;
        o.savedAnchor = AnimationAnchor{a.anchorObject, a.anchorName};
        // Its events run its tree in the code of the type its event GUID names. A GUID that names no
        // installed type leaves it no code, so it plays without its events. The game always writes a
        // GUID with a tree, and older NeoTS2 saves wrote 0.
        if (const ObjectType* owner = code.objectType(a.eventObjectGuid)) {
            o.codeOwner = owner->group;
            o.eventTree = a.eventTree;
        }
        // Its saved blend curve gives its fades: a weight-shift pose held between its start and reverse
        // clips has none, so it hands over at full weight instead of fading out first.
        if (a.lengthMs) {
            o.blendsIn = a.fadeInMs > 0;
            o.blendsOut = a.fadeOutMs > 0;
            o.blendInMs = a.fadeInMs;
            o.blendOutMs = a.fadeOutMs;
        }
        o.blendingOut = o.plays >= 0 && played >= o.lengthMs - o.blendOutMs.value_or(kOverlayBlendMs);
        object.overlays.push_back(std::move(o));
    }
}

// The Animate Sim clip a Sim was playing when the lot was saved. Its type names its priority: "sil"
// low, "sim" medium, "sih" high, "siu" underlay. A rate outside what Animate Sim's speed byte can
// give, or 0, is left out, and the clip then starts as the operands say.
void restoreSimClip(const content::Lot& lot, std::uint32_t id, SimObject& object) {
    const auto saved = lot.savedAnimations.find(id);
    if (saved == lot.savedAnimations.end()) {
        return;
    }
    constexpr float kMinRate = -128.0f / 32, kMaxRate = 127.0f / 32;
    const auto animateSim = [](const std::string& type) { return type == "sil" || type == "sim" || type == "sih" || type == "siu"; };
    for (const auto& a : saved->second) {
        if (a.kind == cobject::SavedAnimation::Kind::Clip && a.state == 1 && animateSim(a.type) && std::isfinite(a.rate) &&
            a.rate != 0 && a.rate >= kMinRate && a.rate <= kMaxRate) {
            object.resumedClip = SimObject::ResumedClip{a.name, a.rate, static_cast<double>(a.startedMsAgo),
                                                        AnimationAnchor{a.anchorObject, a.anchorName}};
            return;
        }
    }
}

// The Animate Object clip an object was playing when the lot was saved ("obj"), carrying on from where
// it got to. Animate Object never waits, so no script starts it again. The game keeps each clip it
// starts in the list, in the order started, and the engine keeps the latest, as a later Animate Object
// takes over the object: the plumbob's turn, started after its default pose. Shipped saves only hold
// clips played once or until stopped. A clip saved with more plays is left out, as its length isn't
// known while the lot loads. So is a rate outside what the speed byte can give.
void restoreObjectClip(const content::Lot& lot, std::uint32_t id, SimObject& object, const World& world) {
    const double nowMs = world.clockMs();
    const auto saved = lot.savedAnimations.find(id);
    if (saved == lot.savedAnimations.end() || object.isPerson()) {
        return;
    }
    constexpr float kMinRate = -128.0f / 32, kMaxRate = 127.0f / 32;
    for (auto it = saved->second.rbegin(); it != saved->second.rend(); ++it) {
        const auto& a = *it;
        if (a.kind != cobject::SavedAnimation::Kind::Clip || a.state != 1 || a.type != "obj") {
            continue;
        }
        if ((a.plays != 1 && a.plays != -1) || !std::isfinite(a.rate) || a.rate < kMinRate || a.rate > kMaxRate) {
            return;
        }
        PlayingAnimation play;
        play.name = a.name;
        play.startMs = nowMs - a.startedMsAgo;
        play.restoredAtMs = nowMs;
        play.speed = a.rate;
        play.flipped = a.flipped;
        play.loops = a.plays == -1;
        // Its saved kind. One without a kind is Normal, or Carry for a carry pose, as the game makes it.
        namespace cat = content::animation_category;
        if (a.category && *a.category >= 0 && *a.category <= cat::kReceptivity) {
            play.category = static_cast<std::uint8_t>(*a.category);
        }
        if (play.category == cat::kNoCategory) {
            play.category = world.animationCarries() && world.animationCarries()(a.name) ? cat::kCarry : cat::kNormal;
        }
        play.lengthMs = std::numeric_limits<float>::infinity();
        object.animation = std::move(play);
        return;
    }
}

} // namespace

World::PlatformRegion platformRegions(const content::Lot& lot) {
    const auto regions = lot.grids.find(content::kPlatformRegionGridInstance);
    constexpr std::size_t kCellSize = 16;  // four 32-bit regions
    if (regions == lot.grids.end() || regions->second.elementSize != kCellSize) {
        return {};
    }
    const int ground = content::groundGridLevel(lot);
    return [grid = std::make_shared<const array3d::Array3D>(regions->second), ground](int level, int x, int y,
                                                                                     int quarter) -> std::optional<std::int32_t> {
        constexpr int kQuarters = 4;
        const auto index = static_cast<std::int64_t>(level) + ground;
        if (quarter < 0 || quarter >= kQuarters || index < 0 || index > std::numeric_limits<std::int32_t>::max()) {
            return std::nullopt;
        }
        const auto cell = grid->at(static_cast<std::int32_t>(index), x, y);
        if (cell.size() != kCellSize) {
            return std::nullopt;
        }
        std::int32_t region = 0;
        std::memcpy(&region, cell.data() + static_cast<std::size_t>(quarter) * sizeof region, sizeof region);
        return region;
    };
}

std::vector<std::string> applySavedState(SimObject& object, const xobj::SavedObject& state, CodeSource& code, const ObjectType& type) {
    std::vector<std::string> problems;
    if (!copyInto(object.attributes, state.attributes)) {
        problems.push_back("more saved attributes than the engine keeps");
    }
    if (!copyInto(object.semiAttributes, state.semiAttributes)) {
        problems.push_back("more saved semi-attributes than the engine keeps");
    }
    std::copy(state.data.begin(), state.data.end(), object.data.begin());
    notePausedRun(object);
    object.thread.temps = state.temps;
    const auto& s = state.tokenSource;
    object.tokenSource = {s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]};
    object.thread.codeGroup = type.group;
    if (state.tail) {
        // An object saved when its type had fewer arrays (an older pack's) gets the rest, empty,
        // as the game loads it.
        object.arrays = state.tail->arrays;
        if (const std::size_t count = object.definition.field(kObjectArrayCountField); object.arrays.size() < count) {
            object.arrays.resize(count);
        }
        object.materialNames = state.tail->strings;
        const auto& t = state.tail->token;
        object.token = InventoryToken{t.guid, t.flags, t.value1, t.value2, t.value3, t.properties};
        object.arrayIterators = state.tail->shorts;
        object.arrayIterators.resize(object.arrays.size(), 0);
        // The saved slots (one object id per container slot) win over the SLOT resource.
        object.slots = state.tail->slots.marker < 0 ? state.tail->slots.values
                                                    : std::vector<std::int16_t>(type.containerSlots, 0);
        for (const auto& e : state.tail->effects) {
            object.savedEffects.push_back({e.name, e.slot, static_cast<std::int32_t>(e.value1), static_cast<std::uint8_t>(e.value2), e.value3});
        }
    } else {
        object.slots.assign(type.containerSlots, 0);
    }
    if (const std::size_t slots = containerSlotsOf(code, type); object.slots.size() < slots) {
        object.slots.resize(slots, 0);
    }
    if (state.person) {
        object.motives = state.person->motives;
        object.motivesAtDecay = state.person->motives2;
        // A broken save can hold any float: levels that aren't numbers start at 0, others are kept
        // to the motive range, as every write to them is.
        for (auto* levels : {&object.motives, &object.motivesAtDecay}) {
            for (float& level : *levels) {
                level = std::isfinite(level) ? std::clamp(level, kMotiveMin, kMotiveMax) : 0.0f;
            }
        }
        copyInto(object.personData, state.person->personData);
    }
    return problems;
}

LotWorldReport buildWorld(const content::Lot& lot, CodeSource& code, World& world) {
    LotWorldReport report;
    RoomMap rooms;
    if (const auto saved = lot.grids.find(content::kRoomGridInstance); saved != lot.grids.end()) {
        rooms = RoomMap(saved->second, lot.roomGraph ? lot.roomGraph->firstLevel : 0);
    } else if (lot.roomGraph) {
        rooms = RoomMap(*lot.roomGraph);
    }
    for (int layer = 0; layer < 2; ++layer) {
        if (const auto graph = lot.layerGraphs.find(content::kLayerGraphInstances[layer]);
            graph != lot.layerGraphs.end()) {
            rooms.assignLayer(layer, RoomMap(graph->second));
        }
    }
    world.setRooms(std::move(rooms));
    if (lot.worldBounds) {
        const auto& b = *lot.worldBounds;
        world.setTileBounds(World::TileBounds{b.minX, b.minY, b.maxX, b.maxY, b.minLevel, b.maxLevel});
    }
    std::vector<std::int16_t> persons;
    for (const auto& saved : lot.objects) {
        const ObjectType* type = saved.type ? code.objectType(saved.type->guid) : nullptr;
        if (!type) {
            report.problems.push_back(
                {saved.id, "no definition for " + (saved.type ? hex(saved.type->guid) + " " + saved.type->name
                                                               : std::string("an untyped object"))});
            continue;
        }
        if (saved.id == 0 || saved.id > 0x7FFF) {
            report.problems.push_back({saved.id, "object id out of range"});
            continue;
        }
        SimObject& object = world.restore(static_cast<std::int16_t>(saved.id), type->group, type->definition);
        const auto& state = saved.state;
        for (std::string& problem : applySavedState(object, state, code, *type)) {
            report.problems.push_back({saved.id, std::move(problem)});
        }
        if (fitsFixedPoint(state.header.location[0]) && fitsFixedPoint(state.header.location[1])) {
            object.x = static_cast<std::int32_t>(state.header.location[0] * 65536.0f);
            object.y = static_cast<std::int32_t>(state.header.location[1] * 65536.0f);
        } else {
            report.problems.push_back({saved.id, "position out of range"});
        }
        object.level = state.header.value1;
        if (const auto& h = state.header;
            h.location[0] >= 0 && h.location[0] < 256 && h.location[1] >= 0 && h.location[1] < 256) {
            object.tile = Tile{static_cast<std::uint8_t>(h.location[0]), static_cast<std::uint8_t>(h.location[1]),
                               static_cast<std::uint8_t>(h.value1)};
        }
        if (auto problem = restoreStack(lot, saved, object, code)) {
            report.problems.push_back({saved.id, *problem});
        } else if (state.person) {
            restoreRuns(state.person->state, object);
            restoreQueue(state.person->state, object, world);
            object.vacationCollectibles = {state.person->state.collectiblesLow, state.person->state.collectiblesHigh};
            persons.push_back(object.id);
        }
        restoreOverlays(lot, saved.id, object, code, world.clockMs());
        restoreSimClip(lot, saved.id, object);
        restoreObjectClip(lot, saved.id, object, world);
        // An object or Sim whose type changed since the save (a patch, expansion or mod) will start over.
        if (saved.type && typeChangedSinceSave(code, *type, *saved.type)) {
            report.changedType.push_back(object.id);
        }
        ++report.objects;
        report.threads += object.thread.isRunning();
    }
    world.orderList(lot.objectMap.order);
    // An interaction on an object saved after its Sim gets the object's code group now that every
    // object is back.
    for (const std::int16_t id : persons) {
        SimObject* sim = world.find(id);
        const auto fill = [&](QueuedInteraction& q) {
            const SimObject* object = q.group == 0 ? world.find(q.object) : nullptr;
            if (object != nullptr) {
                q.group = object->group;
            }
        };
        if (sim == nullptr) {
            continue;
        }
        // A Sim saved in a slot hangs what it starts from now on from that slot's bone.
        constexpr std::size_t kContainerField = 2, kSlotField = 3;
        if (const SimObject* container = sim->data[kContainerField] > 0 ? world.find(sim->data[kContainerField]) : nullptr) {
            const ObjectType* type = code.objectType(container->definition.guid());
            sim->anchor = {container->id, type != nullptr ? slotAnchorName(code, *type, sim->data[kSlotField]) : 0};
            sim->anchoredAtMs = world.clockMs();
        }
        if (sim->running) {
            fill(*sim->running);
        }
        if (sim->runningSub) {
            fill(*sim->runningSub);
        }
        for (QueuedInteraction& q : sim->queue) {
            fill(q);
        }
    }
    for (const auto& [id, tables] : lot.dataTables) {
        SimObject* object = world.find(static_cast<std::int16_t>(id));
        if (object == nullptr) {
            ++report.staleDataTables;
            continue;
        }
        object->dataTables = toDataTables(tables);
    }
    // Each object reads the relationship records of the objects its list names, as the game does.
    // Records it doesn't list are left in the package unread.
    for (const auto& saved : lot.objects) {
        SimObject* object = world.find(static_cast<std::int16_t>(saved.id));
        if (object == nullptr || !saved.state.tail) {
            continue;
        }
        // A broken file can name an id twice or one that isn't an object here.
        std::vector<std::uint32_t> table = loadedRelationshipTable(saved.state.tail->relationshipIds);
        std::set<std::uint32_t> seen;
        std::erase_if(table, [&](std::uint32_t to) {
            return to == 0 || to > 0x7FFF || world.find(static_cast<std::int16_t>(to)) == nullptr || !seen.insert(to).second;
        });
        for (const std::uint32_t to : table) {
            auto& stored = world.relationship(object->id, static_cast<std::int16_t>(to));
            // The game stores 32-bit values but the Relationship primitive moves 16-bit ones.
            if (const auto values = lot.relationships.find({object->id, static_cast<std::int16_t>(to)}); values != lot.relationships.end()) {
                stored.resize(values->second.size());
                for (std::size_t i = 0; i < values->second.size(); ++i) {
                    stored[i] = static_cast<std::int16_t>(values->second[i]);
                }
            }
        }
        object->relationshipTable = std::move(table);
    }
    // As the game reads the object map's wait table, a Placeholder or TemplatePerson among the ids it
    // covers is marked for delete instead. Scripts skip it from then on, but it stays until the lot
    // unloads.
    constexpr std::uint32_t kPlaceholderGuid = 0x3C08965F, kTemplatePersonGuid = 0x7FD96B54;
    for (std::size_t i = 0; i < lot.objectMap.waitTicks.size(); ++i) {
        SimObject* object = world.find(static_cast<std::int16_t>(i + 1));
        if (object != nullptr && (object->definition.guid() == kPlaceholderGuid || object->definition.guid() == kTemplatePersonGuid)) {
            world.keepUntilUnload(*object);
            ++report.keptUntilUnload;
        }
    }
    return report;
}

namespace {

bool isNeighborSim(const ObjectType* type) {
    constexpr std::uint16_t kFamilyTreeOnlyType = 0x11;
    if (type == nullptr) {
        return false;
    }
    const std::uint16_t kind = type->definition.field(objd::field::kType);
    return kind == kPersonType || kind == kFamilyTreeOnlyType;
}

// A want saved by an older game, as the game reads it. Before version 4 its time was kept at half.
// Before version 6 it takes its aspiration points from its definition, and before version 9 its
// influence, both 0 for a want with no definition. Then a save writes it like any other.
void upgradeWant(swaf::Want& w, const content::WantDefinitions* defs) {
    if (w.tail.empty()) {
        return;
    }
    const content::WantDefinition* def = nullptr;
    if (defs != nullptr) {
        const auto it = defs->byGuid.find(w.guid);
        def = it != defs->byGuid.end() ? &it->second : nullptr;
    }
    if (w.version < 4) {
        w.tail[0] = static_cast<std::int32_t>(static_cast<std::uint32_t>(w.tail[0]) * 2u);
    }
    if (w.version < 6 && w.tail.size() == 1) {
        w.tail.push_back(def != nullptr ? def->score : 0);
    }
    if (w.version < 9 && w.tail.size() == 2) {
        w.tail.push_back(def != nullptr ? def->influence : 0);
    }
}

void upgradeWants(std::vector<swaf::Want>& list, const content::WantDefinitions* defs) {
    for (swaf::Want& w : list) {
        upgradeWant(w, defs);
    }
}

void dropNeighbor(SavedNeighborhood& hood, std::int16_t id) {
    hood.neighborIds.erase(id);
    hood.neighbors.erase(id);
    hood.familyTies.erase(id);
    std::erase_if(hood.relationships, [id](const auto& r) { return r.first.first == id || r.first.second == id; });
}

} // namespace

std::shared_ptr<const SavedNeighborhood> savedNeighborhood(const content::Neighborhood& hood, CodeSource& code,
                                                          std::map<std::int32_t, nhoodid::NeighborhoodId> all,
                                                          const content::WantDefinitions* wantDefs) {
    auto saved = std::make_shared<SavedNeighborhood>();
    if (hood.identity) {
        saved->id = hood.identity->id;
        all.try_emplace(hood.identity->id, *hood.identity);
    }
    if (hood.nextNeighborId && *hood.nextNeighborId >= 1 && *hood.nextNeighborId <= INT16_MAX) {
        saved->nextNeighborId = static_cast<std::int16_t>(*hood.nextNeighborId);
    }
    saved->allNeighborhoods = std::make_shared<const std::map<std::int32_t, nhoodid::NeighborhoodId>>(std::move(all));
    saved->neighborIds = hood.neighborIds;
    saved->lotIds = hood.lotIds;
    saved->lotTypes = hood.lotTypes;
    saved->lotOwners = hood.lotOwners;
    saved->lotBuildings = hood.lotBuildings;
    saved->apartments = hood.apartments;
    saved->lotRoomLights = hood.lotRoomLights;
    saved->lotNeighborhoods = hood.lotNeighborhoods;
    for (const auto& [instance, family] : hood.families) {
        saved->familyLots[instance] = family.lot;
        saved->familyCurrentLots[instance] = family.currentLot;
        saved->familyVacationLots[instance] = family.vacationLot;
        saved->familySavedFunds[instance] = family.funds2;
        saved->familyFlags[instance] = family.flags;
        saved->familyRanks[instance] = family.value4;
        saved->familyMembers[instance] = family.members;
    }
    if (hood.record) {
        saved->record = std::make_shared<const ngbh::Neighborhood>(*hood.record);
    }
    for (const auto& [id, tables] : hood.dataTables) {
        saved->dataTables[id] = toDataTables(tables);
    }
    for (const auto& [id, n] : hood.neighbors) {
        const auto dna = hood.dna.find(id);
        saved->neighbors[id] = SavedNeighbor{n.guid, n.personData, dna != hood.dna.end() ? genomeFromProperties(dna->second) : Genome{}};
        saved->neighbors[id].relationshipTable = loadedRelationshipTable(n.relationshipList);
    }
    saved->familyTies = hood.familyTies;
    for (const auto& [id, w] : hood.wants) {
        const auto slots = swaf::slots(w);
        saved->wantSlots[id] = slots;
        if (w.extra) {
            saved->lifetimeWants[id] = *w.extra;
            upgradeWant(saved->lifetimeWants[id], wantDefs);
        }
        saved->wants[id].assign(w.wants.begin(), w.wants.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(w.wants.size(), slots.wants)));
        saved->fears[id].assign(w.fears.begin(), w.fears.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(w.fears.size(), slots.fears)));
        upgradeWants(saved->wants[id], wantDefs);
        upgradeWants(saved->fears[id], wantDefs);
        // History version 2 saves its clock. Older ones take the latest record's time.
        auto& history = saved->wantHistories[id];
        history.records = w.history;
        for (auto& [guid, records] : history.records) {
            upgradeWants(records, wantDefs);
        }
        history.time = w.historyValue;
        if (w.historyVersion < 2) {
            history.time = 0;
            for (const auto& [guid, records] : w.history) {
                for (const auto& r : records) {
                    history.time = std::max(history.time, recordTime(r));
                }
            }
        }
    }
    saved->relationships = hood.relationships;
    for (const std::int16_t id : hood.neighborIds) {
        const auto n = saved->neighbors.find(id);
        if (n == saved->neighbors.end() || !isNeighborSim(code.objectType(n->second.guid))) {
            dropNeighbor(*saved, id);
        }
    }
    return saved;
}

} // namespace ts2::sim
