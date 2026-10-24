#include "content/lot.hpp"
#include "scene/pool_water.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// A lot's levels, wall coverings and pool water worked out for a lot made of the input: grids of any
// size and level count, wall graphs with any levels and points, and raw saved storeys.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace ts2;
    std::size_t at = 0;
    auto byte = [&]() -> std::uint8_t { return at < size ? data[at++] : 0; };
    auto word = [&]() {
        std::uint8_t raw[4] = {byte(), byte(), byte(), byte()};
        std::int32_t v = 0;
        std::memcpy(&v, raw, 4);
        return v;
    };
    auto number = [&]() {
        if (byte() < 200) {
            return static_cast<float>(byte() % 40) * 0.5f;
        }
        const std::int32_t raw = word();
        float f = 0;
        std::memcpy(&f, &raw, 4);
        return f;
    };
    auto grid = [&](std::size_t elementSize) {
        array3d::Array3D g;
        g.width = byte() % 12;
        g.height = byte() % 12;
        g.levels = byte() % 4;
        g.elementSize = byte() < 240 ? elementSize : byte() % 9;
        g.elements.resize(static_cast<std::size_t>(g.width) * g.height * g.levels * g.elementSize);
        for (auto& e : g.elements) e = byte() % 3;
        return g;
    };
    auto graph = [&]() {
        wallgraph::WallGraph g;
        g.firstLevel = byte() < 230 ? byte() % 3 - 1 : word();
        for (std::int32_t i = byte() % 24; i > 0; --i) {
            g.points.push_back({byte() % 24, number(), number(), byte() < 240 ? byte() % 4 - 1 : word()});
        }
        for (std::int32_t i = byte() % 32; i > 0; --i) {
            g.walls.push_back({byte() % 40, byte() % 26, 0, byte() % 26, 0});
        }
        return g;
    };

    content::Lot lot;
    lot.grids[content::kFloorGridInstance] = grid(8);
    lot.grids[content::kSurfaceGridInstance] = grid(4);
    lot.grids[content::kFloorHeightGridInstance] = grid(4);
    lot.roomGraph = graph();
    const auto walls = graph();
    std::vector<walllayer::Entry> coverings;
    for (int i = byte() % 32; i > 0; --i) {
        coverings.push_back({static_cast<std::uint32_t>(byte() % 40), byte(), byte(), byte()});
    }
    lot.wallCoverings = content::coveringsOfRoomWalls(*lot.roomGraph, walls, coverings);
    for (int i = byte() % 8; i > 0; --i) {
        lot.fencePosts.push_back({byte() < 240 ? byte() % 4 - 1 : word(), number(), number(), byte()});
    }

    for (int i = 0; i < 4; ++i) {
        const int index = content::gridIndex(lot, word());
        const int level = content::gridLevel(lot, word());
        if (index < -1000 || index > 1000 || level < -1000 || level > 1000) {
            std::abort();
        }
    }
    const int ground = content::groundGridLevel(lot);
    const auto& floors = lot.grids.at(content::kFloorGridInstance);
    if (ground < 0 || (ground > 0 && static_cast<std::uint32_t>(ground) >= floors.levels)) {
        std::abort();
    }

    const auto view = content::fromGroundStorey(lot);
    for (const auto& p : view.roomGraph->points) {
        if (ground > 0 && p.level < 0) {
            std::abort();
        }
    }

    const auto quarters = scene::poolwater::waterQuarters(lot);
    const auto layers = scene::poolwater::depthLayers(quarters, [](float x, float y) { return x - y; });
    for (const auto& layer : layers) {
        if (layer.positions.size() != layer.alphas.size() * 3 || layer.indices.size() != quarters.size() * 3) {
            std::abort();
        }
        for (const auto i : layer.indices) {
            if (i >= layer.alphas.size()) {
                std::abort();
            }
        }
    }
    return 0;
}
