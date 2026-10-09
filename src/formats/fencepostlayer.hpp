#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ts2::fencepostlayer {

// A fence post: the level (as the lot's height grids count them), its point on the wall graph's
// grid, and the GUID of its fence in the build catalog.
struct Post {
    std::int32_t level = 0;
    float x = 0;
    float y = 0;
    std::uint32_t fence = 0;
};

// cFencePostLayer (resource type 0xAB4BA572).
std::vector<Post> parse(std::span<const std::uint8_t> data);

} // namespace ts2::fencepostlayer
