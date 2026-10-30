#pragma once

#include "content/wants.hpp"
#include "formats/bhav.hpp"
#include "formats/famt.hpp"
#include "formats/neighborhood_id.hpp"
#include "formats/objd.hpp"
#include "formats/swaf.hpp"
#include "formats/weather.hpp"
#include "sim/animation.hpp"
#include "sim/idle_spot.hpp"
#include "sim/look_at.hpp"
#include "sim/data_tables.hpp"
#include "sim/genetics.hpp"
#include "sim/inventory.hpp"
#include "sim/message_subscriptions.hpp"
#include "sim/ray_trace.hpp"
#include "sim/object_timer.hpp"
#include "sim/reaching.hpp"
#include "sim/outside_grid.hpp"
#include "sim/rooms.hpp"
#include "sim/turn.hpp"
#include "sim/walk_plan.hpp"
#include "sim/walking.hpp"
#include "formats/slot.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <climits>
#include <compare>
#include <functional>
#include <iterator>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ts2::sim {

// Sizes cover the highest index the game's scripts use. The engine's real limits are unknown.
constexpr std::size_t kTempCount = 8;
constexpr std::size_t kAttributeCount = 128;
constexpr std::size_t kSemiAttributeCount = 64;
constexpr std::size_t kObjectDataCount = 128;
constexpr std::size_t kPersonDataCount = 256;
constexpr std::size_t kLifeStagePersonData = 0x3A;
// A neighbor's copy of its person data has exactly this many values in the game.
constexpr std::size_t kNeighborPersonDataCount = 231;
constexpr std::size_t kMotiveCount = 16;
constexpr float kMotiveMin = -100.0f;
constexpr float kMotiveMax = 100.0f;

constexpr std::size_t kObjectIdField = 0x0B;
// A multi-tile part's lead (its own id for single-tile objects and leads, as every saved object has).
constexpr std::size_t kLeadTileField = 0x13;
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
    // The caller's Idle for Input runs again when this frame returns (an interaction it started
    // from the queue).
    bool callerIdlesAgain = false;
    // The Idle for Input at pc started an interaction, which has ended when it runs again.
    bool idleStarted = false;
    // The caller runs its instruction again when this frame returns (a turn's or walk's pre-route tree).
    bool callerReruns = false;
    // The Dialog at pc posted a dialog and waits for its answer.
    bool dialogOpen = false;
    // The Make New Character at pc made children and waits for them to be built.
    bool childrenPending = false;
    // How far the Camera Control at pc has got (0 not started).
    std::uint8_t cameraStep = 0;
    // The frame runs the Sim's current interaction, which ends when it returns.
    bool runsInteraction = false;
    // Which one (its QueuedInteraction::id).
    std::uint32_t interactionId = 0;
};

struct ThreadState {
    std::array<std::int16_t, kTempCount> temps{};
    // The stack object a new run starts with (and the bottom frame's when a run ends). While
    // running, each frame keeps its own and a called tree starts with its caller's.
    std::int16_t stackObjectId = 0;
    std::vector<Frame> frames;
    std::uint32_t codeGroup = 0;
    std::optional<bool> pendingResult;
    std::int32_t sleepTicks = 0;
    // The step count as this thread raised the step limit. Lowering it puts the count back to this,
    // so steps run under the raised limit don't count (see Limits::maxRaisedSteps).
    std::size_t stepsAtRaise = 0;

    bool isRunning() const { return !frames.empty(); }
};

// The tile an object stands on, as the game keeps it: the whole parts of the saved x and y
// and the level, one byte each.
class LuaRuntime;

// A joint of an object's model at rest: its place in the model (model units are tiles) and its turn
// about the vertical axis in radians.
struct ModelBone {
    float x = 0;
    float y = 0;
    float yaw = 0;
};

struct Tile {
    std::uint8_t x = 0;
    std::uint8_t y = 0;
    std::uint8_t level = 0;

    auto operator<=>(const Tile&) const = default;
};

// A change Set Motive Change put on a Sim: the motive moves by perTick each tick until it
// reaches limit.
struct MotiveChange {
    std::uint8_t motive = 0;
    float perTick = 0;
    float limit = 0;
};

constexpr std::int32_t kOffLot = -65536;

// The queue icon an interaction gets before it is queued (a pie menu line, autonomy's pick): picture
// `index` of the object type `guid` when that is set, else the object `object` (0: the one the
// interaction is on).
struct InteractionIcon {
    std::int16_t object = 0;
    std::uint32_t guid = 0;
    std::uint32_t index = 0;
};

// An interaction waiting in a Sim's queue: interaction `index` of an object's interaction table.
struct QueuedInteraction {
    std::int16_t object = 0;
    std::uint32_t group = 0;  // the object's code group
    std::uint16_t index = 0;
    std::uint16_t action = 0;
    std::int16_t priority = 0;
    std::array<std::int16_t, 4> args{};
    // Who it is with (the game's second object of an interaction): the object, unless Push
    // Interaction names another. 0 means the object. Social animations are named for it.
    std::int16_t partner = 0;
    // Who queued it: a script's push, the player's pie menu, or a push autonomy chose (for the log).
    enum class Source : std::uint8_t { Script, Player, Autonomy };
    Source source = Source::Script;
    // Whether the game counts it as autonomous ("Interaction Autonomous", person data 0xA6, while
    // it runs): autonomy's own pick, or a push with byte 3 bit 3.
    bool autonomous = false;
    // A continuation (Push Interaction byte 3 bit 2): it starts as soon as the interaction before
    // it ends, before the Sim's main loop runs again.
    bool continuation = false;
    // The queue shows picture `iconIndex` of the object type `iconGuid` when it is set, else of
    // icon(). With neither it leaves the interaction out.
    std::uint32_t iconGuid = 0;
    std::uint32_t iconIndex = 0;
    std::int16_t iconObject = 0;
    // Push Interaction named the Sim it is with, but no object has that id. Then it has no icon
    // object, as with the pregnancy's hidden "*" interactions.
    bool noIconObject = false;
    // The object whose picture the queue shows: `iconObject`, else `partner`, else `object`.
    // 0 for none.
    std::int16_t icon() const {
        return iconObject != 0 ? iconObject : noIconObject ? std::int16_t{0} : partner != 0 ? partner : object;
    }
    // Interaction table flag 0x8000: the queue shows it in a column under its first icon.
    bool nested = false;
    // The player cancelled it while it ran. The queue shows it crossed out until it ends.
    bool cancelled = false;
    // Tells it from the Sim's other interactions while it waits and runs (1 and up, see
    // SimObject::nextInteractionId).
    std::uint32_t id = 0;
    // Its name in the queue's tooltip: the pie menu line it was chosen from. Empty for its
    // interaction table line's.
    std::string name;
};

// A material Change Material put on a mesh group of an object's model (an empty group: the whole model).
struct MaterialChange {
    std::string group;
    std::string material;
    bool operator==(const MaterialChange&) const = default;
};

struct SimObject {
    std::int16_t id = 0;
    // How strongly its lights shine (set by Change Light, 1 until a script sets it). Not saved.
    float lightStrength = 1;
    // Change Material's changes in the order they apply. One for the whole model replaces all
    // before it. `materialVersion` goes up whenever they change. Not saved yet.
    std::vector<MaterialChange> materialChanges;
    std::uint32_t materialVersion = 0;
    // Its recolor's materials, entry n + 1 standing in for entry n of its material names (string
    // table 0x88) when not empty. Read from the lot's save.
    std::vector<std::string> materialNames;
    std::uint32_t group = 0;
    objd::ObjectDefinition definition;
    std::array<std::int16_t, kAttributeCount> attributes{};
    std::array<std::int16_t, kSemiAttributeCount> semiAttributes{};
    std::array<std::int16_t, kObjectDataCount> data{};
    std::array<std::int16_t, kPersonDataCount> personData{};
    std::array<float, kMotiveCount> motives{};
    std::vector<MotiveChange> motiveChanges;
    // The tick a Sim's motives last decayed (they decay every 60 ticks). Not saved.
    std::int64_t motivesUpdatedAt = 0;
    std::vector<QueuedInteraction> queue;  // a Sim's interaction queue, oldest first
    // The running interaction's name in the queue's tooltip: its own when it starts, then what
    // Add/Change the Action String sets (empty: its interaction table line's).
    std::string actionString;
    // Who the interaction it runs is with (see QueuedInteraction::partner). Not saved yet.
    std::int16_t interactionPartner = 0;
    std::uint32_t actionIconGuid = 0;
    std::uint32_t actionIconIndex = 0;
    std::int16_t actionIconObject = 0;
    // The interaction the Sim runs, from when Idle for Input or Gosub Found Action starts it until
    // its tree returns. Its icon is the action icon above. Not saved yet.
    std::optional<QueuedInteraction> running;
    // The one Idle for Input started at once (operand byte 4 bit 0) on top of it, until its tree
    // returns, with its own name and icon. Not saved yet.
    std::optional<QueuedInteraction> runningSub;
    // The id the Sim's last queued interaction got.
    std::uint32_t nextInteractionId = 0;
    // The queue icon of autonomy's last pick, which Gosub Found Action gives the interaction it
    // starts. Not saved yet.
    InteractionIcon foundIcon;
    // Its name (empty: its line's) and the params its action tree runs with. Not saved yet.
    std::string foundName;
    std::array<std::int16_t, 4> foundParams{};
    // Object arrays (Array Operation), each a list of 16-bit values.
    std::vector<std::vector<std::int16_t>> arrays;
    // One iterator (an element index) per array, saved after the arrays.
    std::vector<std::int16_t> arrayIterators;
    // Container slots: the id of the object held in each, 0 when empty.
    std::vector<std::int16_t> slots;
    // A Sim's objects in view for Set to Next 0x18. Each tick the Sim runs marks the list stale. The
    // next search from the start then makes it again, and later searches walk the same list.
    std::vector<std::int16_t> inView;
    bool inViewStale = true;
    // The tick a Sim last looked for something to do by itself (Find Best Interaction runs once a tick).
    std::optional<std::uint64_t> autonomyTick;
    // Script data tables (ObjectDataTable).
    DataTables dataTables;
    // The object's own token, which Manage Inventory copies tokens into and out of, and where the
    // last token copied in came from. Trees read and write these values as data owner 0x27
    // (Temporary Token), fields 0-7 in this order, and the object saves them.
    InventoryToken token;
    struct TokenSource {
        std::int16_t kind = 0;
        std::int16_t id = 0;
        std::int16_t counted = 0;  // 0 singular, anything else counted
        std::int16_t index = 0;    // counted from 0
        std::int16_t guidLow = 0;
        std::int16_t guidHigh = 0;
        std::int16_t flags = 0;    // the token's flags when copied in
        std::int16_t propertyCount = 0;
    } tokenSource;
    // None while the object is not on the lot (new objects, objects saved off the lot).
    std::optional<Tile> tile;
    // Exact position in 1/65536 tiles, as the game keeps it. -1 tile (-65536) when off the lot.
    std::int32_t x = kOffLot;
    std::int32_t y = kOffLot;
    std::int32_t level = 1;  // 1 = ground
    ThreadState thread;
    // The outfit Change Outfit last asked a Sim to wear: a category (in the scripts' numbering, as
    // person data 0x08 "Current Outfit" keeps it) and, when given, the GUID of the outfit's source.
    struct OutfitChange {
        std::int16_t category = 0;
        std::optional<std::uint32_t> sourceGuid;
    };
    std::optional<OutfitChange> outfitChange;
    // What Animate Sim or Animate Object last started on it, while it plays.
    std::optional<PlayingAnimation> animation;
    // The Animate Sim clip a Sim was playing when the lot was saved, with its rate and how far it had
    // played. The game keeps that clip for the Animate Sim the Sim waits in, so that primitive carries on
    // with it when it runs again instead of starting the clip anew from its operands and temps.
    struct ResumedClip {
        std::string name;
        float speed = 1;
        double playedMs = 0;
    };
    std::optional<ResumedClip> resumedClip;
    // The Animate Sim clip it last finished, while that fades out (see PlayingAnimation::blendsOut).
    // For an object, the clip Animate Object last replaced. An instant Animate Stop removes it.
    std::optional<PlayingAnimation> ending;
    // Animations laid over it, oldest first.
    std::vector<PlayingOverlay> overlays;
    // Where a Sim's head looks (Look At): the head's controller, and what each look it was given
    // looks at, newest last, while the controller may still tell of it.
    LookController look;
    std::vector<LookTarget> lookTargets;
    int looksStarted = 0;
    // A Sim's facing once it has turned (see turn.hpp). Before that, its direction (data 1).
    std::optional<float> heading;
    std::optional<Turning> turning;
    std::optional<Walking> walking;
    // When my last walk ended in its stop clip (clock ms) and the foot that stop began on: a walk
    // from standing begun soon after begins on that foot (see planWalkFor).
    struct WalkEnd {
        double ms = 0;
        float foot = 0.5f;
    };
    std::optional<WalkEnd> lastWalkEnd;
    // My last walk once its route handed back to its script: its clips play out their fade under
    // what I do next (see walkWeight). It only shows, so it isn't saved.
    struct WalkTail {
        WalkPlan plan;
        double startMs = 0;  // when the walk started, on the world's clock
        WalkPath path;       // the walk's path, which its clips play on to the end of
        std::int32_t level = 1;
    };
    std::optional<WalkTail> walkTail;
    // When my last interrupted or blocked walk settled (clock ms): the pose I was in fades out over
    // kSettlePoseMs while the walk pose fades in and out over kSettleWalkPoseMs. It only shows, so it
    // isn't saved.
    std::optional<double> walkSettledMs;
    // A routing slot this object was given in place of its type's slot 0 (see route_destinations.hpp).
    std::optional<slot::Entry> ownRoutingSlot;
    // The destinations a Sim's router made for it to make way, until nothing uses them.
    std::vector<std::int16_t> destinations;
    std::optional<PostRouteWait> postRoute;
    std::optional<Reaching> reaching;
    // Timers On Timer set on it, in the order they were set, and the last id given out.
    std::vector<ObjectTimer> timers;
    std::uint16_t lastTimerId = 0;
    // Why a tree another object ran on it at once failed. The simulation resets it on its next turn.
    std::optional<std::string> failedRun;
    // Whether its scripts run on while the game is paused (the plumbob, which follows the selected
    // Sim). It is object data 0x3C as the object was last loaded or reset, so a script setting that
    // field later changes nothing until the next reset.
    bool runsWhilePaused = false;
    bool removed = false;
    // Removed for scripts but kept on the lot until it unloads (see World::keepUntilUnload).
    bool keptUntilUnload = false;

    bool isPerson() const { return definition.field(objd::field::kType) == kPersonType; }
};

// A sound a script starts or stops (Play/Stop Sound Event) on an object (its source).
struct SoundEvent {
    std::string name;
    std::int16_t source = 0;
    bool stop = false;
};

// A neighborhood's neighbors and relationships as saved. Relationship values are 32-bit, as the
// game keeps them. The Relationship primitive moves their low 16 bits.
struct SavedNeighbor {
    std::uint32_t guid = 0;
    std::vector<std::int16_t> personData;
    Genome genes;  // its DNA (SDNA), empty when none is saved
    // A baby made during play: the parents its face is blended from (the first one carried it),
    // which gave each region, and the draw that picks its eyebrows.
    std::int16_t faceParentA = 0, faceParentB = 0;
    FacePicks facePicks;
    std::uint32_t browDraw = 0;
};

struct SavedNeighborhood {
    std::set<std::int16_t> neighborIds;
    std::map<std::int16_t, SavedNeighbor> neighbors;
    std::set<std::int32_t> lotIds;
    std::map<std::int32_t, std::uint8_t> lotTypes;    // each placed lot's type (its description)
    std::map<std::int32_t, std::int32_t> lotOwners;   // and its owner, a neighbor id
    std::map<std::uint32_t, std::int32_t> familyLots;  // each family's home lot (FAMI instance -> lot, 0 for none)
    std::map<std::uint32_t, std::int32_t> familyCurrentLots;   // and the two other lots its FAMI saves
    std::map<std::uint32_t, std::int32_t> familyVacationLots;
    std::map<std::uint32_t, std::uint32_t> familyFlags;  // each family's saved flags (FAMI)
    // Each family's FAMI value4, which orders the game's families (highest first). Its meaning is not known yet.
    std::map<std::uint32_t, std::int32_t> familyRanks;
    std::map<std::uint32_t, std::vector<std::uint32_t>> familyMembers;  // each family's members' GUIDs (FAMI)
    std::map<std::pair<std::int16_t, std::int16_t>, std::vector<std::int32_t>> relationships;
    std::map<std::int16_t, std::vector<famt::Tie>> familyTies;  // each neighbor's, in saved order
    std::map<std::int16_t, std::vector<swaf::Want>> wants;  // each neighbor's current wants
    std::map<std::int16_t, std::vector<swaf::Want>> fears;  // and fears
    std::map<std::int16_t, swaf::Slots> wantSlots;          // and their slots and hold limit
    std::shared_ptr<const ngbh::Neighborhood> record;      // inventories, null when not saved
    std::map<std::int16_t, DataTables> dataTables;         // each neighbor's script data tables
    std::optional<std::int32_t> id;                         // this neighborhood's id
    // Every neighborhood of the Neighborhoods folder by id, as NeighborhoodManager lists them.
    std::shared_ptr<const std::map<std::int32_t, nhoodid::NeighborhoodId>> allNeighborhoods;
};

class World {
public:
    // A new object under the lowest free id, as the game gives them. A removed object keeps its id until
    // the purge. Objects run in id order each tick, so the id decides when a new one runs.
    SimObject& create(std::uint32_t group, objd::ObjectDefinition definition, std::size_t containerSlots = 0);
    // The highest id held since the lot loaded. A freed id stays a free slot below it, so this never goes down.
    std::int16_t highestId() const { return highestId_; }
    // Recreates a saved object under its saved id.
    SimObject& restore(std::int16_t id, std::uint32_t group, objd::ObjectDefinition definition);
    SimObject* find(std::int16_t id);
    const SimObject* find(std::int16_t id) const;
    std::size_t size() const { return objects_.size() - removedCount_ - keptCount_; }

    // Removal only marks the object, as the game queues the delete: find() and searches stop
    // seeing it at once, but findIncludingRemoved() still finds it by id, so code may keep using
    // its data until purge() frees removed objects at the end of the tick.
    SimObject* findIncludingRemoved(std::int16_t id);
    void remove(SimObject& object);
    // Marks an object removed for scripts, as remove() does, but leaves it on the lot: purge() keeps
    // it and it is still drawn. The game does this to a saved Placeholder or TemplatePerson as the
    // lot loads. A later remove() deletes it at the end of that tick.
    void keepUntilUnload(SimObject& object);
    void purge();
    // The removed objects purge() has yet to free, by id.
    std::vector<std::int16_t> removedIds() const;
    // An object's use count (object data 0x3E) is how many Sims run one of its interactions: one
    // more when such a frame goes on a Sim's stack, one less when it comes off, by returning or by a
    // reset. Frames read from a save were counted in the saved data and are not marked.
    void countUse(const Frame& interaction);
    void releaseUse(const Frame& frame);
    void releaseUses(const std::vector<Frame>& frames);

    // The id of the part an interaction made on `part` runs on. A multi-tile part whose interaction group
    // (OBJD field 8) is N above 0 hands its interactions to the part of the same object whose group
    // is -N, so that one part keeps the use count and the script's data. The Wedding Arch's end parts
    // (group 1) hand theirs to its middle part (group -1). Any other object is its own part.
    std::int16_t interactionPart(const SimObject& part) const;

    // Objects whose route footprint may have changed: new and removed ones, and those Update
    // Footprint names. The route world takes them before it is next used, as the game does.
    // Marking any part of a multi-tile object marks every part, since all parts route by the lead.
    void markFootprintChanged(std::int16_t id);
    std::set<std::int16_t> takeFootprintChanges() { return std::exchange(footprintChanges_, {}); }

    // Objects keeping Build and Buy mode off (Generic Sims Calls 0x0A and 0x0B). An object is listed
    // once per call that blocks, and deleting it drops every entry it has.
    void blockBuildBuy(std::int16_t id) { buildBuyBlockers_.push_back(id); }
    void unblockBuildBuy(std::int16_t id) {
        if (const auto it = std::ranges::find(buildBuyBlockers_, id); it != buildBuyBlockers_.end()) {
            buildBuyBlockers_.erase(it);
        }
    }
    bool buildBuyBlocked() const { return !buildBuyBlockers_.empty(); }

    // The ids `from` holds a relationship record towards, in id order.
    std::vector<std::int16_t> relationshipTargets(std::int16_t from) const {
        std::vector<std::int16_t> ids;
        for (auto it = relationships_.lower_bound({from, static_cast<std::int16_t>(INT16_MIN)}); it != relationships_.end() && it->first.first == from; ++it) {
            ids.push_back(it->first.second);
        }
        return ids;
    }

    // Relationship variables `from` holds towards `to`, empty until first grown.
    std::vector<std::int16_t>& relationship(std::int16_t from, std::int16_t to) {
        const auto [it, added] = relationships_.try_emplace({from, to});
        relationshipAdded_ = relationshipAdded_ || added;
        return it->second;
    }

    // Rooms of the lot this world was built from (empty for worlds made in code).
    void setRooms(RoomMap rooms) { rooms_ = std::move(rooms); }
    const RoomMap& rooms() const { return rooms_; }
    // Object data 0x1D holds the room the object stands in. Scripts compare it right after a move,
    // so every move sets it again. A spot outside the room map leaves it as it was.
    void updateRoom(SimObject& object) const;

    // The neighborhood as saved, shared between worlds (a world keeps its own changes).
    // The regions of the lot's second room layer (wall graph instance 6): 0 where its walls enclose
    // nothing. Seasons' weather counts only region 0 as possibly outside.
    const RoomMap& weatherRegions() const { return weatherRegions_; }
    void setWeatherRegions(RoomMap regions) { weatherRegions_ = std::move(regions); }
    // The floor pattern on quarter `quarter` (0-3) of tile (x, y) on `level` (0 = ground), as the lot's
    // floor grid keeps it, with x and y along the grid (as scripts count tiles). Nothing off the grid.
    using FloorPattern = std::function<std::optional<std::uint16_t>(int level, int x, int y, int quarter)>;
    void setFloorPattern(FloorPattern f) { floorPattern_ = std::move(f); }
    const FloorPattern& floorPattern() const { return floorPattern_; }
    // The platform region of a quarter, from the lot's region grid (c3DArray instance 0x14), with the
    // same levels and quarters as the floor pattern. The regions are the same on every lot: 0 the
    // house's foundation, 1 and 2 decks, 3 and 5 pools, 4 split-level interior foundations ("stages").
    // -1 or nothing where no platform is.
    using PlatformRegion = std::function<std::optional<std::int32_t>(int level, int x, int y, int quarter)>;
    void setPlatformRegion(PlatformRegion f) { platformRegion_ = std::move(f); }
    const PlatformRegion& platformRegion() const { return platformRegion_; }

    // The lot's neighbor id remap table (Generic Sims Call 0x30): the neighbor id each id maps to.
    // None when the lot has no remap tables. Unread when it has tables the engine can't read.
    using NeighborRemap = std::unordered_map<std::uint32_t, std::uint32_t>;
    void setNeighborRemap(std::optional<NeighborRemap> remap, bool unread) {
        neighborRemap_ = std::move(remap);
        neighborRemapUnread_ = unread;
    }
    const std::optional<NeighborRemap>& neighborRemap() const { return neighborRemap_; }
    bool neighborRemapUnread() const { return neighborRemapUnread_; }

    // Where rain and snow reach (see OutsideGrid).
    const OutsideGrid& outside() const { return outside_; }
    void setOutside(OutsideGrid outside) { outside_ = std::move(outside); }

    // The lot's walls, tile by tile.
    const WallTiles& walls() const { return walls_; }
    void setWalls(WallTiles walls) { walls_ = std::move(walls); }
    // The walls that block a Sim's view (Set to Next 0x18): built walls only.
    const WallTiles& sightWalls() const { return sightWalls_; }
    void setSightWalls(WallTiles walls) { sightWalls_ = std::move(walls); }

    // The neighborhood's weather and season, as scripts read and change them.
    weather::Weather& weather() { return weather_; }

    // The messages objects subscribed to. Deleted objects leave them when purge() frees them.
    MessageSubscriptions& messageSubscriptions() { return messageSubscriptions_; }

    void setNeighborhood(std::shared_ptr<const SavedNeighborhood> hood) {
        hood_ = std::move(hood);
        ownHood_.reset();
        nextNeighborId_ = hood_ && !hood_->neighborIds.empty() ? static_cast<std::int16_t>(*hood_->neighborIds.rbegin() + 1) : 1;
        neighborRelationships_.clear();
        neighborPersonData_.clear();
        neighborWants_.clear();
        neighborFears_.clear();
        neighborDataTables_.clear();
        inventories_.clear();
    }
    // Ids of the neighborhood's neighbors, null until a neighborhood is set.
    const std::set<std::int16_t>* neighbors() const { return hood_ ? &hood_->neighborIds : nullptr; }
    // Lot ids of the neighborhood, null until a neighborhood is set.
    const std::set<std::int32_t>* lotIds() const { return hood_ ? &hood_->lotIds : nullptr; }
    // A placed lot's type and owner (a neighbor id) from its description, if the neighborhood places it.
    std::optional<std::uint8_t> placedLotType(std::int32_t id) const {
        if (!hood_) {
            return std::nullopt;
        }
        const auto it = hood_->lotTypes.find(id);
        return it != hood_->lotTypes.end() ? std::optional(it->second) : std::nullopt;
    }
    std::int32_t placedLotOwner(std::int32_t id) const {
        if (!hood_) {
            return 0;
        }
        const auto it = hood_->lotOwners.find(id);
        return it != hood_->lotOwners.end() ? it->second : 0;
    }
    // A neighborhood of the Neighborhoods folder by id, or null, and the id of the one the lot is in.
    const nhoodid::NeighborhoodId* neighborhoodById(std::int32_t id) const {
        if (!hood_ || !hood_->allNeighborhoods) {
            return nullptr;
        }
        const auto it = hood_->allNeighborhoods->find(id);
        return it != hood_->allNeighborhoods->end() ? &it->second : nullptr;
    }
    std::optional<std::int32_t> neighborhoodId() const { return hood_ ? hood_->id : std::nullopt; }
    const std::map<std::int32_t, nhoodid::NeighborhoodId>* allNeighborhoods() const {
        return hood_ ? hood_->allNeighborhoods.get() : nullptr;
    }
    // This lot's type (see content::Neighborhood::lotTypes). 0 when the neighborhood doesn't place it.
    std::uint8_t lotType() const { return lotType_; }
    void setLotType(std::uint8_t type) { lotType_ = type; }
    // The lot's number in its neighborhood (Lots/<hood>_Lot<n>), if known.
    std::optional<std::int32_t> lotNumber() const { return lotNumber_; }
    void setLotNumber(std::int32_t number) { lotNumber_ = number; }
    // This lot's size in tiles and, on a beach lot, its beach's side. Zero size when unknown.
    struct LotShape {
        float sizeX = 0, sizeY = 0;
        std::optional<std::uint8_t> beachSide;
    };
    const LotShape& lotShape() const { return lotShape_; }
    void setLotShape(LotShape shape) { lotShape_ = shape; }
    // The lot's full room walls: drawn, of a style that spans floor to floor (not fences, attic, deck
    // or stage walls), in the order of the lot's saved wall coverings. Scripts number them from 0
    // (nWorld.GetNumWallSections, GetRoomOnSideOfWall). Unset for worlds not built from a lot.
    struct WallSection {
        // Its two room graph points. A wall the room graph doesn't have still counts, without them.
        wallgraph::Point from;
        wallgraph::Point to;
        bool placed = false;
    };
    const std::optional<std::vector<WallSection>>& wallSections() const { return wallSections_; }
    void setWallSections(std::vector<WallSection> sections) { wallSections_ = std::move(sections); }
    // A neighbor's saved description, null when unknown.
    const SavedNeighbor* neighbor(std::int16_t id) const {
        if (!hood_) {
            return nullptr;
        }
        const auto it = hood_->neighbors.find(id);
        return it != hood_->neighbors.end() ? &it->second : nullptr;
    }
    // The game's eleven "neighbor data" values of a neighbor (0 for an unknown neighbor): 0 the
    // neighbor's Sim on this lot, 1 whether it belongs to the lot's family, 2 age, 3 and 4 always 0,
    // 5 its mutual friends, 6-10 from its family (see neighborData in world.cpp).
    static constexpr std::uint16_t kNeighborDataCount = 11;
    [[nodiscard]] std::optional<std::int16_t> neighborData(std::int16_t neighborId, std::uint16_t field) const;
    std::int16_t mutualFriends(std::int16_t id) const;
    std::int16_t familyFriends(std::uint32_t family) const;
    bool neighborsAreFriends(std::int16_t a, std::int16_t b) const;
    const std::vector<std::int16_t>* livePersonData(std::int16_t id) const;

    // Whether the family ties record has an entry for neighbor `id`.
    bool hasFamilyTies(std::int16_t id) const { return hood_ && hood_->familyTies.contains(id); }
    // Ends the family tie between neighbors `a` and `b` (Generic Sims Call 0x17): each loses its first
    // tie to the other, of any type. Each gets an entry in the record if it had none. It lasts for
    // this run only, as nothing is saved yet.
    void removeFamilyTie(std::int16_t a, std::int16_t b);
    // Makes neighbor `a` neighbor `b`'s `type` (father, mother, spouse, sibling or child), as the game's
    // family ties do, and returns whether every step took. See World::setFamilyTie.
    bool setFamilyTie(std::int16_t a, std::int16_t b, famt::TieType type);
    // The neighbors neighbor `id` has a family tie of `type` with, in the order the record lists them.
    std::vector<std::int16_t> familyTies(std::int16_t id, famt::TieType type) const {
        std::vector<std::int16_t> out;
        if (hood_) {
            if (const auto it = hood_->familyTies.find(id); it != hood_->familyTies.end()) {
                for (const auto& tie : it->second) {
                    if (tie.type == static_cast<std::uint32_t>(type)) {
                        out.push_back(tie.other);
                    }
                }
            }
        }
        return out;
    }

    // Relationship variable `index` neighbor `from` holds towards `to`, without adding a record, 0
    // when there is none.
    std::int16_t neighborRelationshipValue(std::int16_t from, std::int16_t to, std::size_t index) const {
        const std::vector<std::int32_t>* values = neighborRelationshipIfAny(from, to);
        return values && index < values->size() ? static_cast<std::int16_t>((*values)[index]) : std::int16_t{0};
    }
    // The relationship variables `from` holds towards `to`, as the game keeps them, null without a record.
    const std::vector<std::int32_t>* neighborRelationshipValues(std::int16_t from, std::int16_t to) const {
        return neighborRelationshipIfAny(from, to);
    }
    // How many relationship variables `from` holds towards `to` (0 without a record).
    std::size_t neighborRelationshipSize(std::int16_t from, std::int16_t to) const {
        const std::vector<std::int32_t>* values = neighborRelationshipIfAny(from, to);
        return values ? values->size() : 0;
    }

    // The neighbors `from` holds relationship variables towards (saved or grown), in id order.
    std::vector<std::int16_t> neighborRelationshipTargets(std::int16_t from) const {
        std::set<std::int16_t> targets;
        for (auto it = neighborRelationships_.lower_bound(std::pair<std::int16_t, std::int16_t>{from, static_cast<std::int16_t>(INT16_MIN)}); it != neighborRelationships_.end() && it->first.first == from; ++it) {
            targets.insert(it->first.second);
        }
        if (hood_) {
            for (auto it = hood_->relationships.lower_bound(std::pair<std::int16_t, std::int16_t>{from, static_cast<std::int16_t>(INT16_MIN)}); it != hood_->relationships.end() && it->first.first == from; ++it) {
                targets.insert(it->first.second);
            }
        }
        return {targets.begin(), targets.end()};
    }

    // Relationship variables neighbor `from` holds towards neighbor `to`. They live on the neighbor,
    // so they outlive the lot: the saved values until changed, empty if none were saved.
    std::vector<std::int32_t>& neighborRelationship(std::int16_t from, std::int16_t to) {
        const std::pair key{from, to};
        auto it = neighborRelationships_.find(key);
        if (it == neighborRelationships_.end()) {
            std::vector<std::int32_t> saved;
            if (hood_) {
                if (const auto s = hood_->relationships.find(key); s != hood_->relationships.end()) {
                    saved = s->second;
                }
            }
            it = neighborRelationships_.emplace(key, std::move(saved)).first;
        }
        return it->second;
    }

    // Person data `index` of a neighbor, without making its copy, 0 when there is none.
    std::int16_t neighborPersonValue(std::int16_t id, std::size_t index) const {
        if (const auto it = neighborPersonData_.find(id); it != neighborPersonData_.end()) {
            return index < it->second.size() ? it->second[index] : 0;
        }
        const SavedNeighbor* n = neighbor(id);
        return n && index < n->personData.size() ? n->personData[index] : 0;
    }

    // Adds a neighbor made while playing (Make New Character) and returns its id, or nothing when the
    // neighborhood has no free id. It is kept for this run only, as nothing is saved yet.
    std::optional<std::int16_t> addNeighbor(SavedNeighbor neighbor);
    // The 29-bit count under a new GUID made during play. The game counts time for it, the engine
    // counts the GUIDs it made from 1, so a run repeats.
    std::uint32_t nextGuidCount() { return ++guidCount_ & 0x1FFFFFFF; }
    // Adds a family with no home lot unless the neighborhood has it. It lasts for this run only.
    void addFamily(std::uint32_t family);
    // Adds a Sim's GUID to the end of a family's members unless it is there. It lasts for this run only.
    void addFamilyMember(std::uint32_t family, std::uint32_t guid);
    bool hasFamily(std::uint32_t family) const { return hood_ && hood_->familyLots.contains(family); }
    // The household of the lot being played: of the families whose home is this lot, the one the
    // game lists first, with the highest rank (familyRanks) and then the highest id.
    std::optional<std::uint32_t> lotHousehold() const {
        if (!hood_ || !lotNumber_) {
            return std::nullopt;
        }
        const auto rankOf = [&](std::uint32_t family) {
            const auto it = hood_->familyRanks.find(family);
            return std::pair{it != hood_->familyRanks.end() ? it->second : 0, static_cast<std::int32_t>(family)};
        };
        std::optional<std::uint32_t> first;
        for (const auto& [family, lot] : hood_->familyLots) {
            if (lot == *lotNumber_ && (!first || rankOf(family) > rankOf(*first))) {
                first = family;
            }
        }
        return first;
    }
    // A family's lots as CachedFamily reads them, or nothing for a family the neighborhood doesn't have.
    // A family added during this run has 0 for all three.
    struct FamilyLotIds {
        std::int32_t home = 0, current = 0, vacation = 0;
    };
    std::optional<FamilyLotIds> familyLotIds(std::uint32_t family) const {
        if (!hasFamily(family)) {
            return std::nullopt;
        }
        const auto lotIn = [family](const std::map<std::uint32_t, std::int32_t>& lots) {
            const auto it = lots.find(family);
            return it != lots.end() ? it->second : 0;
        };
        return FamilyLotIds{lotIn(hood_->familyLots), lotIn(hood_->familyCurrentLots), lotIn(hood_->familyVacationLots)};
    }
    // The GUIDs of a family's Sims in its FAMI's order, empty for an unknown family.
    std::span<const std::uint32_t> familyMemberGuids(std::uint32_t family) const {
        if (!hood_) {
            return {};
        }
        const auto it = hood_->familyMembers.find(family);
        return it != hood_->familyMembers.end() ? std::span<const std::uint32_t>(it->second) : std::span<const std::uint32_t>{};
    }

    // A neighbor's copy of its person data: the saved values until changed, null for an unknown
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
    // one until changed. A family or neighbor without one gets a new empty inventory, as in the
    // game. A lot without one has none (null). The game-wide inventory is not saved with the
    // neighborhood and starts empty.
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
        if (saved == nullptr && kind == InventoryKind::Lot) {
            return nullptr;
        }
        return &inventories_.emplace(key, saved ? toInventory(*saved) : Inventory{}).first->second;
    }

    // A new id for an object put away in an inventory (its archive). What the archive keeps of the
    // object is not modelled yet.
    std::uint32_t nextArchiveId() { return ++lastArchiveId_; }

    // A neighbor's current wants: the saved ones until changed, empty if none were saved.
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

    // A neighbor's current fears: the saved ones until changed, empty if none were saved.
    std::vector<swaf::Want>& neighborFears(std::int16_t id) {
        auto it = neighborFears_.find(id);
        if (it == neighborFears_.end()) {
            std::vector<swaf::Want> saved;
            if (hood_) {
                if (const auto s = hood_->fears.find(id); s != hood_->fears.end()) {
                    saved = s->second;
                }
            }
            it = neighborFears_.emplace(id, std::move(saved)).first;
        }
        return it->second;
    }

    // A neighbor's want and fear slots and hold limit (the game's defaults if none were saved).
    swaf::Slots wantSlots(std::int16_t id) const {
        if (hood_) {
            if (const auto s = hood_->wantSlots.find(id); s != hood_->wantSlots.end()) {
                return s->second;
            }
        }
        return {};
    }

    // The neighbor whose Sim is this object (matched by GUID), 0 when there is none.
    std::int16_t neighborIdOf(const SimObject& sim) const { return sim.isPerson() ? neighborIdWithGuid(sim.definition.guid()) : 0; }

    // The neighbor whose Sim has this GUID, 0 when there is none.
    std::int16_t neighborIdWithGuid(std::uint32_t guid) const {
        if (hood_) {
            for (const auto& [id, n] : hood_->neighbors) {
                if (n.guid == guid) {
                    return id;
                }
            }
        }
        return 0;
    }

    // The GUID of a neighbor's Sim, none when the neighborhood has no such neighbor.
    std::optional<std::uint32_t> neighborGuid(std::int16_t id) const {
        if (hood_) {
            if (const auto it = hood_->neighbors.find(id); it != hood_->neighbors.end()) {
                return it->second.guid;
            }
        }
        return std::nullopt;
    }

    // The simulation's clock in milliseconds, moved on by whoever runs the world. Animations are
    // timed by it.
    double clockMs() const { return clockMs_; }
    // When what runs now happens on the animation clock: the moment of the animation event being told
    // (which falls between ticks), else the clock. Animations started now start then.
    double eventClockMs() const { return eventMs_.value_or(clockMs_); }
    void setEventClock(std::optional<double> ms) { eventMs_ = ms; }
    void advanceClock(double ms) { clockMs_ += ms; }
    // Simulation ticks run so far.
    std::uint64_t ticks() const { return ticks_; }
    void countTick() { ++ticks_; }

    // Text notifications (Dialog) shown now, by the ids the game hands out. The oldest go when too
    // many are shown.
    std::int16_t showNotification() {
        constexpr std::size_t kMostShown = 64;
        if (notifications_.size() >= kMostShown) {
            notifications_.erase(notifications_.begin());
        }
        lastNotification_ = static_cast<std::int16_t>(lastNotification_ == INT16_MAX ? 1 : lastNotification_ + 1);
        notifications_.push_back(lastNotification_);
        return lastNotification_;
    }
    bool notificationShown(std::int16_t id) const { return std::find(notifications_.begin(), notifications_.end(), id) != notifications_.end(); }
    bool hideNotification(std::int16_t id) { return std::erase(notifications_, id) != 0; }

    // Whether an interaction with line flag 0x4 is running at once. Only one can at a time.
    bool runningAtOnce() const { return runningAtOnce_; }
    void setRunningAtOnce(bool running) { runningAtOnce_ = running; }

    // Naming Sims (Dialog "Set Baby Name"). Whoever shows the dialog turns asking on, and a request
    // then waits for its answer. With asking off the dialog waits a tick and is true, and no name
    // changes, as OK on the untouched names would. As in the game, the requests wait in one list in
    // the order they were made, one per asking object, and are shown one at a time. A reset leaves an
    // object's request in the list, still shown or holding its answer, and its next naming takes it.
    void setAsksNames(bool asks) { asksNames_ = asks; }
    bool asksNames() const { return asksNames_; }
    // A naming started by `asker` (temps 0-3 of the dialog, 0 where there is none). The first is a
    // baby or the Sim renamed, the second a twin. An earlier request of the asker's that is shown is
    // closed and this one asked anew. One still waiting or answered is taken as this one's instead.
    void askNames(std::int16_t asker, std::vector<std::int16_t> neighbors);
    // The neighbors the shown request names, empty when none is shown.
    const std::vector<std::int16_t>& namesAsked() const;
    // Answers the shown request and shows the next. OK gives each neighbor asked its name in `names`,
    // in the same order, trimmed of spaces and tabs as the game trims them. A neighbor past the end of
    // `names`, and the 0 places, keep theirs. Cancel changes nothing.
    void answerNames(bool ok, const std::vector<std::string>& names);
    // The answer to `asker`'s request once given (true for OK), which ends the request. Nothing while
    // it waits or shows. False when the asker has none, which only a broken caller can see.
    std::optional<bool> takeNamesAnswer(std::int16_t asker);
    // The first names the game gives a new baby, a boy's and a girl's (Live text 0x95, entries 1 and
    // 2), set by whoever has the game's texts. Without them a new baby has no first name.
    void setBabyFirstNames(std::string boy, std::string girl) { babyFirstNames_ = {std::move(boy), std::move(girl)}; }
    const std::string& babyFirstName(bool female) const { return babyFirstNames_[female ? 1 : 0]; }
    void setNeighborFirstName(std::int16_t id, std::string name) {
        firstNames_[id] = std::move(name);
        ++namesVersion_;
    }
    // Counts the first names given while playing, so a name shown can tell it has changed.
    std::uint32_t namesVersion() const { return namesVersion_; }
    // A neighbor's first name when the game gave it one while playing, nothing when its character
    // file's name stands.
    std::optional<std::string> neighborFirstName(std::int16_t id) const {
        const auto it = firstNames_.find(id);
        return it == firstNames_.end() ? std::nullopt : std::optional<std::string>{it->second};
    }

    // Who plays the sounds scripts start and stop. Without it they go unheard.
    using SoundEvents = std::function<void(const SoundEvent&)>;
    void setSoundEvents(SoundEvents events) { soundEvents_ = std::move(events); }
    const SoundEvents& soundEvents() const { return soundEvents_; }
    // What a ray meets in the lot as it is drawn (RayTrace). Without it RayTrace stops the script.
    void setRayQuery(RayQuery query) { rayQuery_ = std::move(query); }
    const RayQuery& rayQuery() const { return rayQuery_; }
    // Where the camera is, as a position on the lot (x and y in tiles, as objects' positions). Without
    // it Turn Body Towards can't face the camera and stops the script.
    using CameraPoint = std::function<std::optional<std::array<float, 2>>()>;
    void setCameraPoint(CameraPoint point) { cameraPoint_ = std::move(point); }
    const CameraPoint& cameraPoint() const { return cameraPoint_; }

    // How long animations last. Without it, animations end as soon as they start.
    void setAnimationLengths(AnimationLengths lengths) { animationLengths_ = std::move(lengths); }
    const AnimationLengths& animationLengths() const { return animationLengths_; }
    void setAnimationCarries(AnimationCarries carries) { animationCarries_ = std::move(carries); }
    const AnimationCarries& animationCarries() const { return animationCarries_; }
    void setAnimationTurns(AnimationTurns turns) { animationTurns_ = std::move(turns); }
    // A locomotion clip's header as the walk planner reads it (see walk_plan.hpp). Without it,
    // walking Sims move at a fixed pace without clips.
    using LocoClips = std::function<std::optional<LocoClip>(const std::string& name)>;
    // When an animation's SimAntics event `id` ("event=N") comes, in milliseconds from its start.
    using AnimationEventTime = std::function<std::optional<float>(const std::string& name, int id)>;
    void setAnimationEventTime(AnimationEventTime time) { animationEventTime_ = std::move(time); }
    const AnimationEventTime& animationEventTime() const { return animationEventTime_; }
    // An animation's SimAntics events in the order they come, empty when it has none.
    using AnimationEvents = std::function<std::vector<AnimationEvent>(const std::string& name)>;
    void setAnimationEvents(AnimationEvents events) { animationEvents_ = std::move(events); }
    const AnimationEvents& animationEvents() const { return animationEvents_; }
    void setLocoClips(LocoClips clips) { locoClips_ = std::move(clips); }
    const LocoClips& locoClips() const { return locoClips_; }
    // A Sim's size against its skeleton's own (its age data's stretch: teens 0.94), which shortens
    // how far its steps carry it. Without it, 1.
    using SimScale = std::function<float(std::int16_t object)>;
    void setSimScale(SimScale scale) { simScale_ = std::move(scale); }
    const SimScale& simScale() const { return simScale_; }
    // A neighbor's outfit property that comes from its looks (makeup, facial hair, hair flags, see
    // scene::OutfitProperties). Without it, scripts that ask stop as unsupported.
    using OutfitProperty = std::function<std::optional<std::int16_t>(std::int16_t neighbor, std::uint16_t category, std::uint16_t property)>;
    void setOutfitProperty(OutfitProperty f) { outfitProperty_ = std::move(f); }
    const OutfitProperty& outfitProperty() const { return outfitProperty_; }
    // Whether an object's looks have a whole outfit in an outfit category (a flag: everyday 1,
    // see scene::OutfitProperties::hasOutfit), none when its looks aren't found. Without it,
    // scripts that ask stop as unsupported.
    using HasOutfit = std::function<std::optional<bool>(const SimObject& object, std::uint32_t category)>;
    void setHasOutfit(HasOutfit f) { hasOutfit_ = std::move(f); }
    const HasOutfit& hasOutfit() const { return hasOutfit_; }
    // The colours babies inherit, read when first asked. Without it babies get no genes.
    using GeneticsCatalogSource = std::function<const content::GeneticsCatalog*()>;
    void setGeneticsCatalog(GeneticsCatalogSource f) { geneticsCatalog_ = std::move(f); }
    const content::GeneticsCatalog* geneticsCatalog() const { return geneticsCatalog_ ? geneticsCatalog_() : nullptr; }
    // The generator the game draws babies' genes on, apart from the simulator's. Where its state
    // comes from is not known yet.
    SimRandom& geneticsRandom() { return geneticsRandom_; }
    // The joint named `bone` (lower case) in an object's model, none without one. Without it, bone
    // slots are not known.
    using ModelBoneLookup = std::function<std::optional<ModelBone>(std::int16_t object, const std::string& bone)>;
    void setModelBones(ModelBoneLookup f) { modelBones_ = std::move(f); }
    const ModelBoneLookup& modelBones() const { return modelBones_; }
    // Plans Go To Routing Slot: a Sim's route to `slot` of `owner` (`joints`: a bone slot's joints, in
    // its order). Set by the simulation, which keeps the route grids. Without it routing is unsupported.
    using SlotRouter = std::function<SlotRoute(const SimObject& sim, const SimObject& owner, const slot::Entry& slot,
                                               const std::vector<std::optional<ModelBone>>& joints)>;
    void setSlotRouter(SlotRouter f) { slotRouter_ = std::move(f); }
    const SlotRouter& slotRouter() const { return slotRouter_; }
    // Find Location For's spots for placing `placed` by routing slot `slot` of `owner` (a bone slot by
    // `joints`, as for routing), best first (x, y in tiles). Set by the simulation, which keeps the
    // route grids. Without it those forms are unsupported.
    using PlacementSpots = std::function<std::vector<std::pair<float, float>>(
        const SimObject& placed, const SimObject& owner, const slot::Entry& slot, std::span<const std::optional<ModelBone>> joints)>;
    void setPlacementSpots(PlacementSpots f) { placementSpots_ = std::move(f); }
    const PlacementSpots& placementSpots() const { return placementSpots_; }
    // Whether `placed` may stand at (x, y) in tiles on `level` facing `direction` (object data 1), by
    // the game's object placement test. Set by the simulation. Without it every spot passes.
    using ObjectFits = std::function<bool(const SimObject& placed, float x, float y, std::int32_t level, std::int16_t direction)>;
    void setObjectFits(ObjectFits f) { objectFits_ = std::move(f); }
    const ObjectFits& objectFits() const { return objectFits_; }
    // The id of the first Sim or object in the way of walking `sim` stepping to (x, y) in tiles on
    // `level`, 0 when the way is free. Set by the simulation. Without it no walk is ever blocked.
    using StepBlocker = std::function<std::int16_t(const SimObject& sim, float x, float y, std::int32_t level)>;
    void setStepBlocker(StepBlocker f) { stepBlocker_ = std::move(f); }
    const StepBlocker& stepBlocker() const { return stepBlocker_; }
    const AnimationTurns& animationTurns() const { return animationTurns_; }

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
    // Influence level by influence points (the want tuning's influence curve inverted), null if none.
    const content::TuningCurve* influenceLevels() const {
        return wantDefs_ && wantDefs_->influenceLevels ? &*wantDefs_->influenceLevels : nullptr;
    }
    // An object category wants can be about, null for an unknown one.
    const content::WantCategory* wantCategory(std::uint32_t id) const {
        if (!wantDefs_) {
            return nullptr;
        }
        const auto it = wantDefs_->categories.find(id);
        return it != wantDefs_->categories.end() ? &it->second : nullptr;
    }

    // The ids of the objects that match, removed ones too, in id order.
    template <typename Predicate>
    std::vector<std::int16_t> idsIncludingRemoved(Predicate matches) const {
        std::vector<std::int16_t> ids;
        for (const auto& [id, object] : objects_) {
            if (matches(*object)) {
                ids.push_back(id);
            }
        }
        return ids;
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
    template <typename Predicate>
    const SimObject* nextAfter(std::int16_t id, Predicate matches) const {
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
    std::shared_ptr<SavedNeighborhood> ownHood_;  // hood_ once something was added to it
    SavedNeighborhood& ownNeighborhood();
    std::uint8_t lotType_ = 0;
    std::int16_t nextNeighborId_ = 1;  // where the search for a new neighbor's id starts
    weather::Weather weather_;
    MessageSubscriptions messageSubscriptions_;
    std::optional<std::int32_t> lotNumber_;
    WallTiles walls_;
    LotShape lotShape_;
    std::optional<std::vector<WallSection>> wallSections_;
    WallTiles sightWalls_;
    RoomMap weatherRegions_;
    OutsideGrid outside_;
    std::map<std::pair<std::int16_t, std::int16_t>, std::vector<std::int32_t>> neighborRelationships_;
    const std::vector<std::int32_t>* neighborRelationshipIfAny(std::int16_t from, std::int16_t to) const {
        const std::pair key{from, to};
        if (const auto it = neighborRelationships_.find(key); it != neighborRelationships_.end()) {
            return &it->second;
        }
        if (hood_) {
            if (const auto s = hood_->relationships.find(key); s != hood_->relationships.end()) {
                return &s->second;
            }
        }
        return nullptr;
    }
    std::map<std::int16_t, std::vector<std::int16_t>> neighborPersonData_;
    std::map<std::int16_t, std::vector<swaf::Want>> neighborWants_, neighborFears_;
    std::map<std::int16_t, DataTables> neighborDataTables_;
    std::map<std::pair<InventoryKind, std::int32_t>, Inventory> inventories_;
    std::uint32_t lastArchiveId_ = 0;
    std::shared_ptr<const content::WantDefinitions> wantDefs_;
    std::shared_ptr<LuaRuntime> scripts_;
    double clockMs_ = 0;
    std::uint64_t ticks_ = 0;
    std::optional<double> eventMs_;
    AnimationLengths animationLengths_;
    AnimationCarries animationCarries_;
    SoundEvents soundEvents_;
    std::vector<std::int16_t> notifications_;
    std::int16_t lastNotification_ = 0;
    bool runningAtOnce_ = false;
    bool asksNames_ = false;
    std::uint32_t namesVersion_ = 0;
    struct NameRequest {
        enum class State { Waiting, Shown, Ok, Cancelled };
        std::int16_t asker = 0;
        std::vector<std::int16_t> neighbors;
        State state = State::Waiting;
    };
    std::vector<NameRequest> nameRequests_;  // in the order asked
    // Shows the first waiting request when none is shown.
    void showNextNames();
    std::map<std::int16_t, std::string> firstNames_;
    std::array<std::string, 2> babyFirstNames_;
    RayQuery rayQuery_;
    CameraPoint cameraPoint_;
    FloorPattern floorPattern_;
    PlatformRegion platformRegion_;
    std::optional<NeighborRemap> neighborRemap_;
    std::uint32_t guidCount_ = 0;
    bool neighborRemapUnread_ = false;
    AnimationTurns animationTurns_;
    OutfitProperty outfitProperty_;
    HasOutfit hasOutfit_;
    GeneticsCatalogSource geneticsCatalog_;
    SimRandom geneticsRandom_{0};
    ModelBoneLookup modelBones_;
    LocoClips locoClips_;
    SimScale simScale_;
    AnimationEventTime animationEventTime_;
    AnimationEvents animationEvents_;
    SlotRouter slotRouter_;
    PlacementSpots placementSpots_;
    ObjectFits objectFits_;
    StepBlocker stepBlocker_;
    std::size_t removedCount_ = 0;
    std::size_t keptCount_ = 0;  // objects kept until the lot unloads
    bool relationshipAdded_ = false;  // since the last purge
    std::set<std::int16_t> footprintChanges_;
    // Each object by its id (1..32767), beside objects_, so find() is one read: scripts look objects
    // up all the time. Null for ids with no object.
    std::vector<SimObject*> byId_ = std::vector<SimObject*>(0x8000, nullptr);
    std::int16_t highestId_ = 0;
    std::vector<std::int16_t> buildBuyBlockers_;
    // The Sim each neighbor GUID is on this lot (0: none), as neighborData field 0 finds it. Social
    // check trees ask it thousands of times a tick, so it's kept until objects are added or removed.
    mutable std::map<std::uint32_t, std::int16_t> simOfNeighbor_;
    void objectsChanged() { simOfNeighbor_.clear(); }
};

} // namespace ts2::sim
