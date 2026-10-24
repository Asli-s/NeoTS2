#include "scene/object_lights.hpp"
#include "scene/object_sun.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// Lot objects' share of the sun and their rooms' lights worked out for a shadow grid, meshes and
// rooms made of the input: any sizes, heights, positions, node origins, placements and light values,
// including non-finite ones.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace ts2::scene;
    std::size_t at = 0;
    auto byte = [&]() -> std::uint8_t { return at < size ? data[at++] : 0; };
    auto number = [&]() {
        if (byte() < 220) {
            return static_cast<float>(byte() % 64) * 0.25f - 2.0f;
        }
        std::uint8_t raw[4] = {byte(), byte(), byte(), byte()};
        float f = 0;
        std::memcpy(&f, raw, 4);
        return f;
    };

    lotshadow::Grid shadow;
    shadow.width = byte() % 40;
    shadow.height = byte() % 40;
    shadow.shadow.resize(static_cast<std::size_t>(shadow.width) * static_cast<std::size_t>(shadow.height));
    for (auto& s : shadow.shadow) s = number();

    Scene scene;
    for (int m = byte() % 4; m > 0; --m) {
        Mesh mesh;
        for (int p = byte() % 4; p > 0; --p) {
            Primitive primitive;
            for (int v = byte() % 12; v > 0; --v) primitive.positions.push_back(number());
            primitive.origin = {number(), number(), number()};
            mesh.primitives.push_back(primitive);
        }
        scene.meshes.push_back(mesh);
    }
    for (int i = byte() % 8; i > 0; --i) {
        Instance instance;
        instance.mesh = static_cast<int>(byte() % 6) - 1;
        instance.part = byte() & 1 ? Part::Object : Part::Other;
        instance.sunTest = static_cast<SunTest>(byte() % 4);
        instance.lightGroup = static_cast<LightGroup>(byte() % 3);
        instance.transform = Transform::at(number(), number(), number(), number());
        scene.instances.push_back(instance);
    }

    objectsun::shade(scene, shadow, number());
    std::map<std::int16_t, lotlighting::Room> rooms;
    for (int r = byte() % 4; r > 0; --r) {
        roomlight::Lights lights;
        lights.ambient = {number(), number(), number()};
        for (int d = byte() % 4; d > 0; --d) lights.directionals.push_back({{number(), number(), number()}, {number(), number(), number()}});
        auto& room = rooms[static_cast<std::int16_t>(byte() % 4)];
        room.groups[lotlighting::kObjects] = lights;
        room.groups[lotlighting::kPortals] = lights;
    }
    std::vector<lotlighting::PortalLight> portals;
    for (int p = byte() % 12; p > 0; --p) {
        lotlighting::PortalLight portal;
        portal.into = static_cast<std::int16_t>(byte() % 4);
        portal.lightPosition = {number(), number(), number()};
        portal.spillPoint = {number(), number(), number()};
        portal.portal.nx = number();
        portal.portal.ny = number();
        if (byte() & 1) {
            windowlight::PortalLights lights;
            lights.beam = {{number(), number(), number()}, number()};
            if (byte() & 1) lights.spill = windowlight::Light{{number(), number(), number()}, number()};
            portal.lights = lights;
        }
        portals.push_back(portal);
    }
    for (auto& instance : scene.instances) instance.level = static_cast<int>(byte() % 8) - 2;
    const std::uint8_t roomSeed = byte();
    objectlights::light(scene, rooms, portals, number(), [&](int level, float x, float) { return std::int16_t((level + static_cast<int>(x) + roomSeed) & 3); }, byte() % 3);
    for (const auto& instance : scene.instances) {
        if (instance.lightSet >= static_cast<int>(scene.lightSets.size())) {
            std::abort();
        }
        for (const int set : instance.partLightSets) {
            if (set >= static_cast<int>(scene.lightSets.size())) {
                std::abort();
            }
        }
    }
    for (const auto& set : scene.lightSets) {
        if (set.lights.size() > 8) {
            std::abort();
        }
    }
    for (const auto& instance : scene.instances) {
        const std::size_t primitives =
            instance.mesh >= 0 && static_cast<std::size_t>(instance.mesh) < scene.meshes.size() ? scene.meshes[static_cast<std::size_t>(instance.mesh)].primitives.size() : 0;
        if (instance.sun.size() > primitives) {
            std::abort();
        }
        for (const float share : instance.sun) {
            if (!(share >= 0 && share <= 1)) {
                std::abort();
            }
        }
    }
    return 0;
}
