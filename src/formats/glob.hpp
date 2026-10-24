#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace ts2::glob {

// A semi-global reference (GLOB): the name of the semi-global group whose shared trees an object
// type's own trees can call.
struct SemiGlobalReference {
    std::string name;
    std::string semiGlobalName;
};

SemiGlobalReference parse(std::span<const std::uint8_t> data);

} // namespace ts2::glob
