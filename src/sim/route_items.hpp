#pragma once

#include "formats/array3d.hpp"
#include "formats/fencepostlayer.hpp"
#include "formats/stringmap.hpp"
#include "formats/terrain.hpp"
#include "formats/walllayer.hpp"
#include "formats/wallgraph.hpp"
#include "sim/route_grid.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace ts2::sim {

// Tells whether a fence (by its GUID in the build catalog) stops Sims. Unknown GUIDs and fences Sims
// may walk over (flowerbed edging) don't.
using FenceBlocks = std::function<bool(std::uint32_t guid)>;

// The route items of one level's walls: per piece a thin solid core and a padded copy (fences have
// their own pair of types), and a costly square at each wall end and where neighbouring walls leave a
// gap wider than 1.05 pi. A diagonal piece is a row of small squares across its tile, each padded.
// Diagonals are kept only on the lot's `width` by `height` tiles, one per tile and direction, so a
// broken file can't make millions of them.
std::vector<RouteItem> wallRouteItems(const wallgraph::WallGraph& rooms, const std::vector<walllayer::Entry>& coverings, int level,
                                      const FenceBlocks& fenceBlocks, int width, int height);

// A fence's post footprint around the post's point (its model's extent), or none for fences that
// don't stop Sims.
using PostFootprint = std::function<std::optional<RouteRect>(std::uint32_t fenceGuid)>;

// The route items of one level's fence posts: the post's footprint and a padded copy around it.
std::vector<RouteItem> postRouteItems(const std::vector<fencepostlayer::Post>& posts, int level, const PostFootprint& footprint);

// The route items of one level's floors: a tile per floored tile (every lot tile on the ground, bare
// ground costing a little more and roads more still), and a strip along each floor edge keeping Sims
// back. `floors` holds pattern ids per quarter tile, `patterns` their names, and `level` counts from
// the ground storey (0), the grid starting at `lowestLevel` (-1 with a basement). `heights` (corner
// heights) and `surface` (grid 21) find ground tiles under a raised floor, which block as foundation.
// `water` (height per corner) blocks a ground tile with a corner under water, as a pond's are.
std::vector<RouteItem> floorRouteItems(const array3d::Array3D& floors, const array3d::Array3D* heights, const array3d::Array3D* surface,
                                       const std::map<std::uint16_t, stringmap::Entry>& patterns, int level, int lowestLevel, int width,
                                       int height, const terrain::Array2D* water = nullptr);

// The rings the path search keeps round another Sim, filed under `owner` (the Sim's id): its
// square grown by 0.1875 tiles at cost 7.5 and by 0.0625 at cost 10, and by 0.375 at cost 5 unless
// it is the route's target. They stay walkable: paths bend round them but may still pass.
std::vector<RouteItem> simRingItems(const RouteRect& square, bool target, std::uint32_t owner);

} // namespace ts2::sim
