#pragma once

#include "formats/scenegraph.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace ts2::reflist {

constexpr std::uint32_t kType = 0xAC506764;

// A reference list (3IDR): the resources a property set or a Sim's age data points to, by index
// (a property set's "resourcekeyidx" picks one). Version 1 entries carry no high instance.
std::vector<scenegraph::Link> parse(std::span<const std::uint8_t> data);

} // namespace ts2::reflist
