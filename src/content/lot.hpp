#pragma once

#include "formats/array3d.hpp"
#include "formats/cobject.hpp"
#include "formats/datatables.hpp"
#include "formats/objm.hpp"
#include "formats/objt.hpp"
#include "formats/stringmap.hpp"
#include "formats/fencepostlayer.hpp"
#include "formats/roofs.hpp"
#include "formats/terrain.hpp"
#include "formats/walllayer.hpp"
#include "formats/wallgraph.hpp"
#include "formats/xobj.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ts2::content {

struct LotObject {
    std::uint32_t id = 0;
    std::uint32_t resourceType = 0;
    std::optional<objt::Entry> type;
    xobj::SavedObject state;
};

struct LotProblem {
    std::uint32_t id = 0;
    std::string message;
};

struct Lot {
    objt::ObjectTypeTable types;
    objm::ObjectMap objectMap;
    objm::ObjectGroups objectGroups;  // ObjM instance 2
    std::vector<LotObject> objects;
    std::vector<LotProblem> problems;
    // Saved-object records of objects the object map does not list (left behind by the game).
    std::size_t staleRecords = 0;
    // The wall graph whose walls name the rooms on each side (instance 5).
    std::optional<wallgraph::WallGraph> roomGraph;
    // The wall graphs of the game's two other room layers (instances 7 and 6), by instance.
    std::map<std::uint32_t, wallgraph::WallGraph> layerGraphs;
    // c3DArray grids by instance. Instance 9 holds the room of every quarter tile.
    std::map<std::uint32_t, array3d::Array3D> grids;
    // c3DArray grids whose tiles hold lists of numbers, by instance (kListGridInstance).
    std::map<std::uint32_t, array3d::ListGrid> listGrids;
    // The floor and wall-covering patterns its grids and walls use, by id (pattern maps 0xE, 0xD).
    std::map<std::uint16_t, stringmap::Entry> floorPatterns, wallPatterns;
    // The coverings of the room graph's wall segments (wall layer instance 4).
    std::vector<walllayer::Entry> wallCoverings;
    // Fence posts (fence rails are wall segments whose wall layer type is the fence's GUID).
    std::vector<fencepostlayer::Post> fencePosts;
    std::vector<roofs::Roof> roofs;
    // The terrain's canvas and paint textures, and c2DArray grids by instance (paint i's strength,
    // 0-255 at four points per tile, is instance kFirstPaintMapInstance + i).
    std::optional<terrain::LotTexture> terrainTextures;
    std::map<std::uint32_t, terrain::Array2D> flatGrids;
    // Each object's model (its scene object record's resource node name, without "_cres"), by object
    // id. Multi-tile parts other than the lead have none.
    std::map<std::uint32_t, std::string> objectModels;
    // The animations each object was playing when the lot was saved, by object id (objects playing
    // none are left out).
    std::map<std::uint32_t, std::vector<cobject::SavedAnimation>> savedAnimations;
    // Each object's model joints as they were posed when the lot was saved, by object id.
    std::map<std::uint32_t, std::vector<cobject::SavedJoint>> savedJoints;
    // Each object's script data tables (instance = object id).
    std::map<std::uint32_t, datatables::DataTables> dataTables;
    // Relationship variables one of the lot's objects holds towards another (SREL, instance =
    // from << 16 | to), by object ids: e.g. a Sim already aware of another's greeting.
    std::map<std::pair<std::int16_t, std::int16_t>, std::vector<std::int32_t>> relationships;
    // The simulator's global variables as saved with the lot (SIMI), empty when it has none.
    std::vector<std::int16_t> globals;
};

constexpr std::uint32_t kRoomGridInstance = 9;
constexpr std::uint32_t kFirstPaintMapInstance = 0x5CBC;
// Grids saved with a variable size per tile: up to three bytes (c2DArray) and a list of u32s
// (c3DArray). What they hold is not known yet.
constexpr std::uint32_t kByteListGridInstance = 0x5CEE;
constexpr std::uint32_t kListGridInstance = 0x0C;
// Wall graphs of room layers 0 and 1 (room fields 1 "outside" and 7 read them).
constexpr std::uint32_t kLayerGraphInstances[2] = {7, 6};
// Floor pattern ids, one per quarter tile per level (ids into Lot::floorPatterns, 0 for none).
constexpr std::uint32_t kFloorGridInstance = 0;
constexpr std::uint32_t kFloorPatternMapInstance = 0xE;
constexpr std::uint32_t kWallPatternMapInstance = 0xD;
constexpr std::uint32_t kWallCoveringLayerInstance = 4;
// Corner heights per level, as floats, from the lot's lowest level up (one more corner than tiles
// along each side). On most lots grid level 0 is the terrain.
constexpr std::uint32_t kFloorHeightGridInstance = 1;
// A byte per quarter tile per level, in the floor grid's quarter order: 0 where the level has no
// surface (on the ground storey, over a pool or a sunken pond).
constexpr std::uint32_t kSurfaceGridInstance = 21;

// The covering layer names the walls of the wall graph (instance 24), whose ids the room graph shares
// only on lots whose walls were never renumbered: this gives the coverings by room graph wall, matched
// by place. A room graph segment with no wall there only divides rooms and gets none.
std::vector<walllayer::Entry> coveringsOfRoomWalls(const wallgraph::WallGraph& rooms, const wallgraph::WallGraph& walls,
                                                   const std::vector<walllayer::Entry>& coverings);

// Loads the objects saved in a lot package (Neighborhoods/<hood>/Lots/<hood>_Lot<n>.package).
// Like the game, the objects are those the object map (ObjM) lists, read with the map's version.
Lot loadLot(const std::filesystem::path& path);

// The index into the lot's grids of a level as walls, fences and roofs save it (0 = the ground
// storey). Grids start at the lot's lowest level, the room graph's firstLevel.
int gridIndex(const Lot& lot, std::int64_t level);
// The grid level an object saved on `storey` (its header's value1, 1 = the ground storey) stands on.
int gridLevel(const Lot& lot, std::int32_t storey);
// The grid level of the ground storey, or 0 when the floor grid has no such level.
int groundGridLevel(const Lot& lot);
// The lot from its ground storey up: its grids without the levels below the ground storey and its
// wall graphs without their walls there, so that grid level 0 is the ground storey, as the lot
// lighting counts levels.
Lot fromGroundStorey(const Lot& lot);

} // namespace ts2::content
