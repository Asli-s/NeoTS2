#pragma once

#include "formats/stringmap.hpp"
#include "formats/wallgraph.hpp"
#include "formats/walllayer.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ts2::content {

// One side of a wall segment: its id in the room wall graph, and whether it is the side on the
// segment's left (walking from its `from` point to its `to` point, as the graph has it).
struct WallSide {
    std::int32_t wall = 0;
    bool left = true;
    bool operator==(const WallSide&) const = default;
};

// Covers one side of a wall with pattern `name` (in the lot's wall pattern list, by its catalog
// GUID in hex), in the lot's wall layer (kWallCoveringLayerInstance). Returns the name of what the
// side showed before (empty for a pattern the list doesn't name), or nothing when the wall has no
// covering entry (it only divides rooms) or already shows that pattern. The pattern list's use
// counts follow.
std::optional<std::string> coverWallSide(std::vector<walllayer::Entry>& coverings, std::map<std::uint16_t, stringmap::Entry>& patterns,
                                         WallSide side, const std::string& name);

// Every wall side facing the same room as `side`, on the same level: the room on that side of its
// wall, or for the outdoors (room 0) every side looking outdoors on that level. Only walls with a
// covering entry for which `coverable` is true, in the graph's order. Empty when `side`'s wall isn't
// in the graph.
std::vector<WallSide> roomWallSides(const wallgraph::WallGraph& graph, const std::vector<walllayer::Entry>& coverings, WallSide side,
                                    const std::function<bool(std::int32_t wall)>& coverable);

} // namespace ts2::content
