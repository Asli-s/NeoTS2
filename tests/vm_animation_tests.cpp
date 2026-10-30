// Animation primitives: Animate Sim, Animate Overlay, Animate Object, overlay events and Animate Stop.
#include "test_support.hpp"

#include "content/animation_lists.hpp"
#include "sim/animation.hpp"
#include "sim/ground_click.hpp"
#include "sim/idle_spot.hpp"
#include "sim/route_portals.hpp"
#include "sim/walking.hpp"
#include "sim/path_passes.hpp"
#include "sim/route_world.hpp"
#include "sim/turn.hpp"
#include "sim/walk_path.hpp"
#include "sim/walk_plan.hpp"
#include <map>
#include <random>

namespace {

// Social animations (list 0x83) are named for me and the Sim my interaction is with. A teen or elder
// plays the adult's when there is none of its own (me first, then the partner, then both).
void socialAnimationsAreNamedForBothSims() {
    Instruction animate;
    animate.opcode = 0x006A;
    animate.trueTarget = T;
    animate.falseTarget = E;
    animate.operands[3] = 32;
    animate.operands[6] = 0x83;
    MemoryCodeSource code;
    code.addStrings(kObjectGroup, 0x83, {"soc-admire"});
    code.add(kObjectGroup, 0x1000, bhav({animate}));
    ts2::sim::World world;
    world.setAnimationLengths([](const std::string& name) -> std::optional<float> {
        if (name == "a2t-soc-admire" || name == "a2a-soc-admire") return 1000.0f;
        return std::nullopt;
    });
    auto person = [&](std::int16_t age) -> ts2::sim::SimObject& {
        auto& p = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
        p.personData[0x3A] = age;
        return p;
    };
    auto& adult = person(30);
    auto& teen = person(17);
    auto& elder = person(60);
    const auto& chair = world.create(kObjectGroup, typed(kChairGuid));
    Vm vm(code, {}, &world);
    auto play = [&](ts2::sim::SimObject& sim, const ts2::sim::SimObject& with) {
        sim.interactionPartner = with.id;
        sim.animation.reset();
        sim.thread = {};
        const Outcome outcome = vm.run(sim, kObjectGroup, 0x1000).outcome;
        return outcome == Outcome::Sleeping && sim.animation ? sim.animation->name : std::string("error");
    };
    CHECK(play(adult, teen) == "a2t-soc-admire");
    CHECK(play(elder, teen) == "a2t-soc-admire");   // e2t- missing: as an adult
    // Animate Sim plays on twice its priority's layer (low 0 .. highest 6, above that taken as highest).
    CHECK(adult.animation && adult.animation->layer == 0);
    for (const auto& [priority, layer] : {std::pair{2, 4}, std::pair{9, 6}}) {
        code.add(kObjectGroup, 0x1001, bhav({[&] { auto in = animate; in.operands[12] = static_cast<std::uint8_t>(priority); return in; }()}));
        adult.interactionPartner = teen.id;
        adult.thread = {};
        adult.animation.reset();
        Vm fresh(code, {}, &world);
        CHECK(fresh.run(adult, kObjectGroup, 0x1001).outcome == Outcome::Sleeping && adult.animation && adult.animation->layer == layer);
    }
    CHECK(play(teen, elder) == "a2a-soc-admire");   // t2e-, a2e-, t2a- missing: both as adults
    CHECK(play(adult, chair) == "error");           // not a person
    adult.interactionPartner = 0;
    CHECK(vm.run(adult, kObjectGroup, 0x1000).outcome == Outcome::Error);
    world.setAnimationLengths([](const std::string&) -> std::optional<float> { return 1000.0f; });
    CHECK(play(elder, adult) == "e2a-soc-admire");  // an elder's own when the game has it

    // Animate Overlay laid on another Sim names the clip for that Sim and its partner: a teen leading a
    // child's dance lays c2a- on the child (no c2t- clip, and a child never plays an adult's).
    Instruction overlay;
    overlay.opcode = 0x006B;
    overlay.trueTarget = T;
    overlay.falseTarget = E;
    const std::uint8_t bytes[] = {0, 0, 0, 32, 0, 0, owner::kMyData, 0x0B, 0, 0x83, 0xFF, 1, 0, 0, 0, 0x08};
    std::copy(std::begin(bytes), std::end(bytes), overlay.operands.begin());
    code.add(kObjectGroup, 0x1002, bhav({overlay}));
    world.setAnimationLengths([](const std::string& name) -> std::optional<float> {
        if (name == "c2a-soc-admire" || name == "a2c-soc-admire") return 1000.0f;
        return std::nullopt;
    });
    auto& child = person(8);
    child.interactionPartner = teen.id;
    teen.interactionPartner = child.id;
    teen.data[0x0B] = child.id;
    teen.thread = {};
    teen.thread.temps[6] = teen.id;  // byte 15 flag 0x08: the IK object is in temp 6
    Vm lay(code, {}, &world);
    CHECK(lay.run(teen, kObjectGroup, 0x1002).outcome == Outcome::True);
    CHECK(child.overlays.size() == 1 && child.overlays[0].name == "c2a-soc-admire" && child.overlays[0].ikTarget == teen.id);

    // Animate Stop by name names a social clip the same way, so it finds the overlay laid above.
    Instruction stop;
    stop.opcode = 0x006C;
    stop.trueTarget = T;
    stop.falseTarget = E;
    const std::uint8_t stopBytes[] = {0, 0, 0, owner::kMyData, 0x0B, 0, 0x83, 0, 0};
    std::copy(std::begin(stopBytes), std::end(stopBytes), stop.operands.begin());
    code.add(kObjectGroup, 0x1003, bhav({stop}));
    teen.thread = {};
    Vm stopper(code, {}, &world);
    CHECK(stopper.run(teen, kObjectGroup, 0x1003).outcome == Outcome::True);
    CHECK(child.overlays.empty());
}

// Animate Sim: plays the named animation from the object's list (else the global one) and waits
// until it has played by the world's clock, at its speed. Animate Object, Overlay and Stop go on at once.
void animateSimWaitsForItsAnimation() {
    auto animate = [](std::uint16_t entry, std::uint8_t list, std::uint8_t speed) {
        Instruction in;
        in.opcode = 0x006A;
        in.trueTarget = T;
        in.falseTarget = E;
        in.operands[0] = static_cast<std::uint8_t>(entry);
        in.operands[3] = speed;
        in.operands[6] = list;
        return in;
    };
    MemoryCodeSource code;
    code.addStrings(kObjectGroup, 0x81, {"", "a2o-shower-getOut-E"});
    code.addStrings(ts2::sim::kGlobalGroup, 0x81, {"a-noExpression", "a-blink", "a-idle-neutral-armsSide-wtC-breatheLoop"});
    Instruction getOut = animate(1, 0x81, 32);
    getOut.trueTarget = 1;
    code.add(kObjectGroup, 0x1000, bhav({
        getOut,
        expr(owner::kTemp, 0, op::kAssign, owner::kLiteral, 1, T, E),
    }));
    code.add(kObjectGroup, 0x1001, bhav({animate(2, 0x81, 64)}));
    Instruction overlay;
    overlay.opcode = 0x006B;
    overlay.trueTarget = T;
    overlay.falseTarget = E;
    overlay.operands[0] = 1;
    overlay.operands[3] = 32;
    overlay.operands[6] = 0x03;  // on "my object id"
    overlay.operands[7] = 0x0B;
    overlay.operands[9] = 0x81;
    code.add(kObjectGroup, 0x1002, bhav({overlay}));

    ts2::sim::World world;
    world.setAnimationLengths([](const std::string& name) -> std::optional<float> {
        if (name == "a2o-shower-getOut-E") return 1966.0f;
        if (name == "a-idle-neutral-armsSide-wtC-breatheLoop") return 3166.0f;
        return std::nullopt;
    });
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);

    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::Sleeping);
    CHECK(sim.animation && sim.animation->name == "a2o-shower-getOut-E");
    CHECK(std::fabs(sim.animation->fadeInMs - 233.33f) < 1e-3f && std::fabs(sim.animation->fadeOutMs - 233.33f) < 1e-3f);
    world.advanceClock(1700);
    CHECK(vm.resume(sim).outcome == Outcome::Sleeping && sim.thread.temps[0] == 0);
    // It goes on once its clip starts fading out, and the clip fades on as the one ending.
    world.advanceClock(40);
    CHECK(vm.resume(sim).outcome == Outcome::True && sim.thread.temps[0] == 1 && !sim.animation);
    CHECK(sim.ending && sim.ending->name == "a2o-shower-getOut-E");

    // From the global list, at double speed: half as long.
    CHECK(vm.run(sim, kObjectGroup, 0x1001).outcome == Outcome::Sleeping);
    CHECK(sim.animation && sim.animation->name == "a-idle-neutral-armsSide-wtC-breatheLoop");
    world.advanceClock(1600);
    CHECK(vm.resume(sim).outcome == Outcome::True);

    CHECK(vm.run(sim, kObjectGroup, 0x1002).outcome == Outcome::True);
    CHECK(sim.overlays.size() == 1 && sim.overlays[0].name == "a2o-shower-getOut-E" && sim.overlays[0].lengthMs == 1966.0f);
    // An entry without a kind is a normal animation, or a carry pose when its clip says so.
    CHECK(sim.overlays[0].category == ts2::content::animation_category::kNoCategory);
    world.setAnimationCarries([](const std::string& name) { return name == "a2o-shower-getOut-E"; });
    CHECK(vm.run(sim, kObjectGroup, 0x1002).outcome == Outcome::True);
    CHECK(sim.overlays.back().category == ts2::content::animation_category::kCarry);
    world.setAnimationCarries([](const std::string&) { return false; });
    CHECK(vm.run(sim, kObjectGroup, 0x1002).outcome == Outcome::True);
    CHECK(sim.overlays.back().category == ts2::content::animation_category::kNormal);
    sim.overlays.resize(1);
    CHECK(sim.overlays[0].plays == 1 && sim.overlays[0].weight == 0.0f && sim.overlays[0].priority == 0);

    // Played as many times as temp 1 says (-1: until stopped, so it never tells its events), at a
    // weight in hundredths and a priority.
    overlay.operands[2] = 0x20;
    overlay.operands[10] = 50;
    overlay.operands[11] = 9;
    code.add(kObjectGroup, 0x1003, bhav({overlay}));
    sim.thread.temps[1] = 2;
    CHECK(vm.run(sim, kObjectGroup, 0x1003).outcome == Outcome::True);
    CHECK(sim.overlays.back().plays == 2 && sim.overlays.back().lengthMs == 2 * 1966.0f && sim.overlays.back().weight == 0.5f &&
          sim.overlays.back().priority == 4);
    sim.thread.temps[1] = 0;
    CHECK(vm.run(sim, kObjectGroup, 0x1003).outcome == Outcome::Error);
    sim.thread.temps[1] = -1;
    CHECK(vm.run(sim, kObjectGroup, 0x1003).outcome == Outcome::True && sim.overlays.back().plays == -1);
    sim.overlays.erase(sim.overlays.begin(), sim.overlays.end() - 1);
    sim.overlays.back().eventTree = 0x1000;
    world.advanceClock(100000);
    ts2::sim::tellOverlayEvents(world, vm, sim.id, [](const ts2::sim::RunResult&) {});
    CHECK(sim.overlays.size() == 1 && !sim.overlays[0].blendingOut);
}

// Animate Overlay list 0x80 reads my age's list in the global group only, else the adult list when
// my age's clip doesn't exist.
void animateOverlaySpecialListUsesTheGlobalAgeLists() {
    Instruction overlay;
    overlay.opcode = 0x006B;
    overlay.trueTarget = T;
    overlay.falseTarget = E;
    overlay.operands[3] = 1;
    overlay.operands[6] = 0x03;
    overlay.operands[7] = 0x0B;
    overlay.operands[9] = 0x80;
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x81, {"a-blink"});
    code.addStrings(ts2::sim::kGlobalGroup, 0x8A, {"t-blink"});
    code.addStrings(ts2::sim::kGlobalGroup, 0x82, {"c-blink"});
    code.addStrings(kObjectGroup, 0x80, {"own-list"});
    code.add(kObjectGroup, 0x1000, bhav({overlay}));
    ts2::sim::World world;
    world.setAnimationLengths([](const std::string& name) -> std::optional<float> {
        if (name == "a-blink" || name == "t-blink") return 500.0f;
        return std::nullopt;
    });
    Vm vm(code, {}, &world);
    const auto plays = [&](std::int16_t age) {
        auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
        sim.personData[0x3A] = age;
        vm.run(sim, kObjectGroup, 0x1000);
        return sim.overlays.empty() ? std::string() : sim.overlays.back().name;
    };
    CHECK(plays(16) == "t-blink");
    CHECK(plays(5) == "a-blink");
    CHECK(plays(30) == "a-blink");
}

// Animate Overlay plays on the object its bytes 6-8 name: a bed's tree lays the bed's clip on the bed
// (here the stack object) while I keep none. Its event tree still runs as me. No such object is an error.
void animateOverlayPlaysOnTheObjectItNames() {
    Instruction overlay;
    overlay.opcode = 0x006B;
    overlay.trueTarget = T;
    overlay.falseTarget = E;
    overlay.operands[3] = 32;
    overlay.operands[4] = 0x01;  // event tree 0x1001
    overlay.operands[5] = 0x10;
    overlay.operands[6] = 0x0A;  // the stack object
    overlay.operands[9] = 0x81;
    MemoryCodeSource code;
    code.addStrings(kObjectGroup, 0x81, {"o2a-dbl-bed-relax-loop-breathe"});
    code.add(kObjectGroup, 0x1000, bhav({overlay}));
    ts2::sim::World world;
    world.setAnimationLengths([](const std::string&) -> std::optional<float> { return 1858.0f; });
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& bed = world.create(kObjectGroup, typed(kFridgeGuid));
    Vm vm(code, {}, &world);
    sim.thread.stackObjectId = bed.id;
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True);
    CHECK(sim.overlays.empty() && bed.overlays.size() == 1 && bed.overlays[0].name == "o2a-dbl-bed-relax-loop-breathe");
    CHECK(bed.overlays[0].caller == sim.id && bed.overlays[0].stackObject == bed.id);
    sim.thread.stackObjectId = 0;
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::Error);
}

// Look At starts a head look at the object it names and notes it in person data 0x29 to 0x2C at once.
// STOP before the head took it cancels it. A speed of 0 is an error.
void lookAtStartsAndStopsAHeadLook() {
    Instruction look;
    look.opcode = 0x006E;
    look.trueTarget = T;
    look.falseTarget = E;
    look.operands[1] = 0x0A;  // the stack object
    look.operands[4] = 22;    // 44 degrees a second each way
    look.operands[5] = 22;
    look.operands[6] = 0xFF;  // held until stopped
    look.operands[7] = 0xFF;
    look.operands[14] = 1;    // its targeting slot 0
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({look}));
    Instruction stop = look;
    stop.operands[0] = 0x01;
    code.add(kObjectGroup, 0x1001, bhav({stop}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& other = world.create(kObjectGroup, typed(0x52525252, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    sim.thread.stackObjectId = other.id;
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True);
    const auto& pd = sim.personData;
    CHECK(pd[0x29] == other.id && pd[0x2A] == 0x1000 && pd[0x2B] == 9901 && pd[0x2C] == -1);
    CHECK(sim.look.following() == 1 && sim.lookTargets.size() == 1 && sim.lookTargets[0].object == other.id);
    CHECK(vm.run(sim, kObjectGroup, 0x1001).outcome == Outcome::True);
    CHECK(sim.look.following() == 0);
    code.add(kObjectGroup, 0x1002, bhav({[&] { Instruction slow = look; slow.operands[4] = 0; return slow; }()}));
    CHECK(vm.run(sim, kObjectGroup, 0x1002).outcome == Outcome::Error);
}

// Animate Overlay byte 15 flag 0x01 flips the overlay by temp 3 in place of byte 2's flag, as a bed's
// clip follows the side the Sim gets in on.
void animateOverlayFlipsFromTemp3() {
    Instruction overlay;
    overlay.opcode = 0x006B;
    overlay.trueTarget = T;
    overlay.falseTarget = E;
    overlay.operands[2] = 0x01;
    overlay.operands[3] = 32;
    overlay.operands[6] = 0x03;  // my object id
    overlay.operands[7] = 0x0B;
    overlay.operands[9] = 0x81;
    overlay.operands[15] = 0x01;
    MemoryCodeSource code;
    code.addStrings(kObjectGroup, 0x81, {"o2a-dbl-bed-relax-getin-start"});
    code.add(kObjectGroup, 0x1000, bhav({overlay}));
    ts2::sim::World world;
    world.setAnimationLengths([](const std::string&) -> std::optional<float> { return 3041.0f; });
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    sim.thread.temps[3] = 0;
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True);
    sim.thread.temps[3] = 1;
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True);
    CHECK(sim.overlays.size() == 2 && !sim.overlays[0].flipped && sim.overlays[1].flipped);
}

// Animate Overlay's object list 0x86 is read from my age's object list first, as a child's bed
// covers play the child clip, and from 0x86 itself when that entry names no animation.
void animateOverlayObjectListUsesTheAgeList() {
    Instruction overlay;
    overlay.opcode = 0x006B;
    overlay.trueTarget = T;
    overlay.falseTarget = E;
    overlay.operands[2] = 0x04;  // entry from param 0
    overlay.operands[3] = 32;
    overlay.operands[6] = 0x03;  // my object id
    overlay.operands[7] = 0x0B;
    overlay.operands[9] = 0x86;
    MemoryCodeSource code;
    code.addStrings(kObjectGroup, 0x86, {"o2a-doublebed-sheets-getin", "o2a-doublebed-sheets-getout-stop"});
    code.addStrings(kObjectGroup, 0x192, {"o2c-dbl-getin", "-"});
    code.add(kObjectGroup, 0x1000, bhav({overlay}));
    ts2::sim::World world;
    world.setAnimationLengths([](const std::string& name) -> std::optional<float> {
        if (name == "-") return std::nullopt;
        return 1000.0f;
    });
    Vm vm(code, {}, &world);
    const auto plays = [&](std::int16_t age, std::int16_t entry) {
        auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
        sim.personData[0x3A] = age;
        const std::array<std::int16_t, 4> args{entry, 0, 0, 0};
        vm.run(sim, kObjectGroup, 0x1000, args);
        return sim.overlays.empty() ? std::string() : sim.overlays.back().name;
    };
    CHECK(plays(5, 0) == "o2c-dbl-getin");
    CHECK(plays(5, 1) == "o2a-doublebed-sheets-getout-stop");
    CHECK(plays(30, 0) == "o2a-doublebed-sheets-getin");
}

// Animate Sim list 0x80 names the clip as Animate Overlay's: my age's list in the global group.
void animateSimSpecialListUsesTheGlobalAgeLists() {
    Instruction animate;
    animate.opcode = 0x006A;
    animate.trueTarget = T;
    animate.falseTarget = E;
    animate.operands[0] = 1;
    animate.operands[3] = 32;
    animate.operands[6] = 0x80;
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x81, {"a-noExpression", "a-sit-crossleg"});
    code.addStrings(ts2::sim::kGlobalGroup, 0x82, {"a-noExpression", "c-sit-crossleg"});
    code.addStrings(kObjectGroup, 0x80, {"", "own-list"});
    code.add(kObjectGroup, 0x1000, bhav({animate}));
    ts2::sim::World world;
    world.setAnimationLengths([](const std::string&) -> std::optional<float> { return 500.0f; });
    Vm vm(code, {}, &world);
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.personData[0x3A] = 8;
    vm.run(sim, kObjectGroup, 0x1000);
    CHECK(sim.animation && sim.animation->name == "c-sit-crossleg");
}

// Limb IK: a leg's and an arm's ends land on targets that are only animation channels, and an arm's
// hand takes the target's orientation.
// An overlay tells its event tree, on the object and sharing its temps, when it starts blending out
// (with the percent played) and when it is gone. Then it is dropped.
void overlayEventsReachTheirTree() {
    MemoryCodeSource code;
    // temp 1 += event id, temp 2 += percent, and temp 0 counts the events.
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kTemp, 1, op::kAdd, owner::kParam, 0, 1, E),
        expr(owner::kTemp, 2, op::kAdd, owner::kParam, 2, 2, E),
        expr(owner::kTemp, 0, op::kAdd, owner::kLiteral, 1, T, E),
    }, 5));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    Vm vm(code, {}, &world);
    ts2::sim::PlayingOverlay overlay;
    overlay.name = "a-idle";
    overlay.lengthMs = 1000;
    overlay.startMs = world.clockMs();
    overlay.eventTree = 0x1001;
    overlay.codeOwner = kObjectGroup;
    sim.overlays.push_back(overlay);
    int stops = 0;
    const auto tell = [&] { ts2::sim::tellOverlayEvents(world, vm, sim.id, [&](const ts2::sim::RunResult&) { ++stops; }); };
    world.advanceClock(700);
    tell();
    CHECK(sim.thread.temps[1] == 0 && sim.overlays.size() == 1);
    // Between ticks, at the animation clock's time.
    ts2::sim::tellOverlayEvents(world, vm, sim.id, [&](const ts2::sim::RunResult&) { ++stops; }, 770);
    CHECK(sim.thread.temps[1] == 9997 && sim.thread.temps[2] == 77 && sim.overlays[0].blendingOut);
    sim.thread.temps = {};
    sim.overlays[0].blendingOut = false;
    world.advanceClock(100);
    tell();
    CHECK(sim.thread.temps[1] == 9997 && sim.thread.temps[2] == 80 && sim.thread.temps[0] == 1 && sim.overlays[0].blendingOut);
    tell();
    CHECK(sim.thread.temps[1] == 9997);
    world.advanceClock(200);
    tell();
    CHECK(sim.thread.temps[1] == 9997 + 9998 + 9999 && sim.thread.temps[2] == 280 && sim.thread.temps[0] == 3 && sim.overlays.empty() && stops == 0);
}

// Animate Sim's event tree hears each SimAntics event of its clip once its time has played, in
// order, with (event, the stack object it started with, percent played, 1, 0). The tree comes from
// the starting tree's code owner, and my temps are put back after it.
void animateSimEventsReachTheirTree() {
    Instruction animate;
    animate.opcode = 0x006A;
    animate.trueTarget = T;
    animate.falseTarget = E;
    animate.operands[3] = 32;
    animate.operands[4] = 0x01;
    animate.operands[5] = 0x10;
    animate.operands[6] = 0x81;
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x81, {"a2o-book-get"});
    code.add(kObjectGroup, 0x1000, bhav({animate}));
    // Data 0x20 += event, data 0x21 = the stack object, data 0x22 = percent, and temp 5 = 7.
    code.add(kObjectGroup, 0x1001, bhav({
        expr(owner::kMyData, 0x20, op::kAdd, owner::kParam, 0, 1, E),
        expr(owner::kMyData, 0x21, op::kAssign, owner::kParam, 1, 2, E),
        expr(owner::kMyData, 0x22, op::kAssign, owner::kParam, 2, 3, E),
        expr(owner::kTemp, 5, op::kAssign, owner::kLiteral, 7, T, E),
    }, 5));
    ts2::sim::World world;
    world.setAnimationLengths([](const std::string&) -> std::optional<float> { return 1000.0f; });
    world.setAnimationEvents([](const std::string& name) {
        return name == "a2o-book-get" ? std::vector<ts2::sim::AnimationEvent>{{200, 2}, {600, 5}} : std::vector<ts2::sim::AnimationEvent>{};
    });
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.thread.stackObjectId = 2;
    Vm vm(code, {}, &world);
    int stops = 0;
    const auto tell = [&] { ts2::sim::tellAnimateSimEvents(world, vm, sim.id, [&](const ts2::sim::RunResult&) { ++stops; }, world.clockMs()); };
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::Sleeping);
    sim.thread.stackObjectId = 3;
    world.advanceClock(100);
    tell();
    CHECK(sim.data[0x20] == 0);
    world.advanceClock(150);
    tell();
    CHECK(sim.data[0x20] == 2 && sim.data[0x21] == 2 && sim.data[0x22] == 25 && sim.thread.temps[5] == 0);
    tell();
    CHECK(sim.data[0x20] == 2);
    world.advanceClock(800);
    tell();
    CHECK(sim.data[0x20] == 7 && sim.data[0x22] == 100 && stops == 0);
    // The Animate Sim still ends with its clip.
    CHECK(vm.resume(sim).outcome == Outcome::True && !sim.animation);
}

// Animate Stop: by kind (overlays) and priority (layers 2p-2, 2p-1) at once, or fading out without
// telling the overlay's event tree.
void animateStopEndsOverlaysByKindAndPriority() {
    auto stop = [](std::uint8_t flags, std::uint8_t kind, std::uint8_t priority) {
        Instruction in;
        in.opcode = 0x006C;
        in.trueTarget = T;
        in.falseTarget = F;
        const std::uint8_t bytes[] = {0, 0, flags, owner::kMyData, 0x0B, 0, 0x86, kind, priority};
        std::copy(std::begin(bytes), std::end(bytes), in.operands.begin());
        return in;
    };
    MemoryCodeSource code;
    code.add(kObjectGroup, 0x1000, bhav({stop(0x40, 1, 1)}));
    code.add(kObjectGroup, 0x1001, bhav({stop(0x42, 3, 0)}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    for (const auto& [name, priority] : std::vector<std::pair<std::string, std::uint8_t>>{{"a-low", std::uint8_t{0}}, {"a-high", std::uint8_t{2}}}) {
        ts2::sim::PlayingOverlay overlay;
        overlay.name = name;
        overlay.priority = priority;
        overlay.lengthMs = 3000;
        overlay.eventTree = 0x1234;
        sim.overlays.push_back(overlay);
    }
    Vm vm(code, {}, &world);
    world.advanceClock(1000);
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True);
    CHECK(sim.overlays.size() == 1 && sim.overlays[0].name == "a-high");
    CHECK(vm.run(sim, kObjectGroup, 0x1001).outcome == Outcome::True);
    CHECK(sim.overlays.size() == 1 && sim.overlays[0].stopped && std::fabs(sim.overlays[0].lengthMs - 1233.33f) < 0.1f);
    world.advanceClock(300);
    ts2::sim::tellOverlayEvents(world, vm, sim.id, [](const ts2::sim::RunResult&) {});
    CHECK(sim.overlays.empty());

    // By category (kind 5, idle): only the overlays whose entry says "(idle)" fade out.
    CHECK(ts2::content::animationCategory("a-idle-neutral-armsside-wtc-breatheloop(idle)") == 4);
    CHECK(ts2::content::animationCategory("a-idle-neutral-armsside-wtc-breatheloop") == 1);
    CHECK(ts2::content::animationCategory("c2o-VGS-carry(Carry)") == 3);
    CHECK(ts2::content::animationCategory("x(facial idle)") == 8 && ts2::content::animationCategory("x(nonsense)") == 0);
    code.add(kObjectGroup, 0x1002, bhav({stop(0x42, 5, 0)}));
    for (const auto& [name, category] : std::vector<std::pair<std::string, std::uint8_t>>{{"a-breathe", std::uint8_t{4}}, {"a-wave", std::uint8_t{1}}}) {
        ts2::sim::PlayingOverlay overlay;
        overlay.name = name;
        overlay.category = category;
        overlay.lengthMs = 3000;
        overlay.startMs = world.clockMs();
        sim.overlays.push_back(overlay);
    }
    Vm idleStop(code, {}, &world);
    world.advanceClock(42);
    CHECK(idleStop.run(sim, kObjectGroup, 0x1002).outcome == Outcome::True);
    CHECK(sim.overlays.size() == 2 && sim.overlays[0].stopped && !sim.overlays[1].stopped);
    // Stopped 42 ms into its 233.33 ms fade in, it falls from the share it had then.
    CHECK(sim.overlays[0].stoppedAt && std::fabs(*sim.overlays[0].stoppedAt - 42.0f / 233.33f) < 0.01f);
}

// Animate Stop by name finds the clip as the start does: a teen's entry naming a clip the game
// doesn't have falls back to the adult list, so the adult clip a teen was given stops.
void animateStopByNameFallsBackToTheAdultClip() {
    Instruction stop;
    stop.opcode = 0x006C;
    stop.trueTarget = T;
    stop.falseTarget = F;
    const std::uint8_t bytes[] = {0, 0, 0x40, owner::kMyData, 0x0B, 0, 0x81, 0, 0};
    std::copy(std::begin(bytes), std::end(bytes), stop.operands.begin());
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x81, {"a-idle-breatheLoop50"});
    code.addStrings(ts2::sim::kGlobalGroup, 0x8A, {"t-idle-breatheLoop50"});
    code.add(kObjectGroup, 0x1000, bhav({stop}));
    ts2::sim::World world;
    world.setAnimationLengths([](const std::string& name) -> std::optional<float> {
        if (name == "a-idle-breatheLoop50") return 1000.0f;
        return std::nullopt;
    });
    auto& teen = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    teen.personData[0x3A] = 17;
    ts2::sim::PlayingOverlay overlay;
    overlay.name = "a-idle-breatheLoop50";
    overlay.priority = 1;
    overlay.lengthMs = 3000;
    teen.overlays.push_back(overlay);
    Vm vm(code, {}, &world);
    CHECK(vm.run(teen, kObjectGroup, 0x1000).outcome == Outcome::True);
    CHECK(teen.overlays.empty());
}

// Animate Object plays its clip once, or temp 1 times with byte 2 flag 0x20, where -1 (the
// plumbob's Main) keeps it playing until Animate Stop gives it the time it played. A count of 0 or
// below -1 is an error.
void animateObjectPlaysUntilStopped() {
    auto animate = [](std::uint8_t flags) {
        Instruction in;
        in.opcode = 0x0069;
        in.trueTarget = T;
        in.falseTarget = E;
        const std::uint8_t bytes[] = {0x01, 0x00, flags, 0x20, 0x00, 0x00, owner::kMyData, 0x0B, 0x00, 0x00, 0x10};
        std::copy(std::begin(bytes), std::end(bytes), in.operands.begin());
        return in;
    };
    Instruction stop;
    stop.opcode = 0x006C;
    stop.trueTarget = T;
    stop.falseTarget = E;
    const std::uint8_t stopBytes[] = {0x01, 0x00, 0x00, owner::kMyData, 0x0B, 0x00, 0x86, 0x00};
    std::copy(std::begin(stopBytes), std::end(stopBytes), stop.operands.begin());
    MemoryCodeSource code;
    code.addStrings(kObjectGroup, 0x86, {"o-spin-pose", "o-spin"});
    code.add(kObjectGroup, 0x1000, bhav({animate(0x20)}));
    code.add(kObjectGroup, 0x1001, bhav({animate(0x00)}));
    code.add(kObjectGroup, 0x1002, bhav({stop}));
    ts2::sim::World world;
    world.setAnimationLengths([](const std::string& name) -> std::optional<float> {
        return name == "o-spin" ? std::optional<float>(2316.0f) : std::nullopt;
    });
    auto& bob = world.create(kObjectGroup, definition("Bob", 0));
    Vm vm(code, {}, &world);
    CHECK(vm.run(bob, kObjectGroup, 0x1001).outcome == Outcome::True);
    CHECK(bob.animation && bob.animation->name == "o-spin" && !bob.animation->loops && bob.animation->speed == 1.0f);
    bob.thread.temps[1] = -1;
    CHECK(vm.run(bob, kObjectGroup, 0x1000).outcome == Outcome::True);
    CHECK(bob.animation && bob.animation->loops);
    world.advanceClock(5000);
    CHECK(bob.animation && !bob.animation->finished(world.clockMs()));
    CHECK(vm.run(bob, kObjectGroup, 0x1002).outcome == Outcome::True);
    CHECK(bob.animation && bob.animation->finished(world.clockMs()) && std::fabs(bob.animation->lengthMs - 5000.0f) < 1.0f);
    bob.thread.temps[1] = 3;
    CHECK(vm.run(bob, kObjectGroup, 0x1000).outcome == Outcome::True);
    CHECK(bob.animation && bob.animation->loops && std::fabs(bob.animation->lengthMs - 3 * 2316.0f) < 1.0f);
    for (const std::int16_t bad : {std::int16_t{0}, std::int16_t{-2}}) {
        bob.thread.temps[1] = bad;
        const RunResult r = vm.run(bob, kObjectGroup, 0x1000);
        CHECK(r.outcome == Outcome::Error && r.reason == "Invalid number of loops");
    }
}

// Animate Object byte 10 flag 0x02 reads the entry from the list for the age of the Sim in temp 5
// (a dining chair sliding as a child sits), else from the object list, flag 0x20 from the running tree's lists instead of the
// object's, and flag 0x01 takes the flip from temp 3.
void animateObjectReadsTheSimsList() {
    constexpr std::uint32_t kChairCode = 0x7F000123;
    auto animate = [](std::uint8_t flags) {
        Instruction in;
        in.opcode = 0x0069;
        in.trueTarget = T;
        in.falseTarget = E;
        const std::uint8_t bytes[] = {0x00, 0x00, 0x00, 0x20, 0x00, 0x00, owner::kStackObjectId, 0x00, 0x00, 0x00, flags};
        std::copy(std::begin(bytes), std::end(bytes), in.operands.begin());
        return in;
    };
    MemoryCodeSource code;
    code.addStrings(kChairCode, 0x86, {"o2a-chair-getIn"});
    code.addStrings(kChairCode, 0x192, {"o2c-chair-getIn"});
    code.addStrings(kObjectGroup, 0x86, {"o-tree-list"});
    code.add(kObjectGroup, 0x1000, bhav({animate(0x13)}));
    code.add(kObjectGroup, 0x1001, bhav({animate(0x20)}));
    code.add(kObjectGroup, 0x1002, bhav({animate(0x02)}));
    ts2::sim::World world;
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    auto& chair = world.create(kChairCode, typed(kChairGuid));
    Vm vm(code, {}, &world);
    sim.thread.temps[5] = sim.id;
    sim.thread.temps[3] = 1;
    sim.thread.stackObjectId = chair.id;
    sim.personData[0x3A] = 8;
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True);
    CHECK(chair.animation && chair.animation->name == "o2c-chair-getIn" && chair.animation->flipped && !chair.animation->loops);
    sim.personData[0x3A] = 30;
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True && chair.animation->name == "o2a-chair-getIn");
    sim.personData[0x3A] = 17;  // no teen list: the object list
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::True && chair.animation->name == "o2a-chair-getIn");
    CHECK(vm.run(sim, kObjectGroup, 0x1001).outcome == Outcome::True && chair.animation->name == "o-tree-list" && !chair.animation->flipped);
    sim.thread.temps[5] = chair.id;
    const RunResult notASim = vm.run(sim, kObjectGroup, 0x1002);
    CHECK(notASim.outcome == Outcome::Error && notASim.reason == "Object referred to must be a person.");
}

// A lot saved while a Sim waited in Animate Sim: the saved clip carries on at its saved rate from
// where it got to, not at the speed temp 2 holds now, and events it had played are not told again.
// Another clip starts as its operands say.
void animateSimCarriesOnTheSavedClip() {
    Instruction animate;
    animate.opcode = 0x006A;
    animate.trueTarget = T;
    animate.falseTarget = E;
    animate.operands[2] = 0x02;  // speed from temp 2
    animate.operands[4] = 0x01;
    animate.operands[5] = 0x10;
    animate.operands[6] = 0x81;
    MemoryCodeSource code;
    code.addStrings(ts2::sim::kGlobalGroup, 0x81, {"a-soc-talkListen-side-loop150-agree-wow"});
    code.add(kObjectGroup, 0x1000, bhav({animate}));
    code.add(kObjectGroup, 0x1001, bhav({expr(owner::kMyData, 0x20, op::kAdd, owner::kParam, 0, T, E)}, 1));
    ts2::sim::World world;
    world.setAnimationLengths([](const std::string&) -> std::optional<float> { return 4350.0f; });
    world.setAnimationEvents([](const std::string&) { return std::vector<ts2::sim::AnimationEvent>{{1000, 2}, {3000, 5}}; });
    auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
    sim.thread.temps[2] = 3;
    sim.resumedClip = ts2::sim::SimObject::ResumedClip{"a-soc-talkListen-side-loop150-AGREE-wow", 1.0f, 2000};
    Vm vm(code, {}, &world);
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::Sleeping && !sim.resumedClip);
    CHECK(sim.animation && sim.animation->speed == 1.0f && sim.animation->lengthMs == 4350.0f && sim.animation->startMs == -2000.0);
    world.advanceClock(1100);
    ts2::sim::tellAnimateSimEvents(world, vm, sim.id, [](const ts2::sim::RunResult&) {}, world.clockMs());
    CHECK(sim.data[0x20] == 5);
    world.advanceClock(1250);
    CHECK(vm.resume(sim).outcome == Outcome::True);

    sim.thread = {};
    sim.thread.temps[2] = 16;
    sim.resumedClip = ts2::sim::SimObject::ResumedClip{"a-other", 1.0f, 2000};
    CHECK(vm.run(sim, kObjectGroup, 0x1000).outcome == Outcome::Sleeping && !sim.resumedClip);
    CHECK(sim.animation && sim.animation->speed == 0.5f && sim.animation->startMs == world.clockMs());
}

// An Animate Sim clip fades in and out over 233.33 ms each, both shortened alike to fit a shorter
// clip, and a fade of a millisecond or less is none.
void animateFadesFitTheClip() {
    const auto both = ts2::sim::animateFades(150, true, true);
    CHECK(std::fabs(both.inMs - 74.9925f) < 1e-3f && std::fabs(both.outMs - 74.9925f) < 1e-3f);
    const auto outOnly = ts2::sim::animateFades(150, false, true);
    CHECK(outOnly.inMs == 0 && std::fabs(outOnly.outMs - 149.985f) < 1e-3f);
    const auto tiny = ts2::sim::animateFades(1.5f, true, true);
    CHECK(tiny.inMs == 0 && tiny.outMs == 0);
    const auto none = ts2::sim::animateFades(1000, false, false);
    CHECK(none.inMs == 0 && none.outMs == 0);
}

// Animate Sim's byte 8 flag 0x01 takes the flip from temp 3 instead of byte 2 (the living chair's
// entry, as the game's sit tree plays it).
void animateSimFlipsFromTemp3() {
    const std::uint8_t entry[] = {0x00, 0x00, 0x04, 0x20, 0x46, 0x20, 0x81, 0x01, 0x01, 0x0A, 0x00, 0x00, 0x01, 0xFF, 0x00, 0x00};
    const auto a = ts2::sim::animateOperands(entry);
    CHECK(a.flipFromTemp && !a.flipped && a.entryFromParam && a.priority == 1);
}

} // namespace

std::vector<Test> vmAnimationTests() {
    return {
        {"Animate Overlay list 0x80 uses the global age lists", animateOverlaySpecialListUsesTheGlobalAgeLists},
        {"Animate Overlay plays on the object it names", animateOverlayPlaysOnTheObjectItNames},
        {"Animate Overlay flips from temp 3", animateOverlayFlipsFromTemp3},
        {"Look At starts and stops a head look", lookAtStartsAndStopsAHeadLook},
        {"Animate Overlay object list uses the age list", animateOverlayObjectListUsesTheAgeList},
        {"Animate Sim list 0x80 uses the global age lists", animateSimSpecialListUsesTheGlobalAgeLists},
        {"social animations are named for both Sims", socialAnimationsAreNamedForBothSims},
        {"Animate Sim waits for its animation", animateSimWaitsForItsAnimation},
        {"Overlay events reach their tree", overlayEventsReachTheirTree},
        {"Animate Sim events reach their tree", animateSimEventsReachTheirTree},
        {"Animate Stop ends overlays by kind and priority", animateStopEndsOverlaysByKindAndPriority},
        {"Animate Stop by name falls back to the adult clip", animateStopByNameFallsBackToTheAdultClip},
        {"Animate Sim flips from temp 3", animateSimFlipsFromTemp3},
        {"Animate Sim clips fade to fit", animateFadesFitTheClip},
        {"Animate Sim carries on the saved clip", animateSimCarriesOnTheSavedClip},
        {"Animate Object plays once, a count from temp 1, or until stopped", animateObjectPlaysUntilStopped},
        {"Animate Object reads the Sim's list", animateObjectReadsTheSimsList},
    };
}
