#include "content/lighting.hpp"
#include "content/lot.hpp"
#include "scene/lot_lighting.hpp"
#include "scene/lot_shadow.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// A whole lot's indoor light worked out for a lot made of the input (any floor grid, wall graph
// with raw float points), with lamps, portals and settings from raw values.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace ts2;
    using namespace ts2::scene;
    std::size_t at = 0;
    auto byte = [&]() -> std::uint8_t { return at < size ? data[at++] : 0; };
    auto number = [&]() {
        const std::uint8_t kind = byte();
        if (kind < 200) {
            return static_cast<float>(kind % 20) * 0.5f - 2.0f;
        }
        float f = 0;
        std::uint8_t raw[4] = {byte(), byte(), byte(), byte()};
        std::memcpy(&f, raw, 4);
        return f;
    };

    content::Lot lot;
    array3d::Array3D floors;
    floors.width = byte() % 16;
    floors.height = byte() % 16;
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

    lotlighting::Inputs in;
    in.lot = &lot;
    for (int i = byte() % 6; i > 0; --i) {
        LightSource s;
        s.color = {number(), number(), number()};
        s.level = byte() % 4 - 1;
        s.room = static_cast<std::int16_t>(byte() % 4);
        s.x0 = number();
        s.y0 = number();
        s.x1 = number();
        s.y1 = number();
        s.falloff = number();
        s.floorIntensity = number();
        s.wallIntensity = number();
        s.interFloorFalloff = number();
        s.directional = byte() & 1;
        s.angle = number();
        s.directionalMix = number();
        in.lamps.push_back(s);
    }
    for (int i = byte() % 6; i > 0; --i) {
        portalrecords::Built b;
        b.room = static_cast<std::int16_t>(byte() % 4);
        b.portal.otherRoom = static_cast<std::int16_t>(byte() % 4);
        b.portal.nx = number();
        b.portal.ny = number();
        b.portal.x0 = number();
        b.portal.y0 = number();
        b.portal.x1 = number();
        b.portal.y1 = number();
        b.portal.height = number();
        b.portal.size = number();
        b.portal.reach = number();
        b.portal.doorOrArch = byte() & 1;
        b.portal.source.x0 = number();
        b.portal.source.y0 = number();
        b.portal.source.x1 = number();
        b.portal.source.y1 = number();
        in.portals.push_back(b);
    }
    in.blocksLight = [](std::uint32_t c) { return c % 3 != 0; };
    in.roomAt = [](int level, float x, float y) { return static_cast<std::int16_t>(x == x && y == y ? (level + static_cast<int>(x > 2)) % 3 : 0); };
    in.quarterRoom = [](int level, int x, int y, int bit) { return static_cast<std::int16_t>((level + x + y + bit) % 4 - 1); };
    const roomsamples::Color light{number(), number(), number()};
    in.covering = [light](bool, const std::string&) { return std::optional<roomsamples::Color>(light); };
    in.roomCount = byte() % 8;

    // Settings from a short light file text built from the input, so settingsFor runs too.
    std::string text = "lightingState day\n  light sun (" + std::to_string(number()) + ", 1, " + std::to_string(number()) + ") (1,1,1) " +
                       std::to_string(number()) + "\n  diffParam insideFloorAmbient " + std::to_string(number()) + "\nend\n";
    auto settings = lotlighting::settingsFor(content::readLightingText(text), "day");
    if (!settings) {
        return 0;
    }
    settings->steps = byte() % 64;
    settings->floorThreshold = number();
    settings->wallThreshold = number();
    settings->outsideFloorAmbient = number();
    settings->unlit = {{number(), number(), number()}, number()};
    (void)lotlighting::compute(in, *settings);

    // The sun shadow over a lot made of the input: any walls (also off the lot and diagonal of any
    // slope), levels and raw corner heights, from any sun.
    lotshadow::Inputs shade;
    shade.width = byte() % 20;
    shade.height = byte() % 20;
    shade.levels = byte() % 4;
    const float rise = number();
    shade.corner = [rise](int x, int y, int level) { return rise * static_cast<float>(x - y) + 3.0f * static_cast<float>(level); };
    shade.floorAt = [](int x, int y, int level, int quarter) { return (x + y + level + quarter) % 3 == 0; };
    for (int i = byte() % 12; i > 0; --i) {
        shade.walls.push_back({static_cast<int>(byte() % 5) - 1, static_cast<int>(byte()) - 100, static_cast<int>(byte()) - 100, static_cast<int>(byte()) - 100,
                               static_cast<int>(byte()) - 100, (byte() & 1) != 0, number()});
    }
    const auto grid = lotshadow::build(shade, {number(), number(), number()});
    (void)grid.shadowAt(number(), number());
    (void)lotshadow::elevation(shade, number(), number(), static_cast<int>(byte()) - 128);
    return 0;
}
