#pragma once

#include "content/lot.hpp"
#include "formats/scenegraph.hpp"
#include "sim/code_source.hpp"
#include "sim/route_grid.hpp"
#include "sim/route_items.hpp"
#include "sim/world.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ts2::sim {

// What routing needs from an object's model; the scene's assets read models, routing doesn't.
struct RouteModel {
    std::optional<scenegraph::DataValue> footprint;  // the model's "footprint" data list
    std::optional<RouteRect> extent;                 // its x/y extent over all meshes, model coordinates
};
// The model named `name` (a resource node name without "_cres"), or null when there is none. What it
// returns must stay valid until the route world is built.
using RouteModelLookup = std::function<const RouteModel*(const std::string& name)>;

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

    int levels() const { return static_cast<int>(grids_.size()); }
    // Level `level` (0 = ground), or null past the top.
    const RouteGrid* level(int level) const;

private:
    std::vector<RouteGrid> grids_;
};

} // namespace ts2::sim
