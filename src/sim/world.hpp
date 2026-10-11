#pragma once

#include "content/wants.hpp"
#include "formats/bhav.hpp"
#include "formats/objd.hpp"
#include "formats/swaf.hpp"
#include "sim/animation.hpp"
#include "sim/data_tables.hpp"
#include "sim/inventory.hpp"
#include "sim/rooms.hpp"

#include <array>
#include <iterator>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ts2::sim {

// Sizes cover the highest index the game's scripts use; the engine's real limits are unknown.
constexpr std::size_t kTempCount = 8;
constexpr std::size_t kAttributeCount = 128;
constexpr std::size_t kSemiAttributeCount = 64;
constexpr std::size_t kObjectDataCount = 128;
constexpr std::size_t kPersonDataCount = 256;
// A neighbor's copy of its person data has exactly this many values in the game.
constexpr std::size_t kNeighborPersonDataCount = 231;
constexpr std::size_t kMotiveCount = 16;
constexpr float kMotiveMin = -100.0f;
constexpr float kMotiveMax = 100.0f;

constexpr std::size_t kObjectIdField = 0x0B;
// Number of object arrays a new object has.
constexpr std::size_t kObjectArrayCountField = objd::field::kObjectArrayCount;
constexpr std::uint16_t kPersonType = 2;

struct Frame {
    std::shared_ptr<const bhav::Behavior> code;
    std::uint32_t group = 0;
    std::uint32_t codeOwner = 0;
    std::uint16_t instance = 0;
    std::uint16_t pc = 0;
    std::vector<std::int16_t> params;
    std::vector<std::int16_t> locals;
    std::int16_t stackObjectId = 0;
    // The object the frame's code runs on: the Sim itself for its own trees, an interaction's
    // object while the Sim runs that interaction. Called trees keep their caller's.
    std::int16_t runningOnId = 0;
    // The caller continues on its true branch when this frame returns, whatever it returns
    // (an interaction started by Idle for Input).
    bool callerContinuesTrue = false;
};

struct ThreadState {
    std::array<std::int16_t, kTempCount> temps{};
    // The stack object a new run starts with (and the bottom frame's when a run ends); while
    // running, each frame keeps its own and a called tree starts with its caller's.
    std::int16_t stackObjectId = 0;
    std::vector<Frame> frames;
    std::uint32_t codeGroup = 0;
    std::optional<bool> pendingResult;
    std::int32_t sleepTicks = 0;

    bool isRunning() const { return !frames.empty(); }
};

// The tile an object stands on, as the game keeps it: the whole parts of the saved x and y
// and the level, one byte each.
class LuaRuntime;

struct Tile {
    std::uint8_t x = 0;
    std::uint8_t y = 0;
    std::uint8_t level = 0;

    bool operator==(const Tile&) const = default;
};

// A change Set Motive Change put on a Sim: the motive moves by perTick each tick until it
// reaches limit (applying it is not done yet).
struct MotiveChange {
    std::uint8_t motive = 0;
    float perTick = 0;
    float limit = 0;
};

constexpr std::int32_t kOffLot = -65536;

// An interaction waiting in a Sim's queue: interaction `index` of an object's interaction table.
struct QueuedInteraction {
    std::int16_t object = 0;
    std::uint32_t group = 0;  // the object's code group
    std::uint16_t index = 0;
    std::uint16_t action = 0;
    std::int16_t priority = 0;
    std::array<std::int16_t, 4> args{};
};

struct SimObject {
    std::int16_t id = 0;
    std::uint32_t group = 0;
    objd::ObjectDefinition definition;
    std::array<std::int16_t, kAttributeCount> attributes{};
    std::array<std::int16_t, kSemiAttributeCount> semiAttributes{};
    std::array<std::int16_t, kObjectDataCount> data{};
    std::array<std::int16_t, kPersonDataCount> personData{};
    std::array<float, kMotiveCount> motives{};
    std::vector<MotiveChange> motiveChanges;
    std::vector<QueuedInteraction> queue;  // a Sim's interaction queue, oldest first
    // What the queue shows for the running interaction (set by Add/Change the Action String).
    std::string actionString;
    std::uint32_t actionIconGuid = 0;
    std::int16_t actionIconObject = 0;
    // Object arrays (Array Operation), each a list of 16-bit values.
    std::vector<std::vector<std::int16_t>> arrays;
    // One iterator (an element index) per array, saved after the arrays.
    std::vector<std::int16_t> arrayIterators;
    // Container slots: the id of the object held in each, 0 when empty.
    std::vector<std::int16_t> slots;
    // Script data tables (ObjectDataTable).
    DataTables dataTables;
    // The object's own token, which Manage Inventory copies tokens into and out of, and where the
    // last token copied in came from.
    InventoryToken token;
    struct TokenSource {
        std::int16_t kind = 0;
        std::int16_t id = 0;
        bool counted = false;
        std::int16_t index = 0;
        std::uint16_t flags = 0;
    } tokenSource;
    // None while the object is not on the lot (new objects, objects saved off the lot).
    std::optional<Tile> tile;
    // Exact position in 1/65536 tiles, as the game keeps it; -1 tile (-65536) when off the lot.
    std::int32_t x = kOffLot;
    std::int32_t y = kOffLot;
    std::int32_t level = 1;  // 1 = ground
    ThreadState thread;
    // What Animate Sim or Animate Object last started on it, while it plays.
    std::optional<PlayingAnimation> animation;
    bool removed = false;

    bool isPerson() const { return definition.field(objd::field::kType) == kPersonType; }
};

// A neighborhood's neighbors and relationships as saved. Relationship values are 16-bit, as the
// Relationship primitive moves them.
struct SavedNeighbor {
    std::uint32_t guid = 0;
    std::vector<std::int16_t> personData;
};

struct SavedNeighborhood {
    std::set<std::int16_t> neighborIds;
    std::map<std::int16_t, SavedNeighbor> neighbors;
    std::set<std::int32_t> lotIds;
    std::map<std::pair<std::int16_t, std::int16_t>, std::vector<std::int16_t>> relationships;
    std::map<std::int16_t, std::vector<swaf::Want>> wants;  // each neighbor's current wants
    std::shared_ptr<const ngbh::Neighborhood> record;      // inventories; null when not saved
    std::map<std::int16_t, DataTables> dataTables;         // each neighbor's script data tables
};

class World {
public:
    SimObject& create(std::uint32_t group, objd::ObjectDefinition definition, std::size_t containerSlots = 0);
    // Recreates a saved object under its saved id; new objects get ids after the highest one.
    SimObject& restore(std::int16_t id, std::uint32_t group, objd::ObjectDefinition definition);
    SimObject* find(std::int16_t id);
    std::size_t size() const { return objects_.size() - removedCount_; }

    // Removal only marks the object: running code may still hold references to it. Lookups and
    // searches stop seeing it at once; purge() frees removed objects when nothing is running.
    void remove(SimObject& object);
    void purge();

    // Relationship variables `from` holds towards `to`; empty until first grown.
    std::vector<std::int16_t>& relationship(std::int16_t from, std::int16_t to) {
        return relationships_[{from, to}];
    }

    // Rooms of the lot this world was built from (empty for worlds made in code).
    void setRooms(RoomMap rooms) { rooms_ = std::move(rooms); }
    const RoomMap& rooms() const { return rooms_; }

    // The neighborhood as saved, shared between worlds (a world keeps its own changes).
    void setNeighborhood(std::shared_ptr<const SavedNeighborhood> hood) {
        hood_ = std::move(hood);
        neighborRelationships_.clear();
        neighborPersonData_.clear();
        neighborWants_.clear();
        neighborDataTables_.clear();
        inventories_.clear();
    }
    // Ids of the neighborhood's neighbors; null until a neighborhood is set.
    const std::set<std::int16_t>* neighbors() const { return hood_ ? &hood_->neighborIds : nullptr; }
    // Lot ids of the neighborhood; null until a neighborhood is set.
    const std::set<std::int32_t>* lotIds() const { return hood_ ? &hood_->lotIds : nullptr; }
    // A neighbor's saved description; null when unknown.
    const SavedNeighbor* neighbor(std::int16_t id) const {
        if (!hood_) {
            return nullptr;
        }
        const auto it = hood_->neighbors.find(id);
        return it != hood_->neighbors.end() ? &it->second : nullptr;
    }

    // Relationship variables neighbor `from` holds towards neighbor `to`. They live on the neighbor,
    // so they outlive the lot: the saved values until changed, empty if none were saved.
    std::vector<std::int16_t>& neighborRelationship(std::int16_t from, std::int16_t to) {
        const std::pair key{from, to};
        auto it = neighborRelationships_.find(key);
        if (it == neighborRelationships_.end()) {
            std::vector<std::int16_t> saved;
            if (hood_) {
                if (const auto s = hood_->relationships.find(key); s != hood_->relationships.end()) {
                    saved = s->second;
                }
            }
            it = neighborRelationships_.emplace(key, std::move(saved)).first;
        }
        return it->second;
    }

    // A neighbor's copy of its person data: the saved values until changed; null for an unknown
    // neighbor.
    std::vector<std::int16_t>* neighborPersonData(std::int16_t id) {
        auto it = neighborPersonData_.find(id);
        if (it == neighborPersonData_.end()) {
            const SavedNeighbor* n = neighbor(id);
            if (n == nullptr) {
                return nullptr;
            }
            std::vector<std::int16_t> values = n->personData;
            values.resize(kNeighborPersonDataCount, 0);
            it = neighborPersonData_.emplace(id, std::move(values)).first;
        }
        return &it->second;
    }

    // A neighbor's script data tables (NeighborDataTable): the saved ones until changed.
    DataTables& neighborDataTables(std::int16_t id) {
        auto it = neighborDataTables_.find(id);
        if (it == neighborDataTables_.end()) {
            DataTables saved;
            if (hood_) {
                if (const auto s = hood_->dataTables.find(id); s != hood_->dataTables.end()) {
                    saved = s->second;
                }
            }
            it = neighborDataTables_.emplace(id, std::move(saved)).first;
        }
        return it->second;
    }

    // One of the neighborhood's inventories (the id picks the lot, family or neighbor): the saved
    // one until changed. Null when the neighborhood has no such inventory. The game-wide inventory
    // is not saved with the neighborhood and starts empty.
    Inventory* inventory(InventoryKind kind, std::int32_t id) {
        const auto key = std::pair{kind, (kind == InventoryKind::Global || kind == InventoryKind::GameWide) ? 0 : id};
        if (auto it = inventories_.find(key); it != inventories_.end()) {
            return &it->second;
        }
        const ngbh::Inventory* saved = nullptr;
        const ngbh::Neighborhood* record = hood_ ? hood_->record.get() : nullptr;
        auto find = [&](const std::map<std::int32_t, ngbh::Inventory>& m) {
            const auto it = m.find(id);
            return it != m.end() ? &it->second : nullptr;
        };
        switch (kind) {
        case InventoryKind::Global:   saved = record ? &record->global : nullptr; break;
        case InventoryKind::Lot:      saved = record ? find(record->lots) : nullptr; break;
        case InventoryKind::Family:   saved = record ? find(record->families) : nullptr; break;
        case InventoryKind::Neighbor: saved = record ? find(record->neighbors) : nullptr; break;
        case InventoryKind::GameWide: break;
        }
        if (saved == nullptr && kind != InventoryKind::Global && kind != InventoryKind::GameWide) {
            return nullptr;
        }
        return &inventories_.emplace(key, saved ? toInventory(*saved) : Inventory{}).first->second;
    }

    // A neighbor's current wants: the saved ones until changed; empty if none were saved.
    std::vector<swaf::Want>& neighborWants(std::int16_t id) {
        auto it = neighborWants_.find(id);
        if (it == neighborWants_.end()) {
            std::vector<swaf::Want> saved;
            if (hood_) {
                if (const auto s = hood_->wants.find(id); s != hood_->wants.end()) {
                    saved = s->second;
                }
            }
            it = neighborWants_.emplace(id, std::move(saved)).first;
        }
        return it->second;
    }

    // The neighbor whose Sim is this object (matched by GUID); 0 when there is none.
    std::int16_t neighborIdOf(const SimObject& sim) const {
        if (!hood_ || !sim.isPerson()) {
            return 0;
        }
        for (const auto& [id, n] : hood_->neighbors) {
            if (n.guid == sim.definition.guid()) {
                return id;
            }
        }
        return 0;
    }

    // The simulation's clock in milliseconds, moved on by whoever runs the world; animations are
    // timed by it.
    double clockMs() const { return clockMs_; }
    void advanceClock(double ms) { clockMs_ += ms; }

    // How long animations last; without it, animations end as soon as they start.
    void setAnimationLengths(AnimationLengths lengths) { animationLengths_ = std::move(lengths); }
    const AnimationLengths& animationLengths() const { return animationLengths_; }

    // The game's Lua state, shared by every world (the game has one).
    void setScripts(std::shared_ptr<LuaRuntime> scripts) { scripts_ = std::move(scripts); }
    LuaRuntime* scripts() const { return scripts_.get(); }

    void setWantDefinitions(std::shared_ptr<const content::WantDefinitions> defs) { wantDefs_ = std::move(defs); }
    bool hasWantDefinitions() const { return wantDefs_ != nullptr; }
    const content::WantDefinition* wantDefinition(std::uint32_t guid) const {
        if (!wantDefs_) {
            return nullptr;
        }
        const auto it = wantDefs_->byGuid.find(guid);
        return it != wantDefs_->byGuid.end() ? &it->second : nullptr;
    }

    template <typename Predicate>
    SimObject* nextAfter(std::int16_t id, Predicate matches) {
        for (auto it = objects_.upper_bound(id); it != objects_.end(); ++it) {
            if (!it->second->removed && matches(*it->second)) {
                return it->second.get();
            }
        }
        return nullptr;
    }

    // Like nextAround, but going down: the first match below `id`, then from the highest id down to
    // `id` itself.
    template <typename Predicate>
    SimObject* previousAround(std::int16_t id, Predicate matches) {
        auto start = objects_.lower_bound(id);
        for (auto it = std::make_reverse_iterator(start); it != objects_.rend(); ++it) {
            if (!it->second->removed && matches(*it->second)) {
                return it->second.get();
            }
        }
        for (auto it = objects_.rbegin(); it != objects_.rend() && it->first >= id; ++it) {
            if (!it->second->removed && matches(*it->second)) {
                return it->second.get();
            }
        }
        return nullptr;
    }

    // Like nextAfter, but wraps around to the lowest id and ends with object `id` itself.
    template <typename Predicate>
    SimObject* nextAround(std::int16_t id, Predicate matches) {
        if (SimObject* next = nextAfter(id, matches)) {
            return next;
        }
        for (auto it = objects_.begin(); it != objects_.end() && it->first <= id; ++it) {
            if (!it->second->removed && matches(*it->second)) {
                return it->second.get();
            }
        }
        return nullptr;
    }

private:
    std::map<std::int16_t, std::unique_ptr<SimObject>> objects_;
    std::map<std::pair<std::int16_t, std::int16_t>, std::vector<std::int16_t>> relationships_;
    RoomMap rooms_;
    std::shared_ptr<const SavedNeighborhood> hood_;
    std::map<std::pair<std::int16_t, std::int16_t>, std::vector<std::int16_t>> neighborRelationships_;
    std::map<std::int16_t, std::vector<std::int16_t>> neighborPersonData_;
    std::map<std::int16_t, std::vector<swaf::Want>> neighborWants_;
    std::map<std::int16_t, DataTables> neighborDataTables_;
    std::map<std::pair<InventoryKind, std::int32_t>, Inventory> inventories_;
    std::shared_ptr<const content::WantDefinitions> wantDefs_;
    std::shared_ptr<LuaRuntime> scripts_;
    double clockMs_ = 0;
    AnimationLengths animationLengths_;
    std::int16_t nextId_ = 1;
    std::size_t removedCount_ = 0;
};

} // namespace ts2::sim
