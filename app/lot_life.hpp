#pragma once

#include "object_materials.hpp"
#include "scene/animation_library.hpp"
#include "scene/object_effects.hpp"
#include "scene/outfit_properties.hpp"
#include "scene/scene.hpp"
#include "scene/newborn_face.hpp"
#include "scene/scene_ray.hpp"
#include "scene/sims.hpp"
#include "sim/catalog.hpp"
#include "sim/clock.hpp"
#include "content/ui_meters.hpp"
#include "formats/tga.hpp"
#include "sim/lot_simulation.hpp"
#include "sim/pie_menu.hpp"
#include "sim/speed_clock.hpp"
#include "route_sources.hpp"
#include "sim_loader.hpp"

#include <array>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace app {

// A lot's simulation, or null (saying why) when it can't be built.
std::unique_ptr<ts2::sim::LotSimulation> loadSimulation(const std::filesystem::path& install, const std::filesystem::path& lot,
                                                        std::optional<std::uint32_t> seed = std::nullopt);

// A lot's Sims driven by their own scripts: the lot's simulation, run in step with real time, and
// each Sim's actor playing the animation its scripts started last (its idle loop until then).
class LotLife {
public:
    // Builds the lot's route world from `routeModels` (read ahead with the lot, or read now when null).
    LotLife(std::unique_ptr<ts2::sim::LotSimulation> sim, ts2::scene::Assets& assets,
            std::shared_ptr<const RouteModelTable> routeModels = nullptr);

    // Builds the Sims scripts wait for (Generic Sims Call 0x1D) in the background, one at a time, with
    // an asset reader of its own for `install`. Without it they are ready at once and each new Sim is
    // built as it appears.
    void loadSimsInBackground(const std::filesystem::path& install);

    // Runs the simulation for `ms` of real time at its speed, then has each Sim's actor play what its
    // scripts animate. It runs whole ticks and the rest waits for the next call. Nothing runs while paused.
    void update(double ms, ts2::scene::Scene& scene);

    // Tells the Sims' scripts which objects are on screen now (see LotSimulation::markOnScreen).
    void markOnScreen(std::span<const std::int16_t> objects) { sim_->markOnScreen(objects); }

    // The game speed and pause, kept in the simulator's globals (scripts change them too).
    ts2::sim::Speed speed();
    void setSpeed(ts2::sim::Speed speed);
    bool paused();
    void setPaused(bool paused);

    // The Sim clock as the live panel shows it, and the household's funds.
    std::string clockText() { return ts2::sim::clockText(sim_->globals()); }
    std::int32_t funds() { return ts2::sim::funds(sim_->globals()); }
    // The selected Sim's Aspiration person data (0 when no Sim is selected).
    std::int16_t aspiration();
    // The selected Sim's object id (0: none), and picking another Sim.
    std::int16_t selected();
    void select(std::int16_t sim);
    // The household's Sims (object ids, in family order), and a Sim's portrait for its life stage
    // from its character file.
    const std::vector<std::int16_t>& household() const { return sim_->household(); }
    [[nodiscard]] std::optional<ts2::tga::Pixels> portrait(std::int16_t sim);
    // A Sim's neighbor id (0 when it has none), and where a neighbor's portrait for its life stage
    // is: its character file and the picture's instance there (nullopt when unknown).
    std::int16_t neighborId(std::int16_t sim);
    struct PortraitFile {
        std::filesystem::path file;
        std::uint32_t instance = 0;
    };
    [[nodiscard]] std::optional<PortraitFile> neighborPortrait(std::int16_t neighbor);
    // A Sim's life stage (person data 0x3A), its motives as the panels show them (whole numbers)
    // and its mood (motive 3). 0 for a Sim that is gone.
    std::int16_t lifeStage(std::int16_t sim);
    std::array<int, 16> motives(std::int16_t sim);
    int mood(std::int16_t sim);
    // A Sim's current wants and fears, and what a want is (null for an unknown one).
    const std::vector<ts2::swaf::Want>& wants(std::int16_t sim);
    const std::vector<ts2::swaf::Want>& fears(std::int16_t sim);
    ts2::swaf::Slots wantSlots(std::int16_t sim);
    // Whether a Sim waits for new wants and fears, and how many times each of its places was
    // satisfied on this lot (fears from 1000).
    bool wantsRefilling(std::int16_t sim);
    const std::map<std::size_t, std::uint32_t>& satisfiedWantPlaces(std::int16_t sim);
    const ts2::content::TuningCurve* influenceLevels() const { return sim_->world().influenceLevels(); }
    // Puts a hold on a Sim's want or fear (by its place in the list), or takes it off: the game's
    // right click. A held one keeps its place until fulfilled (want flags bit 0x01). Past the Sim's
    // hold limit the oldest hold comes off.
    void toggleHold(std::int16_t sim, std::size_t index, bool fear);
    const ts2::content::WantDefinition* wantDefinition(std::uint32_t guid) const { return sim_->world().wantDefinition(guid); }
    // A neighbor's first and last name (its character's catalog strings, lines 0 and 2) and whether
    // she is female (person data 0x41 == 1). Nullopt for an unknown neighbor.
    struct NeighborName {
        std::string first, last;
        bool female = false;
    };
    std::optional<NeighborName> neighborName(std::int16_t id);
    // The parts of a Sim's name as the game shows it over the Sim and its portraits: its neighbor's
    // first and last name, or without a neighbor its character's first name alone. `title` is person
    // data 0xE6, a witch's title (0 for none).
    struct SimName {
        std::string first, last;
        std::int16_t title = 0;
    };
    SimName simName(std::int16_t sim);
    // Changes whenever a first name is given while playing (see World::namesVersion).
    std::uint32_t namesVersion() const { return sim_->world().namesVersion(); }
    // An object's catalog name (its catalog strings' first line). Empty when unknown.
    std::string objectName(std::uint32_t guid);
    bool knowsNeighbor(std::int16_t id) const { return sim_->world().neighbor(id) != nullptr; }
    const ts2::content::WantCategory* wantCategory(std::uint32_t id) const { return sim_->world().wantCategory(id); }
    // A Sim's person data (until the simulation next runs). Empty for a Sim that is gone.
    std::span<const std::int16_t> personData(std::int16_t sim);
    // A neighbor a Sim has a relationship with, as the relationships panel lists it.
    struct Relationship {
        std::int16_t neighbor = 0;
        std::int16_t daily = 0, flags = 0, lifetime = 0;  // relationship values 0, 1 and 2
        bool familyTie = false;                           // value 3 not 0
        bool coins = false;                               // value 9 bit 0
        std::int16_t species = 0, lifeStage = 0;          // the neighbor's person data 0xBA, 0x3A
        bool female = false, skip = false;                // person data 0x41 == 1, 0x44 bit 0
        bool household = false;                           // one of this lot's household
        bool onLot = false;                               // on this lot now
    };
    // The neighbors a Sim holds relationships towards (itself left out), in neighbor id order. Empty
    // for a Sim that is gone or has no family.
    std::vector<Relationship> relationships(std::int16_t sim);
    // A memory as the Simology memories page shows it.
    struct Memory {
        std::uint32_t guid = 0;      // the memory object
        std::uint32_t subject = 0;   // the GUID of who or what it is about (0: nothing)
        bool good = true;
        std::string name;            // with the subject's name put in
        std::string description;     // the same
        std::string subjectFullName; // first and last name when the subject is a Sim, else empty
        std::int16_t subjectNeighbor = 0;  // the subject Sim's neighbor id (0: none)
        std::uint32_t pictureGroup = 0;  // the memory object's group (its picture is image 1 there)
    };
    // A Sim's memories, the last one in its inventory first. Empty for a Sim that is gone.
    std::vector<Memory> memories(std::int16_t sim);
    // An object type in a Sim's own inventory and how many of it there are.
    struct InventoryObject {
        std::uint32_t guid = 0;
        int count = 0;
    };
    // The objects in a Sim's own inventory as the inventory panel lists them: tokens that are shown
    // (flag 1) and are neither memories (flag 2) nor marked for deletion (flag 4), one entry per
    // object type in the order each first appears.
    std::vector<InventoryObject> inventoryObjects(std::int16_t sim);
    // Value 0 of the token of `guid` in a Sim's own inventory. Nullopt when it has none.
    std::optional<std::int16_t> inventoryValue(std::int16_t sim, std::uint32_t guid);
    // All values of the token of `guid` in a Sim's own inventory. Empty when it has none.
    std::vector<std::int16_t> inventoryValues(std::int16_t sim, std::uint32_t guid);
    // Whether a Sim's own inventory has a counted token of `guid` (a career reward it has earned).
    bool hasCountedToken(std::int16_t sim, std::uint32_t guid);
    // A Sim's aspiration score (person data 0xA0). 0 for a Sim that is gone.
    std::int16_t aspirationScore(std::int16_t sim);
    // Whether the mouse can use an object or a Sim (the selected Sim itself too): one on which the
    // selected Sim's pie menu would offer something. The hover marks the object with whether `shift` is
    // held, and with testingCheatsEnabled on a marked object counts only its debug lines.
    bool hoverable(std::uint32_t object, bool shift = false);
    bool isSim(std::uint32_t object);
    // The object a click at a point of the scene on `object` picks, as the game does: for a
    // multi-tile object the part whose spot on its floor is nearest the point (the side of a double
    // bed clicked), for any other the object itself.
    std::uint32_t partAt(std::uint32_t object, const std::array<float, 3>& scenePoint, const ts2::scene::Scene& scene);
    // The selected Sim's pie menu on an object, clicked with `shift` held or not. With testingCheatsEnabled
    // on a Shift-click gives the debug lines alone. Empty when it offers nothing.
    std::vector<ts2::sim::PieChoice> pieMenu(std::uint32_t object, bool shift = false);
    void setTestingCheats(bool on) { sim_->world().testingCheats = on; }
    // A click on empty floor or ground at a point of the scene, on a storey (1 = ground): the game
    // makes a destination object there for the selected Sim and opens its pie menu. Object 0 when
    // none is made.
    struct GroundMenu {
        std::uint32_t object = 0;
        std::vector<ts2::sim::PieChoice> choices;
    };
    GroundMenu groundPieMenu(const std::array<float, 3>& scenePoint, int storey);
    // Queues a choice of that menu for the selected Sim.
    void choose(std::uint32_t object, const ts2::sim::PieChoice& choice);

    // Buy mode (app/lot_life_buy.cpp). Every object Buy mode can sell, the cheapest first, read once.
    const std::vector<ts2::sim::catalog::Item>& buyItems();
    // An item's catalog name and description (its catalog strings' lines 0 and 1).
    std::string catalogText(const ts2::sim::catalog::Item& item, std::size_t line);
    // Makes an object of type `guid` off the lot and starts it, for Buy mode to place. 0 when it
    // can't be made.
    std::int16_t makeForBuying(std::uint32_t guid);
    // Whether an object may stand on the floor (data 4 bit 0, set by its own start-up code). Buy mode
    // places only those yet: objects that go on surfaces or walls can't be placed.
    bool standsOnFloor(std::int16_t object);
    // Where an object held in Buy mode would go for a point of the scene on a storey: the middle of
    // that tile.
    struct BuySpot {
        std::int32_t x = 0, y = 0, level = 1;  // 1/65536 tiles, as objects keep their place
        bool operator==(const BuySpot&) const = default;
    };
    BuySpot buySpot(const std::array<float, 3>& scenePoint, int storey) const;
    // Moves a held object to a spot facing `direction` (0, 2, 4 or 6) and says whether it may stay
    // there by the game's placement test.
    bool holdAt(std::int16_t object, const BuySpot& spot, std::int16_t direction);
    // Where an object stands and which way it faces, to put it back after a move is cancelled.
    // None for an object that is gone or off the lot.
    struct Placed {
        BuySpot spot;
        std::int16_t direction = 0;
    };
    std::optional<Placed> placed(std::int16_t object);
    void putBack(std::int16_t object, const Placed& where);
    // Whether the hand tool may pick an object up: one Buy mode sells (see catalog::isBuyItem) of one
    // tile, standing on the lot, not a Sim and not one the game refuses to remove.
    bool movable(std::int16_t object);
    // An object's current value (object data 0x29), what selling it gives back, and setting it.
    std::int32_t currentValue(std::int16_t object);
    void setCurrentValue(std::int16_t object, std::int32_t value);
    // The object's type (its GUID). 0 for one that is gone.
    std::uint32_t typeOf(std::int16_t object);
    // Takes an object off the lot for good (bought then cancelled, or sold). The Sims forget it.
    void removeObject(std::int16_t object);
    // Adds to the household's funds (less for a negative amount).
    void addFunds(std::int32_t amount);
    // An interaction as the queue shows it, and what its icon pictures: picture `index` of the
    // object type `guid` (0: its usual one) or, when `neighbor` is set, that Sim.
    struct QueuedAction {
        std::uint32_t id = 0;
        bool running = false, cancelled = false, nested = false;
        std::uint32_t guid = 0, index = 0;
        std::int16_t neighbor = 0;
        std::string name;  // its tooltip
    };
    // A Sim's interactions that have an icon: the one it runs and its sub-interaction, then those
    // waiting, in order.
    std::vector<QueuedAction> queue(std::int16_t sim);
    // The player's click on a queue icon (see ts2::sim::cancelInteraction).
    bool cancelInteraction(std::int16_t sim, std::uint32_t id);
    // How far a Sim is through its life stage.
    ts2::content::AgeProgress ageProgress(std::int16_t sim);

    // The time the actors are posed at, in seconds on the simulation's clock: animations go on
    // smoothly between ticks, so this runs ahead of the last tick by the time since it.
    double seconds() const { return drawnMs() / 1000.0; }

    ts2::sim::LotSimulation& simulation() { return *sim_; }
    ts2::scene::AnimationLibrary& animations() { return animations_; }

    // The lot objects the last update moved because a script did (scene instance, where it is drawn
    // now), for the renderer. Objects in a Sim's hand follow the Sim's pose instead.
    const std::vector<std::pair<std::size_t, ts2::scene::Transform>>& movedObjects() const { return moved_; }
    // Lets RayTrace see the lot as it is drawn: its scene, its Sims' poses and the instances shown
    // (all when empty). They must outlive this. Objects created or moved since the last update are
    // brought into the scene before each ray, as the game's are there at once.
    void traceIn(ts2::scene::Scene& scene, const ts2::scene::ScenePose& pose, const std::vector<std::uint8_t>& shown);

    // Tells of the effects clips have reached since the last call, as commands, and of their sounds
    // (by the object playing the clip and the sound's name) to `sound` when given.
    void clipEffects(const ts2::scene::Scene& scene, const std::function<void(const ts2::sim::EffectCommand&)>& send,
                     const std::function<void(std::int16_t object, const std::string& sound)>& sound = {});
    // Where an object's node, or a bone of it, is now, for its effects. An empty bone or "locator" is
    // the node. Effects hide on storeys above `highestLevelShown`.
    ts2::scene::ObjectEffects::Where effectPlace(const ts2::scene::Scene& scene, const ts2::scene::ScenePose& pose, std::int16_t object, const std::string& bone, int highestLevelShown);
    // Where the camera's eye is in the scene, for Sims that turn to face it.
    void setCamera(const std::array<float, 3>& eye) { cameraEye_ = eye; }

    // The lot objects whose materials the last update changed (scene instance, its materials), for
    // the renderer. Materials new to the lot were added to its scene.
    const std::vector<std::pair<std::size_t, std::vector<int>>>& changedMaterials() const { return objectMaterials_.changed(); }

    // The lot instances the last update added or replaced for objects scripts created or removed, in
    // order, for the renderer (see Renderer::setInstance). Their meshes and materials are in the scene.
    const std::vector<std::size_t>& changedInstances() const { return changedInstances_; }
    // The lot instances the last update lit anew because they moved into other light or shade (see
    // Renderer::setLights).
    const std::vector<std::size_t>& relitInstances() const { return relit_; }
    // The lot instances whose shape colour the last update changed (see Renderer::setShapeColor).
    const std::vector<std::size_t>& shapeColored() const { return shapeColored_; }
    // Lights every lot object again where it stands, after the scene's light changed (doors and
    // windows keep theirs). They join relitInstances.
    void relightAll(ts2::scene::Scene& scene);

private:
    // The clock's time the actors are posed at, in milliseconds (see seconds).
    double drawnMs() const { return sim_->world().clockMs() + leadMs_; }
    // A scene point as a route position on the lot (x and y, see SimObject::x).
    std::array<float, 2> routePoint(const std::array<float, 3>& scenePoint) const;
    // The lot object of a scene object id, null for none.
    ts2::sim::SimObject* lotObject(std::uint32_t id);
    // A Sim's own inventory, null for a Sim that is gone or has none.
    const ts2::sim::Inventory* ownInventory(std::int16_t sim);
    // Poses one Sim's actor for this frame: what it plays, where it stands and its lights.
    void poseSim(ts2::scene::Actor& actor, const ts2::sim::SimObject& object, ts2::scene::Scene& scene);
    void placeObjects(ts2::scene::Scene& scene);
    // One pass of placeObjects: the objects that moved since the last frame (on the first pass) and those
    // in `movedContainers`, placed in the pass before. `placedNow` gets the ones it places.
    void placeObjectsOnce(ts2::scene::Scene& scene, const std::set<std::uint32_t>& movedContainers, bool firstPass,
                          std::set<std::uint32_t>& placedNow);
    void hideModels(ts2::scene::Scene& scene);
    void followClips(ts2::scene::Scene& scene);
    void animateObjects(ts2::scene::Scene& scene);
    void hangInAnimated(ts2::scene::Scene& scene);
    void shadeHeld(ts2::scene::Scene& scene);
    void shapeColors(ts2::scene::Scene& scene);
    void shadeCreated(ts2::scene::Scene& scene, std::size_t index, const ts2::scene::Transform& inLot, std::int16_t direction);
    void lookHeads(ts2::scene::Scene& scene, double nowMs);
    // What a Sim's current look aims at on the lot, worked out where the simulation may be read.
    struct LookAim {
        int request = 0;  // the look it is for, 0 for none
        std::int16_t object = 0;  // the object looked at, the lead part for a multi-tile object
        bool objectSlot = false;  // an object's targeting or routing slot, placed by `local`
        std::optional<ts2::scene::Transform> local;  // none when the object has no such slot
        // A slot at a bone: the object's model, its rig and the bone's joint, placed as drawn.
        std::string model, joint;
        std::shared_ptr<const ts2::scene::Rig> rig;
        int node = -1;
    };
    LookAim lookAim(const ts2::sim::SimObject& sim, const ts2::scene::Scene& scene);
    std::optional<ts2::scene::Transform> drawnJoint(const LookAim& aim, double nowMs);
    std::optional<ts2::scene::Transform> partSlot(std::int16_t part, std::size_t slot, const ts2::scene::Scene& scene);
    std::optional<ts2::scene::Transform> slotPlace(const ts2::scene::PlacedObject& placed, std::size_t slot, const ts2::scene::Scene& scene);
    const ts2::scene::Transform& standingPlace(const ts2::scene::PlacedObject& placed, const ts2::scene::Scene& scene) const;
    void addCreatedObjects(ts2::scene::Scene& scene, bool removeSims = true);
    // Puts a Sim created during play into the scene. False when it has no look to draw.
    bool addCreatedSim(ts2::scene::Scene& scene, const ts2::sim::SimObject& sim);
    const ts2::scene::MadeLook* madeLook(std::int16_t neighbor, const ts2::sim::SavedNeighbor::MadeAs& as);
    // Puts a Sim the loader built into the scene, with what it brought of meshes, materials and textures.
    bool addLoadedSim(ts2::scene::Scene& scene, const ts2::sim::SimObject& sim, SimLoader::Built built);
    // Whether a new Sim waits for the loader still, to be drawn once built.
    bool simLoading(const ts2::sim::SimObject& sim) const;
    void wearChangedOutfits(ts2::scene::Scene& scene);
    void buildNewAges(ts2::scene::Scene& scene);
    std::optional<std::set<std::uint32_t>> buildAgeData(const ts2::sim::SimObject& sim, std::uint32_t age);
    // The age data of the object of `guid`, each with its parts, in instance order (an outfit object's).
    std::vector<ts2::scene::AgeData> outfitAgeData(std::uint32_t guid);
    std::optional<ts2::sim::World::BuiltOutfit> buildOutfit(const ts2::sim::SimObject& sim,
                                                            const ts2::sim::World::OutfitBuildRequest& request);
    // A Sim the game dresses from an outfit source as it loads (person data 0x77-0x78, a uniform)
    // wears that outfit once drawn.
    void dressFromSource(std::int16_t id);
    void relight(ts2::scene::Scene& scene, std::size_t index, const ts2::scene::Transform& inLot, int storey);
    // A Sim's scene instance draws nothing from now on and goes to the next Sim drawn.
    void freeSimInstance(ts2::scene::Scene& scene, std::size_t index);
    // Marks a Sim's parts for the renderer. A Sim drawn during play keeps them to free when it goes:
    // `drawnNow` for one drawn this frame, else they replace those it had.
    void keepSimParts(std::int16_t id, const ts2::scene::Actor& actor, bool drawnNow);
    ts2::scene::Assets& assets_;
    std::unique_ptr<ts2::sim::LotSimulation> sim_;
    ts2::scene::AnimationLibrary animations_;
    // Each animation's SimAntics events, made once as the animations are.
    std::map<std::string, std::vector<ts2::sim::AnimationEvent>, std::less<>> animationEvents_;
    struct ClipEffect {
        float ms = 0;
        ts2::anim::EffectEvent event;
    };
    std::map<std::string, std::vector<ClipEffect>, std::less<>> clipEffects_;  // each clip's, by time
    struct ClipSound {
        float ms = 0;
        std::string name;
    };
    std::map<std::string, std::vector<ClipSound>, std::less<>> clipSounds_;  // each clip's, by time
    // How far each playing clip's effects have been told: by its object, start and name, in milliseconds on the clock.
    std::map<std::tuple<std::int16_t, double, std::string>, double> clipEffectsTold_;
    bool clipEffectsStarted_ = false;
    ts2::scene::OutfitProperties outfits_;
    ObjectMaterials objectMaterials_;
    std::unique_ptr<ts2::scene::SceneRays> rays_;
    const ts2::scene::ScenePose* pose_ = nullptr;  // the Sims as last posed
    // The states each Sim's looks took as it was last posed, told at the next update.
    std::map<std::int16_t, std::vector<ts2::sim::LookController::Change>> lookChanges_;
    std::optional<std::array<float, 3>> cameraEye_;
    ts2::scene::Transform lotFrame_;  // the lot's place in the scene
    ts2::sim::TickPacer pacer_;
    double watchMs_ = 0;      // real ms the speed clock has run, which stops while paused
    double frameTicksMs_ = 0;  // real ms the last frame's ticks took, already in watchMs_
    bool wasPaused_ = false;
    double leadMs_ = 0;
    // Per object: its latest own animation (Animate Sim), kept after it ends so its last pose holds, and
    // the one before, fading out under it.
    struct OwnAnimations {
        std::optional<ts2::sim::PlayingAnimation> current, previous;
        // Where the Sim stood as each of its clips started (first drawn), by name and start, for the
        // clips still drawn: one with a root joint plays on there (see LayeredClip::placeMove).
        struct Started {
            std::string name;
            double startMs = 0;
            ts2::scene::Transform place;
            // While the Sim is carried or sits in an object's slot: that place as seen from the bone it
            // hangs at, and which holder and slot that is.
            std::optional<ts2::scene::Transform> fromCarrier;
            std::pair<std::int16_t, std::int16_t> carrierSlot{std::int16_t{0}, std::int16_t{-1}};
        };
        std::vector<Started> started;
        // Keeps where the Sim stands now as the start place of each of its clips that has none yet (its
        // own animation unless it walks, and its overlays), and forgets the clips no longer drawn. A
        // clip started in a slot hangs from the slot's bone as it is now (`hung`), so a driver whose
        // clip starts as the car drives in stays in the seat.
        void noteStarts(const ts2::sim::SimObject& object, const ts2::scene::Transform& place, bool walks,
                        const std::optional<ts2::scene::Transform>& hung, std::pair<std::int16_t, std::int16_t> holderSlot);
        // Where and on which storey its lights were last picked.
        std::array<float, 12> litPlace{};
        int litLevel = -1;
        // The last animation that played out and faded, whose last pose the Sim still holds: a walk
        // starting from it fades in over that pose.
        std::optional<ts2::sim::PlayingAnimation> held;
        bool walking = false;  // while the Sim walks, `previous` is what it did before the walk
        double walkStartMs = 0;  // when that walk started (a walk straight after another starts anew)
        // The walk that handed back, while its clips play out their fade (under the next walk too), and
        // the heading they were last drawn at while it walked.
        std::optional<ts2::sim::SimObject::WalkTail> tail;
        float tailHeading = 0;
        float endedHeading = 0;  // the heading the walk in progress was last drawn at
        // When the Sim's last stopped walk settled (see SimObject::walkSettledMs), and where the
        // Sim stood then.
        std::optional<double> settledMs;
        std::optional<ts2::scene::Transform> settledAt;
    };
    std::vector<ts2::sim::WalkPose> walkPoses_;
    std::vector<ts2::sim::WalkPose> tailPoses_;
    std::map<std::uint32_t, OwnAnimations> ownAnimations_;
    // Per lot object: where the simulation had it when last drawn (place, facing, container, slot).
    struct ObjectPlace {
        std::int32_t x = 0, y = 0, level = 0;
        std::int16_t direction = 0, container = 0, slot = 0;
        bool gone = false;
        bool operator==(const ObjectPlace&) const = default;
    };
    // By object id, read for every object each frame. Ids stop at INT16_MAX, so this stays small.
    struct KnownPlace {
        ObjectPlace place;
        bool known = false;
    };
    std::vector<KnownPlace> objectPlaces_;
    // The world's objects version when the created objects were last looked for (see addCreatedObjects).
    std::optional<std::uint64_t> createdFor_;
    // Per scene instance moved by its object's clip (see followClips): where it stands without the clip.
    std::map<std::size_t, ts2::scene::Transform> clipBases_;
    // Per multi-tile part and container slot: where the slot's bone sits in its lead's model (see partSlot).
    std::map<std::pair<std::int16_t, std::size_t>, std::optional<ts2::scene::Transform>> partSlots_;
    std::set<std::uint32_t> unskinned_;  // objects whose model can't be drawn skinned (see animateObjects)
    std::set<std::size_t> heldNow_;  // the scene instances Sims hold, this frame
    std::set<std::size_t> unshaded_;  // created objects' instances not yet placed out of a Sim's hands
    // The shadow each held object had before a Sim took it, by scene instance (see shadeHeld).
    std::map<std::size_t, ts2::scene::objectshadow::Kind> heldShadows_;
    // Per neighbor: its held wants and fears (GUIDs) in the order they were held. Saved holds come
    // first, wants before fears.
    struct Hold {
        bool fear = false;
        std::uint32_t guid = 0;
        bool operator==(const Hold&) const = default;
    };
    std::map<std::int16_t, std::deque<Hold>> holds_;
    std::vector<std::pair<std::size_t, ts2::scene::Transform>> moved_;
    // The type (GUID) of the object each id was drawn for, the lot's objects and those created since.
    // A freed id goes to the next object created, which may be of another type.
    std::map<std::int16_t, std::uint32_t> drawnTypes_;
    // The model scripts had given each created object when it was drawn (see SimObject::model). An
    // object a script gives another model is drawn again, a lot object too.
    std::map<std::int16_t, std::string> drawnModels_;
    std::map<std::int16_t, std::size_t> created_;  // created objects in the scene: id -> instance
    std::vector<std::size_t> freeInstances_;       // instances of removed created objects, for reuse
    std::vector<std::size_t> freeSimInstances_;    // instances of removed Sims, for the next Sim drawn
    std::map<std::int16_t, std::vector<std::size_t>> createdSims_;  // Sims created during play: id -> instances
    std::shared_future<ts2::scene::BabyCatalog> babyCatalog_;  // read in the background from the start
    std::shared_future<ts2::scene::OutfitCatalog> outfitCatalog_;  // the same, for aging Sims' new outfits and made Sims' looks
    std::map<std::int16_t, ts2::scene::MadeLook> madeLooks_;      // by neighbor (see madeLook)
    // The age data of Sims that aged on this lot or had an outfit written into it, by neighbor id. Their
    // character files don't hold it yet.
    std::map<std::int16_t, ts2::scene::AgeData> agedData_;
    // Each Sim's working copy of its age data, which it is dressed from, by neighbor id (see buildOutfit).
    std::map<std::int16_t, ts2::scene::AgeData> outfitData_;
    bool loadedSimsDressed_ = false;
    std::shared_ptr<ts2::scene::NewbornFaceData> newbornFaces_ = std::make_shared<ts2::scene::NewbornFaceData>();
    std::set<std::int16_t> triedSims_;  // Sims already drawn or tried
    std::unique_ptr<SimLoader> simLoader_;
    std::set<std::int16_t> claimingSims_;  // Sims that took their loaded character (see SimLoader::claim)
    // The loader's meshes, materials, images and cubes -> the scene's.
    std::map<int, int> loadedMeshes_, loadedMaterials_, loadedImages_, loadedCubes_;
    std::vector<std::size_t> changedInstances_;
    std::vector<std::size_t> relit_;
    std::vector<std::size_t> shapeColored_;
    std::optional<std::vector<ts2::sim::catalog::Item>> buyItems_;  // read when Buy mode first opens
};

} // namespace app
