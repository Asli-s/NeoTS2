#pragma once

#include "content/lot.hpp"
#include "formats/scenegraph.hpp"
#include "sim/code_source.hpp"
#include "sim/route_grid.hpp"
#include "sim/route_items.hpp"
#include "sim/world.hpp"

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace ts2::sim {

// What routing needs from an object's model. The scene's assets read models, routing doesn't.
struct RouteModel {
    std::optional<scenegraph::DataValue> footprint;  // the model's "footprint" data list
    std::optional<RouteRect> extent;                 // its x/y extent over all meshes, model coordinates
    std::map<std::string, ModelBone> bones;          // its named joints at rest, by lower-case name
    // Its tree, each node after its parent: the lower-case name (empty for none), the parent's index
    // (-1 for a root) and the place relative to it at rest.
    struct Node {
        std::string name;
        int parent = -1;
        std::array<float, 3> translation{};
        std::array<float, 4> rotation{0, 0, 0, 1};
    };
    std::vector<Node> nodes;
};
// The joint `name` (lower case) of `model` placed in the model with an object's saved joints, each
// replacing the place of the node it names, none when the model has no such joint. Without saved
// joints this is `model.bones`.
std::optional<ModelBone> posedModelBone(const RouteModel& model, const std::string& name, std::span<const cobject::SavedJoint> saved);
// The model named `name` (a resource node name without "_cres"), or null when there is none. What it
// returns must stay valid as long as the route world is used.
using RouteModelLookup = std::function<const RouteModel*(const std::string& name)>;

// What routing reads from the game's models and build catalog.
struct RouteSources {
    FenceBlocks fenceBlocks;
    PostFootprint postFootprint;
    RouteModelLookup models;
};

// The footprint of `object` standing at (x, y) in tiles facing `direction` (object data 1), as
// routing and the game's placement test build it. `lead` and `model` are as for objectRouteItems.
std::vector<RouteRect> objectFootprintAt(const SimObject& object, const SimObject& lead, const ObjectType& type,
                                         const RouteModel* model, float x, float y, std::int16_t direction);

// The route items of one object, as the game adds them: its footprint, each rectangle a blocking
// core with a padded copy around it, or a single walk-through rectangle. None for objects the game
// leaves out (Sims, objects off the lot or held by another, doors and windows, walk-through objects
// without a cost). `lead` is the lead part of a multi-tile object (the object itself otherwise) and
// `model` the lead's model.
std::vector<RouteItem> objectRouteItems(const SimObject& object, const SimObject& lead, const ObjectType& type, CodeSource& code,
                                        const RouteModel* model);

// A lot's route grids, one per level: walls, fences and their posts, floors, and the objects' footprints.
class RouteWorld {
public:
    // The lot's walls, fences, posts and floors.
    RouteWorld(const content::Lot& lot, const FenceBlocks& fenceBlocks, const PostFootprint& postFootprint);

    // Adds every object's footprint, from the objects' current data (their flags are set by their
    // scripts while the lot loads, so this comes after).
    void addObjects(World& world, CodeSource& code, const content::Lot& lot, const RouteModelLookup& models);
    // Brings the footprints of `objects` up to date, as the game does before routing work: each
    // object's footprint items are replaced by its current ones (none once it is gone or held).
    // Doors keep their items. The game adds those elsewhere. Returns the levels whose grid changed.
    std::vector<int> updateObjects(World& world, CodeSource& code, const RouteModelLookup& models, const std::set<std::int16_t>& objects);

    // Where a Sim's rings go (see simRingItems): its level (0 = ground), its square there and whether
    // it is the target of the route being planned.
    struct SimRings {
        int level = 0;
        RouteRect square;
        bool target = false;
        bool operator==(const SimRings&) const = default;
    };
    // Puts Sim `sim`'s rings where `rings` says, or takes them away with none. Returns the levels
    // whose grid changed.
    std::vector<int> ringSim(std::int16_t sim, const std::optional<SimRings>& rings);
    // The Sims that have rings now.
    std::vector<std::int16_t> ringedSims() const;

    int levels() const { return static_cast<int>(grids_.size()); }
    // Level `level` (0 = ground), or null past the top.
    const RouteGrid* level(int level) const;

private:
    std::vector<RouteGrid> grids_;
    std::map<std::uint32_t, std::string> objectModels_;  // the lot's model name of each object id
    std::map<std::int16_t, SimRings> rings_;              // each ringed Sim's rings
};

} // namespace ts2::sim
