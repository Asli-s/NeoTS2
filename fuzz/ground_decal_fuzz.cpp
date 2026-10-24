#include "scene/ground_decal.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// Lays a decal of any centre, width and turn over a small terrain of any heights and tile size.
// Every vertex and texture coordinate must be finite: anything else is a bug.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 20) {
        return 0;
    }
    float values[5];
    std::memcpy(values, data, sizeof values);
    data += sizeof values;
    size -= sizeof values;
    ts2::scene::PaintedTerrain t;
    t.columns = 2 + static_cast<std::uint32_t>(size % 7);
    t.rows = 2 + static_cast<std::uint32_t>(size / 7 % 7);
    t.tileSize = values[4];
    for (std::size_t i = 0; i < static_cast<std::size_t>(t.columns) * t.rows; ++i) {
        t.heights.push_back(i < size ? static_cast<float>(data[i]) : 0.0f);
    }
    const auto p = ts2::scene::groundDecal(t, {values[0], values[1]}, values[2], values[3], 0);
    if (p.positions.size() != p.indices.size() * 3 || p.uvs.size() != p.indices.size() * 2) {
        std::abort();
    }
    for (float v : p.positions) {
        if (!std::isfinite(v)) {
            std::abort();
        }
    }
    for (float v : p.uvs) {
        if (!std::isfinite(v)) {
            std::abort();
        }
    }
    return 0;
}
