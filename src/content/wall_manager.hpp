#pragma once

#include "content/build_scripts.hpp"
#include "formats/array3d.hpp"
#include "formats/fencepostlayer.hpp"
#include "formats/stringmap.hpp"
#include "formats/walllayer.hpp"
#include "formats/wallgraph.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace ts2::content {

// From corner (ax, ay) to corner (bx, by), in tiles on the wall graph's axes.
struct WallSegment {
    float ax = 0, ay = 0, bx = 0, by = 0;
    std::int32_t level = 0;
    WallSegment reversed() const { return {bx, by, ax, ay, level}; }
};

// A quarter of a tile, or two neighbouring quarters (a half cut by a diagonal): the tile and a mask of
// quarter bits, 1 the -x quarter, 2 the +y, 4 the +x and 8 the -y.
struct TileQuarter {
    std::int32_t x = 0, y = 0, level = 0;
    std::uint8_t mask = 0;
};

// The quarter on a section's left, the side its graph's leftRoom names (with y downward).
TileQuarter leftQuarter(const WallSegment& segment);

// The tile's diagonals cut it into the four quarters.
TileQuarter quarterAt(float x, float y, std::int32_t level);

// The lot's tile bounds, ends included.
struct WallBounds {
    std::int32_t minX = 0, minY = 0, minLevel = 0;
    std::int32_t maxX = 0, maxY = 0, maxLevel = 0;
};

// A room grid (lot grids 9, 10 and 11): four 16-bit room ids a tile, one for each quarter.
class RoomGrid {
public:
    RoomGrid(array3d::Array3D& grid, std::int32_t firstLevel, const WallBounds& bounds);
    // The room of the lowest quarter in the mask, 0xFFFF for none or off the grid.
    std::uint16_t roomAt(const TileQuarter& q) const;
    // Queues the quarter on a section's left for room `id`. A quarter's first queue wins.
    void queue(const WallSegment& segment, std::uint16_t id);
    // Gives every queued quarter its room and spreads it over the quarters around that held the old one.
    void flush();

private:
    using Key = std::tuple<std::int32_t, std::int32_t, std::int32_t, std::int8_t>;  // level, x, y, mask
    struct Queued {
        std::uint16_t id = 0;
        bool noSeed = false;  // set when a quarter beside it is queued, and then it doesn't spread
    };
    std::uint16_t* cell(std::int32_t level, std::int32_t x, std::int32_t y);
    void set(std::uint16_t id, const TileQuarter& q);
    void spread(std::uint16_t oldId, std::uint16_t id, const TileQuarter& from);
    bool inBounds(float x, float y) const;

    array3d::Array3D& grid_;
    std::int32_t firstLevel_ = 0;
    WallBounds bounds_;
    std::map<Key, Queued> queued_;
};

// A wall graph at run time, as quad-edges: corners, sections and the rooms on their sides. A corner's
// sections go round it by angle.
class WallLayer {
public:
    explicit WallLayer(const wallgraph::WallGraph& saved);
    ~WallLayer();
    WallLayer(const WallLayer&) = delete;
    WallLayer& operator=(const WallLayer&) = delete;

    bool contains(const WallSegment& segment) const;
    // The sections with an end at (x, y), each from that point, in their order round it.
    std::vector<WallSegment> segmentsAt(float x, float y, std::int32_t level) const;
    // The section the way round it was added, when it is in the layer.
    std::optional<WallSegment> storedOf(const WallSegment& segment) const;
    // The id of the section between the segment's corners, either way round, or -1.
    std::int32_t idOf(const WallSegment& segment) const;
    // Adds a section and, with a room grid, splits the room it closes. `roomOf` gives a section's left room.
    void add(const WallSegment& segment, RoomGrid* grid, const std::function<std::int32_t(const WallSegment&)>& roomOf);
    // Takes a section out and, with a room grid, joins the rooms on its sides. False when there is none.
    bool remove(const WallSegment& segment, RoomGrid* grid, const std::function<std::int32_t(const WallSegment&)>& roomOf);
    wallgraph::WallGraph saved() const;

private:
    struct Vertex;
    struct Face;
    struct Edge;
    struct Ref {
        Edge* edge = nullptr;
        int r = 0;  // 0 the section, 1 its left room, 2 the section reversed, 3 its right room
        bool operator==(const Ref&) const = default;
    };
    static Ref rot(Ref e) { return {e.edge, (e.r + 1) & 3}; }
    static Ref sym(Ref e) { return {e.edge, (e.r + 2) & 3}; }
    static Ref invRot(Ref e) { return {e.edge, (e.r + 3) & 3}; }
    static Ref& onext(Ref e);
    static Ref oprev(Ref e) { return rot(onext(rot(e))); }
    static Ref faceNext(Ref e) { return invRot(onext(rot(e))); }
    static Vertex* org(Ref e);
    static Vertex* dest(Ref e) { return org(sym(e)); }
    static Face*& face(Ref e);
    static void splice(Ref a, Ref b);
    static float angle(Ref e);
    static WallSegment segmentOf(Ref e);

    Vertex* findVertex(float x, float y, std::int32_t level) const;
    Vertex* findOrMake(float x, float y, std::int32_t level);
    std::optional<Ref> find(const WallSegment& segment) const;
    void link(Vertex* v, Ref e);
    void unlink(Vertex* v, Ref e);
    bool inOrbit(Ref start, Ref e) const;
    bool evenCrossings(Ref e) const;
    Face* faceById(std::int32_t id) const;
    Face* newFace();
    void addRooms(Ref e, RoomGrid* grid, const std::function<std::int32_t(const WallSegment&)>& roomOf);
    void joinRooms(Ref e, RoomGrid* grid, const std::function<std::int32_t(const WallSegment&)>& roomOf);
    void reassign(Face* f, const std::function<std::int32_t(const WallSegment&)>& roomOf);

    wallgraph::WallGraph header_;  // what the saved graph keeps besides its lists
    // In the saved order, which the game keeps sorted by level, then x, then y, and the rooms by id.
    std::vector<std::unique_ptr<Vertex>> vertices_;
    std::vector<std::unique_ptr<Edge>> edges_;       // in the order added
    std::vector<std::unique_ptr<Face>> faces_;
};

// The lot's walls as the game's wall manager keeps them: twelve wall graphs, three room grids, the
// wall coverings and the wall patterns.
class WallManager {
public:
    // Graphs and grids by instance. The coverings are wall layer 4, by the ids of wall graph 24, and the
    // patterns are pattern map 0xD.
    WallManager(std::map<std::uint32_t, wallgraph::WallGraph>& graphs, std::map<std::uint32_t, array3d::Array3D>& grids,
                std::vector<walllayer::Entry>& coverings, std::map<std::uint16_t, stringmap::Entry>& patterns,
                const WallBounds& bounds, const BuildScripts& scripts);
    ~WallManager();

    // `style` is a wall style or a half wall's GUID, and the patterns are names, empty for none. False
    // with no lot tile on either side, or for fences, arches, stage sides and invisible light blockers,
    // which are not done yet.
    bool add(std::uint32_t style, const std::string& left, const std::string& right, const WallSegment& segment);
    // Either way round.
    bool has(const WallSegment& segment) const;
    // The walls with an end at (x, y), each from that point, in the order the game walks round it.
    struct WallAt {
        WallSegment segment;
        std::uint32_t style = 0;
    };
    std::vector<WallAt> wallsAt(float x, float y, std::int32_t level) const;
    std::optional<std::uint32_t> styleOn(const WallSegment& segment) const;
    // Fence arches, which stand in a layer of their own: whether one stands on the segment, either way
    // round, and those with an end at (x, y).
    bool archOn(const WallSegment& segment) const;
    // Whether a wall that divides rooms (wall graph 5) stands on the segment, either way round.
    bool dividesOn(const WallSegment& segment) const;
    std::vector<WallSegment> archesAt(float x, float y, std::int32_t level) const;
    // The section the way round it was added, its style and its pattern names, enough to undo taking it out.
    struct Kept {
        WallSegment segment;
        std::uint32_t style = 0;
        std::string left, right;
    };
    std::optional<Kept> keptOn(const WallSegment& segment) const;
    // The pattern on the section's side that leftQuarter(segment) names, empty for none. None with no
    // section there. Setting it returns false with no section there.
    std::optional<std::string> sidePattern(const WallSegment& segment) const;
    bool setSidePattern(const WallSegment& segment, const std::string& name);
    // Either way round. False when there is none.
    bool remove(const WallSegment& segment);
    // Writes the layers back to the saved graphs.
    void save();
    // postSpacing (posttilespacing) puts a post at every so many corners along a run, 0 for none between
    // its ends. A fence with no post model has 0.
    struct Fence {
        bool halfWall = false;
        int postSpacing = 0;
        float height = 0;  // the tallest fence at a corner gives its post
        bool bearsLoad = false;  // XFNC bearsload, which also keeps the ground under it level
    };
    // Nullopt for a style that is not a fence or half wall.
    void setFences(std::function<std::optional<Fence>(std::uint32_t style)> fenceOf) { fenceOf_ = std::move(fenceOf); }
    bool isHalfWall(std::uint32_t style) const;
    std::optional<Fence> fence(std::uint32_t style) const { return fenceOf_ ? fenceOf_(style) : std::nullopt; }
    // Kept sorted by level, then x, then y, as the game keeps them.
    void setPosts(std::vector<fencepostlayer::Post>* posts) { posts_ = posts; }
    // Whether a corner needs a post: one fence or half wall section ends there, three or more meet, two
    // meet at an angle, the fences there with a post spacing are of more than one kind, an arch stands
    // there, or a post stands there already and a fence there has a post spacing.
    bool needsPost(float x, float y, std::int32_t level) const;
    // The posts a change put up, and those it changed or took down as they were before.
    struct PostChanges {
        std::vector<fencepostlayer::Post> added, replaced;
    };
    // Posts for a run of `count` fence sections from (ax, ay) to (bx, by): at its ends, at corners that
    // need one and at every post spacing'th corner. Only corners `built` accepts get one, and a post
    // already there is refreshed.
    PostChanges addRunPosts(std::uint32_t style, float ax, float ay, float bx, float by, std::int32_t level, int count,
                            const std::function<bool(float x, float y)>& built);
    // A fence corner's post changes to postStyleAt when that is another fence's. Returns the post as it
    // was, when it changed. Arches and stages at the corner are not done yet.
    std::optional<fencepostlayer::Post> refreshPost(float x, float y, std::int32_t level);
    // False when there was nothing to change.
    bool addPost(const fencepostlayer::Post& post);
    bool removePost(float x, float y, std::int32_t level);
    std::optional<fencepostlayer::Post> postAt(float x, float y, std::int32_t level) const;
    // The first of the tallest fences round the corner. None with no fence, or with an arch, whose post
    // is not known yet.
    std::optional<std::uint32_t> postStyleAt(float x, float y, std::int32_t level) const;

private:
    struct Layers;
    std::uint32_t styleOf(const WallSegment& segment) const;
    std::int32_t roomByPoint(const WallSegment& segment, int kind) const;
    bool cornerOnStage(float x, float y, std::int32_t level) const;
    std::size_t halfWallLayer(const WallSegment& segment) const;
    void addPattern(const std::string& name);
    std::uint16_t patternId(const std::string& name) const;
    void releasePattern(std::uint16_t id);

    std::map<std::uint32_t, wallgraph::WallGraph>& graphs_;
    std::vector<walllayer::Entry>& coverings_;
    std::map<std::uint16_t, stringmap::Entry>& patterns_;
    WallBounds bounds_;
    const BuildScripts& scripts_;
    std::unique_ptr<Layers> layers_;
    const array3d::Array3D* regions_ = nullptr;  // the platform region grid (instance 0x14), null without one
    std::int32_t firstLevel_ = 0;
    std::function<std::optional<Fence>(std::uint32_t)> fenceOf_;
    std::vector<fencepostlayer::Post>* posts_ = nullptr;
};

} // namespace ts2::content
