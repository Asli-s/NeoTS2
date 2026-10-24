// Building and viewing scenes: lots, Sims, rigs, roofs, walls and views.
#include "test_support.hpp"

#include "../app/object_materials.hpp"
#include "scene/neighborhood_scene.hpp"
#include "scene/hood_drift.hpp"
#include "scene/scene_ray.hpp"

#include <cmath>
#include <filesystem>
#include <limits>
#include <random>

namespace {

// An attic wall's top is the underside of the roof it closes, below its own top or above it.
void atticWallsReachTheRoofsUnderside() {
    ts2::scene::RoofShape roof;
    roof.eave = 3;
    roof.slope = 1;
    roof.thickness = 0.5f;
    roof.planes.push_back({0, 0, 1, 0});  // rising along x from x = 0
    ts2::scene::WallCeiling ceiling;
    CHECK(ceiling.at(1, 0, 6) == 6);
    ceiling.gables.push_back(&roof);
    CHECK(ceiling.at(1, 0, 6) == 3.5f && ceiling.at(5, 0, 6) == 7.5f);
}

// Transforms: a quarter turn about z takes +x to +y, then the translation applies.
void sceneTransformsTurnAndMove() {
    using ts2::scene::Transform;
    float v[3] = {1, 0, 0};
    Transform::at(10, 20, 3, ts2::scene::kPi / 2).apply(v, true);
    CHECK(std::abs(v[0] - 10) < 1e-5f && std::abs(v[1] - 21) < 1e-5f && std::abs(v[2] - 3) < 1e-5f);
    float d[3] = {1, 0, 0};
    Transform::at(10, 20, 3, ts2::scene::kPi / 2).apply(d, false);
    CHECK(std::abs(d[0]) < 1e-5f && std::abs(d[1] - 1) < 1e-5f);
}

// A quad is wound to face the way it's asked to, whatever order its corners come in.
void sceneQuadsFaceTheirNormal() {
    ts2::scene::Primitive p;
    ts2::scene::addQuad(p, {{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}}, {0, 0, -1}, {});
    CHECK(p.indices.size() == 6 && p.positions.size() == 18);
    auto at = [&](std::size_t i, std::size_t k) { return p.positions[i * 3 + k]; };
    const float ux = at(1, 0) - at(0, 0), uy = at(1, 1) - at(0, 1), vx = at(2, 0) - at(0, 0), vy = at(2, 1) - at(0, 1);
    CHECK(ux * vy - uy * vx < 0);  // the first triangle's normal points down
}

// A hipped roof over a 2 x 4 rectangle: four sloped faces meeting at a ridge along the long side,
// eaves out by the overhang and the ridge as high as the slope allows.
void sceneHipRoofRisesToItsRidge() {
    ts2::roofs::Roof roof;
    roof.x0 = 0;
    roof.y0 = 0;
    roof.x1 = 2;
    roof.y1 = 4;
    roof.type = 5;
    ts2::scene::Surfaces surfaces;
    ts2::scene::RoofLook look;
    look.top = 0;
    ts2::scene::addRoof(surfaces, roof, 3.0f, 1.0f, look);
    const auto& top = surfaces[0];
    float lowest = 1e9f, highest = -1e9f, minX = 1e9f;
    for (std::size_t i = 0; i < top.positions.size(); i += 3) {
        minX = std::min(minX, top.positions[i]);
        lowest = std::min(lowest, top.positions[i + 2]);
        highest = std::max(highest, top.positions[i + 2]);
    }
    // Grown by half a tile each way: 3 wide, so the ridge is 1.5 above the eaves.
    CHECK(std::abs(minX + 0.5f) < 1e-4f && std::abs(lowest - 3.0f) < 1e-4f && std::abs(highest - 4.5f) < 1e-4f);
    CHECK(top.indices.size() % 3 == 0 && !top.indices.empty());
}

// Greenhouse roofs as the game places their pieces: lot 19 of G001 (14 tiles across a ridge along y,
// eaves at 6) and a roof one tile wide.
void sceneGreenhouseRoofPiecesMatchTheGame() {
    constexpr float kHalfPi = 1.5707963f;
    auto find = [](const std::vector<ts2::scene::GreenhousePiece>& pieces, float x, float y, float z) {
        for (const auto& p : pieces) {
            if (std::abs(p.x - x) < 1e-4f && std::abs(p.y - y) < 1e-4f && std::abs(p.z - z) < 1e-4f) {
                return &p;
            }
        }
        return static_cast<const ts2::scene::GreenhousePiece*>(nullptr);
    };
    ts2::roofs::Roof wide;
    wide.x0 = 8;
    wide.y0 = 22;
    wide.x1 = 22;
    wide.y1 = 6;
    wide.type = ts2::scene::kGreenhouseRoof;
    wide.extra = 25;
    const auto pieces = ts2::scene::greenhousePieces(wide, 6);
    const auto* near = find(pieces, 8.5f, 10.5f, 6);
    const auto* spine = find(pieces, 14.5f, 10.5f, 12);
    const auto* far = find(pieces, 15.5f, 10.5f, 12);
    const auto* farSide = find(pieces, 16.5f, 10.5f, 11);
    CHECK(near && near->model == "greenhouseRoofRepeatWhite" && std::abs(near->angle + kHalfPi) < 1e-4f);
    CHECK(spine && spine->model == "greenhouseRoofSpineEvenWhite" && std::abs(spine->angle + kHalfPi) < 1e-4f);
    CHECK(far && far->model == "greenhouseRoofSpineEvenWhite" && std::abs(far->angle - kHalfPi) < 1e-4f);
    CHECK(farSide && farSide->model == "greenhouseRoofRepeatWhite" && std::abs(farSide->angle - kHalfPi) < 1e-4f);
    // The near end's caps, one tile before the rectangle.
    const auto* capM = find(pieces, 8.5f, 5.5f, 6);
    const auto* spineCapM = find(pieces, 14.5f, 5.5f, 12);
    const auto* spineCap = find(pieces, 15.5f, 5.5f, 12);
    CHECK(capM && capM->model == "greenhouseRoofRepeatCapMWhite");
    CHECK(spineCapM && spineCapM->model == "greenhouseRoofSpineEvenCapMWhite");
    CHECK(spineCap && spineCap->model == "greenhouseRoofSpineEvenCapWhite");

    ts2::roofs::Roof narrow;
    narrow.x0 = 2;
    narrow.y0 = 20;
    narrow.x1 = 3;
    narrow.y1 = 22;
    narrow.type = ts2::scene::kGreenhouseRoof;
    narrow.extra = 25;
    const auto one = ts2::scene::greenhousePieces(narrow, 0);
    CHECK(one.size() == 4);
    const auto* body = find(one, 2.5f, 20.5f, 0);
    const auto* nearCap = find(one, 2.5f, 19.5f, 0);
    const auto* farCap = find(one, 2.5f, 22.5f, 0);
    CHECK(body && body->model == "greenhouseRoofSpineOddWhite" && std::abs(body->angle + kHalfPi) < 1e-4f);
    CHECK(nearCap && nearCap->model == "greenhouseRoofOddCapWhite" && std::abs(nearCap->angle - kHalfPi) < 1e-4f);
    CHECK(farCap && farCap->model == "greenhouseRoofOddCapWhite" && std::abs(farCap->angle - 3 * kHalfPi) < 1e-4f);
}

// Roofs on two lots: the pieces each roof's top shows, counted by texture cell, are those the game
// shows.
void sceneGridRoofsMatchTheGame() {
    using namespace ts2::scene;
    auto roof = [](float x0, float y0, float x1, float y1, std::int32_t level) {
        ts2::roofs::Roof r;
        r.level0 = r.level1 = level;
        r.x0 = x0;
        r.y0 = y0;
        r.x1 = x1;
        r.y1 = y1;
        r.type = 4;
        return r;
    };
    const auto main = RoofGrid::of(roof(23, 14, 11, 4, 2), 3.75f);
    const auto left = RoofGrid::of(roof(13, 10, 15, 14, 3), 6.75f);
    const auto right = RoofGrid::of(roof(19, 10, 21, 14, 3), 6.75f);
    CHECK(main && left && right);
    CHECK(main->nx == 27 && main->ny == 23 && main->minX == 10.5f && main->minY == 3.5f);
    CHECK(std::abs(main->z(0, 0, 0) - 3.33f) < 1e-4f && std::abs(main->z(1, 0, 0) - 3.83f) < 1e-4f);
    auto pieces = [](const RoofGrid& g, std::vector<const RoofGrid*> others) {
        Surfaces s;
        RoofLook look;
        look.top = 0;
        addRoofGrid(s, g, others, look);
        std::map<int, int> count;
        const auto& p = s[0];
        for (std::size_t t = 0; t + 2 < p.uvs.size() / 2; t += 3) {
            float u = 1, v = 1;
            for (std::size_t k = 0; k < 3; ++k) {
                u = std::min(u, p.uvs[(t + k) * 2]);
                v = std::min(v, p.uvs[(t + k) * 2 + 1]);
            }
            ++count[static_cast<int>(std::lround(v / 0.2f)) * 20 + static_cast<int>(std::lround(u / 0.2f)) * 2];
        }
        return count;
    };
    CHECK((pieces(*main, {&*left, &*right}) == std::map<int, int>{{4, 104}, {20, 32}, {22, 12}, {60, 884}}));
    CHECK((pieces(*right, {&*main, &*left}) == std::map<int, int>{{4, 28}, {20, 8}, {22, 4}, {26, 4}, {42, 2}, {60, 44}}));

    // The Dreamers' house (Pleasantview): a hipped roof over two hipped sheds that meet each other.
    auto ofType = [&](float x0, float y0, float x1, float y1, std::int32_t level, std::uint32_t type) {
        auto r = roof(x0, y0, x1, y1, level);
        r.type = type;
        return r;
    };
    const auto hip = RoofGrid::of(ofType(23, 18, 16, 5, 3, 5), 6.75f);
    const auto shed1 = RoofGrid::of(ofType(25, 5, 23, 15, 2, 2), 3.75f);
    const auto shed2 = RoofGrid::of(ofType(26, 8, 23, 12, 2, 2), 3.75f);
    CHECK(hip && shed1 && shed2);
    CHECK(hip->nx == 17 && hip->ny == 29 && hip->minX == 15.5f && hip->minY == 4.5f);
    CHECK(shed1->nx == 6 && shed1->ny == 23 && shed1->minX == 23 && shed1->minY == 4.5f);
    CHECK(shed2->nx == 8 && shed2->ny == 11 && shed2->minX == 23 && shed2->minY == 7.5f);
    CHECK((pieces(*hip, {&*shed1, &*shed2}) == std::map<int, int>{{0, 32}, {2, 112}, {4, 40}, {60, 672}, {66, 8}, {82, 32}}));
    CHECK((pieces(*shed1, {&*hip, &*shed2}) ==
           std::map<int, int>{{0, 10}, {2, 32}, {4, 12}, {20, 16}, {22, 8}, {26, 4}, {42, 2}, {60, 72}, {66, 4}, {82, 10}}));
    CHECK((pieces(*shed2, {&*hip, &*shed1}) ==
           std::map<int, int>{{0, 10}, {2, 16}, {4, 2}, {22, 8}, {60, 24}, {62, 16}, {64, 4}, {82, 10}}));

    // One roof of each kind on the ground (Cape Cod, rebuilt): a single slope, a gable across its
    // length, and a flat-topped hip meeting a small hip.
    const auto slope = RoofGrid::of(ofType(10, 1, 8, 3, 0, 1), 0);
    const auto across = RoofGrid::of(ofType(10, 5, 8, 7, 0, 3), 0);
    const auto flat = RoofGrid::of(ofType(10, 19, 1, 12, 0, 6), 0);
    const auto small = RoofGrid::of(ofType(9, 9, 7, 11, 0, 5), 0);
    CHECK(slope && across && flat && small);
    CHECK(slope->nx == 7 && slope->ny == 6 && slope->high == 3 && std::abs(slope->z(0, 0, 0) + 0.38f) < 1e-4f);
    CHECK(!across->ridgeAlongX && flat->cap == 7 && flat->peak == 7);
    CHECK((pieces(*slope, {}) == std::map<int, int>{{4, 12}, {60, 48}}));
    CHECK((pieces(*across, {}) == std::map<int, int>{{4, 24}, {60, 48}}));
    CHECK((pieces(*flat, {&*small}) ==
           std::map<int, int>{{0, 28}, {2, 94}, {4, 16}, {20, 2}, {46, 12}, {60, 418}, {62, 2}, {66, 16}, {82, 28}}));
    CHECK((pieces(*small, {&*flat}) == std::map<int, int>{{0, 10}, {2, 28}, {8, 2}, {24, 4}, {44, 2}, {46, 4}, {60, 12}, {82, 10}}));

    // A cone, a dome and an octagonal spire beside it: triangles per part (top, underside, edge
    // board bands) and how high each rises, as in the game.
    auto round = [&](ts2::roofs::Roof r, float slope) {
        r.slope = slope;
        Surfaces s;
        RoofLook look;
        look.top = look.atlas = 0;
        look.under = 1;
        look.edges = 2;
        look.trim = 3;
        CHECK(addRoundRoof(s, r, 0, look, 40, 40));
        std::vector<std::size_t> count;
        float top = -1e9f;
        for (int m = 0; m < 4; ++m) {
            count.push_back(s[m].indices.size() / 3);
            for (std::size_t i = 2; i < s[m].positions.size(); i += 3) {
                top = std::max(top, s[m].positions[i]);
            }
        }
        return std::pair{count, top};
    };
    const auto [cone, coneTop] = round(ofType(6, 11, 3, 9, 0, 7), 1);
    const auto [dome, domeTop] = round(ofType(25, 19, 24, 18, 0, 8), 0.5f);
    const auto [spire, spireTop] = round(ofType(28, 16, 29, 17, 0, 9), 1);
    CHECK((cone == std::vector<std::size_t>{1248, 48, 96, 96}) && std::abs(coneTop - 2.08f) < 1e-4f);
    CHECK((dome == std::vector<std::size_t>{768, 48, 96, 96}) && std::abs(domeTop - (std::sqrt(2.0f) - 0.42f)) < 1e-4f);
    CHECK((spire == std::vector<std::size_t>{528, 8, 32, 32}) && std::abs(spireTop - (2 * std::sqrt(2.0f) - 0.42f)) < 1e-4f);

    // A broken file's roof that would need millions of rings is left out.
    auto steep = ofType(0, 0, 200, 200, 0, 7);
    steep.slope = 200;
    Surfaces none;
    CHECK(addRoundRoof(none, steep, 0, RoofLook{}, 256, 256) && none.empty());
}

// Cutaway cuts a wall on the storey looked at when a room lies behind it as the camera looks, and a
// wall seen edge-on only between two rooms. Walls down cuts every wall that may be cut.
void sceneCutawayCutsWallsInFrontOfRooms() {
    using namespace ts2::scene;
    WallInfo outer;  // along +x, and its left side, toward -y, is the room
    outer.x1 = 1;
    outer.level = 1;
    outer.roomLeft = true;
    View view;
    view.walls = WallView::Cutaway;
    view.level = 1;
    view.facing = {0, -1};  // looking from outside toward the room
    CHECK(cutDown(outer, view));
    view.facing = {0, 1};  // from inside toward the outdoors: the back wall stays up
    CHECK(!cutDown(outer, view));
    view.facing = {1, 0};  // edge-on
    CHECK(!cutDown(outer, view));
    WallInfo inner = outer;
    inner.roomRight = true;
    CHECK(cutDown(inner, view));
    view.walls = WallView::Down;
    CHECK(cutDown(outer, view));
    view.level = 2;
    CHECK(!cutDown(outer, view));
    view.level = 1;
    outer.cuttable = false;  // a foundation
    CHECK(!cutDown(outer, view));
    view.walls = WallView::Up;
    CHECK(!cutDown(inner, view));
}

// What a view shows: nothing above its storey, roofs only with walls up on the top storey, a wall's
// cut version in place of the full one, and a window gone with the wall it sits in.
void sceneViewsShowStoreysWallsAndRoofs() {
    using namespace ts2::scene;
    Scene scene;
    WallInfo wall;
    wall.x1 = 1;
    wall.level = 1;
    wall.roomLeft = wall.roomRight = true;
    scene.walls = {wall};
    auto add = [&](Part part, int level, bool cut = false, std::vector<int> walls = {}) {
        Instance i;
        i.mesh = 0;
        i.part = part;
        i.level = level;
        i.cut = cut;
        i.walls = std::move(walls);
        scene.instances.push_back(i);
    };
    add(Part::Other, 0);                   // 0 terrain
    add(Part::Object, 0);                  // 1 in the yard
    add(Part::Floor, 2);                   // 2 upstairs
    add(Part::Roof, 2);                    // 3
    add(Part::Wall, 1, false, {0});        // 4 full
    add(Part::Wall, 1, true, {0});         // 5 cut
    add(Part::Object, 1, false, {0});      // 6 a window in it
    scene.groundLevel = 0;
    CHECK(topLevel(scene) == 2 && groundLevel(scene) == 0);
    View view;
    view.level = 2;
    CHECK((visibleInstances(scene, view) == std::vector<std::uint8_t>{1, 1, 1, 1, 1, 0, 1}));
    view.level = 1;
    CHECK((visibleInstances(scene, view) == std::vector<std::uint8_t>{1, 1, 0, 0, 1, 0, 1}));
    view.walls = WallView::Down;
    CHECK((visibleInstances(scene, view) == std::vector<std::uint8_t>{1, 1, 0, 0, 0, 1, 0}));
    view.level = 2;
    CHECK((visibleInstances(scene, view) == std::vector<std::uint8_t>{1, 1, 1, 0, 1, 0, 1}));

    // An attic wall a storey above the top one (the Smiths' dormer gables) shows with the roofs only.
    WallInfo gable;
    gable.level = 3;
    gable.attic = true;
    scene.walls.push_back(gable);
    add(Part::Wall, 3, false, {1});        // 7
    CHECK(topLevel(scene) == 2);
    view.walls = WallView::Up;
    CHECK(visibleInstances(scene, view)[7] == 1);
    view.roofs = false;
    CHECK(visibleInstances(scene, view)[7] == 0);

    // A porch roof over the ground floor stays on while the floor above is looked at with the walls up,
    // not in cutaway.
    add(Part::Roof, 1);                    // 8
    view.roofs = true;
    CHECK(visibleInstances(scene, view)[8] == 1 && visibleInstances(scene, view)[3] == 1);
    view.walls = WallView::Cutaway;
    CHECK(visibleInstances(scene, view)[8] == 0 && visibleInstances(scene, view)[3] == 0);
    view.walls = WallView::Up;
    view.level = 1;
    CHECK(visibleInstances(scene, view)[8] == 0);
}

// A stretched rig has every joint's rest translation scaled and its rotations kept.
void stretchedRigScalesJointTranslations() {
    ts2::scene::Rig rig;
    rig.nodes.push_back({"", -1, {1, 1, 1}, {0, 0, 0, 1}});
    rig.nodes.push_back({"r_calf", 0, {0, 0.5f, 0}, {0.1f, 0, 0, 0.995f}});
    const auto teen = ts2::scene::stretched(rig, 0.94f);
    CHECK(teen.nodes[0].translation[0] == 1.0f);
    CHECK(std::fabs(teen.nodes[1].translation[1] - 0.47f) < 1e-6f && teen.nodes[1].rotation == rig.nodes[1].rotation);
}

// A joint's animation turn (its bone data rotation) comes before the angles an animation gives it.
// Keys crossing 180 degrees: the turn goes the short way between frames (through 180, not 0).
void channelTurnsTakeTheShortWay() {
    ts2::anim::Channel c;
    c.flags = static_cast<std::uint32_t>(ts2::anim::ChannelKind::Rotation) << 22 | 0x8000;
    c.components.resize(3);
    c.components[0].keys = {{0, 178, 0, 0}, {33, -176, 0, 0}};
    c.components[1].keys = {{0, 0, 0, 0}};
    c.components[2].keys = {{0, 0, 0, 0}};
    const auto mid = ts2::scene::channelTurn(c, 16.5f);
    CHECK(std::fabs(std::fabs(mid[0]) - 1.0f) < 1e-3f);
    const auto start = ts2::scene::channelTurn(c, 0), expected = ts2::scene::eulerRotation(178, 0, 0);
    CHECK(std::fabs(start[0] - expected[0]) < 1e-5f && std::fabs(start[3] - expected[3]) < 1e-5f);
}

void animatedRotationTurnsFirst() {
    ts2::scene::Rig::Node plain, turned;
    turned.animationTurn = {0.70710678f, 0, 0, 0.70710678f};  // 90 degrees about x
    const auto close = [](const std::array<float, 4>& a, const std::array<float, 4>& b) {
        return std::fabs(a[0] - b[0]) < 1e-5f && std::fabs(a[1] - b[1]) < 1e-5f && std::fabs(a[2] - b[2]) < 1e-5f &&
               std::fabs(a[3] - b[3]) < 1e-5f;
    };
    CHECK(close(ts2::scene::animatedRotation(plain, 0, 90, 0), ts2::scene::eulerRotation(0, 90, 0)));
    CHECK(close(ts2::scene::animatedRotation(turned, 0, 0, 0), turned.animationTurn));
    // 90 about y, then 90 about x in front: (x, y, z, w) = (0.5, 0.5, 0.5, 0.5).
    CHECK(close(ts2::scene::animatedRotation(turned, 0, 90, 0), {0.5f, 0.5f, 0.5f, 0.5f}));
}

// A mirrored clip moves the joint on the other side ("l_" and "r_" swapped) and reflects each value
// across that joint's mirror axis: the turn keeps its part about the axis, the place is negated along it.
void mirroredClipsPlayOnTheOtherSide() {
    CHECK(ts2::scene::mirroredJointName("l_hand") == "r_hand");
    CHECK(ts2::scene::mirroredJointName("r_thigh") == "l_thigh");
    CHECK(ts2::scene::mirroredJointName("L_hand") == "L_hand");
    CHECK(ts2::scene::mirroredJointName("spine0") == "spine0");
    CHECK(ts2::scene::mirroredJointName("l") == "l");
    const std::array<float, 4> q{0.1f, 0.2f, 0.3f, 0.927f};
    CHECK((ts2::scene::mirroredTurn(q, 1) == std::array<float, 4>{-0.1f, 0.2f, -0.3f, 0.927f}));
    CHECK((ts2::scene::mirroredTurn(q, -1) == q));
    CHECK((ts2::scene::mirroredPlace({1, 2, 3}, 2) == std::array<float, 3>{1, 2, -3}));
    CHECK((ts2::scene::mirroredPlace({1, 2, 3}, 5) == std::array<float, 3>{1, 2, 3}));

    ts2::scene::Rig rig;
    rig.nodes.push_back({"root", -1, {0, 0, 0}, {0, 0, 0, 1}});
    rig.nodes.push_back({"l_arm", 0, {1, 0, 0}, {0, 0, 0, 1}});
    rig.nodes.push_back({"r_arm", 0, {-1, 0, 0}, {0, 0, 0, 1}});
    rig.nodes[2].mirrorAxis = 0;
    auto a = std::make_shared<ts2::anim::Animation>();
    a->duration = 1000;
    ts2::anim::Channel c;
    c.name = "l_arm";
    c.flags = static_cast<std::uint32_t>(ts2::anim::ChannelKind::Translation) << 22 | 0x8000;
    c.components.resize(3);
    c.components[0].keys.push_back({0, 2});
    c.components[1].keys.push_back({0, 3});
    c.components[2].keys.push_back({0, 4});
    a->channels.push_back(c);
    const auto plain = ts2::scene::bindAnimation(rig, a);
    const auto mirrored = ts2::scene::bindAnimation(rig, a, true);
    CHECK(plain.translation[1] == 0 && plain.translation[2] == -1);
    CHECK(mirrored.translation[1] == -1 && mirrored.translation[2] == 0);
    std::vector<ts2::scene::Transform> world;
    ts2::scene::poseRig(rig, &mirrored, 0, world);
    CHECK(world[2].m[3] == -2.0f && world[2].m[7] == 3.0f && world[2].m[11] == 4.0f);
    CHECK(world[1].m[3] == 1.0f);
    ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{{&mirrored, 0, 1, 0}}, world);
    CHECK(world[2].m[3] == -2.0f && world[2].m[7] == 3.0f);
}

// A locomotion clip plays from its own heading: joints with a turn axis (the root and the limb
// controls) are turned about it, the rest follow as children. Joints without one are left alone.
void clipsTurnToTheirOwnHeading() {
    constexpr float kQuarter = 1.5707963f;
    const auto near = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::fabs(a[0] - b[0]) < 1e-5f && std::fabs(a[1] - b[1]) < 1e-5f && std::fabs(a[2] - b[2]) < 1e-5f;
    };
    CHECK(near(ts2::scene::turnedPlace({1, 0, 5}, 2, kQuarter), {0, 1, 5}));
    CHECK(near(ts2::scene::turnedPlace({1, 2, 3}, -1, kQuarter), {1, 2, 3}));
    const auto q = ts2::scene::turnedTurn({0, 0, 0, 1}, 2, kQuarter);
    CHECK(std::fabs(q[2] - 0.7071068f) < 1e-5f && std::fabs(q[3] - 0.7071068f) < 1e-5f);

    ts2::scene::Rig rig;
    rig.nodes.push_back({"root_trans", -1, {0, 0, 0}, {0, 0, 0, 1}});
    rig.nodes.push_back({"arm", 0, {1, 0, 0}, {0, 0, 0, 1}});
    rig.nodes.push_back({"loose", -1, {0, 0, 0}, {0, 0, 0, 1}});
    rig.nodes[0].turnAxis = 2;
    auto a = std::make_shared<ts2::anim::Animation>();
    a->duration = 1000;
    for (const char* name : {"root_trans", "loose"}) {
        ts2::anim::Channel c;
        c.name = name;
        c.flags = static_cast<std::uint32_t>(ts2::anim::ChannelKind::Translation) << 22 | 0x8000;
        c.components.resize(3);
        c.components[0].keys.push_back({0, 2});
        c.components[1].keys.push_back({0, 0});
        c.components[2].keys.push_back({0, 0});
        a->channels.push_back(c);
    }
    const auto bound = ts2::scene::bindAnimation(rig, a);
    std::vector<ts2::scene::Transform> world;
    ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{{&bound, 0, 1, 0, 0, -1, kQuarter}}, world);
    CHECK(std::fabs(world[0].m[3]) < 1e-5f && std::fabs(world[0].m[7] - 2) < 1e-5f);
    CHECK(std::fabs(world[2].m[3] - 2) < 1e-5f && std::fabs(world[2].m[7]) < 1e-5f);
}

// Layered posing as the game mixes: per joint the highest layer moving it takes it, the layer below
// gets what that leaves, clips of one layer are averaged by weight, and joints no clip moves rest.
void layeredClipsMixLikeTheGame() {
    ts2::scene::Rig rig;
    rig.nodes.push_back({"root", -1, {0, 0, 0}, {0, 0, 0, 1}});
    rig.nodes.push_back({"arm", 0, {1, 0, 0}, {0, 0, 0, 1}});
    // A clip that moves only the arm, to x = value.
    const auto armAt = [&](float x) {
        auto a = std::make_shared<ts2::anim::Animation>();
        a->duration = 1000;
        ts2::anim::Channel c;
        c.name = "arm";
        c.flags = static_cast<std::uint32_t>(ts2::anim::ChannelKind::Translation) << 22 | 0x8000;
        c.components.resize(3);
        c.components[0].keys.push_back({0, x});
        c.components[1].keys.push_back({0, 0});
        c.components[2].keys.push_back({0, 0});
        a->channels.push_back(c);
        return ts2::scene::bindAnimation(rig, a);
    };
    const auto low = armAt(2), high = armAt(4), other = armAt(6);
    std::vector<ts2::scene::Transform> world;
    const auto armX = [&](std::vector<ts2::scene::LayeredClip> clips) {
        ts2::scene::poseRigLayered(rig, clips, world);
        return world[1].m[3];
    };
    CHECK(armX({}) == 1.0f);
    // A held pose: the starting pose (below layer 0) counts only at first. Then a joint no clip moves
    // keeps where it last was.
    {
        std::vector<ts2::scene::HeldJoint> held;
        ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{{&low, 0, 1, -1}}, world, &held);
        CHECK(world[1].m[3] == 2.0f && held.size() == 2);
        ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{{&low, 0, 1, -1}, {&high, 0, 1, 0}}, world, &held);
        CHECK(world[1].m[3] == 4.0f);
        ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{{&low, 0, 1, -1}}, world, &held);
        CHECK(world[1].m[3] == 4.0f);
    }
    CHECK(armX({{&low, 0, 1, 0}, {&high, 0, 1, 1}}) == 4.0f);
    CHECK(std::fabs(armX({{&low, 0, 1, 0}, {&high, 0, 0.25f, 1}}) - 2.5f) < 1e-5f);
    // On an overlay layer the newer clip goes first: one fading in takes its share from the older one,
    // and one fully in hides it. Layer 0 averages its clips.
    CHECK(std::fabs(armX({{&high, 0, 1, 1, 0}, {&other, 0, 0.25f, 1, 2}}) - 4.5f) < 1e-5f);
    CHECK(armX({{&other, 0, 1, 1, 2}, {&high, 0, 1, 1, 0}}) == 6.0f);
    CHECK(std::fabs(armX({{&high, 0, 1, 0, 0}, {&other, 0, 1, 0, 2}}) - 5.0f) < 1e-5f);
    CHECK(armX({{&low, 0, 1, 0}, {&high, 0, 0.0005f, 1}}) == 2.0f);
    // A clip fading out under a newer one fading in gives way only to it: the two sum to 1. Alone it
    // fades out to the layer below.
    CHECK(std::fabs(armX({{&other, 0, 0.75f, 1, 0, 1}, {&high, 0, 0.25f, 1, 2}}) - 5.5f) < 1e-5f);
    CHECK(std::fabs(armX({{&other, 0, 0.5f, 1, 0, 1}, {&low, 0, 1, -1}}) - 4.0f) < 1e-5f);

    // The same turn saved as 150 and as -210 degrees (opposite quaternions) mixes to that turn.
    const auto armTurned = [&](float degrees) {
        auto a = std::make_shared<ts2::anim::Animation>();
        a->duration = 1000;
        ts2::anim::Channel c;
        c.name = "arm";
        c.flags = static_cast<std::uint32_t>(ts2::anim::ChannelKind::Rotation) << 22 | 0x8000;
        c.components.resize(3);
        c.components[0].keys.push_back({0, degrees});
        c.components[1].keys.push_back({0, 0});
        c.components[2].keys.push_back({0, 0});
        a->channels.push_back(c);
        return ts2::scene::bindAnimation(rig, a);
    };
    const auto turnedOneWay = armTurned(150), turnedOtherWay = armTurned(-210);
    ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{{&turnedOneWay, 0, 0.5f, 0}, {&turnedOtherWay, 0, 0.5f, 0}}, world);
    CHECK(std::fabs(world[1].m[5] - std::cos(150 * 3.14159265f / 180)) < 1e-4f);

    // Channels without flag bit 15 are added, weighted, on top of the mix. On their own they move nothing.
    const auto additive = [](ts2::scene::RigAnimation a) {
        auto copy = std::make_shared<ts2::anim::Animation>(*a.animation);
        copy->channels[0].flags &= ~0x8000u;
        a.animation = copy;
        return a;
    };
    const auto shift = additive(armAt(0.5f));
    CHECK(armX({{&shift, 0, 1, 1}}) == 1.0f);
    CHECK(std::fabs(armX({{&low, 0, 1, 0}, {&shift, 0, 0.5f, 1}}) - 2.25f) < 1e-5f);
    // An added turn of 90 degrees at half weight turns the joint 45 degrees.
    const auto quarter = additive(armTurned(90)), still = armTurned(0), quarter90 = armTurned(90);
    ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{{&still, 0, 1, 0}, {&quarter, 0, 0.5f, 1}}, world);
    CHECK(std::fabs(world[1].m[5] - std::cos(45 * 3.14159265f / 180)) < 1e-4f);
    // The joint's animation turn goes into averaged turns only: an added turn of 0 degrees on a joint
    // with one leaves the mix as it is.
    auto withTurn = rig;
    withTurn.nodes[1].animationTurn = {0.70710678f, 0, 0, 0.70710678f};
    const auto nothing = additive(armTurned(0));
    std::vector<ts2::scene::Transform> alone, added;
    ts2::scene::poseRigLayered(withTurn, std::vector<ts2::scene::LayeredClip>{{&still, 0, 1, 0}}, alone);
    ts2::scene::poseRigLayered(withTurn, std::vector<ts2::scene::LayeredClip>{{&still, 0, 1, 0}, {&nothing, 0, 1, 1}}, added);
    CHECK(std::fabs(alone[1].m[5] - added[1].m[5]) < 1e-5f && std::fabs(alone[1].m[6] - added[1].m[6]) < 1e-5f);
    // An added turn goes after the mixed one, in the joint's own frame.
    auto sideways = additive(armTurned(0));
    {
        auto copy = std::make_shared<ts2::anim::Animation>(*sideways.animation);
        copy->channels[0].components[1].keys = {{0, 90}};
        sideways.animation = copy;
    }
    ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{{&quarter90, 0, 1, 0}, {&sideways, 0, 1, 1}}, world);
    const auto expected = ts2::scene::turnProduct(ts2::scene::channelTurn(quarter90.animation->channels[0], 0),
                                                  ts2::scene::channelTurn(sideways.animation->channels[0], 0));
    const auto made = ts2::scene::Transform::of({0, 0, 0}, expected);
    CHECK(std::fabs(world[1].m[0] - made.m[0]) < 1e-4f && std::fabs(world[1].m[5] - made.m[5]) < 1e-4f && std::fabs(world[1].m[2] - made.m[2]) < 1e-4f);
}

void groundIsOpenWhereItHasNoSurface() {
    // One tile, its x-low quarter without a surface (a pool's edge), and a second level all surface.
    ts2::array3d::Array3D surface;
    surface.width = surface.height = 1;
    surface.levels = 2;
    surface.elementSize = 4;
    surface.elements = {0, 1, 1, 1, 1, 1, 1, 1};
    CHECK(ts2::scene::surfaceOpen(surface, 0, 0.1f, 0.5f) && !ts2::scene::surfaceOpen(surface, 0, 0.9f, 0.5f));
    CHECK(!ts2::scene::surfaceOpen(surface, 0, 0.5f, 0.1f) && !ts2::scene::surfaceOpen(surface, 1, 0.1f, 0.5f));
    CHECK(!ts2::scene::surfaceOpen(surface, 0, -1, 0.5f) && !ts2::scene::surfaceOpen(surface, 5, 0.1f, 0.5f) &&
          !ts2::scene::surfaceOpen(surface, 0, std::nanf(""), 0.5f));
}

void poolWaterHasTheGamesDepthLayers() {
    using namespace ts2::scene;
    // A one-tile pool: open ground over a floor on the level below.
    ts2::content::Lot lot;
    lot.roomGraph.emplace();
    lot.roomGraph->firstLevel = -1;
    ts2::array3d::Array3D floors, surface;
    floors.width = floors.height = surface.width = surface.height = 1;
    floors.levels = surface.levels = 2;
    floors.elementSize = 8;
    surface.elementSize = 4;
    floors.elements = {1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    surface.elements = {1, 1, 1, 1, 0, 0, 0, 0};
    lot.grids.emplace(ts2::content::kFloorGridInstance, floors);
    lot.grids.emplace(ts2::content::kSurfaceGridInstance, surface);
    CHECK(poolwater::waterQuarters(lot).size() == 4);
    lot.grids.at(ts2::content::kSurfaceGridInstance).elements[4] = 1;  // ground over one quarter
    CHECK(poolwater::waterQuarters(lot).size() == 3);

    // A pool three tiles square, level ground: 0.75, 1.5, 2.25 down. Clear on the wall, faint beside it.
    std::vector<poolwater::Quarter> quarters;
    for (int x = 0; x < 3; ++x) {
        for (int y = 0; y < 3; ++y) {
            for (int q = 0; q < 4; ++q) {
                quarters.push_back({x, y, q});
            }
        }
    }
    const auto layers = poolwater::depthLayers(quarters, [](float, float) { return 0.0f; });
    CHECK(layers[0].opacity == 0.2f && layers[1].opacity == 0.2f && layers[2].opacity == 0.5f);
    CHECK(layers[0].indices.size() == 36 * 3 && layers[0].alphas.size() == 16 + 9);
    auto alphaAt = [&](const poolwater::Layer& l, float x, float y) {
        for (std::size_t i = 0; i < l.alphas.size(); ++i) {
            if (l.positions[i * 3] == x && l.positions[i * 3 + 1] == y) return l.alphas[i];
        }
        return -1.0f;
    };
    CHECK(alphaAt(layers[2], 0, 0) == 0.0f && alphaAt(layers[2], 0.5f, 0.5f) == 0.09f);
    CHECK(alphaAt(layers[2], 1.5f, 1.5f) == 0.27f && alphaAt(layers[2], 1, 1) == 0.27f);
    CHECK(layers[0].positions[2] == -0.75f && layers[2].positions[2] == -2.25f);
}

// The packs are found in both installation layouts: the Ultimate Collection's folders named after
// their packs, and the Legacy Collection's EP1-EP9 and SP1-SP8 (where Mansion & Garden is EP9).
// The skin drawn under clothes is the Sim's own skin outfit, as the game shows it: its bare
// top under a body outfit, its bare bottom under a top and bottom. Nothing when it has none.
void skinUnderClothesNames() {
    ts2::scene::SimLook look;
    CHECK(ts2::scene::skinUnderClothes(look).empty());
    look.skinTop = "emtopnaked_nude_s5";
    look.skinBottom = "cubottomnaked_nude_s1";
    CHECK(ts2::scene::skinUnderClothes(look) == "emtopnaked_nude_s5");
    CHECK(ts2::scene::skinUnderClothes(look, true) == "cubottomnaked_nude_s1");
}

// A joint the animation doesn't move is posed with the mesh's own rest place for it, under its posed
// parent. Long hair turns its hair joints further than the skeleton does.
void unanimatedJointsKeepTheMeshRest() {
    using ts2::scene::Transform;
    ts2::scene::Rig rig;
    rig.nodes.push_back({"head", -1, {0, 0, 1}, {0, 0, 0, 1}});
    rig.nodes.push_back({"f_hair", 0, {0, 0, 0}, {0.70710678f, 0, 0, 0.70710678f}});  // a quarter turn about x
    std::vector<Transform> world;
    ts2::scene::poseRig(rig, nullptr, 0, world);
    ts2::scene::Mesh mesh;
    mesh.skin.push_back({"f_hair", Transform{}, Transform{}, Transform::of({0, 0, 0}, {1, 0, 0, 0})});  // a half turn
    mesh.skin.push_back({"f_hair", Transform{}, Transform{}, std::nullopt});
    std::vector<Transform> out;
    ts2::scene::skinMatrices(mesh, {1, 1}, rig, nullptr, world, out);
    CHECK(out.size() == 2);
    float own[3] = {0, 1, 0}, skeleton[3] = {0, 1, 0};
    out[0].apply(own, true);
    out[1].apply(skeleton, true);
    CHECK(std::abs(own[1] + 1) < 1e-5f && std::abs(own[2] - 1) < 1e-5f);
    CHECK(std::abs(skeleton[1]) < 1e-5f && std::abs(skeleton[2] - 2) < 1e-5f);
}

void roomSamplesAreGatheredAsTheGame() {
    using namespace ts2::scene;
    // The game's name hashes and thumbnail average.
    CHECK(ts2::content::nameHash32("TEST") == 0xCC743053u && ts2::content::nameHash24("test") == 0xF86ED0u);
    const std::vector<std::uint8_t> pixels{255, 0, 0, 255, 0, 255, 0, 0, 0, 0, 255, 255};  // the green one is transparent
    const auto avg = ts2::content::averageColor(pixels);
    CHECK(std::abs(avg[0] - 0.5f) < 1e-6f && avg[1] == 0 && std::abs(avg[2] - 0.5f) < 1e-6f);
    CHECK((ts2::content::averageColor({}) == std::array<float, 3>{0, 0, 0}));
    ts2::content::CoveringColors none{std::filesystem::path{}};
    CHECK(!none.color(ts2::content::CoveringKind::Floor, "abc"));

    // A lot two tiles wide with one floor covering, a wall between the tiles, and a portal.
    ts2::content::Lot lot;
    ts2::array3d::Array3D floors;
    floors.width = 2;
    floors.height = 1;
    floors.levels = 1;
    floors.elementSize = 8;
    floors.elements.assign(2 * 8, 0);
    for (std::size_t q = 0; q < 4; ++q) floors.elements[q * 2] = 1;  // tile 0 floored, tile 1 bare
    lot.grids[ts2::content::kFloorGridInstance] = floors;
    lot.floorPatterns[1] = {"abc", 1};
    lot.wallPatterns[2] = {"def", 1};
    ts2::wallgraph::WallGraph graph;
    graph.points = {{1, 1, 0, 0}, {2, 1, 1, 0}};
    graph.walls = {{7, 1, 0, 2, 0}, {8, 2, 0, 1, 0}};  // the same wall twice: counted once
    lot.roomGraph = graph;
    lot.wallCoverings = {{7, 1, 2, 2}};
    roomsamples::LotInputs in;
    in.quarterRoom = [](int, int x, int, int) { return static_cast<std::int16_t>(x == 0 ? 1 : 2); };
    in.faceRoom = [](const wallruns::Key& k) { return static_cast<std::int16_t>(k.side == 8 ? 1 : 2); };
    in.covering = [](bool floor, const std::string&) -> std::optional<roomsamples::Color> {
        if (floor) return roomsamples::Color{1, 1, 1};
        return std::nullopt;  // the wall's covering has no thumbnail: grey
    };
    in.floorLight = [](int, float, float) { return roomsamples::Color{0.8f, 0.8f, 0.8f}; };
    in.wallLight = [](const wallruns::Key&) { return roomsamples::Color{0.4f, 0.4f, 0.4f}; };
    const auto rooms = roomsamples::gather(lot, in, {{2, 1, 0, {0.2f, 0.2f, 0.2f}}, {0, 1, 0, {9, 9, 9}}}, roomsamples::Settings{});
    CHECK(rooms.count(1) && rooms.count(2));
    if (rooms.count(1) && rooms.count(2)) {
        const auto& one = rooms.at(1);
        CHECK(one.weight[roomlight::kFromFloor] == 4 && one.weight[roomlight::kFromCeiling] == 4);
        CHECK(std::abs(one.sum[roomlight::kFromFloor][0] - 4 * 0.2f) < 1e-6f);  // white floor x 0.25 x 0.8
        // The wall face looking into room 1 (-x) shows its grey covering half lit: 0.2 x 0.75.
        CHECK(one.weight[roomlight::kMinusX] == 1 && std::abs(one.sum[roomlight::kMinusX][0] - 0.15f) < 1e-6f);
        const auto& two = rooms.at(2);
        CHECK(two.weight[roomlight::kFromFloor] == 0);  // its tile is bare
        CHECK(two.weight[roomlight::kPlusX] == 2);       // the wall face and the portal
        CHECK(std::abs(two.sum[roomlight::kPlusX][0] - (0.15f + 0.15f)) < 1e-6f);
    }
    // Broken wall points and missing inputs are left out.
    lot.roomGraph->points[0].x = std::numeric_limits<float>::quiet_NaN();
    CHECK(roomsamples::gather(lot, in, {}, roomsamples::Settings{}).at(1).weight[roomlight::kMinusX] == 0);
    CHECK(roomsamples::gather(lot, roomsamples::LotInputs{}, {}, roomsamples::Settings{}).empty());
}

void portableMathMatchesTheLibrary() {
    using namespace ts2::scene::portable;
    // Within a few units in the last place of a double of the C library, over many values.
    auto close = [](double a, double b) { return std::abs(a - b) <= 4e-16 * std::max(1.0, std::abs(b)); };
    bool sinesOk = true, powersOk = true;
    for (int i = -20000; i <= 20000; ++i) {
        const double x = i * 0.000731;  // about -14.6 to 14.6
        sinesOk = sinesOk && close(sine(x), std::sin(x)) && close(cosine(x), std::cos(x));
    }
    for (int i = 1; i <= 400; ++i) {
        for (int j = 0; j <= 100; ++j) {
            const double x = i / 400.0, y = 1 + j * 0.37;
            const double want = std::pow(x, y);
            // The error grows with |y log x| (here up to 600). What the lighting uses is the
            // value rounded to float, which must be the same.
            powersOk = powersOk && std::abs(power(x, y) - want) <= 1e-13 * want &&
                       static_cast<float>(power(x, y)) == static_cast<float>(want);
        }
    }
    CHECK(sinesOk && powersOk);
    CHECK(sine(0) == 0 && cosine(0) == 1 && power(0.5, 2) == 0.25 && power(2, 0) == 1);
    CHECK(std::isnan(sine(std::numeric_limits<double>::infinity())) && std::isnan(power(-1, 2)) && std::isnan(log(-1)));
    CHECK(exp(1000) == std::numeric_limits<double>::infinity() && exp(-1000) == 0 && power(0, 2) == 0);
}

// The picker finds the nearest triangle among shown instances, skips Sims and broken indices. The
// hover light pulses between 0.08 and 0.1 once a second.
void pickerFindsTheNearestShownTriangle() {
    using namespace ts2::scene;
    Scene scene;
    Mesh square;
    Primitive p;
    p.positions = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
    p.indices = {0, 1, 2, 0, 2, 3, 0, 1, 99};  // the last triangle points past the vertices
    square.primitives.push_back(p);
    scene.meshes.push_back(square);
    for (const float z : {0.0f, 2.0f, 4.0f}) {
        Instance i;
        i.mesh = 0;
        i.part = Part::Object;
        i.transform = Transform::at(0, 0, z, 0);
        i.object = static_cast<std::uint32_t>(z) + 1;
        scene.instances.push_back(i);
    }
    Actor sim;
    sim.skinned.push_back({2, {}});
    scene.actors.push_back(sim);
    Picker picker;
    picker.prepare(scene);
    const std::array<float, 3> eye{0.5f, 0.5f, 10}, down{0, 0, -1};
    auto hit = picker.pick(scene, {}, eye, down);
    CHECK(hit && hit->instance == 1 && std::abs(hit->distance - 8) < 1e-5f);  // the Sim at z 4 is left out
    const std::vector<std::uint8_t> shown{1, 0, 1};
    hit = picker.pick(scene, shown, eye, down);
    CHECK(hit && hit->instance == 0);
    CHECK(!picker.pick(scene, {}, {5, 5, 10}, down));
    // An instance added later, and one moved away, are found where they are now.
    Instance added = scene.instances[0];
    added.transform = Transform::at(0, 0, 6, 0);
    scene.instances.push_back(added);
    picker.update(scene, 3);
    hit = picker.pick(scene, {}, eye, down);
    CHECK(hit && hit->instance == 3 && std::abs(hit->distance - 4) < 1e-5f);
    scene.instances[3].transform = Transform::at(5, 5, 6, 0);
    picker.update(scene, 3);
    hit = picker.pick(scene, {}, eye, down);
    CHECK(hit && hit->instance == 1);
    // A turn and a move undone.
    const Transform t = Transform::at(1, 2, 3, 0.7f);
    const Transform back = t.rigidInverse() * t;
    const Transform identity;
    for (std::size_t k = 0; k < 12; ++k) {
        CHECK(std::abs(back.m[k] - identity.m[k]) < 1e-5f);
    }
    CHECK(std::abs(hoverAmbient(0.0) - 0.08f) < 1e-6f && std::abs(hoverAmbient(2.5) - 0.1f) < 1e-6f);
}

// A portrait's UI pose by age and gender, and its camera in front of the head (as the game drew
// five Sims: 1.4506 ahead and 0.05 above the head joint, 20 degrees, looking straight back).
void portraitPoseAndCamera() {
    using namespace ts2::scene;
    CHECK(portraitPose(8, 2, 3) == "a-UIpose-M03");
    CHECK(portraitPose(4, 1, 8) == "a-UIpose-F08");
    CHECK(portraitPose(2, 1, 1) == "c-UIpose-F01");
    CHECK(portraitPose(1, 2, 5) == "p-UIpose-m05");
    CHECK(portraitPose(0x20, 1, 9) == "b-UIpose-04");  // babies have four, and out of range is clamped
    CHECK(portraitPose(8, 2, 0) == "a-UIpose-M01");
    CHECK(portraitPoseCount(0x20) == 4 && portraitPoseCount(8) == 8);
    const auto c = portraitCamera(Transform{}, {-0.0088f, 0.0132f, 1.6752f});
    CHECK(std::abs(c.eye[0] + 0.0088f) < 1e-5f && std::abs(c.eye[1] - 1.4638f) < 1e-4f && std::abs(c.eye[2] - 1.7252f) < 1e-4f);
    CHECK(std::abs(c.target[1] - 0.4638f) < 1e-4f && std::abs(c.target[2] - c.eye[2]) < 1e-6f);
    CHECK(std::abs(c.fovY - 0.3490659f) < 1e-6f && c.nearPlane > 0.68f && c.farPlane < 1.75f);
    // A Sim turned a quarter at (3, 4, 0): the camera stays 1.4506 from it, at head height + 0.05.
    const auto t = portraitCamera(Transform::at(3, 4, 0, 1.5707963f), {0, 0, 1.6f});
    CHECK(std::abs(t.eye[2] - 1.65f) < 1e-4f && std::abs(std::hypot(t.eye[0] - 3, t.eye[1] - 4) - 1.4506f) < 1e-4f);
    CHECK(std::abs(std::hypot(t.target[0] - 3, t.target[1] - 4) - 0.4506f) < 1e-4f);
    // Five lights fixed to the camera: the key comes from the Sim's front left, above.
    const auto lit = portraitLighting(c);
    CHECK(lit.lights.size() == 5 && lit.ambient == (std::array<float, 3>{0, 0, 0}));
    CHECK(std::abs(lit.thumbnailScale - 0.4f) < 1e-6f);  // lit as the game lights family thumbnails
    CHECK(std::abs(lit.lights[0].direction[0] - 0.43493f) < 1e-4f && std::abs(lit.lights[0].direction[1] - 0.79454f) < 1e-4f &&
          std::abs(lit.lights[0].direction[2] - 0.42373f) < 1e-4f && std::abs(lit.lights[0].color[0] - 1.6452f) < 1e-6f);
    CHECK(std::abs(lit.lights[1].direction[2] + 1) < 1e-4f);
}

void compositeLayersBlendAsTheGame() {
    using namespace ts2::scene::textureblend;
    CHECK(weight(0) == 0 && weight(127) == 127 && weight(128) == 129 && weight(255) == 256);
    CHECK(blend(10, 200, 256) == 200 && blend(10, 200, 0) == 10);
    CHECK(blend(0, 255, 129) == 128 && blend(255, 0, 129) == 127 && blend(100, 101, 128) == 101);
    ts2::scene::Image under{1, 1, {200, 100, 0, 255}}, layer{1, 1, {0, 50, 255, 128}};
    drawOver(under, layer);
    CHECK(under.rgba[0] == 99 && under.rgba[1] == 75 && under.rgba[2] == 128 && under.rgba[3] == 255);
}

void compositeLayersUseTheMipLevelOfTheirSize() {
    using ts2::scene::mipLevelOfSize;
    CHECK(mipLevelOfSize(256, 256, 9, 256, 256) == std::size_t{0});
    CHECK(mipLevelOfSize(1024, 1024, 11, 256, 256) == std::size_t{2});
    CHECK(mipLevelOfSize(512, 256, 10, 128, 64) == std::size_t{2});
    // Width and height are halved together, so a level must match both.
    CHECK(!mipLevelOfSize(512, 256, 10, 256, 256));
    // Never stretched up, and no deeper than the levels there are.
    CHECK(!mipLevelOfSize(256, 256, 9, 512, 512));
    CHECK(!mipLevelOfSize(1024, 1024, 2, 256, 256));
    // Only powers of two are halved, and any size matches itself.
    CHECK(!mipLevelOfSize(300, 300, 9, 150, 150));
    CHECK(mipLevelOfSize(300, 200, 1, 300, 200) == std::size_t{0});
    CHECK(!mipLevelOfSize(0, 0, 0, 0, 0));
}

} // namespace

// Change Material's changes reach a lot object's primitives by mesh group (all of them for the whole
// model). "_null" hides them, a material the lot already has is reused and unchanged objects are skipped.
void objectMaterialsFollowChanges() {
    const auto empty = std::filesystem::temp_directory_path() / ("neots2-materials-test-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(empty);
    {
        ts2::scene::Assets assets(empty);
        app::ObjectMaterials materials(assets);
        ts2::scene::Scene scene;
        scene.materials.resize(3);
        scene.materials[0].name = "frame_wood";
        scene.materials[1].name = "screen_off";
        scene.materials[2].name = "Screen_On";
        ts2::scene::Mesh mesh;
        mesh.primitives.resize(3);
        mesh.primitives[0] = {.material = 0, .group = "frame"};
        mesh.primitives[1] = {.material = 1, .group = "screen"};
        mesh.primitives[2] = {.material = 1, .group = "screen"};
        scene.meshes.push_back(mesh);
        scene.instances.resize(1);
        scene.instances[0].mesh = 0;
        ts2::sim::World world;
        auto& tv = world.create(kObjectGroup, typed(kFridgeGuid));
        scene.placedObjects.push_back({.object = static_cast<std::uint32_t>(tv.id), .instance = 0});

        materials.update(world, scene);
        CHECK(materials.changed().empty());
        tv.materialChanges = {{"Screen", "screen_on"}};
        ++tv.materialVersion;
        materials.update(world, scene);
        CHECK(materials.changed().size() == 1 && materials.changed()[0].first == 0);
        CHECK((materials.changed()[0].second == std::vector<int>{0, 2, 2}));
        CHECK(scene.materials.size() == 3);
        materials.update(world, scene);
        CHECK(materials.changed().empty());

        tv.materialChanges = {{"", "screen_off"}, {"frame", "tv_null"}, {"screen", "missing"}};
        ++tv.materialVersion;
        materials.update(world, scene);
        CHECK((materials.changed()[0].second == std::vector<int>{ts2::scene::kHiddenMaterial, 1, 1}));
    }
    std::filesystem::remove_all(empty);
}

// Rays through a scene as drawn: from three quarters up one object's model to another's, meeting
// the start's own model, a wall between (unless hidden) and the target, nearest first.
// The game's neighborhood drift chain, as in effects.package: "nhoodcam" waits 3 s in its sequence,
// then plays "nhoodCamMotion", a metaparticle whose particles play "nhood_cameramotion", whose
// camera block sways the yaw from 0 to 10 degrees over 600 s and sends its position as the goal.
ts2::effects::File driftEffects() {
    using namespace ts2::effects;
    File f;
    auto effect = [&f](const std::string& name, BlockType type) {
        VisualEffect e;
        Component c;
        c.type = static_cast<std::uint8_t>(type);
        e.components.push_back(c);
        f.names[name] = static_cast<std::uint32_t>(f.effects.size());
        f.effects.push_back(e);
    };
    effect("nhoodcam", BlockType::Sequence);
    effect("nhoodcammotion", BlockType::MetaParticles);
    effect("nhood_cameramotion", BlockType::Camera);
    SequenceBlock sequence;
    sequence.items = {{{3, 3}, ""}, {{5000, 5000}, "nhoodCamMotion"}};
    f.sequences.push_back(sequence);
    MetaParticleBlock particles;
    particles.life = {5000, 5000};
    particles.damping = 0.9f;
    particles.wander[0] = {{0.05f, 0.15f}, {2.9f, 3.1f}, 0.1f, 0, 0};
    particles.wander[1] = {{0.025f, 0.075f}, {1.9f, 2.1f}, 0.1f, 0, 0};
    particles.effect = "nhood_cameramotion";
    f.metaParticles.push_back(particles);
    CameraBlock camera;
    camera.flags = 0x1860;
    camera.length = 600;
    camera.curves[5].values = {0, 10};
    f.cameras.push_back(camera);
    return f;
}

// Steps a drift `seconds` long at 120 steps a second, keeping the last step that asked for anything.
ts2::scene::HoodDrift::Step runDrift(ts2::scene::HoodDrift& drift, const std::array<float, 3>& target, float seconds, bool* folded = nullptr) {
    ts2::scene::HoodDrift::Step last;
    for (int i = 0; i < static_cast<int>(seconds * 120); ++i) {
        const auto step = drift.step(target, 1.0f / 120);
        if (folded && step.foldSway) {
            *folded = true;
        }
        if (step.goal || step.sway) {
            last = step;
        }
    }
    return last;
}

// It starts on the 31st resting step, where the camera looks, and moves nothing until the
// sequence's 3 s wait is over. Then the particle starts on the base and the sway is folded once.
void hoodDriftStartsAfterRestAndWait() {
    auto drift = ts2::scene::HoodDrift::load(driftEffects(), 7);
    CHECK(drift);
    const std::array<float, 3> target{500, 600, 312.5f};
    for (int i = 0; i < 30; ++i) {
        const auto step = drift->step(target, 1.0f / 120);
        CHECK(!step.goal && !step.sway && !step.foldSway);
    }
    CHECK(!drift->running());
    drift->step(target, 1.0f / 120);
    CHECK(drift->running());
    bool folded = false;
    CHECK(!runDrift(*drift, target, 2.9f, &folded).goal && !folded);
    const auto first = runDrift(*drift, target, 0.2f, &folded);
    CHECK(folded && first.goal && first.sway);
    CHECK(first.goal && (*first.goal)[2] == 312.5f && std::abs((*first.goal)[0] - 500) < 1 && std::abs((*first.goal)[1] - 600) < 1);
    folded = false;
    runDrift(*drift, target, 1, &folded);
    CHECK(!folded);
    // The rest is time, not steps: two long steps start it as surely as 31 short ones.
    auto slow = ts2::scene::HoodDrift::load(driftEffects(), 7);
    CHECK(slow);
    slow->step(target, 0.2f);
    CHECK(!slow->running());
    slow->step(target, 0.2f);
    CHECK(slow->running());
}

// Mouse or keys end it at once, and the next one waits 30 resting steps again and starts from
// where the camera looks then.
void hoodDriftInterruptRestartsFromTheCamera() {
    auto drift = ts2::scene::HoodDrift::load(driftEffects(), 7);
    CHECK(drift);
    runDrift(*drift, {0, 0, 0}, 5);
    drift->interrupt();
    CHECK(!drift->running());
    const std::array<float, 3> moved{900, 100, 312.5f};
    for (int i = 0; i < 30; ++i) {
        CHECK(!drift->step(moved, 1.0f / 120).goal);
    }
    const auto step = runDrift(*drift, moved, 3.2f);
    CHECK(step.goal && std::abs((*step.goal)[0] - 900) < 2 && std::abs((*step.goal)[1] - 100) < 2);
}

// The sway follows the camera block's curve over its 600 s, in radians, and then holds.
void hoodDriftSwaysAlongItsCurve() {
    auto drift = ts2::scene::HoodDrift::load(driftEffects(), 7);
    CHECK(drift);
    const auto half = runDrift(*drift, {0, 0, 0}, 3.26f + 300);
    CHECK(half.sway && std::abs(*half.sway - 5 * 3.14159265f / 180) < 0.002f);
    const auto end = runDrift(*drift, {0, 0, 0}, 400);
    CHECK(end.sway && std::abs(*end.sway - 10 * 3.14159265f / 180) < 1e-5f);
}

// The particle wanders flat, no faster than its pulls allow against the damping, and the same
// seed always gives the same path while another seed gives another.
void hoodDriftWandersWithinItsPulls() {
    auto a = ts2::scene::HoodDrift::load(driftEffects(), 7);
    auto b = ts2::scene::HoodDrift::load(driftEffects(), 7);
    auto c = ts2::scene::HoodDrift::load(driftEffects(), 8);
    CHECK(a && b && c);
    const std::array<float, 3> target{0, 0, 312.5f};
    const auto pa = runDrift(*a, target, 23.26f), pb = runDrift(*b, target, 23.26f), pc = runDrift(*c, target, 23.26f);
    CHECK(pa.goal && pb.goal && pc.goal);
    if (pa.goal && pb.goal && pc.goal) {
        CHECK(*pa.goal == *pb.goal && *pa.goal != *pc.goal);
        const float moved = std::hypot((*pa.goal)[0], (*pa.goal)[1]);
        CHECK(moved > 1 && moved <= 20 * (3.1f + 2.1f) / 0.9f && (*pa.goal)[2] == 312.5f);
    }
}

// A chain with values the drift can't use, or with a part missing, gives no drift.
void hoodDriftRefusesUnusableEffects() {
    CHECK(ts2::scene::HoodDrift::load(ts2::effects::File{}, 1) == std::nullopt);
    auto damped = driftEffects();
    damped.metaParticles[0].damping = 20;
    CHECK(!ts2::scene::HoodDrift::load(damped, 1));
    auto endless = driftEffects();
    endless.metaParticles[0].life[1] = std::numeric_limits<float>::infinity();
    CHECK(!ts2::scene::HoodDrift::load(endless, 1));
    auto broken = driftEffects();
    broken.cameras[0].curves[5].values[1] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!ts2::scene::HoodDrift::load(broken, 1));
    auto timeless = driftEffects();
    timeless.cameras[0].length = 0;
    CHECK(!ts2::scene::HoodDrift::load(timeless, 1));
    auto missing = driftEffects();
    missing.effects[1].components[0].index = 3;
    CHECK(!ts2::scene::HoodDrift::load(missing, 1));
}

void sceneRaysMeetWhatIsDrawn() {
    using ts2::scene::SceneRays;
    auto upright = [](float halfWidth, float height) {  // a quad in the x = 0 plane
        ts2::scene::Mesh mesh;
        ts2::scene::Primitive p;
        p.positions = {0, -halfWidth, 0, 0, halfWidth, 0, 0, halfWidth, height, 0, -halfWidth, height};
        p.indices = {0, 1, 2, 0, 2, 3};
        mesh.primitives.push_back(p);
        return mesh;
    };
    ts2::scene::Scene scene;
    scene.meshes = {upright(0.1f, 0.2f), upright(1, 2), upright(1, 3)};
    auto place = [&](int mesh, float x, ts2::scene::Part part, std::uint32_t object) {
        ts2::scene::Instance instance;
        instance.mesh = mesh;
        instance.transform = ts2::scene::Transform::at(x, 0, 0, 0);
        instance.part = part;
        instance.object = object;
        scene.instances.push_back(instance);
        if (object != 0) {
            scene.placedObjects.push_back({.object = object, .instance = scene.instances.size() - 1});
        }
    };
    place(0, 0, ts2::scene::Part::Object, 10);
    place(1, 4, ts2::scene::Part::Object, 20);
    place(2, 2, ts2::scene::Part::Wall, 0);
    SceneRays rays(scene);
    const ts2::scene::ScenePose pose;
    auto r = rays.trace({10, 0, 0}, SceneRays::Point{20, 0, 0}, pose, {});
    CHECK(r.hits.size() == 3 && !r.slotMissing && !r.objectMissing);
    CHECK(r.hits.size() == 3 && r.hits[0].object == 10 && r.hits[0].t < 1e-4f);
    CHECK(r.hits.size() == 3 && r.hits[1].kind == SceneRays::Hit::Kind::Wall && std::abs(r.hits[1].t - 0.5f) < 1e-4f);
    CHECK(r.hits.size() == 3 && r.hits[2].object == 20 && std::abs(r.hits[2].t - 1) < 1e-4f);
    r = rays.trace({10, 0, 0}, SceneRays::Point{20, 0, 0}, pose, {1, 1, 0});  // the wall hidden
    CHECK(r.hits.size() == 2 && r.hits[1].object == 20);
    CHECK(rays.trace({10, 3, 0}, SceneRays::Point{20, 0, 0}, pose, {}).slotMissing);
    CHECK(rays.trace({99, 0, 0}, std::nullopt, pose, {}).objectMissing);
    r = rays.trace({10, 0, 0}, std::nullopt, pose, {});  // ahead: along +y, beside the others
    CHECK(r.hits.empty());
    // An object with no model is a point where it stands, and nothing to meet.
    place(-1, 1, ts2::scene::Part::Object, 30);
    r = rays.trace({30, 0, 0}, SceneRays::Point{20, 0, 0}, pose, {});
    CHECK(r.hits.size() == 2 && !r.objectMissing);
    CHECK(r.hits.size() == 2 && r.hits[0].kind == SceneRays::Hit::Kind::Wall && std::abs(r.hits[0].t - 1.0f / 3) < 1e-4f);
    CHECK(rays.trace({20, 0, 0}, SceneRays::Point{30, 0, 0}, pose, {}).hits.size() == 2);
}

// The tiles the plumbob over a lot is centred on: the lot's tiles less the row along each road side,
// turned with the lot. Pleasantview's lots 11, 68, 49, 16, 18 and 36, whose plumbob centres the game
// draws at x0 + x1 and y0 + y1 times 5.
void effectTilesLeaveOutTheRoadRow() {
    struct Case {
        std::uint32_t x, y;
        std::uint8_t width, depth, facing, ownFacing, roadSides;
        std::int64_t xs, ys;
    };
    const Case cases[] = {{67, 56, 4, 3, 2, 2, 0x04, 137, 115}, {67, 62, 4, 4, 2, 2, 0x04, 137, 128}, {75, 57, 4, 3, 2, 2, 0x04, 153, 117},
                          {78, 51, 3, 2, 0, 2, 0x04, 160, 104}, {78, 54, 3, 2, 0, 2, 0x04, 160, 110}, {84, 66, 3, 3, 0, 0, 0x03, 172, 134}};
    for (const auto& c : cases) {
        ts2::nhood::LotDescription lot;
        lot.x = c.x;
        lot.y = c.y;
        lot.width = c.width;
        lot.depth = c.depth;
        lot.facing = c.facing;
        lot.ownFacing = c.ownFacing;
        lot.roadSides = c.roadSides;
        const auto r = ts2::scene::effectTiles(lot);
        CHECK(r[0] + r[2] == c.xs && r[1] + r[3] == c.ys);
    }
}

std::vector<Test> sceneTests() {
    return {
        {"composite layers use the mip level of their size", compositeLayersUseTheMipLevelOfTheirSize},
        {"composite layers blend as the game", compositeLayersBlendAsTheGame},
        {"Stretched rig scales joint translations", stretchedRigScalesJointTranslations},
        {"Layered clips mix like the game", layeredClipsMixLikeTheGame},
        {"Mirrored clips play on the other side", mirroredClipsPlayOnTheOtherSide},
        {"Clips turn to their own heading", clipsTurnToTheirOwnHeading},
        {"Skin under clothes is named as the game names it", skinUnderClothesNames},
        {"Room samples are gathered as the game gathers them", roomSamplesAreGatheredAsTheGame},
        {"Portable maths matches the C library", portableMathMatchesTheLibrary},
        {"Joints the animation leaves keep the mesh's own rest", unanimatedJointsKeepTheMeshRest},
        {"scene transforms turn and move", sceneTransformsTurnAndMove},
        {"scene quads face their normal", sceneQuadsFaceTheirNormal},
        {"scene hip roof rises to its ridge", sceneHipRoofRisesToItsRidge},
        {"scene greenhouse roof pieces match the game", sceneGreenhouseRoofPiecesMatchTheGame},
        {"scene grid roofs match the game", sceneGridRoofsMatchTheGame},
        {"scene cutaway cuts walls in front of rooms", sceneCutawayCutsWallsInFrontOfRooms},
        {"scene views show storeys, walls and roofs", sceneViewsShowStoreysWallsAndRoofs},
        {"ground is open where it has no surface", groundIsOpenWhereItHasNoSurface},
        {"effect tiles leave out the road row", effectTilesLeaveOutTheRoadRow},
        {"pool water has the game's depth layers", poolWaterHasTheGamesDepthLayers},
        {"attic walls reach the roof's underside", atticWallsReachTheRoofsUnderside},
        {"picker finds the nearest shown triangle", pickerFindsTheNearestShownTriangle},
        {"portrait pose and camera", portraitPoseAndCamera},
        {"animated rotation turns first", animatedRotationTurnsFirst},
        {"channel turns take the short way", channelTurnsTakeTheShortWay},
        {"object materials follow Change Material", objectMaterialsFollowChanges},
        {"scene rays meet what is drawn", sceneRaysMeetWhatIsDrawn},
        {"neighborhood drift starts after rest and wait", hoodDriftStartsAfterRestAndWait},
        {"neighborhood drift restarts from the camera", hoodDriftInterruptRestartsFromTheCamera},
        {"neighborhood drift sways along its curve", hoodDriftSwaysAlongItsCurve},
        {"neighborhood drift wanders within its pulls", hoodDriftWandersWithinItsPulls},
        {"neighborhood drift refuses unusable effects", hoodDriftRefusesUnusableEffects},
    };
}
