#include "scene/lamp_lights.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

// Lamp records from raw settings, placements and model trees: NaN, infinities, huge values,
// cycles and missing children.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace ts2::scene;
    std::size_t at = 0;
    auto byte = [&]() -> std::uint8_t { return at < size ? data[at++] : 0; };
    auto number = [&]() {
        float f = 0;
        std::uint8_t raw[4] = {byte(), byte(), byte(), byte()};
        std::memcpy(&f, raw, 4);
        return f;
    };

    lamplights::Settings s;
    s.min = {number(), number()};
    s.max = {number(), number()};
    s.intensity = number();
    s.color = {number(), number(), number()};
    s.directional = byte() & 1;
    s.falloff = number();
    s.directionalFalloff = number();
    ts2::content::Lighting::Modifier m{number(), number(), number(), number()};
    s = lamplights::scaled(s, m, number());

    lamplights::LotQueries lot;
    lot.lowestLevel = static_cast<std::int8_t>(byte());
    lot.highestLevel = static_cast<std::int8_t>(byte());
    const float floors = number();
    lot.floorHeight = [floors](int level, float, float) { return floors * static_cast<float>(level); };
    lot.tileOnLot = [](int x, int y, int level) { return x >= 0 && y >= 0 && x < 64 && y < 64 && level >= 0 && level < 4; };
    lot.roomAt = [](float x, float, int) { return static_cast<std::int16_t>(x > 1 ? 2 : 0); };

    ts2::scenegraph::ResourceNode model;
    model.nodes.resize(byte() % 12);
    for (auto& n : model.nodes) {
        n.kind = static_cast<ts2::scenegraph::SceneNode::Kind>(byte() % 5);
        n.name = byte() & 1 ? "lamp_light" : "";
        n.translation = {number(), number(), number()};
        n.rotation = {number(), number(), number(), number()};
        for (int c = byte() % 4; c > 0; --c) {
            n.children.push_back(byte() % 16);
        }
    }
    const lamplights::Placement object{{number(), number(), number()}, lamplights::facingRotation(static_cast<std::int8_t>(byte()))};
    for (const auto& node : lamplights::lightNodes(model, object)) {
        (void)lamplights::sourceFor(s, node.place, lot);
    }
    lamplights::Placement p;
    p.position = {number(), number(), number()};
    p.rotation = {number(), number(), number(), number()};
    (void)lamplights::sourceFor(s, p, lot);
    return 0;
}
