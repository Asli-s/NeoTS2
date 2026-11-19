#include "sim/relationship_table.hpp"

#include <algorithm>
#include <array>

namespace ts2::sim {

namespace {

// Chains in bucket order, each holding its ids front first.
using Buckets = std::array<std::vector<std::uint32_t>, kRelationshipTableBuckets>;

void pushFront(Buckets& buckets, std::uint32_t id) {
    auto& chain = buckets[relationshipBucket(id)];
    chain.insert(chain.begin(), id);
}

std::vector<std::uint32_t> listed(const Buckets& buckets) {
    std::vector<std::uint32_t> out;
    for (const auto& chain : buckets) {
        out.insert(out.end(), chain.begin(), chain.end());
    }
    return out;
}

bool holds(std::span<const std::uint32_t> ids, std::uint32_t id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

} // namespace

std::size_t relationshipBucket(std::uint32_t id) {
    std::uint32_t h = id * 0x1001u;
    h = ((h >> 22) ^ h) * 0x11u;
    h = ((h >> 9) ^ h) * 0x401u;
    h = ((h >> 2) ^ h) * 0x81u;
    h = (h >> 12) ^ h;
    return h % kRelationshipTableBuckets;
}

std::vector<std::uint32_t> loadedRelationshipTable(std::span<const std::uint32_t> saved) {
    if (saved.size() > kRelationshipTableBuckets) {
        return {saved.begin(), saved.end()};
    }
    Buckets buckets;
    for (const std::uint32_t id : saved) {
        pushFront(buckets, id);
    }
    return listed(buckets);
}

void addToRelationshipTable(std::vector<std::uint32_t>& table, std::uint32_t id) {
    if (table.size() >= kRelationshipTableBuckets) {
        table.push_back(id);
        return;
    }
    const std::size_t bucket = relationshipBucket(id);
    const auto at = std::find_if(table.begin(), table.end(), [bucket](std::uint32_t other) { return relationshipBucket(other) >= bucket; });
    table.insert(at, id);
}

std::vector<std::uint32_t> savedRelationshipList(std::span<const std::uint32_t> table,
                                                 std::span<const std::uint32_t> kept,
                                                 std::span<const std::uint32_t> added) {
    std::vector<std::uint32_t> left;
    for (const std::uint32_t id : table) {
        if (holds(kept, id)) {
            left.push_back(id);
        }
    }
    std::vector<std::uint32_t> fresh;
    for (const std::uint32_t id : added) {
        if (!holds(left, id) && !holds(fresh, id)) {
            fresh.push_back(id);
        }
    }
    if (left.size() + fresh.size() > kRelationshipTableBuckets) {
        left.insert(left.end(), fresh.begin(), fresh.end());
        return left;
    }
    Buckets buckets;
    for (const std::uint32_t id : left) {
        buckets[relationshipBucket(id)].push_back(id);
    }
    for (const std::uint32_t id : fresh) {
        pushFront(buckets, id);
    }
    return listed(buckets);
}

} // namespace ts2::sim
