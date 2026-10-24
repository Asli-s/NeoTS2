#pragma once

#include "scene/assets.hpp"
#include "sim/route_world.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace app {

// A model's x/y extent over all its meshes at full detail, or none when it has no meshes.
[[nodiscard]] std::optional<ts2::sim::RouteRect> modelExtent(ts2::scene::Assets& assets, const std::string& name);

// The route models one lot needs (its objects' models and its fence posts), read ahead on the
// thread that loads the lot, which owns the assets then. Read-only once made.
struct RouteModelTable {
    std::map<std::string, ts2::sim::RouteModel> models;              // by model name
    std::map<std::uint32_t, std::optional<ts2::sim::RouteRect>> posts;  // post footprint by fence GUID
};
std::shared_ptr<const RouteModelTable> readRouteModels(ts2::scene::Assets& assets, const std::filesystem::path& lot);

// What routing reads from the assets: fences, fence posts and object models, from `table` when it
// has them, else read from `assets` once and kept. Only use the result on one thread at a time with
// everything else that reads `assets`, which must outlive it.
ts2::sim::RouteSources routeSources(ts2::scene::Assets& assets, std::shared_ptr<const RouteModelTable> table = nullptr);

} // namespace app
