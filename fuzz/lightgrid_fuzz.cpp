#include "content/lot.hpp"
#include "scene/light_grid.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// The floor light grid built from a lot made of the input: a floor grid of any size and content,
// and a room wall graph with raw float points (NaN, infinities, huge, off-grid and long walls),
// then lit by one lamp.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace ts2;
    std::size_t at = 0;
    auto byte = [&]() -> std::uint8_t { return at < size ? data[at++] : 0; };
    auto number = [&]() {
        const std::uint8_t kind = byte();
        if (kind < 200) {
            return static_cast<float>(kind % 40) - 4.0f;  // mostly whole tile lines, some outside
        }
        float f = 0;
        std::uint8_t raw[4] = {byte(), byte(), byte(), byte()};
        std::memcpy(&f, raw, 4);
        return f;
    };

    content::Lot lot;
    array3d::Array3D floors;
    floors.width = byte() % 40;
    floors.height = byte() % 40;
    floors.levels = byte() % 4;
    floors.elementSize = 8;
    floors.elements.resize(static_cast<std::size_t>(floors.width) * floors.height * floors.levels * 8);
    for (auto& e : floors.elements) {
        e = byte() & 1;
    }
    if (byte() != 3) {
        lot.grids[content::kFloorGridInstance] = floors;
    }
    wallgraph::WallGraph graph;
    const std::size_t points = byte() % 64;
    for (std::size_t i = 0; i < points; ++i) {
        graph.points.push_back({static_cast<std::int32_t>(i), number(), number(), static_cast<std::int8_t>(byte() % 4 - 1)});
    }
    const std::size_t walls = byte() % 96;
    for (std::size_t i = 0; i < walls; ++i) {
        graph.walls.push_back({static_cast<std::int32_t>(i), byte() % 70, 0, byte() % 70, 0});
        lot.wallCoverings.push_back({static_cast<std::uint32_t>(i), byte() & 1 ? 1u : 0x10000u, 0, 0});
    }
    lot.roomGraph = graph;

    const int level = static_cast<std::int8_t>(byte()) % 6;
    const auto lotWalls = scene::wallruns::wallsOf(lot, [](std::uint32_t covering) { return covering < 0x10000; });
    auto in = scene::lightgrid::fromLot(lot, level, lotWalls);
    const std::uint8_t rooms = byte();
    in.roomAt = [rooms](float x, float y) { return static_cast<std::int16_t>((static_cast<int>(x * 4) ^ static_cast<int>(y * 4)) & rooms & 7); };
    if (byte() == 9 && !in.floor.empty()) {
        in.floor.pop_back();  // mismatched sizes must be refused
    }
    auto grid = scene::lightgrid::build(in);
    if (grid.fits()) {
        scene::LightSource lamp;
        lamp.color = {1, 1, 1};
        lamp.level = level;
        lamp.falloff = 1;
        lamp.floorIntensity = 1;
        lamp.x0 = number();
        lamp.y0 = number();
        lamp.x1 = lamp.x0 + 1;
        lamp.y1 = lamp.y0 + 1;
        lamp.room = static_cast<std::int16_t>(byte() & 7);
        scene::floorlight::light(grid, {lamp}, nullptr);
    }
    return 0;
}
