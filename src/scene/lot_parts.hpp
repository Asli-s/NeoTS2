#pragma once

// Pieces of a lot's scene shared by lot_scene.cpp, walls.cpp and roofs.cpp.

#include "content/lot.hpp"
#include "formats/roofs.hpp"
#include "scene/builder.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ts2::scene {

constexpr float kPi = 3.14159265358979f;
constexpr float kLevelHeight = 3.0f;      // storey height, used where a lot has no height grid
constexpr float kWallThickness = 0.15f;   // walls with thickness
constexpr float kFloorThickness = 0.15f;  // the slab under floors above the ground
// How high the walls-down view leaves cut walls (the game's, and cutaway is taken to cut them the same).
constexpr float kCutWallHeight = 0.2f;

// The lot's corner heights per grid level (grid 1), from the lot's lowest level up.
class Heights {
public:
    explicit Heights(const content::Lot& lot);
    // Keeping its own copy of the height grid, so it outlives the lot.
    explicit Heights(std::shared_ptr<const array3d::Array3D> grid);

    // The height at the grid corner nearest (x, y).
    float corner(int level, float x, float y) const;
    // The average height of the four corners of the tile containing (x, y).
    float tile(int level, float x, float y) const;

    const array3d::Array3D* grid() const { return grid_; }

private:
    float at(int level, int x, int y, bool& found) const;

    std::shared_ptr<const array3d::Array3D> kept_;
    const array3d::Array3D* grid_ = nullptr;
};

// Whether a level of the surface grid (content::kSurfaceGridInstance) is open at (x, y): the
// quarter of the tile holding the point has no surface there.
bool surfaceOpen(const array3d::Array3D& surface, int level, float x, float y);

// A wall segment: its two ends and, at each, the heights of its bottom and top.
struct WallQuad {
    float x0, y0, x1, y1, bottom0, top0, bottom1, top1;
};

// A roof as the game builds it: heights in grid steps on points half a tile apart over the roof's
// rectangle and overhang (see roof_grid.cpp). Only roof types built this way so far have one.
struct RoofGrid {
    // gable (types 3, 4), hip (5), hipped shed (2), single slope (1), hip with a flat top (6)
    enum class Kind { gable, hip, shed, slope, flatHip };
    int type = 0;
    Kind kind = Kind::gable;
    float minX = 0, minY = 0;  // the first grid point
    int nx = 0, ny = 0;        // grid points along x and y
    float maxX() const;        // the last grid point
    float maxY() const;
    float scale = 0.5f;        // rise per grid step (the saved slope)
    float eave = 0;            // the height of the eaves' storey
    int level = 0;             // the level the roof is saved on
    bool ridgeAlongX = true;
    // Which way the roof lies, as the game keeps it: for gables and hips the ridge runs along x, for
    // sheds and slopes the roof is longer along y. It sets which faces the ridge stands between.
    bool lengthwise = true;
    int high = -1;             // a shed's or slope's high side: 0 low x, 1 high x, 2 low y, 3 high y
    int cap = 0;               // a flat-topped hip's height, in grid steps
    std::vector<float> steps;  // per point (row by row), in grid steps
    float peak = 0;

    [[nodiscard]] static std::optional<RoofGrid> of(const roofs::Roof& roof, float eave);
    // Which way the triangles at a cell point (row + col even: split along (0,0)-(1,1)).
    int parity(int row, int col) const;
    // Facing bits, by a triangle's uphill direction: 1 -x, 2 +x, 4 -y, 8 +y. Another roof touching
    // this one at a ridge or hip point does not join a triangle facing one of the point's bits.
    int ridgeMask(int row, int col) const;  // on the ridge (the highest points), 0 elsewhere
    int hipMask(int row, int col) const;    // on a hip (a diagonal from a corner), 0 elsewhere
    // A grid point's height in a layer (0 the top, 1 between the edge board's bands, 2 the underside).
    float z(int row, int col, int layer) const;
    // The top's height anywhere over the grid.
    float surface(float x, float y) const;
    // The top's height at a grid point (none elsewhere or outside the grid).
    [[nodiscard]] std::optional<float> pointHeight(float x, float y) const;
    // The top's height where it covers a roof on fromLevel (for a lower level, only within the walls).
    [[nodiscard]] std::optional<float> coveringHeight(float x, float y, int fromLevel) const;
};

// Whether a saved roof's corners and slope are real numbers on a lot-sized area. A broken file's
// roof is left out rather than built huge.
bool sensibleRoof(const roofs::Roof& roof);

// A roof's shape: its height anywhere is the lowest of the planes rising from its sloped edges. Its
// gable ends are left open, to be closed by the walls under them.
struct RoofShape {
    float eave = 0, slope = 1;
    float thickness = 0;  // the slab's depth: its underside runs this far below its top
    // Its sloped faces in plan, each with its plane: height = c + cx * x + cy * y.
    struct Face {
        std::vector<std::array<float, 2>> outline;
        std::array<float, 3> plane;
    };
    std::vector<Face> faces;
    std::vector<std::array<float, 4>> planes;  // a point on a sloped edge and its inward normal (x, y, nx, ny)
    // The lines under the gable ends (the sides of the roof's rectangle) from a to b, and outward.
    struct Gable {
        std::array<float, 2> a, b, out;
    };
    std::vector<Gable> gables;
    std::optional<RoofGrid> grid;  // when set, the roof's surface (the rest is not used)

    float height(float x, float y) const;
    // Where the roof's height bends on the way from p to q, as fractions of the way, with 0 and 1.
    std::vector<float> bends(const std::array<float, 2>& p, const std::array<float, 2>& q) const;
};


// An attic wall (the wall style the game adds under a roof's gable end) reaches from its floor to the
// underside of the roofs it closes, above or below its own top. With none it keeps its own top.
struct WallCeiling {
    std::vector<const RoofShape*> gables;

    float at(float x, float y, float top) const;
    // Where the top bends on the way from p to q, as fractions of the way, with 0 and 1.
    std::vector<float> bends(const std::array<float, 2>& p, const std::array<float, 2>& q) const;
};

// A wall segment drawn with thickness: the material of each side by name (empty: not drawn) and
// where each side's edge ends at both ends of the segment.
struct ThickWall {
    WallQuad q;
    std::array<std::int32_t, 2> points{};  // wall graph point ids at (x0, y0) and (x1, y1)
    int level = 0;
    bool thick = true;  // wall styles without thickness (deck skirts) are a single plane
    bool cuttable = true;  // the wall style may be cut down in views (foundations and deck skirts not)
    // The style lets light through (walls.txt blocksLight false, but not the pool's edge): the game
    // draws it with its pattern's own material, lit as things outdoors, without a light map.
    bool clear = false;
    bool toRoof = false;   // an attic wall: up to the roof over it (see WallCeiling)
    bool roomLeft = false, roomRight = false;  // a room, not outdoors, on that side
    std::string left, right;
    std::string leftCovering, rightCovering;  // each side's covering, without door and window holes
    WallCeiling ceiling;
    std::string leftMask, rightMask;  // wall masks of doors and windows in this wall
    bool leftMaskMirrored = false, rightMaskMirrored = false;
    std::array<float, 2> leftA{}, leftB{}, rightA{}, rightB{};
    std::array<bool, 2> joined{};  // another thick wall meets it at (x0, y0), at (x1, y1)
};

// The top of the space where three or more walls meet, which no wall's own top covers: the point
// and the side corners around it, at the walls' top height there.
struct WallHub {
    float x = 0, y = 0, top = 0, bottom = 0;
    std::vector<std::array<float, 2>> corners;
    std::vector<std::size_t> walls;  // the walls meeting there
};

// A door or window's wall mask: the wall tile it cuts, as a tile offset from the object's own
// (east, south) and level, and the face it cuts by the direction it faces (n: the object's front).
struct WallMask {
    int east = 0, south = 0, level = 0;
    std::string face, material;
};

// Wall masks by lowercase model name, from material names "<model>_<east>_<south>_<level>_<face>_wallmask".
std::map<std::string, std::vector<WallMask>> wallMasks(const std::vector<std::string>& materialNames);

// Joins the walls meeting at each wall graph point: going around the point, each wall's side line
// ends where it meets the facing side line of the next wall (mitred corners, T and cross joins).
std::vector<WallHub> joinWalls(std::vector<ThickWall>& walls);

// A light-map rect a wall face reads, as texture coordinates: u0, v0 (its top) and u1, v1 (its bottom).
using LightRect = std::array<float, 4>;

// A wall's faces: each side's covering, its top (in `topMaterial`, the texture tiled once a tile over the
// ground plan, its ends too). With `cutHeight` they are cut down to that
// height above its bottom, otherwise up to its ceiling. With `lightRects` (left, right side), the
// sides also get light-map coordinates as the game lays them: along the face from its start to
// its end (cut short at inside corners), from the bottom of the rect up a storey.
void addThickWall(SceneBuilder& builder, Surfaces& surfaces, const ThickWall& w, int topMaterial, float cutHeight = 0,
                  const std::array<std::optional<LightRect>, 2>* lightRects = nullptr);

// The materials a roof is drawn with.
struct RoofLook {
    int top = -1, under = -1, trim = -1, edges = -1, cap = -1, valley = -1;
    int atlas = -1;  // the whole top texture (grid roofs pick their pieces from it)
};

// A grid roof's top, underside and edge board. `others` are the lot's other grid roofs, which cut
// this one where they are above it and change the pieces where they meet it.
void addRoofGrid(Surfaces& surfaces, const RoofGrid& roof, const std::vector<const RoofGrid*>& others, const RoofLook& look);

// A cone, dome or octagonal spire (types 7 to 9) as the game builds it. False for other types. A
// spire reaches a tile past its rectangle, within the lot (lotWidth x lotHeight tiles).
bool addRoundRoof(Surfaces& surfaces, const roofs::Roof& roof, float eave, const RoofLook& look, float lotWidth, float lotHeight);

// The Seasons greenhouse roof: a glass gable built from model pieces, with no shingles.
constexpr std::uint32_t kGreenhouseRoof = 15;

// One piece of a greenhouse roof: its model, where it stands and its turn about the vertical.
struct GreenhousePiece {
    std::string model;
    float x = 0, y = 0, z = 0, angle = 0;
};

// A greenhouse roof's pieces, eaves at height `eave`: one per tile, rising one unit per tile
// to the ridge, and a stack at each gable end one tile past the rectangle.
std::vector<GreenhousePiece> greenhousePieces(const roofs::Roof& roof, float eave);

// A roof over its rectangle, eaves at height `eave`. `slope` is rise over run.
RoofShape addRoof(Surfaces& surfaces, const roofs::Roof& roof, float eave, float slope, const RoofLook& look);

// Where roof `a` runs into roof `b` (a dormer into the main roof): a strip of `material` along the
// line where their surfaces meet.
void addValleys(Surfaces& surfaces, const RoofShape& a, const RoofShape& b, int material);


} // namespace ts2::scene
