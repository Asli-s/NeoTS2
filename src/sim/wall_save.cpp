#include "sim/wall_save.hpp"

#include "formats/array3d.hpp"
#include "formats/resource_types.hpp"
#include "formats/stringmap.hpp"
#include "formats/walllayer.hpp"
#include "sim/wall_build.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace ts2::sim {

namespace {

constexpr std::uint32_t kRoomGraph = 5, kWallGraph = 24, kFullWallGraph = 22;
// The room layers' graphs and their room grids.
constexpr std::pair<std::uint32_t, std::uint32_t> kRoomLayers[] = {{6, 0xA}, {7, 0xB}};

// A wall's place: its level and its ends, the lesser first.
struct Place {
    std::int32_t level = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    auto operator<=>(const Place&) const = default;
};

std::optional<Place> placeOf(const wallgraph::Wall& w, const wallgraph::PointIndex& points) {
    const auto* a = points(w.from);
    const auto* b = points(w.to);
    if (a == nullptr || b == nullptr) {
        return std::nullopt;
    }
    Place p{a->level, static_cast<std::int32_t>(std::lround(a->x)), static_cast<std::int32_t>(std::lround(a->y)),
            static_cast<std::int32_t>(std::lround(b->x)), static_cast<std::int32_t>(std::lround(b->y))};
    if (std::pair(p.x1, p.y1) < std::pair(p.x0, p.y0)) {
        std::swap(p.x0, p.x1);
        std::swap(p.y0, p.y1);
    }
    return p;
}

// The walls (segments with a wall layer entry) of a graph by place, with their entries.
std::map<Place, walllayer::Entry> wallsByPlace(const wallgraph::WallGraph& graph, const std::vector<walllayer::Entry>& coverings) {
    std::map<std::int32_t, const walllayer::Entry*> byId;
    for (const auto& c : coverings) {
        byId.emplace(static_cast<std::int32_t>(c.wall), &c);
    }
    const wallgraph::PointIndex points(graph);
    std::map<Place, walllayer::Entry> out;
    for (const auto& w : graph.walls) {
        const auto entry = byId.find(w.id);
        if (const auto place = placeOf(w, points); place && entry != byId.end()) {
            out.emplace(*place, *entry->second);
        }
    }
    return out;
}

// Takes the graph's segments at `places` away, and the points no segment uses then. Returns their ids.
std::set<std::int32_t> removeAt(wallgraph::WallGraph& graph, const std::set<Place>& places) {
    std::set<std::int32_t> removed;
    {
        const wallgraph::PointIndex points(graph);
        for (const auto& w : graph.walls) {
            if (const auto place = placeOf(w, points); place && places.contains(*place)) {
                removed.insert(w.id);
            }
        }
    }
    if (removed.empty()) {
        return removed;
    }
    std::erase_if(graph.walls, [&](const wallgraph::Wall& w) { return removed.contains(w.id); });
    std::set<std::int32_t> used;
    for (const auto& w : graph.walls) {
        used.insert(w.from);
        used.insert(w.to);
    }
    std::erase_if(graph.points, [&](const wallgraph::Point& p) { return !used.contains(p.id); });
    return removed;
}

// Puts a segment at the place unless one is there, its sides in `room`. Returns its id, new or found.
// With `like` (the room graph), a new one runs the way that graph's segment there runs, so that its
// left and right are the same sides, and takes that segment's id and its points' ids where this graph
// doesn't use them, as the game keeps the room and wall graphs' ids alike.
std::int32_t addAt(wallgraph::WallGraph& graph, const Place& place, std::int32_t room, const wallgraph::WallGraph* like = nullptr) {
    {
        const wallgraph::PointIndex points(graph);
        for (const auto& w : graph.walls) {
            if (placeOf(w, points) == place) {
                return w.id;
            }
        }
    }
    const wallgraph::Wall* likeWall = nullptr;
    const wallgraph::Point *likeFrom = nullptr, *likeTo = nullptr;
    if (like != nullptr) {
        const wallgraph::PointIndex points(*like);
        for (const auto& w : like->walls) {
            if (placeOf(w, points) == place) {
                likeWall = &w;
                likeFrom = points(w.from);
                likeTo = points(w.to);
                break;
            }
        }
    }
    const auto take = [&](std::int32_t id, std::size_t counter) {
        graph.counters[counter] = std::max(graph.counters[counter], id + 1);
        return id;
    };
    // An end: the graph's point there, or a new one, with the other graph's id where free.
    const auto end = [&](std::int32_t x, std::int32_t y, const wallgraph::Point* likePoint) {
        for (const auto& p : graph.points) {
            if (p.level == place.level && std::lround(p.x) == x && std::lround(p.y) == y) {
                return p.id;
            }
        }
        if (likePoint != nullptr && std::none_of(graph.points.begin(), graph.points.end(), [&](const auto& p) { return p.id == likePoint->id; })) {
            graph.points.push_back({take(likePoint->id, 0), static_cast<float>(x), static_cast<float>(y), place.level});
            return likePoint->id;
        }
        return graphPointAt(graph, place.level, x, y);
    };
    const bool unused = likeWall != nullptr && std::none_of(graph.walls.begin(), graph.walls.end(), [&](const auto& w) { return w.id == likeWall->id; });
    const std::int32_t id = unused ? take(likeWall->id, 1) : nextGraphId(graph, 1);
    std::int32_t from = 0, to = 0;
    if (likeFrom != nullptr && likeTo != nullptr) {
        from = end(static_cast<std::int32_t>(std::lround(likeFrom->x)), static_cast<std::int32_t>(std::lround(likeFrom->y)), likeFrom);
        to = end(static_cast<std::int32_t>(std::lround(likeTo->x)), static_cast<std::int32_t>(std::lround(likeTo->y)), likeTo);
    } else {
        from = end(place.x0, place.y0, nullptr);
        to = end(place.x1, place.y1, nullptr);
    }
    graph.walls.push_back({id, from, room, to, room});
    return id;
}

struct Saved {
    dbpf::ResourceKey key;
    std::vector<std::uint8_t> bytes;
};

} // namespace

WallChanges wallChanges(dbpf::Package& original, const content::Lot& edited) {
    WallChanges changes;
    std::map<std::pair<std::uint32_t, std::uint32_t>, Saved> saved;
    for (const auto& e : original.entries()) {
        const bool wanted = e.key.type == types::kWallGraph || (e.key.type == types::kWallLayer && e.key.instance == content::kWallCoveringLayerInstance) ||
                            (e.key.type == types::kArray3D && (e.key.instance == content::kRoomGridInstance || e.key.instance == 0xA || e.key.instance == 0xB)) ||
                            (e.key.type == types::kPatternMap && e.key.instance == content::kWallPatternMapInstance);
        if (wanted && !saved.contains({e.key.type, e.key.instance})) {
            saved[{e.key.type, e.key.instance}] = {e.key, original.read(e)};
        }
    }
    auto find = [&](std::uint32_t type, std::uint32_t instance) -> const Saved* {
        const auto it = saved.find({type, instance});
        return it != saved.end() ? &it->second : nullptr;
    };
    auto put = [&](const Saved& s, std::vector<std::uint8_t> bytes) {
        if (bytes != s.bytes) {
            changes.resources.push_back({s.key, std::move(bytes)});
        }
    };
    const auto name = [](const Saved& s) { return std::span(s.bytes).first(64); };

    try {
        const auto* rooms = find(types::kWallGraph, kRoomGraph);
        const auto* walls = find(types::kWallGraph, kWallGraph);
        const auto* layer = find(types::kWallLayer, content::kWallCoveringLayerInstance);
        const auto* patterns = find(types::kPatternMap, content::kWallPatternMapInstance);
        if (rooms == nullptr || walls == nullptr || layer == nullptr || patterns == nullptr || !edited.roomGraph) {
            throw std::runtime_error("the lot has no room graph, wall graph, wall layer or wall pattern map");
        }
        const auto savedRooms = wallgraph::parse(rooms->bytes);
        auto wallGraph = wallgraph::parse(walls->bytes);
        auto entries = walllayer::parse(layer->bytes);

        // What changed, by place: the walls as loaded against the walls now.
        const auto before = wallsByPlace(savedRooms, content::coveringsOfRoomWalls(savedRooms, wallGraph, entries));
        const auto after = wallsByPlace(*edited.roomGraph, edited.wallCoverings);
        std::set<Place> removed, built;
        std::set<std::int32_t> levels;
        for (const auto& [place, entry] : before) {
            if (!after.contains(place)) {
                if (entry.type != kPlainWallStyle) {
                    throw std::runtime_error("a wall other than a plain wall was taken down");
                }
                removed.insert(place);
                levels.insert(place.level);
            }
        }
        for (const auto& [place, entry] : after) {
            if (!before.contains(place)) {
                if (entry.type != kPlainWallStyle) {
                    throw std::runtime_error("a wall other than a plain wall was built");
                }
                built.insert(place);
                levels.insert(place.level);
            }
        }
        changes.built = built.size();
        changes.removed = removed.size();

        // The wall graph and its layer: walls taken away and made, then every wall whose place's
        // coverings changed given the new ones.
        const auto gone = removeAt(wallGraph, removed);
        std::erase_if(entries, [&](const walllayer::Entry& e) { return gone.contains(static_cast<std::int32_t>(e.wall)); });
        for (const auto& place : built) {
            const auto& entry = after.at(place);
            const std::int32_t id = addAt(wallGraph, place, 0, &*edited.roomGraph);
            entries.push_back({static_cast<std::uint32_t>(id), entry.type, entry.left, entry.right});
        }
        {
            const wallgraph::PointIndex points(wallGraph);
            std::map<std::int32_t, Place> placeOfWall;
            for (const auto& w : wallGraph.walls) {
                if (const auto place = placeOf(w, points)) {
                    placeOfWall.emplace(w.id, *place);
                }
            }
            for (auto& e : entries) {
                const auto place = placeOfWall.find(static_cast<std::int32_t>(e.wall));
                if (place == placeOfWall.end() || built.contains(place->second)) {
                    continue;
                }
                const auto was = before.find(place->second);
                const auto now = after.find(place->second);
                if (was != before.end() && now != after.end() && (was->second.left != now->second.left || was->second.right != now->second.right)) {
                    e.left = now->second.left;
                    e.right = now->second.right;
                }
            }
        }
        put(*walls, wallgraph::write(wallGraph, name(*walls)));
        put(*layer, walllayer::write(layer->bytes, entries));
        put(*patterns, stringmap::write(patterns->bytes, edited.wallPatterns));

        if (!built.empty() || !removed.empty()) {
            // The room graph and its rooms as Build mode left them.
            put(*rooms, wallgraph::write(*edited.roomGraph, name(*rooms)));
            const auto* roomGrid = find(types::kArray3D, content::kRoomGridInstance);
            const auto grid = edited.grids.find(content::kRoomGridInstance);
            if (roomGrid != nullptr && grid != edited.grids.end()) {
                put(*roomGrid, array3d::write(roomGrid->bytes, grid->second.elements));
            }
            // The full walls' graph, without rooms.
            if (const auto* full = find(types::kWallGraph, kFullWallGraph)) {
                auto graph = wallgraph::parse(full->bytes);
                removeAt(graph, removed);
                for (const auto& place : built) {
                    addAt(graph, place, 0);
                }
                put(*full, wallgraph::write(graph, name(*full)));
            }
            // The room layers, their rooms numbered again on the levels that changed.
            for (const auto& [graphInstance, gridInstance] : kRoomLayers) {
                const auto* layerGraph = find(types::kWallGraph, graphInstance);
                if (layerGraph == nullptr) {
                    continue;
                }
                auto graph = wallgraph::parse(layerGraph->bytes);
                removeAt(graph, removed);
                for (const auto& place : built) {
                    addAt(graph, place, kUnnumberedRoom);
                }
                for (const auto level : levels) {
                    renumberRooms(graph, level);
                }
                put(*layerGraph, wallgraph::write(graph, name(*layerGraph)));
                if (const auto* layerGrid = find(types::kArray3D, gridInstance)) {
                    auto roomsByQuarter = array3d::parse(layerGrid->bytes);
                    for (const auto level : levels) {
                        updateRoomGrid(roomsByQuarter, graph, level);
                    }
                    put(*layerGrid, array3d::write(layerGrid->bytes, roomsByQuarter.elements));
                }
            }
        }
    } catch (const std::exception& ex) {
        changes.resources.clear();
        changes.problems.push_back(std::format("walls: {}", ex.what()));
    }
    return changes;
}

} // namespace ts2::sim
