#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ts2::qfs {

bool isCompressed(std::span<const std::uint8_t> data);

std::vector<std::uint8_t> decompress(std::span<const std::uint8_t> data);

} // namespace ts2::qfs
