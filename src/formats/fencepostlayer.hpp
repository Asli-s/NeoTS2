#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ts2::fencepostlayer {

// A fence post: the level (as the wall graph counts them), its point on the wall graph's
// grid, and the GUID of its fence in the build catalog.
struct Post {
    std::int32_t level = 0;
    float x = 0;
    float y = 0;
    std::uint32_t fence = 0;
};

// cFencePostLayer (resource type 0xAB4BA572).
std::vector<Post> parse(std::span<const std::uint8_t> data);

constexpr std::uint32_t kType = 0xAB4BA572;

// The layer as the game writes it (version 1). Throws std::runtime_error for a list too long to write.
std::vector<std::uint8_t> write(const std::vector<Post>& posts);

} // namespace ts2::fencepostlayer
