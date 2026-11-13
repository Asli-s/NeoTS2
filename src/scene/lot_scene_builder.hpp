// Builds a lot's scene from its saved file, as addLot runs it. Split over lot_scene.cpp and the
// lot_scene_*.cpp files beside it, each a part of the lot.
#pragma once

#include "content/lot.hpp"
#include "formats/scenegraph.hpp"
#include "formats/terrain.hpp"
#include "scene/builder.hpp"
#include "scene/lot_parts.hpp"
#include "scene/lot_scene.hpp"
#include "scene/pool_water.hpp"
#include "scene/rig.hpp"
#include "scene/sim_builder.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace ts2::scene::lotscene {

// A placed object, before objects held in slots are moved onto their container's slot.
struct Shown {
    const content::LotObject* object;
    int mesh;
    std::string model;
    Transform world;
    float yaw;
    std::size_t instance = 0;  // index into the scene's instances once placed
    std::string boneModel;     // the name the builder knows its posed model's bones by
    std::vector<scenegraph::MaterialOverride> overrides;
    std::vector<scenegraph::MaterialOverride> chosen;  // see PlacedObject::chosen
};

// Lot object data: 1 its facing, 2 the object it is held by, 3 its slot there, 0x13 a multi-tile
// part's lead and 0x2A its placement flags.
constexpr std::size_t kFacingData = 1, kContainerData = 2, kSlotData = 3, kLeadTileData = 0x13, kPlacementData = 0x2A;

// The container slots kept for each object, for placing what scripts put into them.
constexpr std::size_t kMaxShownSlots = 16;
// The pool's edge (wall style 0x1D) lets light through by walls.txt but the game treats it as stopping it.
constexpr std::uint32_t kPoolEdgeStyle = 0x1D;

// Middles of wall segments (doubled, so they are whole numbers) by level.
using EdgeKey = std::tuple<int, int, int>;
inline EdgeKey edgeKey(float x0, float y0, float x1, float y1, int level) {
    return {static_cast<int>(std::lround(x0 + x1)), static_cast<int>(std::lround(y0 + y1)), level};
}

class LotSceneBuilder {
public:
    LotSceneBuilder(SceneBuilder& builder, const content::Lot& lot, const std::filesystem::path& path, bool log)
        : b_(builder), assets_(builder.assets()), lot_(lot), path_(path), heights_(lot), log_(log) {}

    LotSceneStats build();

private:
    // Walls, fences and light maps (lot_scene.cpp).
    void collectWalls();
    void collectStageSides();
    void placeFencePosts();
    LightMaps& lightMaps();
    int addIncidence(const Image& lightMap);
    void makeFloorLightMaps();
    void log(const content::LotObject& o, const std::string& what, const std::string& model);
    void cutWalls();
    void drawWalls();

    // Objects and Sims (lot_scene_objects.cpp).
    void placeObjects();
    objectshadow::Facts shadowFactsOf(const content::LotObject& object, const std::string& model, int mesh);
    SunTest sunTestOf(const content::LotObject& object);
    void placeSims();
    void holdBySim(const content::LotObject& sim, const std::filesystem::path& characterFile, Actor& actor,
                   const std::vector<Transform>& world);
    std::array<float, 3> heightRange(int mesh) const;

    // The terrain and the floors (lot_scene_floors.cpp).
    void drawTerrain();
    void drawFloors();
    // The slab under each floored quarter tile above the ground (its ceiling and edge bands).
    void drawFloorSlabs(const array3d::Array3D& floors, int ground, const array3d::Array3D* surface);

    // Pool and pond water (lot_scene_water.cpp).
    std::array<float, 6> poolBox(const std::vector<poolwater::Quarter>& quarters, int ground);
    void drawPoolWater();
    void drawPondWater();
    void drawPondsAsPools(const terrain::Array2D& water, const std::function<float(int, int)>& terrain, int ground);

    // Roofs (lot_scene_roofs.cpp).
    void shapeRoofs();
    void placeRoofs();
    static RoofShape gridShape(const roofs::Roof& roof, float eave, RoofGrid grid);

    SceneBuilder& b_;
    Assets& assets_;
    const content::Lot& lot_;
    std::filesystem::path path_;
    Heights heights_;
    bool log_;
    LotSceneStats stats_;
    int edge_ = -1;
    int wallTop_ = -1;
    std::vector<ThickWall> walls_;
    // For each of walls_, its room graph wall id (-1 for none) and whether its sides take coverings.
    std::vector<std::int32_t> wallIds_;
    std::vector<bool> wallCoverable_;
    int wallBase_ = 0;  // index of walls_[0] in the scene's walls
    std::map<EdgeKey, std::size_t> wallAt_;  // walls_ by their middle
    struct ShapedRoof {
        Surfaces surfaces;
        RoofShape shape;
        RoofLook look;
        std::vector<GreenhousePiece> greenhouse;
        int level = 0;
        int valley = -1;
    };

    std::vector<ShapedRoof> roofs_;  // reserved up front: walls point at their shapes
    std::vector<Shown> shown_;
    RigLibrary rigs_{assets_};
};

} // namespace ts2::scene::lotscene
