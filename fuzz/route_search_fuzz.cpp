#include "sim/route_grid.hpp"
#include "sim/route_search.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// The path search over a route grid made of the input: rectangles of any type and cost (raw floats
// too: NaN, infinities, huge), some with doors, then starts and goals anywhere, inside obstacles or
// off the grid, and doors in any state.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace ts2::sim;
    std::size_t at = 0;
    auto byte = [&]() -> std::uint8_t { return at < size ? data[at++] : 0; };
    auto number = [&]() {
        const std::uint8_t kind = byte();
        if (kind < 220) {
            return static_cast<float>(kind % 72) / 8.0f - 0.5f;  // eighths of a tile, some outside
        }
        float f = 0;
        std::uint8_t raw[4] = {byte(), byte(), byte(), byte()};
        std::memcpy(&f, raw, 4);
        return f;
    };

    RouteGrid grid(8, 8);
    const int items = byte() % 40;
    for (int i = 0; i < items; ++i) {
        RouteItem item;
        item.rect = {number(), number(), number(), number()};
        item.type = byte() % 18;
        item.cost = byte() < 200 ? 1.0f + byte() % 20 : kBlockedCost;
        item.kind = static_cast<RouteItemKind>(byte() % 3);
        item.makesFloor = byte() & 1;
        item.owner = byte() % 4;
        if (byte() & 1) {
            item.door = RouteDoor{number(), number(), number(), number()};
        }
        if (RouteGrid::accepts(item)) {
            grid.add(item);
        }
    }
    const RouteLinks links = RouteLinks::build(grid);
    std::vector<RouteStart> starts(byte() % 5);
    for (auto& s : starts) {
        s = {number(), number()};
    }
    std::vector<RouteGoal> goals(byte() % 9);
    for (auto& g : goals) {
        g = {number(), number(), static_cast<float>(byte())};
    }
    std::vector<RouteDoorState> doors(byte() % 5);
    for (auto& d : doors) {
        d.owner = byte() % 4;
        d.extra = number();
        const std::uint8_t bits = byte();
        d.active[0] = bits & 1;
        d.active[1] = bits & 2;
        d.refused[0] = bits & 4;
        d.refused[1] = bits & 8;
    }
    RouteRandom jitter(byte()), heuristic(byte());
    (void)planRoute(grid, links, starts, goals, number(), number(), doors, jitter, heuristic);
    return 0;
}
