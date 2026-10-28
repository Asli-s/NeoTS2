#include "scene/window_light.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Window light with portals, settings and samples made from the input (raw floats included).
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace ts2::scene::windowlight;
    std::size_t at = 0;
    auto byte = [&]() -> std::uint8_t { return at < size ? data[at++] : 0; };
    auto number = [&]() {
        float f = 0;
        std::uint8_t raw[4] = {byte(), byte(), byte(), byte()};
        std::memcpy(&f, raw, 4);
        return f;
    };
    auto color = [&]() { return std::array<float, 3>{number(), number(), number()}; };
    auto mods = [&]() { return Modifiers{number(), number(), number(), number()}; };

    Daylight day;
    day.sunColor = color();
    day.sunStrength = number();
    day.sunDirection = color();
    day.sky = color();
    day.insidePortals = mods();
    day.outsidePortals = mods();
    day.insidePortalSpills = mods();
    day.outsidePortalSpills = mods();
    day.doorOrArchDirect = number();
    day.suppressionDimsRoomLight = byte() & 1;
    day.skylight = byte() & 1;
    day.saturation = number();
    day.outdoorLights.resize(byte() % 6);
    for (auto& l : day.outdoorLights) l = {color(), number(), color()};
    day.outdoorStrength = number();
    const Suppression supp{number(), number(), number()};

    std::vector<Portal> portals(byte() % 12);
    for (auto& p : portals) {
        p.otherRoom = static_cast<std::int16_t>(byte() % 4);
        p.nx = number();
        p.ny = number();
        p.x0 = number();
        p.y0 = number();
        p.x1 = number();
        p.y1 = number();
        p.height = static_cast<float>(byte() % 3);
        p.doorOrArch = byte() & 1;
        p.size = number();
        p.tint = color();
        p.reach = number();
        p.source.color = color();
        p.source.falloff = number();
    }
    suppress(portals, supp);
    const std::uint8_t flags = byte();
    for (const auto& p : portals) {
        (void)lightsFor(p, flags & 1, day, color());
    }
    return 0;
}
