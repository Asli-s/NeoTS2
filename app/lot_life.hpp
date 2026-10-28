#pragma once

#include "object_materials.hpp"
#include "scene/animation_library.hpp"
#include "scene/outfit_properties.hpp"
#include "scene/scene.hpp"
#include "scene/scene_ray.hpp"
#include "sim/clock.hpp"
#include "content/ui_meters.hpp"
#include "formats/tga.hpp"
#include "sim/lot_simulation.hpp"
#include "sim/pie_menu.hpp"
#include "route_sources.hpp"

#include <array>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace app {

// A lot's simulation, or null (saying why) when it can't be built.
std::unique_ptr<ts2::sim::LotSimulation> loadSimulation(const std::filesystem::path& install, const std::filesystem::path& lot);

// A lot's Sims driven by their own scripts: the lot's simulation, run in step with real time, and
// each Sim's actor playing the animation its scripts started last (its idle loop until then).
class LotLife {
public:
    // Builds the lot's route world from `routeModels` (read ahead with the lot, or read now when null).
    LotLife(std::unique_ptr<ts2::sim::LotSimulation> sim, ts2::scene::Assets& assets,
            std::shared_ptr<const RouteModelTable> routeModels = nullptr);

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
    std::vector<ts2::swaf::Want> wants(std::int16_t sim);
    std::vector<ts2::swaf::Want> fears(std::int16_t sim);
    ts2::swaf::Slots wantSlots(std::int16_t sim);
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
    // An object's catalog name (its catalog strings' first line). Empty when unknown.
    std::string objectName(std::uint32_t guid);
    // Whether the neighborhood has this neighbor.
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
    // selected Sim's pie menu would offer something.
    bool hoverable(std::uint32_t object);
    bool isSim(std::uint32_t object);
    // The object a click at a point of the scene on `object` picks, as the game does: for a
    // multi-tile object the part whose spot on its floor is nearest the point (the side of a double
    // bed clicked), for any other the object itself.
    std::uint32_t partAt(std::uint32_t object, const std::array<float, 3>& scenePoint, const ts2::scene::Scene& scene);
    // The selected Sim's pie menu on an object. Empty when it offers nothing.
    std::vector<ts2::sim::PieChoice> pieMenu(std::uint32_t object);
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
    double seconds() const { return (sim_->world().clockMs() + leadMs_) / 1000.0; }

    ts2::sim::LotSimulation& simulation() { return *sim_; }
    ts2::scene::AnimationLibrary& animations() { return animations_; }

    // The lot objects the last update moved because a script did (scene instance, where it is drawn
    // now), for the renderer. Objects in a Sim's hand follow the Sim's pose instead.
    const std::vector<std::pair<std::size_t, ts2::scene::Transform>>& movedObjects() const { return moved_; }
    // Lets RayTrace see the lot as it is drawn: its scene, its Sims' poses and the instances shown
    // (all when empty). They must outlive this. Objects created or moved since the last update are
    // brought into the scene before each ray, as the game's are there at once.
    void traceIn(ts2::scene::Scene& scene, const ts2::scene::ScenePose& pose, const std::vector<std::uint8_t>& shown);
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
    // Lights every lot object again where it stands, after the scene's light changed (doors and
    // windows keep theirs). They join relitInstances.
    void relightAll(ts2::scene::Scene& scene);

private:
    // The lot object of a scene object id, null for none.
    ts2::sim::SimObject* lotObject(std::uint32_t id);
    void placeObjects(ts2::scene::Scene& scene);
    void followClips(ts2::scene::Scene& scene);
    void animateObjects(ts2::scene::Scene& scene);
    void lookHeads(ts2::scene::Scene& scene, double nowMs);
    // What a Sim's current look aims at on the lot, worked out where the simulation may be read.
    struct LookAim {
        int request = 0;  // the look it is for, 0 for none
        std::int16_t object = 0;  // the object looked at, the lead part for a multi-tile object
        bool objectSlot = false;  // an object's targeting or routing slot, placed by `local`
        std::optional<ts2::scene::Transform> local;  // none when the object has no such slot
    };
    LookAim lookAim(const ts2::sim::SimObject& sim, const ts2::scene::Scene& scene);
    std::optional<ts2::scene::Transform> partSlot(std::int16_t part, std::size_t slot, const ts2::scene::Scene& scene);
    const ts2::scene::Transform& standingPlace(const ts2::scene::PlacedObject& placed, const ts2::scene::Scene& scene) const;
    void addCreatedObjects(ts2::scene::Scene& scene);
    void relight(ts2::scene::Scene& scene, std::size_t index, const ts2::scene::Transform& inLot, int storey);
    ts2::scene::Assets& assets_;
    std::unique_ptr<ts2::sim::LotSimulation> sim_;
    ts2::scene::AnimationLibrary animations_;
    ts2::scene::OutfitProperties outfits_;
    ObjectMaterials objectMaterials_;
    std::unique_ptr<ts2::scene::SceneRays> rays_;
    const ts2::scene::ScenePose* pose_ = nullptr;  // the Sims as last posed
    // The states each Sim's looks took as it was last posed, told at the next update.
    std::map<std::int16_t, std::vector<ts2::sim::LookController::Change>> lookChanges_;
    std::optional<std::array<float, 3>> cameraEye_;
    ts2::scene::Transform lotFrame_;  // the lot's place in the scene
    double pendingMs_ = 0;
    double leadMs_ = 0;
    // Per object: its latest own animation (Animate Sim), kept after it ends so its last pose holds, and
    // the one before, fading out under it.
    struct OwnAnimations {
        std::optional<ts2::sim::PlayingAnimation> current, previous;
        // Where the Sim stood while `current` played (last drawn), and while `previous` did.
        std::optional<ts2::scene::Transform> currentPlace, previousPlace;
        // Where and on which storey its lights were last picked.
        std::array<float, 12> litPlace{};
        int litLevel = -1;
        // The last animation that played out and faded, whose last pose the Sim still holds: a walk
        // starting from it fades in over that pose.
        std::optional<ts2::sim::PlayingAnimation> held;
        bool walking = false;  // while the Sim walks, `previous` is what it did before the walk
        double walkStartMs = 0;  // when that walk started (a walk straight after another starts anew)
        // The walk's last clips as drawn, the heading they were drawn at, and when it ended (negative:
        // none), so that an animation starting as the walk ends fades in over the walk's last pose.
        std::vector<ts2::sim::LocoClip> endedClips;
        std::vector<ts2::sim::WalkPose> endedPoses;
        float endedHeading = 0;
        double walkEndMs = -1;
    };
    std::vector<ts2::sim::WalkPose> walkPoses_;
    std::map<std::uint32_t, OwnAnimations> ownAnimations_;
    // Per lot object: where the simulation had it when last drawn (place, facing, container, slot).
    struct ObjectPlace {
        std::int32_t x = 0, y = 0, level = 0;
        std::int16_t direction = 0, container = 0, slot = 0;
        bool gone = false;
        bool operator==(const ObjectPlace&) const = default;
    };
    std::map<std::uint32_t, ObjectPlace> objectPlaces_;
    // Per scene instance moved by its object's clip (see followClips): where it stands without the clip.
    std::map<std::size_t, ts2::scene::Transform> clipBases_;
    // Per multi-tile part and container slot: where the slot's bone sits in its lead's model (see partSlot).
    std::map<std::pair<std::int16_t, std::size_t>, std::optional<ts2::scene::Transform>> partSlots_;
    std::set<std::uint32_t> unskinned_;  // objects whose model can't be drawn skinned (see animateObjects)
    // Per neighbor: its held wants and fears (GUIDs) in the order they were held. Saved holds come
    // first, wants before fears.
    struct Hold {
        bool fear = false;
        std::uint32_t guid = 0;
        bool operator==(const Hold&) const = default;
    };
    std::map<std::int16_t, std::deque<Hold>> holds_;
    std::vector<std::pair<std::size_t, ts2::scene::Transform>> moved_;
    // Objects up to this id were on the lot when it was loaded or have been seen since (ids are
    // never given out twice).
    std::int16_t lastSeenId_ = 0;
    std::map<std::int16_t, std::size_t> created_;  // created objects in the scene: id -> instance
    std::vector<std::size_t> freeInstances_;       // instances of removed created objects, for reuse
    std::vector<std::size_t> changedInstances_;
    std::vector<std::size_t> relit_;
};

} // namespace app
