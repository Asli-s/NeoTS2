#include "formats/ngbh.hpp"
#include "sim/clock.hpp"
#include "sim/code_source.hpp"
#include "sim/motive_update.hpp"
#include "sim/object_reset.hpp"
#include "sim/overlay_events.hpp"
#include "sim/vm.hpp"
#include "sim/world.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// SimAntics code made of the input run by the VM as a lot runs it: private, semi-global and global
// trees of any primitive (0x00-0x7F) and calls between them, with any operands, exits and counts,
// constants and strings for any table, and four object types (a Sim among them) whose functions and
// interactions are those trees. A small world of objects and two Sims then runs a few ticks.

namespace {

using ts2::bhav::Behavior;
using ts2::bhav::Instruction;
namespace sim = ts2::sim;

constexpr std::uint32_t kObjectGroup = 0x7F000001;
constexpr std::uint32_t kSemiGroup = 0x7F000002;
constexpr std::uint32_t kSimGroup = 0x7F000099;
constexpr std::uint32_t kControllerGuid = 0x0BADF00D, kFridgeGuid = 0x6C50BA66, kChairGuid = 0x12345678, kSimGuid = 0x51515151;
// Fuzzed trees mostly loop until the step limit, so it bounds a run's time.
constexpr std::size_t kMaxStepsPerRun = 400;
constexpr std::size_t kMaxTicks = 12;

class Input {
public:
    Input(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    std::uint8_t byte() { return at_ < size_ ? data_[at_++] : 0; }
    std::uint16_t word() { return static_cast<std::uint16_t>(byte() | byte() << 8); }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t at_ = 0;
};

// The trees by instance: privates from 0x1000, semi-globals from 0x2000, globals from 0x100.
struct Trees {
    std::size_t privates = 0, semis = 0, globals = 0;

    std::uint16_t call(Input& in) const {
        const std::uint8_t kind = in.byte();
        const std::uint8_t which = in.byte();
        if (kind < 0xA0) {
            return static_cast<std::uint16_t>(0x1000 + which % std::max<std::size_t>(privates, 1));
        }
        if (kind < 0xD8) {
            return static_cast<std::uint16_t>(0x2000 + which % std::max<std::size_t>(semis, 1));
        }
        return static_cast<std::uint16_t>(0x100 + which % std::max<std::size_t>(globals, 1));
    }
};

// Mostly a later node, so fuzzed trees run through their code instead of looping until the step
// limit. One pick in eight may go back.
std::uint16_t target(Input& in, std::size_t at, std::size_t count) {
    const std::uint8_t b = in.byte();
    if (b < 0xC0 && at + 1 < count) {
        return static_cast<std::uint16_t>(at + 1 + b % (count - at - 1));
    }
    if (b < 0xE0) {
        return static_cast<std::uint16_t>(b % count);
    }
    return b < 0xEC ? ts2::bhav::kTargetTrue : b < 0xF8 ? ts2::bhav::kTargetFalse : ts2::bhav::kTargetError;
}

Instruction instruction(Input& in, const Trees& trees, std::size_t at, std::size_t count) {
    Instruction ins;
    const std::uint8_t kind = in.byte();
    ins.opcode = kind < 0xC0 ? kind & 0x7F : trees.call(in);
    ins.trueTarget = target(in, at, count);
    ins.falseTarget = target(in, at, count);
    for (std::size_t i = 0; i < 8; ++i) {
        ins.operands[i] = in.byte();
    }
    const std::uint8_t more = in.byte();
    if (more & 1) {
        for (std::size_t i = 8; i < ins.operands.size(); ++i) {
            ins.operands[i] = in.byte();
        }
    }
    ins.nodeVersion = (more >> 1) & 3;
    return ins;
}

class Code : public sim::CodeSource {
public:
    explicit Code(Input& in) {
        trees_.privates = in.byte() % 6 + 1;
        trees_.semis = in.byte() % 4;
        trees_.globals = in.byte() % 4;
        auto readTrees = [&](std::uint16_t first, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) {
                behaviors_[static_cast<std::uint16_t>(first + i)] = std::make_shared<const Behavior>(tree(in));
            }
        };
        readTrees(0x1000, trees_.privates);
        readTrees(0x2000, trees_.semis);
        readTrees(0x100, trees_.globals);

        for (std::size_t t = in.byte() % 6; t > 0; --t) {
            std::vector<std::int16_t> values(in.byte() % 10);
            for (auto& v : values) {
                v = static_cast<std::int16_t>(in.word());
            }
            constants_.push_back(std::move(values));
        }
        for (std::size_t s = in.byte() % 8; s > 0; --s) {
            std::string text(in.byte() % 12, '\0');
            for (auto& c : text) {
                c = static_cast<char>(in.byte());
            }
            strings_.push_back(std::move(text));
        }
        std::size_t named = 0;
        for (auto& [instance, b] : behaviors_) {
            if (!strings_.empty()) {
                auto renamed = std::make_shared<Behavior>(*b);
                renamed->name = strings_[named++ % strings_.size()];
                b = std::move(renamed);
            }
        }

        for (const auto& [group, guid, type] : {std::tuple{kObjectGroup, kControllerGuid, 4}, std::tuple{kObjectGroup, kFridgeGuid, 4},
                                                std::tuple{kObjectGroup, kChairGuid, 4}, std::tuple{kSimGroup, kSimGuid, int{sim::kPersonType}}}) {
            objectType(in, group, guid, static_cast<std::uint16_t>(type));
        }
        for (std::size_t i = in.byte() % 4; i > 0; --i) {
            ts2::ttab::Interaction line;
            line.action = trees_.call(in);
            line.test = in.byte() & 1 ? trees_.call(in) : 0;
            line.flags = in.word();
            line.autonomy = in.byte();
            table_.interactions.push_back(std::move(line));
        }
    }

    const sim::ObjectType& type(std::uint32_t guid) const { return types_.at(guid); }

    std::shared_ptr<const Behavior> behavior(std::uint32_t group, std::uint16_t instance) override {
        const bool global = instance < 0x1000;
        if (global != (group == sim::kGlobalGroup)) {
            return nullptr;
        }
        const auto it = behaviors_.find(instance);
        return it != behaviors_.end() ? it->second : nullptr;
    }
    const std::vector<std::int16_t>* constants(std::uint32_t, std::uint16_t instance) override {
        return constants_.empty() ? nullptr : &constants_[instance % constants_.size()];
    }
    std::uint32_t semiGlobalGroup(std::uint32_t) override { return kSemiGroup; }
    const sim::ObjectType* objectType(std::uint32_t guid) override {
        const auto it = types_.find(guid);
        return it != types_.end() ? &it->second : nullptr;
    }
    const std::vector<std::uint32_t>& objectTypeGuids() override { return guids_; }
    std::optional<std::string> string(std::uint32_t, std::uint16_t instance, std::size_t index) override {
        if (strings_.empty() || index >= strings_.size()) {
            return std::nullopt;
        }
        return strings_[(instance + index) % strings_.size()];
    }
    std::optional<std::string> interactionString(std::uint32_t group, std::uint16_t instance, std::size_t index) override {
        return string(group, instance, index);
    }
    std::optional<std::uint16_t> behaviorByName(std::uint32_t, const std::string& name) override {
        for (const auto& [instance, b] : behaviors_) {
            if (b->name == name) {
                return instance;
            }
        }
        return std::nullopt;
    }
    std::shared_ptr<const ts2::ttab::InteractionTable> interactionTable(std::uint32_t, std::uint16_t) override {
        return std::shared_ptr<const ts2::ttab::InteractionTable>(std::shared_ptr<void>(), &table_);
    }
    const std::vector<ts2::objf::Function>* objectFunctions(const sim::ObjectType& type) override {
        const auto it = functions_.find(type.definition.guid());
        return it != functions_.end() ? &it->second : nullptr;
    }

private:
    Behavior tree(Input& in) const {
        Behavior b;
        b.format = 0x8009;
        b.argCount = in.byte() % 9;
        b.localCount = in.byte() % 9;
        const std::size_t count = in.byte() % 16 + 1;
        for (std::size_t i = 0; i < count; ++i) {
            b.instructions.push_back(instruction(in, trees_, i, count));
        }
        return b;
    }

    // Main (entry 1) and a few other functions are fuzzed trees, and some OBJD fields are fuzzed too.
    void objectType(Input& in, std::uint32_t group, std::uint32_t guid, std::uint16_t kind) {
        ts2::objd::ObjectDefinition d;
        d.fields.assign(ts2::objd::kMaxFields, 0);
        for (std::size_t i = in.byte() % 6; i > 0; --i) {
            d.fields[in.byte() % ts2::objd::kMaxFields] = in.word();
        }
        d.fields[ts2::objd::field::kGuid1] = static_cast<std::uint16_t>(guid);
        d.fields[ts2::objd::field::kGuid2] = static_cast<std::uint16_t>(guid >> 16);
        d.fields[ts2::objd::field::kType] = kind;
        d.fields[ts2::objd::field::kObjectArrayCount] %= 5;
        sim::ObjectType type{group, std::move(d)};
        type.containerSlots = in.byte() % 3;
        type.containerSlotHeights.assign(type.containerSlots, 0.5f);

        std::vector<ts2::objf::Function> functions(0x30);
        functions[1].action = trees_.call(in);
        for (std::size_t i = in.byte() % 6; i > 0; --i) {
            auto& f = functions[in.byte() % functions.size()];
            f.action = trees_.call(in);
            f.check = in.byte() & 1 ? trees_.call(in) : 0;
        }
        functions_[guid] = std::move(functions);
        types_.emplace(guid, std::move(type));
        guids_.push_back(guid);
    }

    Trees trees_;
    std::map<std::uint16_t, std::shared_ptr<const Behavior>> behaviors_;
    std::vector<std::vector<std::int16_t>> constants_;
    std::vector<std::string> strings_;
    std::map<std::uint32_t, sim::ObjectType> types_;
    std::map<std::uint32_t, std::vector<ts2::objf::Function>> functions_;
    std::vector<std::uint32_t> guids_;
    ts2::ttab::InteractionTable table_;
};

std::shared_ptr<sim::SavedNeighborhood> neighborhood() {
    auto record = std::make_shared<ts2::ngbh::Neighborhood>();
    ts2::xobj::Token token;
    token.guid = 4242;
    token.properties = {1, 2};
    record->neighbors[8].singular = {token};
    auto hood = std::make_shared<sim::SavedNeighborhood>();
    hood->record = record;
    hood->neighborIds = {8, 9};
    hood->neighbors[8].guid = kSimGuid;
    hood->neighbors[8].personData.assign(231, 1);
    hood->neighbors[9].guid = kSimGuid;
    hood->neighbors[9].personData.assign(231, 2);
    return hood;
}

// The world as a lot simulation runs it, one tick at a time (see LotSimulation::tick).
class Lot {
public:
    explicit Lot(Code& code) : code_(code), vm_(code, {kMaxStepsPerRun, 64}, &world_), motives_(code, 0) {
        world_.setNeighborhood(neighborhood());
        for (const std::uint32_t guid : {kControllerGuid, kFridgeGuid, kChairGuid, kSimGuid, kFridgeGuid, kSimGuid}) {
            const sim::ObjectType& type = code.type(guid);
            world_.create(type.group, type.definition, type.containerSlots);
        }
        for (const std::int16_t id : ids()) {
            start(id);
        }
    }

    void tick() {
        world_.advanceClock(1000.0 / 23.83);
        const auto now = ids();
        for (const std::int16_t id : now) {
            sim::tellOverlayEvents(world_, vm_, id, [](const sim::RunResult&) {}, world_.clockMs());
        }
        for (const std::int16_t id : now) {
            step(id);
            runTimers(id);
        }
        world_.purge();
        sim::advanceClock(vm_.globals);
        world_.countTick();
        ++ticks_;
    }

    sim::Vm& vm() { return vm_; }

    // What the run left: the globals and every object's id, data and temps.
    std::vector<std::int32_t> state() {
        std::vector<std::int32_t> out(vm_.globals.begin(), vm_.globals.end());
        world_.nextAfter(0, [&](const sim::SimObject& o) {
            out.push_back(o.id);
            out.insert(out.end(), o.data.begin(), o.data.end());
            out.insert(out.end(), o.thread.temps.begin(), o.thread.temps.end());
            return false;
        });
        return out;
    }

private:
    std::vector<std::int16_t> ids() {
        std::vector<std::int16_t> out;
        world_.nextAfter(0, [&](const sim::SimObject& o) {
            out.push_back(o.id);
            return false;
        });
        return out;
    }

    // A reset and Init, as when an object is placed. A Sim gets its Main tree back.
    void start(std::int16_t id) {
        sim::SimObject* o = world_.find(id);
        const sim::ObjectType* type = o != nullptr ? code_.objectType(o->definition.guid()) : nullptr;
        if (type == nullptr) {
            return;
        }
        if (o->isPerson()) {
            sim::resetSim(*o, *type, code_);
            return;
        }
        sim::resetObject(*o, *type, code_);
        const auto* functions = code_.objectFunctions(*type);
        if (functions != nullptr && (*functions)[0].action != 0) {
            (void)vm_.runOnce(*o, type->group, (*functions)[0].action);
        }
        if (sim::SimObject* again = world_.find(id); again != nullptr && !again->removed) {
            sim::finishSingleTileReset(*again, *type);
        }
    }

    // Objects whose thread ended start over, so short fuzzed Main trees keep running.
    void step(std::int16_t id) {
        sim::SimObject* o = world_.find(id);
        if (o == nullptr) {
            return;
        }
        o->failedRun.reset();
        if (!o->thread.isRunning()) {
            start(id);
            o = world_.find(id);
            if (o == nullptr || !o->thread.isRunning()) {
                return;
            }
        }
        o->inViewStale = true;
        if (o->thread.sleepTicks > 0) {
            --o->thread.sleepTicks;
            motives_.tick(*o, static_cast<std::int64_t>(ticks_));
            return;
        }
        (void)vm_.resume(*o);
        if (sim::SimObject* ran = world_.find(id); ran != nullptr && !ran->removed) {
            motives_.tick(*ran, static_cast<std::int64_t>(ticks_));
        }
    }

    void runTimers(std::int16_t id) {
        sim::SimObject* owner = world_.find(id);
        if (owner == nullptr || owner->removed || owner->timers.empty()) {
            return;
        }
        const sim::ObjectType* type = code_.objectType(owner->definition.guid());
        const std::size_t count = owner->timers.size();
        for (std::size_t step = 0, i = 0; step < count && i < owner->timers.size(); ++step) {
            sim::ObjectTimer& timer = owner->timers[i];
            if (--timer.countdown >= 1) {
                ++i;
                continue;
            }
            const sim::ObjectTimer fired = timer;
            bool again = false;
            if (type != nullptr) {
                again = vm_.runEvent(*owner, type->group, fired.tree, fired.setter, fired.args).outcome == sim::Outcome::True;
            }
            owner = world_.find(id);
            if (owner == nullptr || owner->removed) {
                return;
            }
            const auto at = std::find_if(owner->timers.begin(), owner->timers.end(), [&](const sim::ObjectTimer& t) { return t.id == fired.id; });
            if (at == owner->timers.end()) {
                continue;
            }
            if (fired.repeats && again) {
                at->countdown = fired.interval;
                ++i;
            } else {
                owner->timers.erase(at);
            }
        }
    }

    Code& code_;
    sim::World world_;
    sim::Vm vm_;
    sim::MotiveUpdate motives_;
    std::size_t ticks_ = 0;
};

std::vector<std::int32_t> run(const std::uint8_t* data, std::size_t size) {
    Input in(data, size);
    const std::size_t ticks = in.byte() % kMaxTicks + 1;
    Code code(in);
    Lot lot(code);
    for (std::size_t i = in.byte() % 8; i > 0; --i) {
        lot.vm().globals[in.byte()] = static_cast<std::int16_t>(in.word());
    }
    for (std::size_t i = 0; i < ticks; ++i) {
        lot.tick();
    }
    return lot.state();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto first = run(data, size);
    // Every fourth input runs again: a different result means memory read before it was written,
    // or state kept from the run before (Windows has no sanitizer for either).
    if (size % 4 == 0 && run(data, size) != first) {
        std::abort();
    }
    return 0;
}
