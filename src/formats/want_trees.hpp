#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ts2::want_trees {

// A want tree (type 0x6D814AFE in WantTrees.package): the nodes that decide which wants a Sim
// is offered and how much each one scores. A node's want becomes a candidate when its parent's
// want is in the Sim's history (a root needs no parent).

// One of a node's seven scoring factors ("ads"), each weighted into the score.
struct Ad {
    std::int32_t weight = 0;
    // A factor of 0 or less makes the whole score 0.
    bool superZero = false;
    // The ticked choices of an age, aspiration or personality factor, by their names in the file.
    std::vector<std::string> ticked;
    // The range of a mood, relationship, aspiration score or person data factor. The score rises
    // from min to max (falls when min is above max). For person data, index names the field.
    std::int32_t min = 0, max = 0, index = 0;
};

enum class AdKind : std::uint8_t { Age, Aspiration, Mood, Personality, Relationship, AspirationScore, PersonData };
inline constexpr std::size_t kAdCount = 7;

struct Node {
    std::uint32_t wantId = 0;  // the want definition's GUID
    std::uint32_t id = 0;      // the node's own number, unique across the trees
    bool visible = false;      // only visible nodes are offered, the rest only lead to their children
    bool regeneration = false; // offered again after its want was had
    std::int32_t lifetime = 0; // in hours. The file's "frequency" is never read by the game.
    std::uint32_t objectField = 0, integerField = 0;
    bool inheritSubject = false;
    std::int32_t minScore = 0, maxScore = 0;
    // Set on a few nodes. When set, it replaces the want definition's score for this node's want.
    std::optional<std::int32_t> scoreOverride;
    Ad ads[kAdCount];
    std::int32_t parent = -1;            // index in Tree::nodes, -1 for the root
    std::vector<std::int32_t> children;  // indexes in Tree::nodes
};

struct Tree {
    std::uint32_t id = 0;
    std::string folder;
    std::vector<Node> nodes;  // the root first
};

// Trees are cut at 64 levels and 4096 nodes, well past any shipped one (26 levels, 578 nodes), so a
// broken file can't nest without end or ask for huge memory.
inline constexpr std::size_t kMaxDepth = 64, kMaxNodes = 4096;

// The tree in a resource's text. Missing values stay at their defaults. Empty for a resource with
// no root node.
Tree parse(std::string_view text);

} // namespace ts2::want_trees
