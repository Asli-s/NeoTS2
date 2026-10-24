// Lot lighting as the game lights it: light maps, walls, rooms, portals and shadows.
#include "test_support.hpp"
#include "content/lighting_state.hpp"
#include "scene/lot_skirt.hpp"
#include "scene/incidence_map.hpp"

namespace {

void neighborhoodLightMapLooksUpNormals() {
    // A 4x4 lookup image whose pixel (x, y) is (40 * x + 10, 40 * y + 10, 200).
    ts2::scene::NeighborhoodLight light;
    light.sun = {0.6f, 0, 0.8f};
    light.lookup.width = light.lookup.height = 4;
    for (std::uint8_t y = 0; y < 4; ++y) {
        for (std::uint8_t x = 0; x < 4; ++x) {
            light.lookup.rgba.insert(light.lookup.rgba.end(), {static_cast<std::uint8_t>(40 * x + 10), static_cast<std::uint8_t>(40 * y + 10), 200, 255});
        }
    }
    // Flat ground, 5 x 3 vertices: every texel looks up the middle pixel (2, 2): (90, 90, 200), the
    // 200 rounded up twice as the game does (200 -> 201 -> 202).
    const std::uint32_t cols = 5, rows = 3;
    std::vector<float> heights(cols * rows, 100.0f), normals;
    for (std::size_t i = 0; i < heights.size(); ++i) {
        normals.insert(normals.end(), {0, 0, 1});
    }
    auto map = ts2::scene::neighborhoodLightMap(cols, rows, heights, normals, 10, light);
    CHECK(map.width == 4 && map.height == 2);
    CHECK(map.rgba[0] == 90 && map.rgba[1] == 90 && map.rgba[2] == 202 && map.rgba[3] == 255);
    // A tall ridge on the sunny side (x = 4) shades the ground beside it: its normals lean away
    // from the sun, so they look up another pixel.
    for (std::uint32_t y = 0; y < rows; ++y) {
        heights[y * cols + 4] = 200.0f;
    }
    map = ts2::scene::neighborhoodLightMap(cols, rows, heights, normals, 10, light);
    CHECK(map.rgba[(0 * 4 + 3) * 4] != 90);
}

void objectsGetTheSunTheBuildingsLeaveThem() {
    using namespace ts2::scene;
    // A lot one tile across with a wall's shadow 3 high over its x-low half.
    lotshadow::Grid shadow;
    shadow.width = shadow.height = lotshadow::kSamplesPerTile;
    shadow.shadow.assign(64, -1000);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 4; ++x) shadow.shadow[static_cast<std::size_t>(y * 8 + x)] = 3;
    }
    CHECK(objectsun::at(shadow, 1, {0.2f, 0.5f, 0}) == 0 && objectsun::at(shadow, 1, {0.2f, 0.5f, 2.5f}) == 0.5f);
    CHECK(objectsun::at(shadow, 1, {0.2f, 0.5f, 3.5f}) == 1 && objectsun::at(shadow, 1, {0.8f, 0.5f, 0}) == 1);
    CHECK(objectsun::at(shadow, 1, {-0.1f, 0.5f, 0}) == 1 && objectsun::at(shadow, 1, {0.95f, 0.5f, 0}) == 1 &&
          objectsun::at(shadow, 1, {std::nanf(""), 0.5f, 0}) == 1);

    // A box 0.4 wide standing at x 0.4, its centre at x 0.5 in the sun. As a window, its node at
    // 0.1 behind it is in the shadow. As a fence, the point 0.1 above its top is above the shadow.
    Scene scene;
    Mesh mesh;
    Primitive p;
    p.positions = {-0.1f, -0.1f, 0, 0.3f, 0.1f, 2.5f};
    p.origin = {-0.1f, 0, 0};
    mesh.primitives.push_back(p);
    scene.meshes.push_back(mesh);
    Instance box;
    box.mesh = 0;
    box.part = Part::Object;
    box.transform = Transform::at(0.4f, 0.5f, 0, 0);
    Instance window = box, fence = box, stairs = box;
    window.sunTest = SunTest::NodePosition;
    fence.sunTest = SunTest::AboveBox;
    fence.transform = Transform::at(0.2f, 0.5f, 0.5f, 0);
    stairs.sunTest = SunTest::Never;
    const auto centre = objectsun::testPoints(scene, box);
    CHECK(centre.size() == 1 && std::abs(centre[0][0] - 0.5f) < 1e-5f && std::abs(centre[0][2] - 1.25f) < 1e-5f);
    const auto node = objectsun::testPoints(scene, window);
    CHECK(node.size() == 1 && std::abs(node[0][0] - 0.3f) < 1e-5f && node[0][2] == 0);
    const auto above = objectsun::testPoints(scene, fence);
    CHECK(above.size() == 1 && std::abs(above[0][0] - 0.3f) < 1e-5f && std::abs(above[0][2] - 3.1f) < 1e-5f);
    scene.instances = {box, window, fence, stairs};
    objectsun::shade(scene, shadow, 1);
    CHECK(scene.instances[0].sun == std::vector<float>{1} && scene.instances[1].sun == std::vector<float>{0});
    CHECK(scene.instances[2].sun == std::vector<float>{1} && scene.instances[3].sun.empty());

    // The same for one object wherever it stands now: the box moved into the shadow.
    const auto sunAt = objectsun::thingSun(std::make_shared<const lotshadow::Grid>(shadow), 1);
    Instance moved = box;
    moved.transform = Transform::at(0.0f, 0.5f, 0, 0);
    CHECK(sunAt && sunAt(scene, moved) == std::vector<float>{0} && sunAt(scene, box) == std::vector<float>{1});
    CHECK(sunAt && sunAt(scene, stairs).empty());
    CHECK(!objectsun::thingSun(nullptr, 1));
}

void objectsInRoomsGetTheirRoomsLights() {
    using namespace ts2::scene;
    roomlight::Lights lights;
    lights.ambient = {0.425f, 0.407f, 0.38f};
    lights.directionals.push_back({{0.615f, 0.587f, 0.55f}, {-0.0f, -1, -0.75f}});
    lights.directionals.push_back({{0.2f, 0.2f, 0.2f}, {-0.0f, -0.0f, -1}});
    lights.directionals.push_back({{0.1f, 0.1f, 0.1f}, {0, 0, 1}});
    const auto set = objectlights::fromRoom(lights);
    // The game's light axes: a side light tilted 30.5 degrees (not the stored 36.9), straight up and down.
    CHECK(set.ambient == lights.ambient && set.lights.size() == 3 && set.lights[0].color == lights.directionals[0].color);
    CHECK(std::abs(set.lights[0].direction[1] - 0.8615f) < 1e-4f && std::abs(set.lights[0].direction[2] - 0.5077f) < 1e-4f);
    CHECK(std::abs(set.lights[1].direction[2] - 1) < 1e-6f && std::abs(set.lights[2].direction[2] + 1) < 1e-6f);

    Scene scene;
    scene.meshes.emplace_back().primitives.emplace_back();
    Instance prop, outdoors, window, sim, wall;
    prop.part = outdoors.part = window.part = sim.part = Part::Object;
    prop.mesh = outdoors.mesh = window.mesh = sim.mesh = wall.mesh = 0;
    prop.transform = Transform::at(1.5f, 1.5f, 3, 0);
    prop.level = 2;  // the ground storey is grid level 1: this stands a storey up, in room 4
    outdoors.transform = Transform::at(5.5f, 1.5f, 0, 0);
    outdoors.level = 1;
    window.transform = sim.transform = wall.transform = prop.transform;
    window.level = sim.level = wall.level = 2;
    window.lightGroup = LightGroup::Portals;
    sim.lightGroup = LightGroup::Sims;
    scene.instances = {prop, outdoors, window, sim, wall};
    std::map<std::int16_t, lotlighting::Room> rooms;
    rooms[4].groups[lotlighting::kObjects] = lights;
    objectlights::light(scene, rooms, {}, 0.275f, [](int level, float x, float) { return std::int16_t(level == 1 && x < 4 ? 4 : 0); }, 1);
    CHECK(scene.lightSets.size() == 1 && scene.instances[0].lightSet == 0 && scene.instances[1].lightSet == -1);
    CHECK(scene.instances[2].lightSet == -1 && scene.instances[3].lightSet == -1 && scene.instances[4].lightSet == -1);
}

// Things in a room also get the point lights of the windows lighting it, as the game scores them by
// the corners of each part's box: the spill only in front of its plane and within its reach, the
// sun beam (a spot light) only within its cone, nothing from windows lighting other rooms.
void objectsGetTheWindowLightsOfTheirRoom() {
    using namespace ts2::scene;
    Scene scene;
    Mesh mesh;
    mesh.primitives.resize(1);
    mesh.primitives[0].positions = {-0.25f, -0.25f, 0, 0.25f, 0.25f, 0.5f};
    scene.meshes.push_back(mesh);
    Instance thing;
    thing.part = Part::Object;
    thing.mesh = 0;
    thing.level = 1;
    thing.transform = Transform::at(1.5f, 1.5f, 0, 0);
    scene.instances = {thing};
    roomlight::Lights objects;
    objects.ambient = {0.4f, 0.4f, 0.4f};
    objects.directionals.push_back({{0.6f, 0.6f, 0.6f}, {0, 1, -0.75f}});
    std::map<std::int16_t, lotlighting::Room> rooms;
    rooms[4].groups[lotlighting::kObjects] = objects;
    // A window in the wall at x = 0 lighting room 4 (+x).
    lotlighting::PortalLight window;
    window.into = 4;
    window.lightPosition = {0.1f, 1.5f, 1.2f};
    window.spillPoint = {0.1f, 1.5f, 1.5f};
    window.portal.nx = 1;
    window.lights.emplace();
    window.lights->spill = windowlight::Light{{0.6f, 0.6f, 0.45f}, 0.4f};
    window.lights->beam = {{0.7f, 0.68f, 0.47f}, 1.3f};
    window.spot = true;
    window.lightAxis = {0, 0.848f, 0.53f};  // the beam shines along -y: away from the thing
    window.coneAngle = 40 * 3.14159265f / 180;
    window.conePenumbra = 20 * 3.14159265f / 180;
    auto other = window;
    other.into = 5;
    std::vector<lotlighting::PortalLight> portals{window, other};
    const auto roomAt = [](int level, float x, float) { return std::int16_t(level == 0 && x > 0 ? 4 : 0); };
    objectlights::light(scene, rooms, portals, 0.275f, roomAt, 1);
    CHECK(scene.instances[0].lightSet == 0 && scene.lightSets.size() == 1);
    if (scene.lightSets.empty()) return;
    const auto& lights = scene.lightSets[0].lights;
    // The directional first, then the spill: its reach, sqrt((|rgb| / 0.275 - 1) / 0.4) = 2.496.
    CHECK(lights.size() == 2 && !lights[0].point && lights[1].point && !lights[1].spot);
    if (lights.size() == 2) CHECK(std::abs(lights[1].range - 2.4963f) < 1e-3f && lights[1].position == window.lightPosition);

    // Turned toward the thing, the beam reaches it too. A thing behind the spill's plane gets neither.
    portals[0].lightAxis = {-0.848f, 0, 0.53f};
    objectlights::light(scene, rooms, portals, 0.275f, roomAt, 1);
    CHECK(scene.lightSets.size() == 1 && scene.lightSets[0].lights.size() == 3 && scene.lightSets[0].lights[2].spot);
    if (scene.lightSets.size() == 1 && scene.lightSets[0].lights.size() == 3) {
        CHECK(std::abs(scene.lightSets[0].lights[2].coneOuter - 0.866025f) < 1e-5f);
    }
    portals[0].spillPoint = {1.9f, 1.5f, 1.5f};
    portals[0].lightAxis = {0, 0.848f, 0.53f};
    objectlights::light(scene, rooms, portals, 0.275f, roomAt, 1);
    CHECK(scene.lightSets.size() == 1 && scene.lightSets[0].lights.size() == 1);
}

// A window's part whose node is in a room gets the room's door and window light with the floor light
// map's texel under the node (times 1.5) as ambient. The part outdoors keeps the daylight.
void doorAndWindowPartsInRoomsGetTheFloorLightUnderThem() {
    using namespace ts2::scene;
    Scene scene;
    Mesh mesh;
    mesh.primitives.resize(3);
    mesh.primitives[0].origin = {0.5f, 1.85f, 0.94f};  // outside the wall at y = 2
    mesh.primitives[1].origin = {0.5f, 2.15f, 0.94f};  // inside
    mesh.primitives[2].origin = {0.5f, 2.15f, 2.29f};  // inside, higher up: the same texel
    scene.meshes.push_back(mesh);
    Instance window;
    window.part = Part::Object;
    window.mesh = 0;
    window.level = 2;
    window.lightGroup = LightGroup::Portals;
    window.sunTest = SunTest::NodePosition;
    scene.instances = {window};
    Image floor{16, 32, std::vector<std::uint8_t>(16 * 32 * 4, 0)};
    const std::size_t texel = (static_cast<std::size_t>(17) * 16 + 4) * 4;  // floor(2.15 * 8), floor(0.5 * 8)
    floor.rgba[texel] = 95;
    floor.rgba[texel + 1] = 93;
    floor.rgba[texel + 2] = 90;
    scene.images = {Image{}, floor};
    scene.lightMaps.emplace();
    scene.lightMaps->floors = {0, 1};
    roomlight::Lights portals;
    portals.ambient = {0.17f, 0.16f, 0.15f};
    portals.ambientFromLightMap = true;
    portals.directionals.push_back({{0.044f, 0.042f, 0.039f}, {0, 1, -0.75f}});
    std::map<std::int16_t, lotlighting::Room> rooms;
    rooms[12].groups[lotlighting::kPortals] = portals;
    objectlights::light(scene, rooms, {}, 0.275f, [](int level, float, float y) { return std::int16_t(level == 1 && y > 2 ? 12 : 0); }, 1);
    const auto& parts = scene.instances[0].partLightSets;
    CHECK((parts == std::vector<int>{-1, 0, 0}) && scene.lightSets.size() == 1);
    if (scene.lightSets.empty()) return;
    const auto& set = scene.lightSets[0];
    CHECK(std::abs(set.ambient[0] - 95 * 1.5f / 255) < 1e-6f && std::abs(set.ambient[2] - 90 * 1.5f / 255) < 1e-6f);
    CHECK(set.lights.size() == 1 && set.lights[0].color == portals.directionals[0].color);
}

void onlyWallShadowsAreHiddenOnLots() {
    using ts2::scene::isWallShadowGroup;
    CHECK(isWallShadowGroup("wallshadow") && isWallShadowGroup("NorthWallShadow") && isWallShadowGroup("southwallshadow") &&
          isWallShadowGroup("eastwallshadow") && isWallShadowGroup("westwallshadow"));
    CHECK(!isWallShadowGroup("groundshadow") && !isWallShadowGroup("curtainvalue_southwallshadow_alpha") &&
          !isWallShadowGroup("plantficus_leavesshadow_alpha") && !isWallShadowGroup("body"));
}

// A floor grid of one room, every cell linked to its neighbours, lit only by its ambient.
ts2::scene::floorlight::Grid testFloor(int width, int height) {
    ts2::scene::floorlight::Grid g;
    g.width = width;
    g.height = height;
    g.insideAmbient = {0.05f, 0.05f, 0.05f};
    g.cells.resize(static_cast<std::size_t>(4 * width * height));
    g.tiles.assign(static_cast<std::size_t>(width * height), {true, 0});
    for (auto& cell : g.cells) {
        cell.halves[0].valid = true;
        cell.halves[0].room = 1;
        cell.halves[0].links = 0xFF;
    }
    return g;
}

void floorLightSpreadsAndFades() {
    using namespace ts2::scene::floorlight;
    ts2::scene::LightSource lamp;
    lamp.color = {1, 1, 1};
    lamp.room = 1;
    lamp.x1 = lamp.y1 = 0.5f;  // the first half tile only

    auto g = testFloor(1, 1);
    CHECK(light(g, {lamp}, nullptr));
    CHECK(std::abs(floorLight(g, 0, 0) - 1) < 1e-6f);
    CHECK(std::abs(floorLight(g, 1, 0) - (1 / 1.4f + 0.01f)) < 1e-6f);   // one step, falloff 1 x 0.4
    CHECK(std::abs(floorLight(g, 0, 1) - (1 / 1.4f + 0.01f)) < 1e-6f);
    CHECK(std::abs(floorLight(g, 1, 1) - (1 / 1.56f + 0.01f)) < 1e-6f);  // diagonal: 1.4 times the falloff

    // A wall between the first and second tile: no links across it.
    g = testFloor(2, 1);
    for (int y = 0; y < 2; ++y) {
        g.at(1, y)->halves[0].links &= static_cast<std::uint8_t>(~0xE0);  // directions 5, 6, 7 go right
        g.at(2, y)->halves[0].links &= static_cast<std::uint8_t>(~0x0E);  // 1, 2, 3 go left
    }
    CHECK(light(g, {lamp}, nullptr));
    CHECK(floorLight(g, 1, 0) > 0.5f);
    CHECK(floorLight(g, 2, 0) == 0.05f && floorLight(g, 3, 1) == 0.05f);

    // A lamp in another room lights nothing.
    g = testFloor(1, 1);
    ts2::scene::LightSource other = lamp;
    other.room = 2;
    CHECK(light(g, {other}, nullptr));
    CHECK(floorLight(g, 0, 0) == 0.05f);

    // From the level above: a floor between stops it, otherwise it loses half.
    ts2::scene::LightSource above = lamp;
    above.level = 1;
    g = testFloor(1, 1);
    CHECK(light(g, {above}, [](int, int, int) { return true; }));
    CHECK(floorLight(g, 0, 0) == 0.05f);
    CHECK(light(g, {above}, [](int, int, int) { return false; }));
    CHECK(std::abs(floorLight(g, 0, 0) - 0.5f) < 1e-6f);

    // A grid whose cells do not match its size is left alone.
    g = testFloor(1, 1);
    g.cells.pop_back();
    CHECK(light(g, {lamp}, nullptr));
    CHECK(floorLight(g, 0, 0) == 0);
}

// One wall face two tiles long along x, looking toward -y, in room 1.
ts2::scene::walllight::Walls testWall() {
    ts2::scene::walllight::Walls w;
    ts2::scene::walllight::Run run;
    run.room = 1;
    for (int i = 0; i < 2; ++i) {
        ts2::scene::walllight::Segment s;
        s.x0 = static_cast<float>(i);
        s.x1 = static_cast<float>(i + 1);
        run.segments.push_back(s);
    }
    w.runs.push_back(run);
    return w;
}

// A level of plain floor for the grid builder, every tile floored and in room 1.
ts2::scene::lightgrid::LevelInput gridInput(int width, int height) {
    ts2::scene::lightgrid::LevelInput in;
    in.width = width;
    in.height = height;
    in.floor.assign(static_cast<std::size_t>(width * height), 1);
    in.walls.assign(static_cast<std::size_t>(width * height), 0);
    in.roomAt = [](float, float) { return std::int16_t{1}; };
    return in;
}

void lightGridIsBuiltAsTheGame() {
    using namespace ts2::scene;
    // Open floor: each half tile links to every neighbour inside the grid, nothing at the edge.
    auto g = lightgrid::build(gridInput(2, 1));
    CHECK(g.fits());
    CHECK(g.at(0, 0)->halves[0].links == 0x70);  // down, down-right, right
    CHECK(g.at(1, 0)->halves[0].links == 0x7C);
    CHECK(g.at(1, 1)->halves[0].links == 0xC7);
    CHECK(g.at(0, 0)->halves[0].valid && !g.at(0, 0)->halves[1].valid);
    CHECK(g.at(0, 0)->halves[0].room == 1 && !g.at(0, 0)->halves[0].outside);

    // A wall between the two tiles: nothing links across it, and lamp light stays on its side.
    auto in = gridInput(2, 1);
    in.walls[0] = lightgrid::kPlusX;
    in.walls[1] = lightgrid::kMinusX;
    g = lightgrid::build(in);
    for (int y = 0; y < 2; ++y) {
        CHECK((g.at(1, y)->halves[0].links & 0xE0) == 0);
        CHECK((g.at(2, y)->halves[0].links & 0x0E) == 0);
    }
    LightSource lamp;
    lamp.color = {1, 1, 1};
    lamp.room = 1;
    lamp.x1 = lamp.y1 = 0.5f;
    CHECK(floorlight::light(g, {lamp}, nullptr));
    CHECK(g.at(1, 0)->halves[0].light[0] > 0.5f);
    CHECK(g.at(2, 0)->halves[0].light[0] == 0);

    // A diagonal wall along x = y splits the tile: both halves of each quarter take part, each
    // with the room on its side, and nothing links across the wall.
    in = gridInput(1, 1);
    in.walls[0] = lightgrid::kDiagonalXEqualsY;
    in.roomAt = [](float x, float y) { return static_cast<std::int16_t>(y < x ? 2 : 0); };
    g = lightgrid::build(in);
    CHECK(g.tiles[0].diagonal == 2);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            const auto& c = *g.at(x, y);
            CHECK(c.halves[0].valid && c.halves[1].valid);
            CHECK(c.halves[0].room == 2 && !c.halves[0].outside);
            CHECK(c.halves[1].room == 0 && c.halves[1].outside);
        }
    }
    lamp.x0 = 0.5f;
    lamp.x1 = 1;
    lamp.y1 = 0.5f;
    lamp.room = 2;
    g = lightgrid::build(in);
    CHECK(floorlight::light(g, {lamp}, nullptr));
    CHECK(g.at(1, 0)->halves[0].light[0] > 0.5f);
    CHECK(g.at(0, 1)->halves[1].light[0] == 0);

    // Sizes that don't match or are out of range give an empty grid.
    in = gridInput(2, 2);
    in.floor.pop_back();
    CHECK(!lightgrid::build(in).fits());
    CHECK(!lightgrid::build(gridInput(0, 3)).fits());
    CHECK(!lightgrid::build(gridInput(257, 1)).fits());

    // From a lot: an upper tile counts as floor when it or the tile above has floor. Walls come
    // from the room wall graph, with fences and bad points left out.
    ts2::content::Lot lot;
    ts2::array3d::Array3D floors;
    floors.width = 3;
    floors.height = 1;
    floors.levels = 3;
    floors.elementSize = 8;
    floors.elements.assign(3 * 3 * 8, 0);
    floors.elements[(1 * 3 + 0) * 8] = 5;  // level 1, tile 0
    floors.elements[(2 * 3 + 1) * 8 + 6] = 5;  // level 2, tile 1, one quarter
    lot.grids[ts2::content::kFloorGridInstance] = floors;
    ts2::wallgraph::WallGraph graph;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    graph.points = {{1, 1, 0, 1}, {2, 1, 1, 1}, {3, 2, 0, 1}, {4, 2, 1, 1}, {5, nan, 0, 1}, {6, 0.5f, 1, 1}};
    graph.walls = {{1, 1, 0, 2, 0}, {2, 3, 0, 4, 0}, {3, 5, 0, 2, 0}, {4, 6, 0, 1, 0}};
    lot.roomGraph = graph;
    lot.wallCoverings = {{1, 1, 0, 0}, {2, 0x12345678, 0, 0}, {3, 1, 0, 0}, {4, 1, 0, 0}};
    const auto blocks = [](std::uint32_t covering) { return covering < 0x10000; };
    const auto walls = wallruns::wallsOf(lot, blocks);
    const auto ground = lightgrid::fromLot(lot, 0, walls);
    CHECK(ground.floor == std::vector<std::uint8_t>({1, 1, 1}));
    const auto upper = lightgrid::fromLot(lot, 1, walls);
    CHECK(upper.floor == std::vector<std::uint8_t>({1, 1, 0}));
    CHECK(upper.walls == std::vector<std::uint8_t>({lightgrid::kPlusX, lightgrid::kMinusX, 0}));
    CHECK(lightgrid::fromLot(lot, -1, walls).floor.empty());
}

void wallLightFadesAlongAndUp() {
    using namespace ts2::scene::walllight;
    const Pixel p = pack({0.25f, 0.5f, 1.0f});
    CHECK(p.r == 64 && p.g == 128 && p.b == 255 && p.a == 255);

    ts2::scene::LightSource lamp;
    lamp.color = {1, 1, 1};
    lamp.room = 1;
    lamp.x0 = lamp.x1 = 0.25f;
    lamp.y0 = lamp.y1 = -1;  // one tile in front of the first half

    auto w = testWall();
    std::set<int> changed;
    resetToAmbient(w);
    CHECK(lightFromSources(w, {lamp}, nullptr, changed));
    // 1 x (1 / 1.4)^2 at two half-tiles' distance. A step along fades by 1 / 1.4 in fixed point.
    const auto& s0 = w.runs[0].segments[0];
    CHECK(changed.count(0) == 1 && w.runs[0].lamplit);
    CHECK(s0.halves[0][2].r == 131 && s0.halves[0][0].r == 93);
    CHECK(s0.halves[1][2].r == 93 && s0.halves[1][5].r == 66);

    // Behind the face, or in another room: nothing.
    w = testWall();
    changed.clear();
    ts2::scene::LightSource behind = lamp;
    behind.y0 = behind.y1 = 1;
    ts2::scene::LightSource elsewhere = lamp;
    elsewhere.room = 2;
    resetToAmbient(w);
    CHECK(lightFromSources(w, {behind, elsewhere}, nullptr, changed));
    CHECK(changed.empty() && w.runs[0].segments[0].halves[0][2].r == 0);

    // The floor in front lights a dark wall at half its share.
    w = testWall();
    changed.clear();
    resetToAmbient(w);
    lightFromFloor(w, [](float, float, int) { return std::array<float, 3>{0.5f, 0.5f, 0.5f}; }, changed);
    CHECK(changed.count(0) == 1 && w.runs[0].segments[1].halves[1][3].r == 32);  // 0.5 x 0.5 x 0.5

    // A wall linked to itself above (bad data) still finishes.
    w = testWall();
    w.runs[0].segments[0].up = {0, 0};
    changed.clear();
    resetToAmbient(w);
    CHECK(lightFromSources(w, {lamp}, nullptr, changed));
    CHECK(spread(w, changed));
    CHECK(w.runs[0].segments[1].halves[1][2].r > 0);
}

void roomLightMatchesTheGame() {
    using namespace ts2::scene::roomlight;
    // A room with no light gets the unlit colour on its sides and floor (the game's values for such a room).
    std::array<std::array<float, 3>, 6> samples{};
    addUnlitLight(samples, {0.45f, 0.425f, 0.4f}, 0.4f, false);
    CHECK(samples[0][0] == 0.45f && samples[4][2] == 0.4f && samples[5][0] == 0);
    // Without a threshold, only rooms no lamp or window lights get it, all of it.
    std::array<std::array<float, 3>, 6> lit{}, unlit{};
    addUnlitLight(lit, {0.45f, 0.425f, 0.4f}, 0, true);
    addUnlitLight(unlit, {0.45f, 0.425f, 0.4f}, 0, false);
    CHECK(lit[0][0] == 0 && unlit[0][0] == 0.45f);

    Settings object;
    object.overall = 0.5f;
    object.directionalBoost = 4;
    const auto l = lightsFor(samples, object);
    CHECK(l && l->directionals.size() == 2);
    CHECK(std::abs(l->ambient[0] - 0.444375f) < 1e-5f);
    CHECK(std::abs(l->directionals[0].color[0] - 0.3825f) < 1e-5f);  // (0.54 - 0.444375) x 4
    CHECK(l->directionals[0].direction == (std::array<float, 3>{1, 0, -0.75f}));
    CHECK(l->directionals[1].direction == (std::array<float, 3>{-1, 0, -0.75f}));
    CHECK(std::abs(average(samples)[0] - 0.375f) < 1e-6f);

    Settings five = object;
    five.type = 0;
    CHECK(!lightsFor(samples, five));

    // A window facing +x and a little +y feeds those two sides by how much it faces them.
    Samples s;
    s.addFacing(0.8f, 0.6f, {1, 1, 1});
    const auto a = s.averages();
    CHECK(a[kPlusX][0] == 1 && a[kPlusY][0] == 1 && a[kMinusX][0] == 0 && s.weight[kPlusX] == 0.8f);
}

void windowLightMatchesTheGame() {
    using namespace ts2::scene::windowlight;
    // The game's day state of a lot.
    Daylight day;
    day.sunColor = {0.97f, 0.94f, 0.83f};
    day.sunStrength = 2;
    day.sunDirection = {0.54091f, 0.43273f, 0.72122f};
    day.sky = {0.5f, 0.5f, 0.5f};
    day.insidePortals = {0.5f, 1.3f, 0.7f, 1};
    day.insidePortalSpills = {0.9f, 0.4f, 0.8f, 0.9f};

    // Two windows a tile apart on the same wall take 0.2 x (1 - 1/4) of each other's light.
    Portal w;
    w.otherRoom = 1;
    w.nx = -1;
    w.y1 = 1;
    w.size = 1.5f;
    w.tint = {1, 0.99362f, 0.772f};
    w.source.color = {1.25f, 1.25f, 1.25f};
    std::vector<Portal> pair{w, w};
    pair[1].y0 = 1;
    pair[1].y1 = 2;
    suppress(pair, {});
    CHECK(std::abs(pair[0].suppression - 0.85f) < 1e-6f && std::abs(pair[1].suppression - 0.85f) < 1e-6f);

    // Looking out from the outdoors into room 1, against the sun's x.
    const auto l = lightsFor(pair[0], true, true, day, {0, 0, 0});
    CHECK(l && l->spill && l->sources.size() == 2);
    const float sun = 0.54091f;  // how much the window faces the sun
    CHECK(std::abs(l->beam.color[0] - 1.94f * 0.5f * 0.85f * 1.5f * sun) < 1e-5f && l->beam.falloff == 1.3f);
    CHECK(std::abs(l->spill->color[2] - 0.5f * 0.772f * 0.9f * 0.85f * 1.5f) < 1e-5f && l->spill->falloff == 0.4f);
    CHECK(std::abs(l->sources[0].color[0] - 1.25f * 1.94f * 0.7f * sun) < 1e-5f);  // the sun beam on floors and walls
    CHECK(std::abs(l->sources[1].color[0] - 1.25f * 0.5f * 0.8f) < 1e-5f && std::abs(l->sources[1].falloff - 0.9f) < 1e-6f);

    // A door lets no sun beam through (doorOrArchDirectMultiplier 0), and without sun nothing is supported.
    Portal door = pair[0];
    door.doorOrArch = true;
    const auto d = lightsFor(door, true, true, day, {0, 0, 0});
    CHECK(d && d->beam.color[0] == 0 && d->sources.size() == 1);
    CHECK(!lightsFor(pair[0], true, false, day, {0, 0, 0}));
}

void wallRunsAreLaidOutAsTheGame() {
    using namespace ts2::scene;
    // A room two tiles by one, the same on two levels.
    std::vector<wallruns::Wall> walls;
    for (int level = 0; level < 2; ++level) {
        walls.push_back({level, 0, 0, 2, 0});
        walls.push_back({level, 2, 0, 2, 1});
        walls.push_back({level, 2, 1, 0, 1});
        walls.push_back({level, 0, 1, 0, 0});
    }
    const auto roomAt = [](int, float x, float y) { return static_cast<std::int16_t>(x > 0 && x < 2 && y > 0 && y < 1 ? 1 : 0); };
    auto layout = wallruns::build(walls, roomAt, [](int, const wallruns::Key&) { return false; });
    CHECK(layout.complete);
    // Inside and outside of each level, one run per straight side.
    CHECK(layout.runs.size() == 16);
    int inside = 0, segments = 0, linked = 0;
    for (std::size_t r = 0; r < layout.runs.size(); ++r) {
        const auto& run = layout.runs[r];
        inside += run.room == 1;
        for (std::size_t s = 0; s < run.segments.size(); ++s) {
            ++segments;
            const auto& seg = run.segments[s];
            const auto& link = run.level == 0 ? seg.up : seg.down;
            CHECK((run.level == 0 ? seg.down : seg.up).run < 0);
            if (link.run >= 0) {
                ++linked;
                const auto& other = layout.keys[static_cast<std::size_t>(link.run)][static_cast<std::size_t>(link.segment)];
                auto same = layout.keys[r][s];
                same.level = other.level;
                CHECK(other == same && other.level != run.level);
            }
        }
    }
    CHECK(inside == 8 && segments == 24 && linked == 24);
    // The face along y = 0 looking into the room names tile (0, 0) from its -y side, and is walked
    // so that it looks to its right.
    bool found = false;
    for (std::size_t r = 0; r < layout.runs.size(); ++r) {
        for (std::size_t s = 0; s < layout.keys[r].size(); ++s) {
            if (layout.keys[r][s] == wallruns::Key{0, 0, 0, 1, 0}) {
                found = true;
                const auto& seg = layout.runs[r].segments[s];
                CHECK(seg.y0 == 0 && seg.y1 == 0 && seg.x1 < seg.x0 && layout.runs[r].room == 1);
            }
        }
    }
    CHECK(found);
    // Solid floor between the levels: no links.
    layout = wallruns::build(walls, roomAt, [](int, const wallruns::Key&) { return true; });
    for (const auto& run : layout.runs) {
        for (const auto& seg : run.segments) {
            CHECK(seg.up.run < 0 && seg.down.run < 0);
        }
    }

    // A lone diagonal wall: its two faces, one run each, naming the halves of the tile.
    layout = wallruns::build({{0, 0, 0, 1, 1}}, roomAt, nullptr);
    CHECK(layout.runs.size() == 2 && layout.runs[0].diagonal && layout.runs[1].diagonal);
    std::set<int> parts;
    for (const auto& keys : layout.keys) {
        CHECK(keys.size() == 1 && keys[0].side == 0x20);
        parts.insert(keys[0].part);
        CHECK(wallruns::quarters(keys[0]) == (keys[0].part == 1 ? 0xC : 3));
    }
    CHECK(parts == std::set<int>({1, 3}));

    // Bad walls are left out, and too many stop at the limit.
    layout = wallruns::build({{0, -1, 0, 1, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 2, 1}, {0, 0, 0, 100000, 0}, {1000, 0, 0, 1, 0}}, roomAt, nullptr);
    CHECK(layout.runs.empty());
    std::vector<wallruns::Wall> many;
    for (int y = 0; y < 200; ++y) {
        many.push_back({0, 0, y, 20, y});
    }
    layout = wallruns::build(many, roomAt, nullptr);
    std::size_t faces = 0;
    for (const auto& run : layout.runs) {
        faces += run.segments.size();
    }
    CHECK(!layout.complete && faces <= 4096);

    // The wall light a portal reads: its face's texel in the second half, fourth row, over 255.
    // Outdoors the outside ambient is taken off. Where no face is, it is black.
    layout = wallruns::build({{0, 0, 0, 1, 0}}, [](int, float, float y) { return static_cast<std::int16_t>(y > 0 ? 4 : 0); }, nullptr);
    CHECK(layout.runs.size() == 2);
    for (auto& run : layout.runs) {
        run.segments[0].halves[1][3] = {10, 20, 64, 255};
    }
    const std::size_t roomFace = layout.runs[0].room == 4 ? 0 : 1;
    const auto faceIn = layout.keys[roomFace][0], faceOut = layout.keys[1 - roomFace][0];
    const std::array<float, 3> white{1, 1, 1};
    auto in = wallruns::portalSample(layout.runs, layout, faceIn, true, white, 0.25f);
    CHECK(in[0] == static_cast<float>(64 * static_cast<double>(std::bit_cast<float>(0x3B808081u))) && in[2] == static_cast<float>(10 * static_cast<double>(std::bit_cast<float>(0x3B808081u))));
    auto out = wallruns::portalSample(layout.runs, layout, faceOut, true, white, 0.25f);
    CHECK(std::abs(out[0] - (64.0f / 255 - 0.25f)) < 1e-6f);
    CHECK(wallruns::portalSample(layout.runs, layout, faceOut, false, white, 0.25f) == in);
    auto missing = faceIn;
    missing.x = 40;
    CHECK((wallruns::portalSample(layout.runs, layout, missing, true, white, 0.25f) == std::array<float, 3>{0, 0, 0}));
}

void wallTexelsAndCornersAreFinishedAsTheGame() {
    using namespace ts2::scene;
    using walllight::Pixel;
    // A room two tiles by one on two levels: each loop's runs follow each other round it.
    std::vector<wallruns::Wall> walls;
    for (int level = 0; level < 2; ++level) {
        walls.push_back({level, 0, 0, 2, 0});
        walls.push_back({level, 2, 0, 2, 1});
        walls.push_back({level, 2, 1, 0, 1});
        walls.push_back({level, 0, 1, 0, 0});
    }
    const auto roomAt = [](int, float x, float y) { return static_cast<std::int16_t>(x > 0 && x < 2 && y > 0 && y < 1 ? 1 : 0); };
    auto layout = wallruns::build(walls, roomAt, [](int, const wallruns::Key&) { return false; });
    bool cycles = true;
    for (std::size_t r = 0; r < layout.runs.size(); ++r) {
        const auto& run = layout.runs[r];
        const auto& s = run.segments.front();
        cycles = cycles && run.next >= 0 && layout.runs[static_cast<std::size_t>(run.next)].previous == static_cast<int>(r) &&
                 run.angle == static_cast<float>(std::atan2(static_cast<double>(s.y1) - s.y0, static_cast<double>(s.x1) - s.x0));
    }
    CHECK(cycles);
    // Inside each room the loop turns right four times. The game misses the turn from -pi/2 to pi
    // (its angle test does not wrap), so three corners per level, none outside.
    std::vector<std::vector<lightcorners::Tile>> tiles;
    for (const auto& keys : layout.keys) {
        tiles.emplace_back();
        for (const auto& k : keys) tiles.back().push_back({k.x, k.y, k.level});
    }
    walllight::Walls lit;
    lit.runs = layout.runs;
    CHECK(lightcorners::corners(lit, tiles).size() == 6);
    for (auto& run : lit.runs) run.lamplit = true;
    CHECK(lightcorners::corners(lit, tiles).empty());  // both runs lamp-lit: no corner

    // Texels: each segment's halves with its neighbours' edges, the level above's bottom row on top.
    for (std::size_t r = 0; r < lit.runs.size(); ++r) {
        for (std::size_t s = 0; s < lit.runs[r].segments.size(); ++s) {
            for (std::size_t h = 0; h < 2; ++h) {
                for (std::size_t row = 0; row < 6; ++row) {
                    lit.runs[r].segments[s].halves[h][row] = {static_cast<std::uint8_t>(row), static_cast<std::uint8_t>(h), static_cast<std::uint8_t>(s),
                                                              static_cast<std::uint8_t>(r)};
                }
            }
        }
    }
    walllight::pad(lit);
    bool padded = true;
    int seen = 0;
    for (const auto& run : lit.runs) {
        if (run.level != 0 || run.segments.size() != 2 || run.segments[0].up.run < 0) continue;
        ++seen;
        const auto& first = run.segments[0];
        const auto& second = run.segments[1];
        const auto& aboveRun = lit.runs[static_cast<std::size_t>(first.up.run)];
        const auto at = static_cast<std::size_t>(first.up.segment);
        const auto& above = aboveRun.segments[at];
        const auto& corner = at > 0 ? aboveRun.segments[at - 1].halves[1][5] : above.halves[0][5];
        padded = padded && first.texels[1][3] == first.halves[0][2] && first.texels[3][3] == second.halves[0][2] &&
                 first.texels[0][3] == first.halves[0][2] &&            // no previous segment: its own edge
                 first.texels[1][0] == above.halves[0][5] &&            // the level above's bottom row
                 first.texels[1][7] == first.halves[0][5] &&            // nothing below: its own bottom row
                 first.texels[0][0] == corner;
    }
    CHECK(seen > 0 && padded);

    // A lamp-lit run turning a right angle into an unlit one: its last columns are pulled toward
    // the ambient (texel values from the game's walls on the Caliente lot).
    walllight::Walls corner;
    corner.insideAmbient = {0.00085f, 0.00075f, 0.00075f};
    corner.runs.resize(2);
    corner.runs[0].room = corner.runs[1].room = 4;
    corner.runs[0].lamplit = true;
    corner.runs[0].angle = std::bit_cast<float>(0x40490FDBu);  // pi
    corner.runs[1].angle = std::bit_cast<float>(0x3FC90FDBu);  // pi / 2
    corner.runs[0].next = 1;
    corner.runs[1].previous = 0;
    corner.runs[0].segments.resize(2);
    corner.runs[1].segments.resize(1);
    for (auto& s : corner.runs[0].segments) {
        for (auto& column : s.texels) column.fill({4, 4, 5, 255});
    }
    corner.runs[0].segments[1].texels[3].fill({3, 3, 4, 255});
    const auto untouched = corner.runs[1].segments[0].texels;
    lightcorners::blendWalls(corner);
    const auto& end = corner.runs[0].segments[1].texels;
    const auto& start = corner.runs[0].segments[0].texels;
    CHECK((end[3][3] == Pixel{1, 1, 1, 255}));  // 80%
    CHECK((end[1][3] == Pixel{3, 3, 4, 255}));  // 40%
    CHECK((start[3][3] == Pixel{3, 3, 4, 255}) && (start[2][3] == Pixel{4, 4, 5, 255}) && (end[0][3] == Pixel{4, 4, 5, 255}));
    CHECK(corner.runs[1].segments[0].texels == untouched);

    // Floor cells inside a corner are pulled toward the ambient, the nearest most. The game skips
    // the nearest seven cells without floor but blends the farther ones regardless.
    floorlight::Grid grid;
    grid.width = grid.height = 2;
    grid.cells.resize(16);
    grid.tiles.resize(4);
    for (auto& c : grid.cells) {
        for (auto& h : c.halves) {
            h.valid = true;
            h.light = {1, 1, 1};
        }
    }
    grid.at(3, 3)->halves[0].valid = false;  // nearest cell, no floor: left alone
    grid.at(0, 2)->halves[0].valid = false;  // eighth cell, no floor: blended anyway
    lightcorners::blendFloor(grid, {{1, 1, 0, 1, false}});
    const float w1 = std::bit_cast<float>(0x3F3DBDBEu), w7 = std::bit_cast<float>(0x3DA0A0A1u);
    CHECK(grid.at(3, 3)->halves[0].light[0] == 1);
    CHECK(grid.at(2, 3)->halves[0].light[0] == static_cast<float>(1.0 - w1));
    CHECK(grid.at(0, 2)->halves[0].light[0] == static_cast<float>(1.0 - w7));
    CHECK(grid.at(0, 0)->halves[0].light[0] == 1 && grid.at(2, 3)->halves[1].light[0] == 1);
    // A short run: two by two cells, both halves. Other levels and even directions are left alone.
    lightcorners::blendFloor(grid, {{1, 1, 0, 1, true}, {1, 1, 1, 1, false}, {1, 1, 0, 2, false}});
    CHECK(grid.at(2, 3)->halves[1].light[0] == static_cast<float>(1.0 - std::bit_cast<float>(0x3E909091u)));
    // Far-off or broken corners do nothing.
    const auto before = grid.cells;
    lightcorners::blendFloor(grid, {{std::numeric_limits<int>::max(), 0, 0, 1, false}, {-5, -5, 0, 3, false}});
    bool same = true;
    for (std::size_t i = 0; i < grid.cells.size(); ++i) same = same && grid.cells[i].halves[0].light == before[i].halves[0].light;
    CHECK(same);
}

void lampsGiveTheGamesRecords() {
    using namespace ts2::scene;
    // A ceiling lamp of macro Six in the day state: 1.5 x .7 and falloff 1 x .5, the game's values.
    ts2::content::Lighting::LightAttributes a;
    a.diffusionIntensity = 1.5f;
    a.diffusionColor = std::array<float, 3>{1, 0.86f, 0.75f};
    a.diffusionFloorScale = 0.65f;
    ts2::content::Lighting::Modifier inside{0.8f, 0.7f, 0.35f, 0.5f};
    const auto settings = lamplights::scaled(lamplights::settingsFor(&a), inside, 1);
    CHECK(settings.intensity == 1.05f && settings.falloff == 0.5f && settings.interFloorFalloff == 0.5f);
    CHECK(lamplights::settingsFor(nullptr).intensity == 0);

    lamplights::LotQueries lot;
    lot.highestLevel = 1;
    lot.floorHeight = [](int level, float, float) { return 3.0f * static_cast<float>(level); };
    lot.tileOnLot = [](int x, int y, int level) { return x >= 0 && y >= 0 && x < 20 && y < 20 && level >= 0 && level <= 1; };
    float roomX = -1, roomY = -1;
    lot.roomAt = [&](float x, float y, int level) {
        roomX = x;
        roomY = y;
        return static_cast<std::int16_t>(level + 2);
    };
    // Hanging from the ceiling of level 1, facing +x: its -z points down, so its +y gives the facing.
    lamplights::Placement p;
    p.position = {8.5f, 8.5f, 5.7f};
    p.rotation = {0, 0, -0.70710678f, 0.70710678f};
    const auto s = lamplights::sourceFor(settings, p, lot);
    CHECK(s.has_value());
    if (s) {
        CHECK(s->level == 1 && s->room == 3);
        CHECK(std::abs(s->angle) < 1e-6f);
        CHECK(s->x0 == 8 && s->y0 == 8 && s->x1 == 9 && s->y1 == 9);
        CHECK(std::abs(s->color[0] - 1.05f) < 1e-6f && s->floorIntensity == 0.65f && !s->directional);
        CHECK(roomX == 8.5f && roomY == 8.5f);
    }
    // A window-style light (its area wholly ahead) takes its room in front of it.
    auto ahead = settings;
    ahead.min = {0, 0.2f};
    ahead.max = {0, 2};
    const auto w = lamplights::sourceFor(ahead, p, lot);
    CHECK(w.has_value() && std::abs(roomX - 8.7f) < 1e-5f && std::abs(roomY - 8.5f) < 1e-5f);
    if (w) {
        CHECK(std::abs(w->x0 - 8.7f) < 1e-5f && std::abs(w->x1 - 10.5f) < 1e-5f && w->y0 == 8.5f && w->y1 == 8.5f);
    }
    // No colour, off the lot or nonsense positions: no record.
    auto dark = settings;
    dark.intensity = 0;
    CHECK(!lamplights::sourceFor(dark, p, lot));
    auto off = p;
    off.position = {-3, 4, 1};
    CHECK(!lamplights::sourceFor(settings, off, lot));
    off.position = {std::numeric_limits<float>::quiet_NaN(), 4, 1};
    CHECK(!lamplights::sourceFor(settings, off, lot));
    off.position = {1e30f, 4, 1};
    CHECK(!lamplights::sourceFor(settings, off, lot));

    // The game's turn for each saved facing, to the bit (its sign form included).
    const std::uint32_t turns[8][2] = {{0x3F3504F3, 0x3F3504F3}, {0x3EC3EF16, 0x3F6C835E}, {0, 0x3F800000}, {0x3EC3EF13, 0xBF6C835F},
                                       {0x3F3504F2, 0xBF3504F4}, {0x3F6C835E, 0xBEC3EF18}, {0x3F800000, 0xB33BBD2E}, {0x3F6C835E, 0x3EC3EF15}};
    for (int d = 0; d < 8; ++d) {
        const auto q = lamplights::facingRotation(d);
        CHECK(std::bit_cast<std::uint32_t>(q[2]) == turns[d][0] && std::bit_cast<std::uint32_t>(q[3]) == turns[d][1]);
        CHECK(q[0] == 0 && q[1] == 0);
    }

    // A model's light nodes, placed through the nodes above them, then where the object stands.
    ts2::scenegraph::ResourceNode model;
    model.nodes.resize(3);
    model.nodes[0].kind = ts2::scenegraph::SceneNode::Kind::Root;
    model.nodes[0].children = {1};
    model.nodes[1].kind = ts2::scenegraph::SceneNode::Kind::Transform;
    model.nodes[1].translation = {0, 0, 2};
    model.nodes[1].children = {2, 1, 7};  // itself again and a missing node are ignored
    model.nodes[2].kind = ts2::scenegraph::SceneNode::Kind::LightRef;
    model.nodes[2].name = "lamp_light";
    model.nodes[2].translation = {0, 0.5f, 0};
    const auto lights = lamplights::lightNodes(model, {{3, 4, 0}, lamplights::facingRotation(0)});  // turned a quarter
    CHECK(lights.size() == 1 && lights[0].name == "lamp_light");
    if (!lights.empty()) {
        const auto& at = lights[0].place;
        CHECK(std::abs(at.position[0] - 2.5f) < 1e-5f && std::abs(at.position[1] - 4) < 1e-5f && std::abs(at.position[2] - 2) < 1e-6f);
    }
}

void portalsAreBuiltAsTheGame() {
    using namespace ts2::scene;
    // A wall along x = 2 on the ground floor, with room 1 west of it and room 2 east.
    portalrecords::LotQueries lot;
    lot.lamps.highestLevel = 1;
    lot.lamps.floorHeight = [](int level, float, float) { return 3.0f * static_cast<float>(level); };
    lot.lamps.tileOnLot = [](int x, int y, int level) { return x >= 0 && y >= 0 && x < 8 && y < 8 && level >= 0 && level <= 1; };
    lot.lamps.roomAt = [](float x, float, int) { return static_cast<std::int16_t>(x < 2 ? 1 : 2); };
    lot.inLot = [](float x, float y) { return x >= 0 && y >= 0 && x < 8 && y < 8; };
    lot.wallAt = [](const portalrecords::Edge& e) { return e.x0 == 2 && e.x1 == 2 && e.level == 0; };
    lot.roomInFront = [](const portalrecords::Edge& e) { return static_cast<std::int16_t>(e.y1 > e.y0 ? 2 : 1); };  // normal (dy, -dx)
    lot.roomLit = [](std::int16_t) { return true; };

    // A window light just west of the wall shining east (its -z turned to +x), and one further
    // along the wall, three tiles on.
    portalrecords::PortalObject window;
    window.edge = {2, 0, 0, 2, 1};
    portalrecords::PortalLight light;
    light.place.position = {1.9f, 0.5f, 1.5f};
    light.place.rotation = {0, -0.70710678f, 0, 0.70710678f};
    light.hasLight = true;
    light.intensity = 1.5f;
    light.color = {1, 0.99362f, 0.772f};
    light.falloffRate = 1;
    light.settings.intensity = 1.25f;
    light.settings.min = {0, 0.2f};
    light.settings.max = {0, 2};
    window.lights.push_back(light);
    light.place.position[1] = 3.5f;
    window.lights.push_back(light);
    const auto built = portalrecords::build(window, lot, 0.1f);
    CHECK(built.size() == 2);
    if (built.size() == 2) {
        const auto& b = built[0];
        CHECK(b.room == 1 && b.portal.otherRoom == 2);
        CHECK(b.portal.nx == 1 && b.portal.ny == 0);
        CHECK(b.portal.x0 == 2 && b.portal.x1 == 2 && b.portal.y1 - b.portal.y0 == -1);  // walked with its normal into room 1
        CHECK(b.portal.height == 0 && b.portal.size == 1.5f && b.portal.tint[2] == 0.772f && b.portal.reach == 1);
        CHECK(std::abs(b.spillPoint[0] - 2.1f) < 1e-6f && b.spillPoint[1] == 0.5f && b.spillPoint[2] == 1.5f);
        CHECK(std::abs(b.portal.source.color[0] - 1.25f) < 1e-6f && std::abs(b.portal.source.x0 - 2.1f) < 1e-5f);
        // The second light is moved onto its own tile of the wall.
        CHECK(std::min(built[1].portal.y0, built[1].portal.y1) == 3 && built[1].spillPoint[1] == 3.5f);
    }

    // No light resource: no size, a white tint. A light facing along the wall: no portal.
    window.lights.resize(1);
    window.lights[0].hasLight = false;
    auto plain = portalrecords::build(window, lot, 0.1f);
    CHECK(plain.size() == 1 && plain[0].portal.size == 0 && plain[0].portal.tint[2] == 1);
    window.lights[0].place.rotation = {0.70710678f, 0, 0, 0.70710678f};  // -z turned to +y
    CHECK(portalrecords::build(window, lot, 0.1f).empty());

    // Rooms not lit to 100, no wall at the light's tile, or broken positions: nothing.
    window.lights[0].place.rotation = {0, -0.70710678f, 0, 0.70710678f};
    lot.roomLit = [](std::int16_t room) { return room != 2; };
    CHECK(portalrecords::build(window, lot, 0.1f).empty());
    lot.roomLit = nullptr;
    window.lights[0].place.position = {1.9f, 0.5f, -1};
    CHECK(portalrecords::build(window, lot, 0.1f).empty());
    window.lights[0].place.position = {std::numeric_limits<float>::quiet_NaN(), 0.5f, 1.5f};
    CHECK(portalrecords::build(window, lot, 0.1f).empty());
    window.lights[0].place.position = {1.9f, 1e30f, 1.5f};
    CHECK(portalrecords::build(window, lot, 0.1f).empty());
    lot.wallAt = [](const portalrecords::Edge&) { return false; };
    window.lights[0].place.position = {1.9f, 0.5f, 1.5f};
    CHECK(portalrecords::build(window, lot, 0.1f).empty());
}

void lotLightingIsWorkedOutAsTheGame() {
    using namespace ts2::scene;
    // Settings: inside ambients, colour and thresholds from the state, outside ones always from the
    // global settings. Room settings by group, else the plain name. The last unlit room line wins.
    const auto lighting = ts2::content::readLightingText(
        "set outsideFloorAmbient 0.25\n"
        "set outsideDiffusionAmbientColor (1, 0.5, 1)\n"
        "set insideFloorAmbient 0.5\n"
        "set roomOverallStrength 0.4\n"
        "set object:roomOverallStrength 0.5\n"
        "set portalSuppressionRadius 3\n"
        "lightingState day\n"
        "  skylight (1,1,1) 0.5\n"
        "  light sun (1.5, 1.2, 2) (1, .94, .875) 2\n"
        "  diffParam insideFloorAmbient 0.001\n"
        "  diffParam outsideFloorAmbient 0.9\n"
        "  diffParam insideAmbientColor (.85, .75, .75)\n"
        "  diffParam floorThreshold .3\n"
        "  unlitRoomColor (.3, .3, .3) -threshold .3\n"
        "  unlitRoomColor (.45, .425, .4) -threshold .4\n"
        "  modifiers insidePortals 0.5 .7 1.3 1\n"
        "end\n");
    CHECK(!lotlighting::settingsFor(lighting, "night"));
    const auto s = lotlighting::settingsFor(lighting, "day");
    CHECK(s.has_value());
    if (!s) return;
    CHECK(s->insideFloorAmbient == 0.001f && s->outsideFloorAmbient == 0.25f && s->outsideColor[1] == 0.5f);
    CHECK(s->insideColor[0] == 0.85f && s->floorThreshold == 0.3f && s->wallThreshold == 0.4f);
    CHECK(s->sun && s->day.sky[0] == 0.5f && std::abs(s->day.sunDirection[2] - 2 / std::sqrt(1.5f * 1.5f + 1.2f * 1.2f + 4)) < 1e-6f);
    CHECK(s->day.insidePortals.practical == 0.5f && s->day.insidePortals.diffusion == 0.7f && s->day.insidePortals.practicalFalloff == 1.3f);
    CHECK(s->groups[lotlighting::kObjects].overall == 0.5f && s->groups[lotlighting::kSims].overall == 0.4f);
    CHECK(s->unlit.color[0] == 0.45f && s->unlit.threshold == 0.4f && s->suppression.radius == 3);

    // A lot two tiles wide, floored, with a lamp over the first tile in room 1. Room 2 has no tiles.
    ts2::content::Lot lot;
    ts2::array3d::Array3D floors;
    floors.width = 2;
    floors.height = 1;
    floors.levels = 1;
    floors.elementSize = 8;
    floors.elements.assign(2 * 8, 0);
    for (std::size_t q = 0; q < 8; ++q) floors.elements[q * 2] = 1;
    lot.grids[ts2::content::kFloorGridInstance] = floors;
    lot.floorPatterns[1] = {"abc", 1};
    lot.roomGraph = ts2::wallgraph::WallGraph{};
    lotlighting::Inputs in;
    in.lot = &lot;
    LightSource lamp;
    lamp.color = {1, 1, 1};
    lamp.room = 1;
    lamp.x0 = 0;
    lamp.y0 = 0;
    lamp.x1 = 1;
    lamp.y1 = 1;
    lamp.floorIntensity = 0.65f;
    in.lamps = {lamp};
    in.roomAt = [](int, float, float) { return std::int16_t{1}; };
    in.quarterRoom = [](int, int, int, int) { return std::int16_t{1}; };
    in.covering = [](bool, const std::string&) { return std::optional<roomsamples::Color>(roomsamples::Color{1, 1, 1}); };
    in.roomCount = 3;
    const auto r = lotlighting::compute(in, *s);
    CHECK(r.complete && r.floors.size() == 1 && r.rooms.size() == 2);
    if (r.floors.size() == 1) {
        const auto& g = r.floors[0];
        CHECK(g.at(0, 0) && g.at(3, 0) && g.at(0, 0)->halves[0].light[0] > g.at(3, 0)->halves[0].light[0]);  // brighter under the lamp
        CHECK(g.at(3, 0) && g.at(3, 0)->halves[0].light[0] > s->insideFloorAmbient * s->insideColor[0]);    // and it spread
    }
    if (r.rooms.count(1) && r.rooms.count(2)) {
        CHECK(r.rooms.at(1).groups[lotlighting::kObjects].has_value());
        // Nothing reaches room 2: the whole unlit room light on its sides and floor, none from above.
        const auto& two = r.rooms.at(2).samples;
        CHECK(two[roomlight::kPlusX] == s->unlit.color && two[roomlight::kFromFloor] == s->unlit.color);
        CHECK((two[roomlight::kFromCeiling] == std::array<float, 3>{0, 0, 0}));
    }
    // Filling a scene's light maps: the floor image gets the texels (the lamp's tile brighter), a
    // map of the wrong size and a missing wall image are left alone.
    Scene scene;
    scene.images.resize(2);
    for (auto& image : scene.images) {
        image.width = 16;
        image.height = 8;
        image.rgba.assign(16 * 8 * 4, 128);
    }
    scene.images[1].width = 8;
    scene.lightMaps.emplace();
    scene.lightMaps->floors = {0, 1};
    lotlightmaps::fill(scene, r, *s);
    const auto& floorMap = scene.images[0].rgba;
    CHECK(floorMap[(3 * 16 + 3) * 4] > floorMap[(3 * 16 + 12) * 4] && floorMap[3] == 255 && scene.images[1].rgba[0] == 128);
    lotlightmaps::fill(scene, lotlighting::Result{}, *s);  // nothing worked out: nothing changes
    CHECK(scene.images[0].rgba == floorMap);

    // No lot, or no floors: nothing.
    CHECK(lotlighting::compute(lotlighting::Inputs{}, *s).floors.empty());
    ts2::content::Lot bare;
    in.lot = &bare;
    CHECK(lotlighting::compute(in, *s).rooms.empty());
}

void lotLightingStateFollowsTheClock() {
    using ts2::content::lotLightingState;
    // As shipped: morning and evening off, so 6 am is night and 6 pm day.
    const auto shipped = ts2::content::readLightingText(
        "setb morningEvening false\n"
        "lightingState day\nend\nlightingState night\nend\nlightingState nightNoPortals : night\nend\n"
        "lightingState morning\nend\nlightingState evening\nend\nlightingState Winter\nend\n"
        "lightingState overcast\nend\nlightingState vacationNight\nend\n");
    CHECK(lotLightingState(shipped, 0) == "day");
    CHECK(lotLightingState(shipped, 1) == "day");
    CHECK(lotLightingState(shipped, 2) == "night");
    CHECK(lotLightingState(shipped, 3) == "night");
    // Medium lighting takes the NoPortals state where there is one.
    ts2::content::LightingConditions medium;
    medium.portalLighting = false;
    CHECK(lotLightingState(shipped, 2, medium) == "nightNoPortals");
    CHECK(lotLightingState(shipped, 0, medium) == "day");
    // Seasons: the season or overcast by day (any case), "day" for a season with no state. Nights
    // don't change.
    ts2::content::LightingConditions winter;
    winter.season = 3;
    CHECK(lotLightingState(shipped, 0, winter) == "Winter");
    CHECK(lotLightingState(shipped, 2, winter) == "night");
    winter.overcast = true;
    CHECK(lotLightingState(shipped, 0, winter) == "overcast");
    ts2::content::LightingConditions spring;
    spring.season = 0;
    CHECK(lotLightingState(shipped, 0, spring) == "day");
    ts2::content::LightingConditions away;
    away.vacation = true;
    CHECK(lotLightingState(shipped, 2, away) == "vacationNight");
    CHECK(lotLightingState(shipped, 0, away) == "day");
    // Morning and evening are on when no file sets them.
    const auto both = ts2::content::readLightingText("lightingState day\nend\nlightingState morning\nend\nlightingState evening\nend\n");
    CHECK(lotLightingState(both, 3) == "morning");
    CHECK(lotLightingState(both, 1) == "evening");
    CHECK(lotLightingState(both, 2).empty());
    CHECK(lotLightingState(ts2::content::readLightingText(""), 0).empty());
    // The sky follows the state's part of the day.
    using ts2::content::StateTime;
    CHECK(ts2::content::stateTime("night") == StateTime::Night && ts2::content::stateTime("nightNoPortals") == StateTime::Night);
    CHECK(ts2::content::stateTime("vacationNight") == StateTime::Night && ts2::content::stateTime("NeighborhoodNight") == StateTime::Night);
    CHECK(ts2::content::stateTime("Winter") == StateTime::Day && ts2::content::stateTime("NoPortals") == StateTime::Day);
    CHECK(ts2::content::stateTime("evening") == StateTime::Evening);
    CHECK(ts2::content::lotSkybox(StateTime::Night) == "skymap-generic-night-envcube");
    CHECK(ts2::content::lotSkybox(StateTime::Day) == "skymap-generic-day-highres-envcube");
    // The environment colour lot imposters take. The game draws them at night with (0.35, 0.35, 0.7).
    const auto night = ts2::content::readLightingText("lightingState night\n   environment (0.5, 0.5, 1) 0.7\nend\nlightingState day\nend\n");
    const auto& env = night.states.at("night").environment;
    CHECK(std::abs(env[0] - 0.35f) < 1e-6f && std::abs(env[1] - 0.35f) < 1e-6f && std::abs(env[2] - 0.7f) < 1e-6f);
    CHECK((night.states.at("day").environment == std::array<float, 3>{1, 1, 1}));
    // The lot skirt's ground and roads take the material's night colour scales, and back.
    ts2::scene::Scene scene;
    scene.instances.resize(3);
    scene.instances[0].part = ts2::scene::Part::Skirt;
    scene.instances[1].part = ts2::scene::Part::Skirt;
    scene.instances[1].skirtRoad = true;
    scene.instances[2].part = ts2::scene::Part::Object;
    CHECK((ts2::scene::lightLotSkirt(scene, true).empty()));
    CHECK((ts2::scene::lightLotSkirt(scene, false) == std::vector<std::size_t>{0, 1}));
    CHECK((scene.instances[0].skirtScale == std::array<float, 3>{0.96f, 0.96f, 0.96f}));
    CHECK((scene.instances[1].skirtScale == std::array<float, 3>{0.83f, 0.83f, 0.94f}));
    CHECK((scene.instances[2].skirtScale == std::array<float, 3>{1, 1, 1}));
    CHECK((ts2::scene::lightLotSkirt(scene, true).size() == 2 && scene.instances[1].skirtScale[2] == 1));
}

void lotSunShadowIsCastAsTheGame() {
    using namespace ts2::scene;
    // A flat lot four tiles square with one wall along x = 2 (a storey high, to the floor above).
    lotshadow::Inputs in;
    in.width = in.height = 4;
    in.levels = 2;
    in.corner = [](int, int, int level) { return 3.0f * static_cast<float>(level); };
    in.floorAt = [](int, int, int, int) { return false; };
    in.walls = {{0, 2, 0, 2, 4}};
    const float l = std::sqrt(1.5f * 1.5f + 1.2f * 1.2f + 4.0f);
    const std::array<float, 3> sun{1.5f / l, 1.2f / l, 2 / l};
    const auto g = lotshadow::build(in, sun);
    CHECK(g.width == 32 && g.height == 32);
    // The wall's top goes into the column on its side away from the sun (x < 2) ...
    CHECK(g.heights[static_cast<std::size_t>(12 * 32 + 15)] == 3.0f && g.heights[static_cast<std::size_t>(12 * 32 + 16)] == 0.0f);
    // ... and its shadow falls that way, sinking a sixth of a unit a sample (0.125 x 2 / 1.5).
    CHECK(std::abs(g.shadowAt(1.5f, 1.6f) - (3.0f - 0.16667f * 3)) < 0.01f);
    CHECK(g.shadowAt(2.6f, 1.6f) <= 0.0f && g.shadowAt(-1, 0) == -1000.0f);  // the sunny side: no higher than the ground
    // Under the shadow the sun fades over a unit. Above it, all of it.
    CHECK(lotshadow::sunVisible(-1, 1) == 1 && lotshadow::sunVisible(0.25f, 1) == 0.75f && lotshadow::sunVisible(2, 1) == 0);
    // The topmost floor over a quarter is its height, and the ground stays below.
    in.walls.clear();
    in.floorAt = [](int x, int y, int level, int) { return level == 1 && x == 1 && y == 1; };
    const auto upper = lotshadow::build(in, sun);
    CHECK(upper.heights[static_cast<std::size_t>(12 * 32 + 12)] == 3.0f && upper.groundAt(12, 12) == 0.0f && upper.heights[0] == 0.0f);
    // No sun from above, or nothing to shade: no shadow. A broken lot: nothing.
    CHECK(lotshadow::build(in, {0, 0, -1}).shadow[0] == -1000.0f);
    CHECK(lotshadow::build(lotshadow::Inputs{}, sun).width == 0);
    in.width = 100000;
    CHECK(lotshadow::build(in, sun).heights.empty());
    // Elevation between corners, and a storey per level beyond the lot's.
    in.width = 4;
    CHECK(lotshadow::elevation(in, 0.5f, 0.5f, 1) == 3.0f && lotshadow::elevation(in, 0.5f, 0.5f, 3) == 9.0f &&
          lotshadow::elevation(in, 0.5f, 0.5f, -1) == -3.0f);
}

void lightMapTexelsAreWrittenAsTheGame() {
    using namespace ts2::scene;
    using walllight::Pixel;
    // One tile: its left quarters lit 0.25, its right ones 0.75, no links to anything around it.
    auto tile = [](int level) {
        floorlight::Grid g;
        g.level = level;
        g.width = g.height = 1;
        g.cells.resize(4);
        g.tiles.resize(1);
        g.tiles[0].floor = true;
        for (int i = 0; i < 4; ++i) {
            auto& h = g.cells[static_cast<std::size_t>(i)].halves[0];
            h.valid = true;
            const float v = (i & 1) ? 0.75f : 0.25f;
            h.light = {v, v, v};
        }
        return g;
    };
    const Pixel fill{9, 9, 9, 255};
    // Ground level: texel centres at (i + 0.5) / 8, so the edges show each side's own light and the
    // middle blends. A texel's byte is 256 x the light (the shader doubles it again).
    const auto ground = lightmaptexels::floorTexels(tile(0), fill);
    CHECK(ground.width == 8 && ground.height == 8);
    CHECK(ground.at(0, 0) && ground.at(0, 0)->r == 64 && ground.at(7, 7)->r == 192 && ground.at(3, 4)->r == 112 && ground.at(4, 2)->r == 144);
    CHECK(ground.at(7, 0)->a == 255 && ground.at(5, 5)->g == ground.at(5, 5)->b);
    // Upper levels: texels from edge to edge (i / 7).
    const auto upper = lightmaptexels::floorTexels(tile(1), fill);
    CHECK(upper.at(0, 3)->r == 64 && upper.at(7, 3)->r == 192);
    // A tile without floor keeps the fill.
    auto bare = tile(0);
    bare.tiles[0].floor = false;
    CHECK((*lightmaptexels::floorTexels(bare, fill).at(4, 4) == fill));
    CHECK(lightmaptexels::floorTexels(floorlight::Grid{}, fill).texels.empty());

    // Rect sizes by face and height.
    CHECK(lightmaptexels::panelSize(true, 1).width == 4 && lightmaptexels::panelSize(false, 1).height == 8);
    CHECK(lightmaptexels::panelSize(true, 3).height == 24 && lightmaptexels::panelSize(false, 9).height == 32);
    // Walls: a flat 128 grid comes out at half (the game's 0.5 scale) in every rect shape.
    std::array<walllight::Column, 4> flat{};
    for (auto& c : flat) c.fill({128, 128, 128, 255});
    const auto normal = lightmaptexels::panelTexels(flat, {8, 24}, {0, 0, 0}, 0.5f, false, 0.137f);
    const auto small = lightmaptexels::panelTexels(flat, {4, 8}, {0, 0, 0}, 0.5f, false, 0.137f);
    CHECK(normal.size() == 8 * 24 && (normal.front() == Pixel{64, 64, 64, 255}) && (normal.back() == Pixel{64, 64, 64, 255}));
    CHECK(small.size() == 4 * 8 && (small[13] == Pixel{64, 64, 64, 255}));
    // A tall rect fades above the sampled rows (64 x 3883 / 4096). An attic's inside face clears them.
    const auto tall = lightmaptexels::panelTexels(flat, {8, 32}, {0, 0, 0}, 0.5f, false, 0.137f);
    const auto attic = lightmaptexels::panelTexels(flat, {8, 32}, {0, 0, 0}, 0.5f, true, 0.137f);
    CHECK(tall.size() == 8 * 32 && tall[24 * 8].r == 60 && tall[25 * 8].r == 56 && (attic[24 * 8] == Pixel{0, 0, 0, 0}) && attic[23 * 8].r == 64);
    // Faces looking outdoors have the outside ambient taken off first.
    const auto out = lightmaptexels::panelTexels(flat, {8, 24}, {0.25f, 0.25f, 0.25f}, 0.5f, false, 0.137f);
    CHECK(out[50].r == 32);
    // Shapes the game has no rule for give nothing written. Broken sizes give nothing at all.
    CHECK(lightmaptexels::panelTexels(flat, {8, 20}, {0, 0, 0}, 0.5f, false, 0.137f)[0].a == 0);
    CHECK(lightmaptexels::panelTexels(flat, {0, 24}, {0, 0, 0}, 0.5f, false, 0.137f).empty());

    // The unlit room light added to a room's texels: half the colour times its share, as bytes
    // (the game's own add on the Caliente lot: room 7, share 0.63356, gave 36, 34, 32).
    const std::array<float, 3> unlit{0.45f, 0.425f, 0.4f};
    CHECK((lightmaptexels::roomAdd(unlit, 0.633562f) == Pixel{32, 34, 36, 0}));
    CHECK((lightmaptexels::roomAdd(unlit, 0) == Pixel{0, 0, 0, 0}) && lightmaptexels::roomAdd({4, 4, 4}, 1).r == 255);
    auto panel = normal;
    lightmaptexels::addRoomLight(panel, {200, 1, 2, 0});
    CHECK((panel[7] == Pixel{255, 65, 66, 255}));  // saturating, alpha kept
    // On floors each tile takes its room's add. Room 0 (outdoors) takes none.
    auto lit = tile(0);
    for (auto& c : lit.cells) c.halves[0].room = 3;
    auto image = lightmaptexels::floorTexels(lit, fill);
    lightmaptexels::addRoomLight(image, lit, [](std::int16_t room) { return room == 3 ? Pixel{10, 10, 10, 0} : Pixel{}; });
    CHECK(image.at(0, 0)->r == 74 && image.at(7, 7)->r == 202);
    for (auto& c : lit.cells) c.halves[0].room = 0;
    auto outdoors = lightmaptexels::floorTexels(lit, fill);
    lightmaptexels::addRoomLight(outdoors, lit, [](std::int16_t) { return Pixel{10, 10, 10, 0}; });
    CHECK(outdoors.at(0, 0)->r == 64);

    // Sunlight outdoors, with the day state's lights: the game's sunlit lot ground is 165, 159, 136.
    auto normalized = [](float x, float y, float z) {
        const float l = std::sqrt(x * x + y * y + z * z);
        return std::array<float, 3>{x / l, y / l, z / l};
    };
    lightmaptexels::Outdoor day;
    day.lights = {{{0.97f, 0.94f, 0.83f}, 2, normalized(1.5f, 1.2f, 2)}, {{0.8f, 0.8f, 0.73f}, 0.5f, normalized(-1.5f, -1.2f, 2)},
                  {{1, 0.8f, 0.9f}, 0.5f, normalized(1, 1, -1)}};
    day.skylight = {0.5f, 0.5f, 0.5f};
    CHECK((lightmaptexels::outdoorAdd(day, {0, 0, 1}) == Pixel{136, 159, 165, 0}));
    lightmaptexels::Outdoor none;
    CHECK((lightmaptexels::outdoorAdd(none, {0, 0, 1}) == Pixel{0, 0, 0, 0}));
    // Only tiles outdoors get it.
    auto yard = tile(0);
    for (auto& c : yard.cells) c.halves[0].outside = true;
    yard.cells[0].halves[0].light = {0.25f, 0.25f, 0.25f};
    auto ground2 = lightmaptexels::floorTexels(yard, fill);
    const auto before = *ground2.at(0, 0);
    lightmaptexels::addOutdoorLight(ground2, yard, day);
    CHECK(ground2.at(0, 0)->r == std::min(255, before.r + 165));
    auto indoors = lightmaptexels::floorTexels(tile(0), fill);
    lightmaptexels::addOutdoorLight(indoors, tile(0), day);
    CHECK(indoors.at(0, 0)->r == 64);
}

void indoorLightStaysSafeOnBadData() {
    using namespace ts2::scene;
    // A grid with an absurd size gives no cells instead of overflowing.
    floorlight::Grid huge;
    huge.width = 0x7FFFFFFF;
    huge.height = 1;
    CHECK(!huge.at(0, 0) && !huge.at(-1, 5));

    // Colours round the same whatever the processor's rounding mode (halves to even).
    const int mode = std::fegetround();
    std::fesetround(FE_UPWARD);
    const auto up = walllight::pack({2.5f / 256, 0, 0.5f / 256});
    std::fesetround(mode);
    CHECK(up.r == 2 && up.b == 0);

    // A negative falloff makes light grow as it spreads. Bright colours must not wrap to dark.
    auto w = testWall();
    w.falloffScale = -0.3f;
    LightSource lamp;
    lamp.color = {4, 4, 4};
    lamp.room = 1;
    lamp.x0 = lamp.x1 = 0.25f;
    lamp.y0 = lamp.y1 = -1;
    std::set<int> changed;
    walllight::resetToAmbient(w);
    CHECK(walllight::lightFromSources(w, {lamp}, nullptr, changed));
    CHECK(walllight::spread(w, changed));
    for (const auto& s : w.runs[0].segments) {
        for (const auto& half : s.halves) {
            for (const auto& p : half) {
                CHECK(p.r == 255);
            }
        }
    }

    // Walls stacked over 20 levels, each level two walls and every wall linking up to both
    // walls above: visiting every path would take about 2^20 visits a step. It must finish
    // quickly and still light the walls.
    walllight::Walls stack;
    for (int level = 0; level < 20; ++level) {
        for (int k = 0; k < 2; ++k) {
            walllight::Run run;
            run.level = level;
            run.room = 1;
            for (int i = 0; i < 2; ++i) {
                walllight::Segment s;
                s.x0 = static_cast<float>(2 * k + i);
                s.x1 = s.x0 + 1;
                if (level < 19) {
                    s.up = {2 * (level + 1) + i, 0};
                }
                if (level > 0) {
                    s.down = {2 * (level - 1) + i, 0};
                }
                run.segments.push_back(s);
            }
            stack.runs.push_back(run);
        }
    }
    stack.steps = 1000;
    CHECK(!walllight::light(stack, {lamp}, nullptr, nullptr));  // stopped at its limit, and says so
    CHECK(stack.runs[0].segments[0].halves[0][2].r > 0);
    auto plain = testWall();
    CHECK(walllight::light(plain, {lamp}, nullptr, nullptr));
    auto floor = testFloor(4, 4);
    CHECK(floorlight::light(floor, {lamp}, nullptr));
}

// The lights a normal-mapped surface lights per pixel, as the game's shader scripts pick them.
void perPixelLightsFollowTheShaderScripts() {
    using Light = ts2::scene::ObjectLighting::Light;
    auto set = [](std::initializer_list<char> kinds) {
        ts2::scene::ObjectLighting l;
        for (const char k : kinds) {
            Light light;
            light.point = k != 'd';
            light.spot = k == 's';
            l.lights.push_back(light);
        }
        return l;
    };
    using V = std::vector<std::size_t>;
    CHECK(ts2::scene::perPixelLights(set({}), 8).empty());
    CHECK((ts2::scene::perPixelLights(set({'d'}), 8) == V{0}));
    CHECK((ts2::scene::perPixelLights(set({'d', 'd', 'd', 'd', 'd'}), 8) == V{0, 1, 2}));
    CHECK((ts2::scene::perPixelLights(set({'d', 'd', 's', 'p'}), 8) == V{0, 1, 3}));  // points before spots
    CHECK((ts2::scene::perPixelLights(set({'d', 's'}), 8) == V{0, 1}));
    CHECK((ts2::scene::perPixelLights(set({'p', 'p'}), 8) == V{0}));
    CHECK((ts2::scene::perPixelLights(set({'d', 'd', 'd', 'p'}), 8) == V{0, 1, 2}));
    CHECK((ts2::scene::perPixelLights(set({'d', 'p'}), 1) == V{0}));  // only the lights drawn with count
}

// Incidence maps as the game makes them of its light maps: 2 x 2 blocks of a wall light-map
// page and the incidence texels the game made of them (Lighting.txt's scale 100, walls' 0.8).
void incidenceMapsFollowTheGame() {
    struct Case {
        std::array<std::array<std::uint8_t, 3>, 4> block;  // upper left, upper right, lower left, lower right
        std::uint8_t r, g, b;
    };
    const Case cases[] = {
        {{{{88, 79, 70}, {89, 79, 70}, {89, 80, 71}, {91, 81, 71}}}, 146, 164, 255},
        {{{{93, 83, 73}, {95, 85, 75}, {95, 84, 75}, {99, 88, 77}}}, 191, 178, 255},
        {{{{87, 76, 68}, {90, 79, 71}, {87, 77, 69}, {92, 81, 72}}}, 206, 156, 255},
    };
    for (const auto& c : cases) {
        ts2::scene::Image map{2, 2, {}};
        for (const auto& t : c.block) {
            map.rgba.insert(map.rgba.end(), {t[0], t[1], t[2], 255});
        }
        const auto incidence = ts2::scene::incidenceMap(map, 100, 0.8f);
        CHECK(incidence.width == 1 && incidence.height == 1);
        CHECK(incidence.rgba == (std::vector<std::uint8_t>{c.r, c.g, c.b, 255}));
    }
    // An even light map: straight out everywhere, half the size. Nothing without a whole block.
    ts2::scene::Image even{6, 4, std::vector<std::uint8_t>(6 * 4 * 4, 90)};
    const auto flat = ts2::scene::incidenceMap(even, 100, 1);
    CHECK(flat.width == 3 && flat.height == 2 && flat.rgba[0] == 128 && flat.rgba[1] == 128 && flat.rgba[2] == 255);
    CHECK(ts2::scene::incidenceMap(ts2::scene::Image{1, 4, std::vector<std::uint8_t>(16, 0)}, 100, 1).rgba.empty());
    CHECK(ts2::scene::incidenceMap(ts2::scene::Image{2, 2, std::vector<std::uint8_t>(3, 0)}, 100, 1).rgba.empty());
    CHECK(ts2::scene::incidenceMap(even, 0, 1).rgba.empty());
}

} // namespace

std::vector<Test> lightingTests() {
    return {
        {"Per-pixel lights follow the shader scripts", perPixelLightsFollowTheShaderScripts},
        {"Incidence maps follow the game", incidenceMapsFollowTheGame},
        {"Floor light spreads and fades as the game's", floorLightSpreadsAndFades},
        {"Floor light grid is built as the game builds it", lightGridIsBuiltAsTheGame},
        {"Wall runs are laid out as the game lays them out", wallRunsAreLaidOutAsTheGame},
        {"Wall texels and corners are finished as the game finishes them", wallTexelsAndCornersAreFinishedAsTheGame},
        {"Lamps give the game's floor and wall light records", lampsGiveTheGamesRecords},
        {"Portals are built as the game builds them", portalsAreBuiltAsTheGame},
        {"Lot lighting is worked out as the game works it out", lotLightingIsWorkedOutAsTheGame},
        {"Lot lighting state follows the clock as in the game", lotLightingStateFollowsTheClock},
        {"Light-map texels are written as the game writes them", lightMapTexelsAreWrittenAsTheGame},
        {"The lot's sun shadow is cast as the game casts it", lotSunShadowIsCastAsTheGame},
        {"Wall light fades along and up the walls as the game's", wallLightFadesAlongAndUp},
        {"Room light matches the game", roomLightMatchesTheGame},
        {"Window light matches the game", windowLightMatchesTheGame},
        {"Indoor light stays safe on bad data", indoorLightStaysSafeOnBadData},
        {"neighborhood light map looks up normals", neighborhoodLightMapLooksUpNormals},
        {"only wall shadows are hidden on lots", onlyWallShadowsAreHiddenOnLots},
        {"objects get the sun the buildings leave them", objectsGetTheSunTheBuildingsLeaveThem},
        {"objects in rooms get their room's lights", objectsInRoomsGetTheirRoomsLights},
        {"door and window parts in rooms get the floor light under them", doorAndWindowPartsInRoomsGetTheFloorLightUnderThem},
        {"objects get the window lights of their room", objectsGetTheWindowLightsOfTheirRoom},
    };
}
