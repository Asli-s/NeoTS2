#pragma once

#include "content/lot.hpp"
#include "content/package_rewrite.hpp"
#include "formats/dbpf.hpp"

#include <string>
#include <vector>

namespace ts2::sim {

// Saving a lot's walls (second stage of saving). The game keeps each wall in several wall graphs,
// each with its own ids: the room graph (5, every wall, fence and room divider, with the rooms on its
// sides), the wall graph (24, every wall and fence, which the wall layer's ids name), the full walls
// (22, styles 1 and 0x17), and the two room layers with their own rooms (6: full walls; 7: those and
// style 3), whose rooms by quarter tile are grids 0xA and 0xB as the room graph's are grid 9.
struct WallChanges {
    std::vector<content::ReplacedResource> resources;  // only those whose bytes changed
    std::vector<std::string> problems;                 // why the walls couldn't be saved; nothing then
    std::size_t built = 0, removed = 0;
};

// The resources of the lot package `original` (the lot as saved) that change with `edited`'s walls
// and coverings: its room graph, wall coverings, wall patterns and room grid as Build mode left them.
// Walls built or taken down (Build mode's wall tool makes and removes only full walls, style 1) are
// found by their place against the walls as saved, and made or removed in each other graph alike;
// the room layers' rooms are numbered again on the levels that changed. The coverings of walls kept
// are written where they changed.
WallChanges wallChanges(dbpf::Package& original, const content::Lot& edited);

} // namespace ts2::sim
