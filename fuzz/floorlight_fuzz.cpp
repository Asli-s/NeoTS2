#include "scene/floor_light.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Floor light with grids and sources made from the input: any links, rooms and flags, and raw
// floats for the sources (NaN, infinities and huge values included).
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace ts2::scene::floorlight;
    std::size_t at = 0;
    auto byte = [&]() -> std::uint8_t { return at < size ? data[at++] : 0; };
    auto number = [&]() {
        float f = 0;
        std::uint8_t raw[4] = {byte(), byte(), byte(), byte()};
        std::memcpy(&f, raw, 4);
        return f;
    };

    Grid grid;
    grid.width = 1 + byte() % 32;
    grid.height = 1 + byte() % 32;
    grid.level = static_cast<std::int8_t>(byte());
    grid.steps = byte() * 4;
    grid.falloffScale = number();
    grid.insideAmbient = {number(), number(), number()};
    grid.outsideAmbient = {number(), number(), number()};
    // Sometimes one cell short, which must be refused.
    const std::size_t count = static_cast<std::size_t>(grid.columns() * grid.rows()) - (byte() == 7 ? 1 : 0);
    grid.cells.resize(count);
    grid.tiles.resize(static_cast<std::size_t>(grid.width * grid.height));
    for (auto& tile : grid.tiles) {
        const std::uint8_t b = byte();
        tile.floor = b & 1;
        tile.diagonal = static_cast<std::uint8_t>(b >> 1);
    }
    grid.dimOutside = byte() & 1;
    for (auto& cell : grid.cells) {
        for (auto& half : cell.halves) {
            const std::uint8_t flags = byte();
            half.valid = flags & 1;
            half.outside = flags & 2;
            half.room = static_cast<std::int16_t>(flags >> 5);
            half.links = byte();
            half.toSecond = byte();
        }
    }
    std::vector<ts2::scene::LightSource> sources(byte() % 16);
    for (auto& s : sources) {
        s.color = {number(), number(), number()};
        std::uint32_t level = 0;
        for (int i = 0; i < 4; ++i) {
            level = (level << 8) | byte();
        }
        s.level = static_cast<std::int32_t>(level);
        s.falloff = number();
        s.floorIntensity = number();
        s.interFloorFalloff = number();
        s.x0 = number();
        s.y0 = number();
        s.x1 = number();
        s.y1 = number();
        s.room = static_cast<std::int16_t>(byte() % 8);
    }
    const std::uint8_t stopMask = byte();
    light(grid, sources, [stopMask](int x, int y, int level) { return ((x + y + level) & 7) == (stopMask & 7); });
    for (int i = 0; i < 8; ++i) {
        sample(grid, number(), number());
    }
    return 0;
}
