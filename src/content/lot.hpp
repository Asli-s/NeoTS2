#pragma once

#include "formats/array3d.hpp"
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
    std::vector<LotObject> objects;
    std::vector<LotProblem> problems;
    // Saved-object records of objects the object map does not list (left behind by the game).
    std::size_t staleRecords = 0;
    // The wall graph whose walls name the rooms on each side (instance 5).
    std::optional<wallgraph::WallGraph> roomGraph;
    // The wall graphs of the game's two other room layers (instances 7 and 6), by instance.
    std::map<std::uint32_t, wallgraph::WallGraph> layerGraphs;
    // c3DArray grids by instance; instance 9 holds the room of every quarter tile.
    std::map<std::uint32_t, array3d::Array3D> grids;
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
    // Each object's script data tables (instance = object id).
    std::map<std::uint32_t, datatables::DataTables> dataTables;
};

constexpr std::uint32_t kRoomGridInstance = 9;
constexpr std::uint32_t kFirstPaintMapInstance = 0x5CBC;
// Wall graphs of room layers 0 and 1 (room fields 1 "outside" and 7 read them).
constexpr std::uint32_t kLayerGraphInstances[2] = {7, 6};
// Floor pattern ids, one per quarter tile per level (ids into Lot::floorPatterns; 0 for none).
constexpr std::uint32_t kFloorGridInstance = 0;
constexpr std::uint32_t kFloorPatternMapInstance = 0xE;
constexpr std::uint32_t kWallPatternMapInstance = 0xD;
constexpr std::uint32_t kWallCoveringLayerInstance = 4;
// Corner heights per level, as floats: the terrain on level 0, each floor above (one more corner
// than tiles along each side).
constexpr std::uint32_t kFloorHeightGridInstance = 1;

// Loads the objects saved in a lot package (Neighborhoods/<hood>/Lots/<hood>_Lot<n>.package).
// Like the game, the objects are those the object map (ObjM) lists, read with the map's version.
Lot loadLot(const std::filesystem::path& path);

} // namespace ts2::content
