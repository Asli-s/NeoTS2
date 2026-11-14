#include "sim/wall_build.hpp"

#include "content/floor_edit.hpp"
#include "sim/rooms.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <unordered_map>

namespace ts2::sim {

namespace {

// Wall layer types from here up are fences' GUIDs; below they are wall styles.
constexpr std::uint32_t kFirstFenceGuid = 0x10000;

bool at(const wallgraph::Point& p, std::int32_t level, std::int32_t x, std::int32_t y) {
    return p.level == level && std::lround(p.x) == x && std::lround(p.y) == y;
}

// A new id for a graph's points (counter 0) or walls (counter 1): the graph's next one, as the game
// counts them, or past the largest in use when the counter is behind.
std::int32_t nextId(wallgraph::WallGraph& graph, std::size_t counter) {
    std::int32_t id = graph.counters[counter];
    if (counter == 0) {
        for (const auto& p : graph.points) {
            id = std::max(id, p.id + 1);
        }
    } else {
        for (const auto& w : graph.walls) {
            id = std::max(id, w.id + 1);
        }
    }
    graph.counters[counter] = id + 1;
    return id;
}

// The graph's point at a corner, made when there is none.
std::int32_t pointAt(wallgraph::WallGraph& graph, std::int32_t level, std::int32_t x, std::int32_t y) {
    for (const auto& p : graph.points) {
        if (at(p, level, x, y)) {
            return p.id;
        }
    }
    const std::int32_t id = nextId(graph, 0);
    graph.points.push_back({id, static_cast<float>(x), static_cast<float>(y), level});
    return id;
}

// The segment between two corners of a step, either way round, or the graph's end.
std::vector<wallgraph::Wall>::iterator segmentAt(wallgraph::WallGraph& graph, const WallStep& s) {
    const wallgraph::PointIndex points(graph);
    return std::find_if(graph.walls.begin(), graph.walls.end(), [&](const wallgraph::Wall& w) {
        const auto* a = points(w.from);
        const auto* b = points(w.to);
        return a != nullptr && b != nullptr &&
               ((at(*a, s.level, s.x0, s.y0) && at(*b, s.level, s.x1, s.y1)) || (at(*a, s.level, s.x1, s.y1) && at(*b, s.level, s.x0, s.y0)));
    });
}

void countUse(std::map<std::uint16_t, stringmap::Entry>& patterns, std::uint16_t id, int delta) {
    const auto it = patterns.find(id);
    if (id == 0 || it == patterns.end()) {
        return;
    }
    auto& uses = it->second.uses;
    const std::int64_t now = static_cast<std::int64_t>(uses) + delta;
    uses = now > 0 ? static_cast<std::uint32_t>(now) : 0;
}

} // namespace

std::vector<WallStep> wallSteps(std::int32_t level, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1) {
    std::vector<WallStep> steps;
    constexpr std::int32_t kLongest = 256;
    const bool alongX = std::abs(x1 - x0) >= std::abs(y1 - y0);
    const std::int32_t length = std::min(alongX ? std::abs(x1 - x0) : std::abs(y1 - y0), kLongest);
    const std::int32_t sx = alongX ? (x1 > x0 ? 1 : -1) : 0, sy = alongX ? 0 : (y1 > y0 ? 1 : -1);
    for (std::int32_t i = 0; i < length; ++i) {
        steps.push_back({level, x0 + sx * i, y0 + sy * i, x0 + sx * (i + 1), y0 + sy * (i + 1)});
    }
    return steps;
}

bool insideLot(const wallgraph::WallGraph& graph, const WallStep& s) {
    const auto within = [&](std::int32_t v, std::int32_t size) { return v >= 0 && v <= size; };
    if (!within(s.x0, graph.width) || !within(s.x1, graph.width) || !within(s.y0, graph.height) || !within(s.y1, graph.height)) {
        return false;
    }
    // Along the border: both corners on the same edge of the lot.
    const bool onX = s.x0 == s.x1 && (s.x0 == 0 || s.x0 == graph.width);
    const bool onY = s.y0 == s.y1 && (s.y0 == 0 || s.y0 == graph.height);
    return !onX && !onY;
}

std::vector<std::int32_t> buildWalls(wallgraph::WallGraph& graph, std::vector<walllayer::Entry>& coverings,
                                     std::map<std::uint16_t, stringmap::Entry>& patterns, const std::vector<WallStep>& steps,
                                     const std::string& pattern, const std::function<bool(const WallStep&)>& allowed) {
    std::vector<std::int32_t> built;
    const std::uint16_t id = content::patternId(patterns, pattern);
    for (const auto& s : steps) {
        if (!insideLot(graph, s) || (allowed && !allowed(s))) {
            continue;
        }
        std::int32_t wall = 0;
        if (const auto there = segmentAt(graph, s); there != graph.walls.end()) {
            wall = there->id;
            if (std::any_of(coverings.begin(), coverings.end(), [&](const walllayer::Entry& e) { return static_cast<std::int32_t>(e.wall) == wall; })) {
                continue;  // a wall or a fence already
            }
        } else {
            wall = nextId(graph, 1);
            const std::int32_t from = pointAt(graph, s.level, s.x0, s.y0);
            const std::int32_t to = pointAt(graph, s.level, s.x1, s.y1);
            graph.walls.push_back({wall, from, kUnnumberedRoom, to, kUnnumberedRoom});
        }
        coverings.push_back({static_cast<std::uint32_t>(wall), kPlainWallStyle, id, id});
        countUse(patterns, id, +2);
        built.push_back(wall);
    }
    return built;
}

std::vector<std::int32_t> removeWalls(wallgraph::WallGraph& graph, std::vector<walllayer::Entry>& coverings,
                                      std::map<std::uint16_t, stringmap::Entry>& patterns, const std::vector<WallStep>& steps,
                                      const std::function<bool(const WallStep&)>& allowed) {
    std::vector<std::int32_t> removed;
    for (const auto& s : steps) {
        if (allowed && !allowed(s)) {
            continue;
        }
        const auto there = segmentAt(graph, s);
        if (there == graph.walls.end()) {
            continue;
        }
        const std::int32_t wall = there->id;
        const auto covering = std::find_if(coverings.begin(), coverings.end(), [&](const walllayer::Entry& e) {
            return static_cast<std::int32_t>(e.wall) == wall && e.type == kPlainWallStyle;
        });
        if (covering == coverings.end()) {
            continue;  // a fence, a wall of another kind, or no wall
        }
        countUse(patterns, covering->left, -1);
        countUse(patterns, covering->right, -1);
        coverings.erase(covering);
        graph.walls.erase(there);
        removed.push_back(wall);
    }
    if (!removed.empty()) {
        std::set<std::int32_t> used;
        for (const auto& w : graph.walls) {
            used.insert(w.from);
            used.insert(w.to);
        }
        std::erase_if(graph.points, [&](const wallgraph::Point& p) { return !used.contains(p.id); });
    }
    return removed;
}

void renumberRooms(wallgraph::WallGraph& graph, std::int32_t level) {
    const wallgraph::PointIndex points(graph);
    const auto onLevel = [&](const wallgraph::Wall& w) {
        const auto* a = points(w.from);
        return a != nullptr && a->level == level;
    };
    // The rooms the level's walls named before: those none names after leave the graph's list.
    std::set<std::int32_t> namedBefore;
    for (const auto& w : graph.walls) {
        if (onLevel(w)) {
            namedBefore.insert(w.leftRoom);
            namedBefore.insert(w.rightRoom);
        }
    }
    // Each side of each segment on the level its own mark: the rooms the graph finds then are the
    // places apart, each named by one of its sides' marks.
    constexpr std::int32_t kFirstMark = 1 << 20;
    wallgraph::WallGraph marked = graph;
    std::unordered_map<std::int32_t, std::pair<std::size_t, bool>> sideOfMark;
    for (std::size_t i = 0; i < marked.walls.size(); ++i) {
        if (!onLevel(graph.walls[i])) {
            continue;
        }
        const auto left = kFirstMark + static_cast<std::int32_t>(2 * i), right = left + 1;
        marked.walls[i].leftRoom = left;
        marked.walls[i].rightRoom = right;
        sideOfMark[left] = {i, true};
        sideOfMark[right] = {i, false};
    }
    const RoomMap places(marked);
    // The place each side faces: the mark naming the place its own mark is in. Sides whose mark
    // lost the vote share their place with the side that won it.
    std::unordered_map<std::int32_t, std::int32_t> placeOfMark;
    std::map<std::int32_t, std::map<std::int32_t, int>> votes;  // place: old room: sides
    std::map<std::int32_t, int> sides;                          // place: sides naming it
    for (const auto& [mark, side] : sideOfMark) {
        const auto& w = marked.walls[side.first];
        const auto* a = points(w.from);
        const auto* b = points(w.to);
        if (a == nullptr || b == nullptr) {
            continue;
        }
        // A quarter just off the segment's middle on that side: the wall graph's left side faces (dy, -dx).
        const float mx = (a->x + b->x) / 2, my = (a->y + b->y) / 2;
        const float dx = b->x - a->x, dy = b->y - a->y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0) {
            continue;
        }
        const float out = side.second ? 1.0f : -1.0f;
        const float px = mx + out * dy / len * 0.25f, py = my - out * dx / len * 0.25f;
        const std::int32_t place = places.roomAt(level, px, py);
        placeOfMark[mark] = place;
        const std::int32_t old = side.second ? graph.walls[side.first].leftRoom : graph.walls[side.first].rightRoom;
        ++sides[place];
        if (old != kUnnumberedRoom) {
            ++votes[place][old];
        }
    }
    // The outdoors first, then the larger places, each keeping the old room most of its sides named.
    std::int32_t highest = 0;
    for (const auto& w : graph.walls) {
        highest = std::max({highest, w.leftRoom, w.rightRoom});
    }
    for (const auto r : graph.rooms) {
        highest = std::max(highest, r);
    }
    std::vector<std::int32_t> order;
    for (const auto& [place, n] : sides) {
        order.push_back(place);
    }
    const auto outdoors = [&](std::int32_t place) {
        const auto* info = places.room(place);
        return place == RoomMap::kNoRoom || (info != nullptr && info->touchesLotEdge);
    };
    std::stable_sort(order.begin(), order.end(), [&](std::int32_t a, std::int32_t b) {
        if (outdoors(a) != outdoors(b)) {
            return outdoors(a);
        }
        const auto* ia = places.room(a);
        const auto* ib = places.room(b);
        return (ia ? ia->quarterTiles : 0) > (ib ? ib->quarterTiles : 0);
    });
    std::map<std::int32_t, std::int32_t> roomOfPlace;
    std::set<std::int32_t> taken{0};
    for (const std::int32_t place : order) {
        if (outdoors(place)) {
            roomOfPlace[place] = 0;
            continue;
        }
        std::int32_t best = 0;
        int bestVotes = 0;
        for (const auto& [room, n] : votes[place]) {
            if (room != 0 && !taken.contains(room) && n > bestVotes) {
                best = room;
                bestVotes = n;
            }
        }
        if (best == 0) {
            best = ++highest;
            graph.rooms.push_back(best);
        }
        taken.insert(best);
        roomOfPlace[place] = best;
    }
    for (const auto& [mark, side] : sideOfMark) {
        const auto place = placeOfMark.find(mark);
        if (place == placeOfMark.end()) {
            continue;
        }
        auto& w = graph.walls[side.first];
        (side.second ? w.leftRoom : w.rightRoom) = roomOfPlace[place->second];
    }
    std::set<std::int32_t> named{0};
    for (const auto& w : graph.walls) {
        named.insert(w.leftRoom);
        named.insert(w.rightRoom);
    }
    std::erase_if(graph.rooms, [&](std::int32_t r) { return namedBefore.contains(r) && !named.contains(r); });
}

void updateRoomGrid(array3d::Array3D& rooms, const wallgraph::WallGraph& graph, std::int32_t level) {
    constexpr std::size_t kQuarters = 4;
    if (rooms.elementSize != kQuarters * 2) {
        return;
    }
    const RoomMap now(graph);
    const std::int32_t l = level - graph.firstLevel;
    for (std::int32_t x = 0; x < static_cast<std::int32_t>(rooms.width); ++x) {
        for (std::int32_t y = 0; y < static_cast<std::int32_t>(rooms.height); ++y) {
            if (!rooms.contains(l, x, y)) {
                continue;
            }
            const std::size_t at = ((static_cast<std::size_t>(l) * rooms.width + static_cast<std::size_t>(x)) * rooms.height + static_cast<std::size_t>(y)) *
                                   rooms.elementSize;
            for (int q = 0; q < static_cast<int>(kQuarters); ++q) {
                const std::int32_t room = now.quarterRoom(level, x, y, q);
                if (room == RoomMap::kNoRoom || at + kQuarters * 2 > rooms.elements.size()) {
                    continue;
                }
                // The grid keeps a tile's quarters the other way round (see RoomMap).
                const std::size_t slot = at + static_cast<std::size_t>(3 - q) * 2;
                rooms.elements[slot] = static_cast<std::uint8_t>(room & 0xFF);
                rooms.elements[slot + 1] = static_cast<std::uint8_t>((room >> 8) & 0xFF);
            }
        }
    }
}

} // namespace ts2::sim

namespace ts2::sim {

std::int32_t nextGraphId(wallgraph::WallGraph& graph, std::size_t counter) {
    return nextId(graph, counter);
}

std::int32_t graphPointAt(wallgraph::WallGraph& graph, std::int32_t level, std::int32_t x, std::int32_t y) {
    return pointAt(graph, level, x, y);
}

} // namespace ts2::sim
