#pragma once

#include "content/build_scripts.hpp"
#include "content/lot.hpp"
#include "content/wall_manager.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

namespace ts2::content {

// A corner of the lot's height grid, in the wall graph's x and y, on a level as walls count them (0 is
// the ground storey).
struct TerrainCorner {
    std::int32_t x = 0, y = 0, level = 0;
    bool operator==(const TerrainCorner&) const = default;
};

// Why the game refused to change a corner's height: a placement error code and the point it is about.
struct TerrainRefusal {
    int code = 0;
    float x = 0, y = 0;
    std::int32_t level = 0;
};

struct TerrainObject {
    bool sim = false;
    std::uint16_t placement = 0;  // object data 0x2A
    std::uint16_t hidden = 0;     // object data 0x22, the hidden flags
    std::uint16_t wallFlags = 0;  // object data 0x0D
    // Its footprint rectangles where it stands, x0 y0 x1 y1 in the wall graph's axes, and whether walls
    // test it by their bounding box: a door whose wall flags have bits 0x50, or an object with OBJD
    // 0x4E set, unless it is a part of a multi-tile object.
    std::vector<std::array<float, 4>> footprint;
    bool byBox = false;
    std::int32_t type = 0;        // object definition field 9
    float x = 0, y = 0;           // where it stands, in the wall graph's axes
    std::int32_t direction = 0;   // object data 1
    float elevation = 0;
    // Helper objects (Come And See, Destination, Swim Here, grave markers) and models with a node named
    // "post_trans" never stop the storey under them getting lower.
    bool belowExempt = false;
};
// The objects on a tile of the lot, in the wall graph's axes, on a level as walls count them.
using TerrainObjects = std::function<std::vector<TerrainObject>(std::int32_t x, std::int32_t y, std::int32_t level)>;

// A wall style as the wall checks tell them apart: its kind and the rules its script or fence sets.
struct WallRules {
    // Walls 300 and 301 are the walls.txt styles of those ids.
    enum class Kind { Wall, Fence, FenceArch, HalfWall, Wall300, Wall301 };
    Kind kind = Kind::Wall;
    bool mayPlaceDiagonally = true;
    bool submersible = true;  // may stand with every side in pond water
    bool flatBottom = true;   // both ends at one height
    bool supportUnderneath = true;
    bool blocksLocomotion = true;
};

// Height and pond water changes, each corner once with its first old and last new value, for undo.
class TerrainChanges {
public:
    struct Height {
        TerrainCorner corner;
        float before = 0, after = 0;
    };
    struct Water {
        std::int32_t x = 0, y = 0;
        float before = 0, after = 0;
    };
    bool empty() const { return heights_.empty() && water_.empty(); }
    // Writes every corner's other value and swaps them: the first call undoes, the next redoes.
    void toggle(Lot& lot);
    void noteHeight(const TerrainCorner& corner, float before, float after);
    void noteWater(std::int32_t x, std::int32_t y, float before, float after);
    std::vector<Height> heights() const;
    std::vector<Water> water() const;

private:
    std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>, Height> heights_;
    std::map<std::pair<std::int32_t, std::int32_t>, Water> water_;
};

// The game's ground levelling for the wall and floor tools, with its terrain check. Changes go into
// `changes` as they are made.
class TerrainEdit {
public:
    // `beachSide` is the lot's beach side on a beach lot (see isBeachLocation), none elsewhere.
    TerrainEdit(Lot& lot, const WallManager& walls, const BuildScripts& scripts, TerrainObjects objects, TerrainChanges& changes,
                std::optional<std::uint8_t> beachSide);

    float height(const TerrainCorner& corner) const;
    // Whether a tile quarter (one bit in the mask) is held up from right below, for the wall checks'
    // support test. Without it only the ground storey is.
    void setHeldUp(std::function<bool(const TileQuarter&)> heldUp) { heldUp_ = std::move(heldUp); }
    // Whether a floor may go on a tile, for the check's flag 8, which refuses a corner of such a tile
    // (error 0x6C). The tile is in the wall graph's axes, on a level as walls count them.
    void setFloorAllowed(std::function<bool(const TerrainCorner& tile)> allowed) { floorAllowed_ = std::move(allowed); }
    // The game's terrain check of putting `corner` at `h`. With `refusals` every reason is listed,
    // without it the check stops at the first.
    bool check(const TerrainCorner& corner, float h, std::uint32_t flags, std::vector<TerrainRefusal>* refusals) const;
    // Each returns false when any corner was refused. A diamond room's corners are grown by two all round.
    bool flattenSection(const WallSegment& section, float h, std::uint32_t style, std::vector<TerrainRefusal>& refusals);
    bool flattenBox(const TerrainCorner& a, const TerrainCorner& b, float h, std::uint32_t style, std::vector<TerrainRefusal>& refusals);
    bool flattenDiamond(const TerrainCorner& a, const TerrainCorner& b, float h, std::uint32_t style, std::vector<TerrainRefusal>& refusals);
    // The floor tool's levelling: every corner of the box from `a` to `b` goes to `h` where the check allows.
    void flattenFloorBox(const TerrainCorner& a, const TerrainCorner& b, float h);
    // Refused (error 0x46) when every side of the section that is on the lot has a pool one storey down.
    bool overPool(const WallSegment& section, std::vector<TerrainRefusal>& refusals) const;
    // The game's checks of building `section` once its ground is levelled. Returns false when refused,
    // with the reasons in `refusals`. A side off the lot stops the check at once, the other reasons
    // gather.
    bool checkWall(const WallSegment& section, const WallRules& rules, std::vector<TerrainRefusal>& refusals) const;
    // The check's flags for the wall and floor tools' levelling.
    static constexpr std::uint32_t kWallFlags = 0x2BB7;
    // After a foundation, deck or stage tile is taken out: puts `corner` (on the storey that was the
    // platform's top) a storey above the corner under it, where the check allows. The check also
    // refuses corners of tiles that still hold a floor up, when the corner is above the ground storey
    // or a platform is near it one storey down.
    void platformRemoved(const TerrainCorner& corner);

private:
    bool flattenCorner(const TerrainCorner& corner, float h, std::uint32_t style, std::vector<TerrainRefusal>& refusals);
    bool flattenOne(const TerrainCorner& corner, float h, std::vector<TerrainRefusal>& refusals);
    void setHeight(const TerrainCorner& corner, float h);
    void write(const TerrainCorner& corner, float h);
    void push(const TerrainCorner& corner, float h);
    void reflood(const TerrainCorner& corner, float terrainBefore);
    void put(const TerrainCorner& corner, float h);

    bool cornerInLot(const TerrainCorner& c) const;
    bool tileInLot(const TerrainCorner& t) const;
    WallRules::Kind kindOf(std::uint32_t style) const;
    bool crossesDiagonal(const TileQuarter& tile, bool stageSide) const;
    bool supported(const WallSegment& section) const;
    bool boxCutByWall(const TerrainObject& o, std::int32_t level) const;
    bool pieceOn(const TerrainCorner& tile) const;
    // The road and sidewalk tiles, which the game keeps from being built on or levelled.
    bool tileLocked(const TerrainCorner& t) const;
    // On the lot but not in its outer ring of tiles.
    bool tileInside(const TerrainCorner& t) const;
    // Whether a corner of the tile is ground storey ground under pond water.
    bool tileWet(const TerrainCorner& t) const;
    bool tilesBad(const TerrainCorner& c, std::vector<TerrainRefusal>* refusals) const;
    bool floorBad(const TerrainCorner& c, std::vector<TerrainRefusal>* refusals) const;
    bool objectsBad(const TerrainCorner& c, float h, std::vector<TerrainRefusal>* refusals) const;
    bool upBad(const TerrainCorner& c, float h, std::uint32_t flags, std::vector<TerrainRefusal>* refusals) const;
    bool downBad(const TerrainCorner& c, float h, std::uint32_t flags, std::vector<TerrainRefusal>* refusals) const;
    bool wallsBad(const TerrainCorner& c, std::vector<TerrainRefusal>* refusals) const;
    bool poolBad(const TerrainCorner& c) const;
    bool floorHeldBad(const TerrainCorner& c, std::vector<TerrainRefusal>* refusals) const;
    bool platformNear(const TerrainCorner& c) const;
    bool poolUnder(const TileQuarter& q) const;
    bool fenceBad(const TerrainCorner& c, float h, std::vector<TerrainRefusal>* refusals) const;
    bool pieceBad(const TerrainCorner& c) const;
    bool beachBad(const TerrainCorner& c) const;
    float groundAt(const TerrainCorner& changed, float h, float x, float y, std::int32_t level) const;
    bool needsFlat(std::uint32_t style, bool top) const;
    void refuse(std::vector<TerrainRefusal>* refusals, int code, float x, float y, std::int32_t level) const;

    Lot& lot_;
    const WallManager& walls_;
    const BuildScripts& scripts_;
    TerrainObjects objects_;
    std::function<bool(const TileQuarter&)> heldUp_;
    std::function<bool(const TerrainCorner&)> floorAllowed_;
    TerrainChanges& changes_;
    WorldBounds bounds_;
    std::optional<std::uint8_t> beachSide_;
};

} // namespace ts2::content
