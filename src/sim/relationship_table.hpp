#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ts2::sim {

// The order of a neighbor's relationship list (the neighbors it has a relationship record towards)
// as the game keeps it: a hash table by neighbor id with 193 buckets, where each id goes to the
// front of its bucket's chain, and a save lists the table bucket by bucket. Loading a saved list
// therefore swaps ids that share a bucket.

// How many ids the table holds before it grows. The game's order after growing is not known yet,
// so a longer list keeps its order and new ids go at its end.
constexpr std::size_t kRelationshipTableBuckets = 193;

// The bucket the game puts neighbor `id` in.
std::size_t relationshipBucket(std::uint32_t id);

// The table's order after loading `saved`, a description's list as saved.
std::vector<std::uint32_t> loadedRelationshipTable(std::span<const std::uint32_t> saved);

// The list a description saves: `table` (an order the table held) without the ids not in `kept`,
// then `added`, in the order they were added, each to the front of its bucket.
std::vector<std::uint32_t> savedRelationshipList(std::span<const std::uint32_t> table,
                                                 std::span<const std::uint32_t> kept,
                                                 std::span<const std::uint32_t> added);

} // namespace ts2::sim
