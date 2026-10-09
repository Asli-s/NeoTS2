#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ts2::bcon {

struct Constants {
    std::string name;
    std::uint8_t flags = 0;
    std::vector<std::int16_t> values;
};

Constants parse(std::span<const std::uint8_t> data);

} // namespace ts2::bcon
