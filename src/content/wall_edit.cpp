#include "content/wall_edit.hpp"

#include "content/floor_edit.hpp"

#include <algorithm>

namespace ts2::content {

namespace {

void countUse(std::map<std::uint16_t, stringmap::Entry>& patterns, std::uint16_t id, int delta) {
    const auto it = patterns.find(id);
    if (id == 0 || it == patterns.end()) {
        return;
    }
    auto& uses = it->second.uses;
    uses = delta < 0 ? (uses > 0 ? uses - 1 : 0) : uses + 1;
}

} // namespace

std::optional<std::string> coverWallSide(std::vector<walllayer::Entry>& coverings, std::map<std::uint16_t, stringmap::Entry>& patterns,
                                         WallSide side, const std::string& name) {
    const auto entry = std::find_if(coverings.begin(), coverings.end(),
                                    [&](const walllayer::Entry& e) { return static_cast<std::int32_t>(e.wall) == side.wall; });
    if (entry == coverings.end()) {
        return std::nullopt;
    }
    const std::uint16_t id = patternId(patterns, name);
    std::uint16_t& shown = side.left ? entry->left : entry->right;
    if (id == 0 || shown == id) {
        return std::nullopt;
    }
    const auto old = patterns.find(shown);
    std::string before = old != patterns.end() ? old->second.name : std::string();
    countUse(patterns, shown, -1);
    shown = id;
    countUse(patterns, id, +1);
    return before;
}

std::vector<WallSide> roomWallSides(const wallgraph::WallGraph& graph, const std::vector<walllayer::Entry>& coverings, WallSide side,
                                    const std::function<bool(std::int32_t wall)>& coverable) {
    std::vector<WallSide> out;
    const auto start = std::find_if(graph.walls.begin(), graph.walls.end(), [&](const wallgraph::Wall& w) { return w.id == side.wall; });
    if (start == graph.walls.end()) {
        return out;
    }
    const wallgraph::PointIndex points(graph);
    const auto levelOf = [&](const wallgraph::Wall& w) -> std::optional<std::int32_t> {
        const auto* p = points(w.from);
        return p != nullptr ? std::optional(p->level) : std::nullopt;
    };
    const std::int32_t room = side.left ? start->leftRoom : start->rightRoom;
    const auto level = levelOf(*start);
    const auto covered = [&](std::int32_t wall) {
        return std::any_of(coverings.begin(), coverings.end(), [&](const walllayer::Entry& e) { return static_cast<std::int32_t>(e.wall) == wall; });
    };
    for (const auto& w : graph.walls) {
        if (levelOf(w) != level || !covered(w.id) || (coverable && !coverable(w.id))) {
            continue;
        }
        if (w.leftRoom == room) {
            out.push_back({w.id, true});
        }
        if (w.rightRoom == room) {
            out.push_back({w.id, false});
        }
    }
    return out;
}

} // namespace ts2::content
