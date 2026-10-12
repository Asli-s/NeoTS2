#include "scene/wall_runs.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

// Wall runs from walls made of the input: any ends (mostly near each other so loops, corners,
// crossings and stubs form), levels and floor answers, then lit by the floor.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace ts2::scene;
    std::size_t at = 0;
    auto byte = [&]() -> std::uint8_t { return at < size ? data[at++] : 0; };
    auto coordinate = [&]() {
        const std::uint8_t b = byte();
        return b < 240 ? b % 12 : static_cast<int>(static_cast<std::int8_t>(byte())) * 300;
    };

    std::vector<wallruns::Wall> walls(byte() % 128);
    for (auto& w : walls) {
        w.level = static_cast<std::int8_t>(byte()) % 4;
        w.x0 = coordinate();
        w.y0 = coordinate();
        w.x1 = coordinate();
        w.y1 = coordinate();
    }
    const std::uint8_t floors = byte(), rooms = byte();
    auto layout = wallruns::build(
        walls, [rooms](int level, float x, float y) { return static_cast<std::int16_t>((level + static_cast<int>(x) + static_cast<int>(y)) & rooms & 3); },
        [floors](int level, const wallruns::Key& k) { return ((level + k.x + k.y + k.side) & 7) < (floors & 7); });

    // Every link must name a real segment.
    for (const auto& run : layout.runs) {
        for (const auto& s : run.segments) {
            for (const auto& link : {s.up, s.down}) {
                if (link.run >= 0 && (static_cast<std::size_t>(link.run) >= layout.runs.size() ||
                                      static_cast<std::size_t>(link.segment) >= layout.runs[static_cast<std::size_t>(link.run)].segments.size())) {
                    std::abort();
                }
            }
        }
    }
    walllight::Walls lit;
    lit.runs = std::move(layout.runs);
    walllight::light(lit, {}, nullptr, [](float x, float, int) { return std::array<float, 3>{x * 0.1f, 0.5f, 1}; });
    return 0;
}
