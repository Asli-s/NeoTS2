#include "scene/portal_records.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

// Portals from raw edges, light placements, light values and lot answers: NaN, infinities, huge
// values, lights far along their wall and floors that never rise above them.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace ts2::scene;
    std::size_t at = 0;
    auto byte = [&]() -> std::uint8_t { return at < size ? data[at++] : 0; };
    auto number = [&]() {
        const std::uint8_t kind = byte();
        if (kind < 128) {
            return static_cast<float>(kind % 24) * 0.5f - 2;
        }
        float f = 0;
        std::uint8_t raw[4] = {byte(), byte(), byte(), byte()};
        std::memcpy(&f, raw, 4);
        return f;
    };

    portalrecords::LotQueries lot;
    lot.lamps.lowestLevel = static_cast<std::int8_t>(byte()) % 4;
    lot.lamps.highestLevel = static_cast<std::int8_t>(byte()) % 6;
    const float storey = number();
    lot.lamps.floorHeight = [storey](int level, float, float) { return storey * static_cast<float>(level); };
    lot.lamps.tileOnLot = [](int x, int y, int level) { return x >= 0 && y >= 0 && x < 16 && y < 16 && level >= 0 && level < 4; };
    lot.lamps.roomAt = [](float x, float, int) { return static_cast<std::int16_t>(x > 4 ? 2 : 1); };
    const std::uint8_t answers = byte();
    lot.inLot = [answers](float x, float) { return (answers & 1) || (x >= 0 && x < 16); };
    lot.wallAt = [answers](const portalrecords::Edge& e) { return (answers & 2) || e.level >= 0; };
    lot.roomInFront = [](const portalrecords::Edge& e) { return static_cast<std::int16_t>(e.y1 > e.y0 ? 2 : 1); };
    if (answers & 4) {
        lot.roomLit = [](std::int16_t room) { return room != 3; };
    }

    portalrecords::PortalObject object;
    object.edge = {number(), number(), static_cast<std::int8_t>(byte()), number(), number()};
    object.doorOrArch = byte() & 1;
    for (int i = byte() % 6; i > 0; --i) {
        portalrecords::PortalLight light;
        light.place.position = {number(), number(), number()};
        light.place.rotation = {number(), number(), number(), number()};
        light.hasLight = byte() & 1;
        light.intensity = number();
        light.color = {number(), number(), number()};
        light.falloffRate = number();
        light.settings.intensity = number();
        light.settings.min = {number(), number()};
        light.settings.max = {number(), number()};
        object.lights.push_back(light);
    }
    (void)portalrecords::build(object, lot, number());
    return 0;
}
