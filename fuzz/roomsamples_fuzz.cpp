#include "content/covering_colors.hpp"
#include "content/lot.hpp"
#include "scene/room_samples.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

// Room samples gathered from a lot made of the input (any floor grid, pattern names, wall graph
// with raw float points and coverings) with raw light values, and the thumbnail average on raw bytes.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace ts2;
    std::size_t at = 0;
    auto byte = [&]() -> std::uint8_t { return at < size ? data[at++] : 0; };
    auto number = [&]() {
        const std::uint8_t kind = byte();
        if (kind < 200) {
            return static_cast<float>(kind % 20) - 2.0f;
        }
        float f = 0;
        std::uint8_t raw[4] = {byte(), byte(), byte(), byte()};
        std::memcpy(&f, raw, 4);
        return f;
    };

    (void)content::averageColor(std::span<const std::uint8_t>(data, size));
    (void)content::nameHash24(std::string(reinterpret_cast<const char*>(data), std::min<std::size_t>(size, 64)));

    content::Lot lot;
    array3d::Array3D floors;
    floors.width = byte() % 24;
    floors.height = byte() % 24;
    floors.levels = byte() % 3;
    floors.elementSize = 8;
    floors.elements.resize(static_cast<std::size_t>(floors.width) * floors.height * floors.levels * 8);
    for (auto& e : floors.elements) e = byte() % 4;
    lot.grids[content::kFloorGridInstance] = floors;
    for (std::uint16_t i = 1; i < 4; ++i) {
        lot.floorPatterns[i] = {byte() & 1 ? "abc" : "", 1};
        lot.wallPatterns[i] = {byte() & 1 ? "def" : "", 1};
    }
    wallgraph::WallGraph graph;
    for (std::int32_t i = 0; i < byte() % 32; ++i) graph.points.push_back({i, number(), number(), static_cast<std::int8_t>(byte() % 3)});
    for (std::int32_t i = 0; i < byte() % 48; ++i) {
        graph.walls.push_back({i, byte() % 34, 0, byte() % 34, 0});
        lot.wallCoverings.push_back({static_cast<std::uint32_t>(i), 1, static_cast<std::uint16_t>(byte() % 5), static_cast<std::uint16_t>(byte() % 5)});
    }
    lot.roomGraph = graph;

    const scene::roomsamples::Color light{number(), number(), number()};
    scene::roomsamples::LotInputs in;
    in.quarterRoom = [](int level, int x, int y, int bit) { return static_cast<std::int16_t>((level + x + y + bit) % 4 - 1); };
    in.faceRoom = [](const scene::wallruns::Key& k) { return static_cast<std::int16_t>((k.x + k.side) % 4 - 1); };
    const bool known = byte() & 1;
    in.covering = [known, light](bool, const std::string&) -> std::optional<scene::roomsamples::Color> {
        if (known) return light;
        return std::nullopt;
    };
    in.floorLight = [light](int, float, float) { return light; };
    in.wallLight = [light](const scene::wallruns::Key&) { return light; };
    std::vector<scene::roomsamples::PortalInput> portals;
    for (int i = byte() % 8; i > 0; --i) portals.push_back({static_cast<std::int16_t>(byte() % 5 - 1), number(), number(), light});
    scene::roomsamples::Settings settings{number(), number(), number()};
    for (auto& [room, samples] : scene::roomsamples::gather(lot, in, portals, settings)) {
        auto averages = samples.averages();
        scene::roomlight::addUnlitLight(averages, light, number(), byte() & 1);
    }
    return 0;
}
