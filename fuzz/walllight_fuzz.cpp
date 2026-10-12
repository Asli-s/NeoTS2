#include "scene/light_corners.hpp"
#include "scene/wall_light.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Wall light with walls and sources made from the input: any links (including loops and links
// out of range), levels and rooms, and raw floats for positions and lights; then the corner
// blends on those walls and on a small floor.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace ts2::scene::walllight;
    std::size_t at = 0;
    auto byte = [&]() -> std::uint8_t { return at < size ? data[at++] : 0; };
    auto number = [&]() {
        float f = 0;
        std::uint8_t raw[4] = {byte(), byte(), byte(), byte()};
        std::memcpy(&f, raw, 4);
        return f;
    };
    auto smallInt = [&]() { return static_cast<int>(static_cast<std::int8_t>(byte())); };

    Walls walls;
    walls.falloffScale = number();
    walls.insideAmbient = {number(), number(), number()};
    walls.outsideAmbient = {number(), number(), number()};
    walls.steps = byte() * 4;
    walls.brightFloor = number();
    walls.darkFloor = number();
    walls.runs.resize(byte() % 32);
    for (auto& run : walls.runs) {
        run.level = byte() % 4;  // few levels, so links between them are often valid
        run.room = static_cast<std::int16_t>(byte() % 4);
        run.diagonal = byte() & 1;
        run.previous = byte() % 34 - 1;
        run.next = byte() % 34 - 1;
        run.angle = number();
        run.segments.resize(byte() % 6);
        for (auto& s : run.segments) {
            s.x0 = number();
            s.y0 = number();
            s.x1 = number();
            s.y1 = number();
            s.up = {byte() % 34 - 1, byte() % 7 - 1};
            s.down = {byte() % 34 - 1, byte() % 7 - 1};
        }
    }
    std::vector<ts2::scene::LightSource> sources(byte() % 8);
    for (auto& s : sources) {
        s.color = {number(), number(), number()};
        s.level = smallInt();
        s.room = static_cast<std::int16_t>(byte() % 4);
        s.x0 = number();
        s.y0 = number();
        s.x1 = number();
        s.y1 = number();
        s.falloff = number();
        s.wallIntensity = number();
        s.interFloorFalloff = number();
        s.directional = byte() & 1;
        s.angle = number();
        s.directionalMix = number();
    }
    const float floorValue = number();
    light(walls, sources, [](float x, float, int) { return x > 3; },
          [floorValue](float, float, int) { return std::array<float, 3>{floorValue, floorValue, floorValue}; });

    using namespace ts2::scene;
    for (auto& run : walls.runs) run.lamplit = byte() & 1;
    lightcorners::blendWalls(walls);
    std::vector<std::vector<lightcorners::Tile>> tiles(byte() % 34);
    for (auto& t : tiles) {
        t.resize(byte() % 3);
        for (auto& tile : t) tile = {smallInt() * 1000000 + smallInt(), smallInt(), smallInt()};
    }
    auto corners = lightcorners::corners(walls, tiles);
    for (std::size_t i = byte() % 4; i > 0; --i) {
        corners.push_back({smallInt(), smallInt(), smallInt(), byte(), (byte() & 1) != 0});
    }
    floorlight::Grid grid;
    grid.width = byte() % 4;
    grid.height = byte() % 4;
    grid.level = smallInt() % 2;
    grid.insideAmbient = {number(), number(), number()};
    grid.outsideAmbient = {number(), number(), number()};
    grid.cells.resize(static_cast<std::size_t>(4 * grid.width * grid.height));
    grid.tiles.resize(static_cast<std::size_t>(grid.width * grid.height));
    for (auto& c : grid.cells) {
        for (auto& h : c.halves) {
            h.valid = byte() & 1;
            h.outside = byte() & 1;
            h.light = {number(), number(), number()};
        }
    }
    lightcorners::blendFloor(grid, corners);
    return 0;
}
