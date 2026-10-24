#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ts2::luascript {

// A compiled Lua 5.0 script from ObjectScripts.package: a name and the chunk Lua loads.
// Type 0x9012468A holds shared modules, type 0x9012468B the scripts objects run.
struct Script {
    std::uint32_t value = 0;  // meaning unknown, normally 0
    std::string name;
    std::vector<std::uint8_t> chunk;
};

Script parse(std::span<const std::uint8_t> data);

// The chunk rewritten for this build of Lua: the game's chunks were compiled on 32 bits (4-byte
// string lengths). Everything else is copied unchanged.
std::vector<std::uint8_t> nativeChunk(std::span<const std::uint8_t> chunk);

} // namespace ts2::luascript
