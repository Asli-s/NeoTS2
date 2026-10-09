#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace ts2::glob {

struct SemiGlobalReference {
    std::string name;
    std::string semiGlobalName;
};

SemiGlobalReference parse(std::span<const std::uint8_t> data);

} // namespace ts2::glob
