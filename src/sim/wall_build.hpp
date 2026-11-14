#pragma once

#include "formats/array3d.hpp"
#include "formats/stringmap.hpp"
#include "formats/wallgraph.hpp"
#include "formats/walllayer.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ts2::sim {

// Build mode's wall tool on a lot's room wall graph and wall layer.

// The wall style the Wall tool builds (walls.txt: a full wall, thick, floor to floor).
constexpr std::uint32_t kPlainWallStyle = 1;
// A room a new wall's sides have until rooms are numbered again.
constexpr std::int32_t kUnnumberedRoom = -0x7FFF;

// One tile edge on a level, from corner (x0, y0) to (x1, y1) next to it, in wall graph tiles.
struct WallStep {
    std::int32_t level = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// The tile edges along a straight line from corner (x0, y0) to (x1, y1) on a level, one a tile, from
// the start. A line neither along x nor along y runs along the longer of the two from the start.
std::vector<WallStep> wallSteps(std::int32_t level, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1);

// Whether a tile edge lies inside the lot: not on its border (where the graph traces the lot's edge).
bool insideLot(const wallgraph::WallGraph& graph, const WallStep& step);

// Builds a wall of style kPlainWallStyle on each tile edge `allowed` lets it, covered on both sides with
// `pattern` (a name for the lot's wall pattern list). An edge with a wall or fence already is left; a
// segment there that only divided rooms becomes a wall. New segments and points join the graph with
// fresh ids, their sides in kUnnumberedRoom. Returns the graph ids of the walls built.
std::vector<std::int32_t> buildWalls(wallgraph::WallGraph& graph, std::vector<walllayer::Entry>& coverings,
                                     std::map<std::uint16_t, stringmap::Entry>& patterns, const std::vector<WallStep>& steps,
                                     const std::string& pattern, const std::function<bool(const WallStep&)>& allowed);

// Takes down the walls of style kPlainWallStyle on the tile edges `allowed` lets it: their segments
// leave the graph and the wall layer, and points no segment uses any longer go too. Returns their ids.
std::vector<std::int32_t> removeWalls(wallgraph::WallGraph& graph, std::vector<walllayer::Entry>& coverings,
                                      std::map<std::uint16_t, stringmap::Entry>& patterns, const std::vector<WallStep>& steps,
                                      const std::function<bool(const WallStep&)>& allowed);

// Numbers the rooms on a level again after its walls changed, as each segment's sides name them:
// the places its segments enclose apart from one another. A place reaching the lot's edge is the
// outdoors (room 0). Every other keeps the room most of its walls named before, as long as no larger
// place took it, else gets a new number.
void renumberRooms(wallgraph::WallGraph& graph, std::int32_t level);

// Writes a level's rooms, as the wall graph names them now, into the room grid the lot saves
// (kRoomGridInstance: a 16-bit room per quarter tile, its levels from the graph's firstLevel). Quarters
// the graph names no room for keep theirs.
void updateRoomGrid(array3d::Array3D& rooms, const wallgraph::WallGraph& graph, std::int32_t level);

} // namespace ts2::sim
