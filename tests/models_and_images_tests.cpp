// Pictures, movies, models and animation data.
#include "test_support.hpp"
#include "scene/ground_decal.hpp"
#include "scene/normal_map.hpp"

namespace {

// Data list values of type 1 (a flag byte, any non-zero is true) and 4 (two floats).
void dataListReadsFlagsAndTwoFloatVectors() {
    std::vector<std::uint8_t> d;
    auto u32 = [&d](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    auto str = [&d](const std::string& s) {
        d.push_back(static_cast<std::uint8_t>(s.size()));
        d.insert(d.end(), s.begin(), s.end());
    };
    u32(0);
    u32(1);
    u32(0x6A836D56);
    str("cDataListExtension");
    u32(0x6A836D56);
    u32(1);
    str("cExtension");
    u32(0);
    u32(3);
    d.push_back(7);
    str("root");
    u32(2);
    d.push_back(1);
    str("on");
    d.push_back(5);
    d.push_back(4);
    str("uv");
    u32(std::bit_cast<std::uint32_t>(0.5f));
    u32(std::bit_cast<std::uint32_t>(2.0f));
    const auto lists = ts2::scenegraph::parseDataLists(d);
    CHECK(lists.size() == 1);
    const auto* on = lists[0].find("on");
    const auto* uv = lists[0].find("uv");
    CHECK(on != nullptr && on->type == ts2::scenegraph::DataValue::Type::Bool && on->integer == 1);
    CHECK(uv != nullptr && uv->type == ts2::scenegraph::DataValue::Type::Vector2 && (uv->floats == std::vector<float>{0.5f, 2.0f}));
}

// A shape part ends in a flag byte, a counted list of extra names and a number. Parts after one with
// extra names still read right.
void shapeSkipsPartExtraNames() {
    std::vector<std::uint8_t> d;
    auto u32 = [&d](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    auto str = [&d](const std::string& s) {
        d.push_back(static_cast<std::uint8_t>(s.size()));
        d.insert(d.end(), s.begin(), s.end());
    };
    auto block = [&](const std::string& name, std::uint32_t version) {
        str(name);
        u32(0);
        u32(version);
    };
    u32(0);
    u32(1);
    u32(0xFC6EB1F7);
    block("cShape", 8);
    block("cSGResource", 2);
    str("chair_shpe");
    block("cReferentNode", 1);
    block("cObjectGraphNode", 4);
    u32(0);
    str("chair");
    u32(0);  // detail levels
    u32(0);  // geometry nodes
    u32(2);
    str("body");
    str("body_txmt");
    d.push_back(0);
    u32(2);
    str("a");
    str("bb");
    u32(7);
    str("cushion");
    str("cushion_txmt");
    d.push_back(1);
    u32(0);
    u32(0);
    const auto shape = ts2::scenegraph::parseShape(d);
    CHECK(shape.parts.size() == 2 && shape.parts[1].group == "cushion" && shape.parts[1].material == "cushion_txmt");
}

// Before version 8 a shape's items refer to their geometry node by link (two bytes and the index in
// the links) instead of naming it. Version 6 has no level-of-detail list, as in lot imposters.
void shapeBeforeVersion8LinksItsGeometry() {
    for (const std::uint32_t version : {6u, 7u}) {
        std::vector<std::uint8_t> d;
        auto u32 = [&d](std::uint32_t v) {
            for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        };
        auto str = [&d](const std::string& s) {
            d.push_back(static_cast<std::uint8_t>(s.size()));
            d.insert(d.end(), s.begin(), s.end());
        };
        auto block = [&](const std::string& name, std::uint32_t v) {
            str(name);
            u32(0);
            u32(v);
        };
        u32(1);
        u32(0xFFFFFFFF);
        u32(0xFF753AAF);
        u32(0x7BA3838C);
        u32(1);
        u32(0xFC6EB1F7);
        block("cShape", version);
        block("cSGResource", 2);
        str("imposter_shpe");
        block("cReferentNode", 1);
        block("cObjectGraphNode", 4);
        u32(0);
        str("");
        if (version > 6) {
            u32(0);  // detail levels
        }
        u32(1);
        u32(0);
        d.push_back(1);
        d.push_back(1);
        u32(0);
        u32(1);
        str("walls_1");
        str("wall1material");
        d.push_back(0);
        u32(0);
        u32(0);
        const auto shape = ts2::scenegraph::parseShape(d);
        CHECK(shape.geometryLinks.size() == 1 && shape.geometryLinks[0] == 0u && shape.geometryNodes[0].empty());
        CHECK(shape.parts.size() == 1 && shape.parts[0].material == "wall1material");
    }
}

// Meshes before version 3 store index lists as u32s. Version 1 groups have no bone list.
void oldMeshVersionsParse() {
    for (const std::uint32_t version : {1u, 2u, 3u, 4u}) {
        std::vector<std::uint8_t> d;
        auto u32 = [&d](std::uint32_t v) {
            for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        };
        auto str = [&d](const std::string& s) {
            d.push_back(static_cast<std::uint8_t>(s.size()));
            d.insert(d.end(), s.begin(), s.end());
        };
        auto indexList = [&](std::initializer_list<std::uint16_t> values) {
            u32(static_cast<std::uint32_t>(values.size()));
            for (const auto v : values) {
                if (version < 3) {
                    u32(v);
                } else {
                    d.push_back(static_cast<std::uint8_t>(v));
                    d.push_back(static_cast<std::uint8_t>(v >> 8));
                }
            }
        };
        u32(0);
        u32(1);
        u32(0xAC4F8687);
        str("cGeometryDataContainer");
        u32(0);
        u32(version);
        str("cSGResource");
        u32(0);
        u32(2);
        str("chair_tslocator_gmdc");
        u32(1);  // one element: a position
        u32(1);
        u32(0x5B830781);
        u32(0);
        u32(2);
        u32(0);
        u32(12);
        for (int i = 0; i < 3; ++i) u32(0);
        indexList({0});
        u32(1);  // one link
        indexList({0});
        u32(1);
        u32(1);
        indexList({});
        indexList({});
        indexList({});
        u32(1);  // one group
        u32(2);
        u32(0);
        str("seat");
        indexList({0, 0, 0});
        u32(0xFFFFFFFF);
        if (version > 1) {
            indexList({3, 4});
        }
        u32(0);  // no joints
        const auto mesh = ts2::scenegraph::parseMesh(d);
        CHECK(mesh.version == version && mesh.links.size() == 1 && (mesh.links[0].elements == std::vector<std::uint16_t>{0}));
        CHECK(mesh.groups.size() == 1 && mesh.groups[0].name == "seat" && mesh.groups[0].indices.size() == 3);
        CHECK(mesh.groups[0].bones == (version > 1 ? std::vector<std::uint16_t>{3, 4} : std::vector<std::uint16_t>{}));
    }
}

// Textures before version 9 keep all mip levels in one block, largest first, and have no float
// after each texture (version 7) or no colour either (version 5). Version 7 has the name string.
void oldTextureVersionsParse() {
    for (const std::uint32_t version : {5u, 7u}) {
        std::vector<std::uint8_t> d;
        auto u32 = [&d](std::uint32_t v) {
            for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        };
        auto str = [&d](const std::string& s) {
            d.push_back(static_cast<std::uint8_t>(s.size()));
            d.insert(d.end(), s.begin(), s.end());
        };
        u32(0);
        u32(1);
        u32(0x1C4A276C);
        str("cImageData");
        u32(0);
        u32(version);
        str("cSGResource");
        u32(0);
        u32(2);
        str("wall_txtr");
        u32(2);  // 2 x 2
        u32(2);
        u32(3);  // a byte per pixel
        u32(2);  // two mip levels
        u32(0x3F800000);
        u32(1);  // one texture
        u32(0);
        if (version > 6) {
            str("");
        }
        u32(5);
        for (const std::uint8_t b : std::initializer_list<std::uint8_t>{1, 2, 3, 4, 9}) d.push_back(b);
        if (version >= 6) {
            u32(0xFF808080);
        }
        const auto image = ts2::scenegraph::parseImage(d);
        CHECK(image.mips.size() == 2 && (image.mips[0].data == std::vector<std::uint8_t>{9}));
        CHECK((image.mips[1].data == std::vector<std::uint8_t>{1, 2, 3, 4}));
        d.pop_back();
        bool threw = false;
        try {
            ts2::scenegraph::parseImage(d);
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }
}

// A clip keeps the body stretch it was made for. On another skeleton a joint's translation moves by
// the joint's rest place's change (a climb's 3-storey rise keeps its size, the hips stand lower by as
// much as at rest), translations of joints the rig lacks are scaled, and rotations are left alone.
void clipsFitTheSkeletonsStretch() {
    ts2::scene::Rig adultRig;
    adultRig.nodes.push_back({"root_trans", -1, {0, 0, 1.0f}, {0, 0, 0, 1}});
    const ts2::scene::Rig teenRig = ts2::scene::stretched(adultRig, 0.94f);
    const auto clip = [](float bodyStretch, const char* joint) {
        ts2::anim::Animation a;
        a.bodyStretch = bodyStretch;
        for (const auto kind : {ts2::anim::ChannelKind::Translation, ts2::anim::ChannelKind::Rotation}) {
            ts2::anim::Channel c;
            c.name = joint;
            c.flags = static_cast<std::uint32_t>(kind) << 22 | 0x8000;
            for (int k = 0; k < 3; ++k) {
                c.components.push_back({{{0, 4.0f, 0.5f, 0.5f}}});
            }
            a.channels.push_back(c);
        }
        return a;
    };
    auto adult = clip(1.0f, "root_trans");
    ts2::scene::fitToStretch(adult, teenRig);
    const auto& up = adult.channels[0].components[2].keys[0];
    CHECK(std::fabs(up.value - 3.94f) < 1e-5f && up.in == 0.5f);
    CHECK(adult.channels[0].components[0].keys[0].value == 4.0f);
    CHECK(adult.channels[1].components[2].keys[0].value == 4.0f);
    auto other = clip(1.0f, "no_such_joint");
    ts2::scene::fitToStretch(other, teenRig);
    CHECK(std::fabs(other.channels[0].components[0].keys[0].value - 3.76f) < 1e-5f);
    auto teen = clip(0.94f, "root_trans");
    ts2::scene::fitToStretch(teen, teenRig);
    CHECK(teen.channels[0].components[2].keys[0].value == 4.0f);
}

void limbIkReachesChannelTargets() {
    using ts2::anim::Animation;
    using ts2::scene::Rig;
    const auto curve = [](float v) {
        ts2::anim::Curve c;
        c.keys.push_back({0, v, 0, 0});
        return c;
    };
    const auto channel = [&](const std::string& name, ts2::anim::ChannelKind kind, std::vector<float> values) {
        ts2::anim::Channel c;
        c.name = name;
        c.flags = 15u << 25 | static_cast<std::uint32_t>(kind) << 22 | 0x8000;  // of no IK chain
        for (const float v : values) c.components.push_back(curve(v));
        return c;
    };
    // The angle a joint's world transform turns by, in radians.
    const auto angle = [](const ts2::scene::Transform& t) {
        return std::acos(std::clamp((t.m[0] + t.m[5] + t.m[10] - 1) / 2, -1.0f, 1.0f));
    };
    const auto h = [](const char* s) { return ts2::hash::crc32(s); };
    for (const bool arm : {false, true}) {
        Rig rig;
        rig.nodes.push_back({"skel", -1, {0, 0, 0}, {0, 0, 0, 1}});
        const std::vector<std::string> names = arm ? std::vector<std::string>{"a0", "a1", "a2", "a3", "a4"}
                                                   : std::vector<std::string>{"a0", "a1", "a2"};
        for (std::size_t i = 0; i < names.size(); ++i)
            rig.nodes.push_back({names[i], static_cast<int>(i), {0, i == 0 ? 1.0f : 0.25f, 0}, {0, 0, 0, 1}});
        auto animation = std::make_shared<Animation>();
        animation->channels.push_back(channel("ctr", ts2::anim::ChannelKind::Translation, {0.2f, 1.3f, 0.1f}));
        animation->channels.push_back(channel("ctr", ts2::anim::ChannelKind::Rotation, {0, 0, 90}));
        animation->channels.push_back(channel("pole", ts2::anim::ChannelKind::Translation, {0, 1.4f, 1}));
        ts2::anim::IkChain chain;
        chain.strategy = ts2::anim::kLimbStrategy;
        chain.begin = h("a0");
        chain.end = h(arm ? "a4" : "a2");
        chain.pole = h("pole");
        ts2::anim::IkTarget target;
        target.node = h("ctr");
        target.position = h("ctr");
        chain.targets.push_back(target);
        animation->chains.push_back(chain);
        const auto bound = ts2::scene::bindAnimation(rig, animation);
        CHECK(bound.limbs.size() == 1 && bound.limbs[0].joints.size() == names.size());
        std::vector<ts2::scene::Transform> world;
        ts2::scene::poseRig(rig, &bound, 0, world);
        const auto& end = world.back().m;
        CHECK(std::fabs(end[3] - 0.2f) < 1e-4f && std::fabs(end[7] - 1.3f) < 1e-4f && std::fabs(end[11] - 0.1f) < 1e-4f);
        // Turned 90 degrees about z: the end's x axis points along +y.
        CHECK(std::fabs(end[4] - 1.0f) < 1e-4f);
    }

    // Two targets weighted half and half: the leg reaches between them. With the chain's weight at 0
    // it keeps the animation's own pose (here the rest pose, its end at y 1.5). At half weight it still
    // reaches fully, as no clip turns its joints. With the clip turning its first joint (to rest) it
    // goes only part of the way. A turn of the clip's own counts in full against the limb's weight
    // on the joint (the larger of its targets' 0.5 and its pole's 0.5), so the joint takes a third of
    // the solved turn. A turn tagged with the chain counts by 1 minus the chain's weight: half.
    float solvedAngle = 0;
    for (const float chainWeight : {1.0f, 0.5f, 0.0f}) {
      for (const int clipTurns : {0, 1, 2}) {
        if (clipTurns != 0 && chainWeight != 0.5f) continue;
        Rig rig;
        rig.nodes.push_back({"skel", -1, {0, 0, 0}, {0, 0, 0, 1}});
        for (int i = 0; i < 3; ++i) rig.nodes.push_back({"a" + std::to_string(i), i, {0, i == 0 ? 1.0f : 0.25f, 0}, {0, 0, 0, 1}});
        auto animation = std::make_shared<Animation>();
        animation->channels.push_back(channel("ctr", ts2::anim::ChannelKind::Translation, {0.2f, 1.3f, 0.1f}));
        animation->channels.push_back(channel("ctr2", ts2::anim::ChannelKind::Translation, {0.2f, 1.3f, -0.1f}));
        animation->channels.push_back(channel("ctr_a", ts2::anim::ChannelKind::Scalar, {0.5f}));
        animation->channels.push_back(channel("ctr2_a", ts2::anim::ChannelKind::Scalar, {0.5f}));
        animation->channels.push_back(channel("w", ts2::anim::ChannelKind::Scalar, {chainWeight}));
        animation->channels.push_back(channel("pole", ts2::anim::ChannelKind::Translation, {0, 1.4f, 1}));
        if (clipTurns != 0) {
            auto turn = channel("a0", ts2::anim::ChannelKind::Rotation, {0, 0, 0});
            if (clipTurns == 2) {
                turn.flags &= ~(15u << 25);  // of chain 0
            }
            animation->channels.push_back(turn);
        }
        ts2::anim::IkChain chain;
        chain.strategy = ts2::anim::kLimbStrategy;
        chain.begin = h("a0");
        chain.end = h("a2");
        chain.pole = h("pole");
        chain.extra = h("w");
        for (const char* name : {"ctr", "ctr2"}) {
            ts2::anim::IkTarget target;
            target.position = h(name);
            target.weight = h((std::string(name) + "_a").c_str());
            chain.targets.push_back(target);
        }
        animation->chains.push_back(chain);
        const auto bound = ts2::scene::bindAnimation(rig, animation);
        CHECK(bound.limbs.size() == 1 && bound.limbs[0].targets.size() == 2);
        std::vector<ts2::scene::Transform> world;
        ts2::scene::poseRig(rig, &bound, 0, world);
        const auto& end = world.back().m;
        if (chainWeight == 1.0f) {
            solvedAngle = angle(world[1]);
            CHECK(solvedAngle > 0.1f);
        }
        if (clipTurns != 0) {
            CHECK(std::fabs(end[3] - 0.2f) > 0.01f || std::fabs(end[7] - 1.3f) > 0.01f);
            CHECK(std::fabs(angle(world[1]) - solvedAngle / (clipTurns == 1 ? 3 : 2)) < 1e-3f);
            // The layered mix weighs it the same way.
            std::vector<ts2::scene::Transform> layered;
            ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{{&bound, 0, 1, 0, 0}}, layered);
            CHECK(std::fabs(angle(layered[1]) - angle(world[1])) < 1e-4f);
        } else if (chainWeight > 0) {
            CHECK(std::fabs(end[3] - 0.2f) < 1e-4f && std::fabs(end[7] - 1.3f) < 1e-4f && std::fabs(end[11]) < 1e-4f);
        } else {
            CHECK(std::fabs(end[3]) < 1e-4f && std::fabs(end[7] - 1.5f) < 1e-4f && std::fabs(end[11]) < 1e-4f);
        }
      }
    }

    // Two clips with the same limb, one fading into the other: the limb reaches between their targets
    // by the clips' weights rather than jumping to the newer clip's.
    {
        Rig rig;
        rig.nodes.push_back({"skel", -1, {0, 0, 0}, {0, 0, 0, 1}});
        for (int i = 0; i < 3; ++i) rig.nodes.push_back({"a" + std::to_string(i), i, {0, i == 0 ? 1.0f : 0.25f, 0}, {0, 0, 0, 1}});
        const auto clipReaching = [&](float z) {
            auto animation = std::make_shared<Animation>();
            animation->duration = 1000;
            animation->channels.push_back(channel("ctr", ts2::anim::ChannelKind::Translation, {0.2f, 1.3f, z}));
            animation->channels.push_back(channel("pole", ts2::anim::ChannelKind::Translation, {0, 1.4f, 1}));
            ts2::anim::IkChain chain;
            chain.strategy = ts2::anim::kLimbStrategy;
            chain.begin = h("a0");
            chain.end = h("a2");
            chain.pole = h("pole");
            ts2::anim::IkTarget target;
            target.position = h("ctr");
            chain.targets.push_back(target);
            animation->chains.push_back(chain);
            return ts2::scene::bindAnimation(rig, animation);
        };
        const auto older = clipReaching(0.1f), newer = clipReaching(-0.1f);
        std::vector<ts2::scene::Transform> world;
        ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{{&older, 0, 0.75f, 1, 0}, {&newer, 0, 0.25f, 1, 1}}, world);
        CHECK(std::fabs(world.back().m[11] - 0.05f) < 1e-4f);
    }

    // A clip whose root move is left out (the climb under a walk fading in) still moves the body, but
    // the root goes where the other clip has it.
    {
        Rig rig;
        rig.nodes.push_back({"root_trans", -1, {0, 0, 0}, {0, 0, 0, 1}});
        rig.nodes.push_back({"spine", 0, {0, 0, 0}, {0, 0, 0, 1}});
        const auto clipAt = [&](float rootZ, float spineZ) {
            auto animation = std::make_shared<Animation>();
            animation->duration = 1000;
            animation->channels.push_back(channel("root_trans", ts2::anim::ChannelKind::Translation, {0, 0, rootZ}));
            animation->channels.push_back(channel("spine", ts2::anim::ChannelKind::Translation, {0, 0, spineZ}));
            return ts2::scene::bindAnimation(rig, animation);
        };
        const auto climb = clipAt(4, 1), walk = clipAt(1, 0);
        ts2::scene::LayeredClip held{&climb, 0, 0.5f, 0, 0};
        held.noRootMove = true;
        std::vector<ts2::scene::Transform> world;
        ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{held, {&walk, 0, 0.5f, 0, 1}}, world);
        CHECK(std::fabs(world[0].m[11] - 1) < 1e-4f && std::fabs(world[1].m[11] - 1.5f) < 1e-4f);
    }

    // A clip fading out by its body only keeps reaching where it did in the world. Under one that turns
    // the root its target stays put (a chat's listen clips handing over keep the feet still). When the
    // Sim was placed anew as the latest started (getting into bed handing over to lying there, a
    // quarter turn round) its target moves back by that change of place (placeMove).
    {
        Rig rig;
        rig.nodes.push_back({"skel", -1, {0, 0, 0}, {0, 0, 0, 1}});
        rig.nodes.push_back({"root_trans", 0, {0, 0, 0}, {0, 0, 0, 1}});
        rig.nodes.push_back({"root_rot", 1, {0, 0, 0}, {0, 0, 0, 1}});
        for (int i = 0; i < 3; ++i) rig.nodes.push_back({"a" + std::to_string(i), i == 0 ? 2 : 2 + i, {0, i == 0 ? 1.0f : 0.25f, 0}, {0, 0, 0, 1}});
        auto getIn = std::make_shared<Animation>();
        getIn->duration = 1000;
        getIn->channels.push_back(channel("root_rot", ts2::anim::ChannelKind::Rotation, {0, 0, 0}));
        getIn->channels.push_back(channel("ctr", ts2::anim::ChannelKind::Translation, {0.2f, 1.3f, 0.1f}));
        getIn->channels.push_back(channel("pole", ts2::anim::ChannelKind::Translation, {0, 1.4f, 1}));
        ts2::anim::IkChain chain;
        chain.strategy = ts2::anim::kLimbStrategy;
        chain.begin = h("a0");
        chain.end = h("a2");
        chain.pole = h("pole");
        ts2::anim::IkTarget target;
        target.position = h("ctr");
        chain.targets.push_back(target);
        getIn->chains.push_back(chain);
        auto lie = std::make_shared<Animation>();
        lie->duration = 1000;
        lie->channels.push_back(channel("root_rot", ts2::anim::ChannelKind::Rotation, {0, 0, 90}));
        const auto fading = ts2::scene::bindAnimation(rig, getIn), lying = ts2::scene::bindAnimation(rig, lie);
        ts2::scene::LayeredClip under{&fading, 0, 1, 0, 0};
        under.bodyOnly = true;
        ts2::scene::LayeredClip over{&lying, 0, 1, 1, 1};
        std::vector<ts2::scene::Transform> world;
        ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{under, over}, world);
        // Its target (0.2, 1.3, 0.1) doesn't turn with the root, so the turned arm doesn't reach the
        // turned target (-1.3, 0.2, 0.1).
        CHECK(std::hypot(world.back().m[3] + 1.3f, world.back().m[7] - 0.2f, world.back().m[11] - 0.1f) > 0.05f);
        // The same target after a quarter turn of the Sim's place round z.
        under.placeMove = ts2::scene::Transform::of({0, 0, 0}, {0, 0, 0.70710678f, 0.70710678f});
        ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{under, over}, world);
        CHECK(std::fabs(world.back().m[3] + 1.3f) < 1e-3f && std::fabs(world.back().m[7] - 0.2f) < 1e-3f && std::fabs(world.back().m[11] - 0.1f) < 1e-3f);
    }

    // A clip that started where the Sim no longer stands plays its root there: the whole body moves
    // by the change of place, also when root_trans's own axes are turned from the skeleton's (the
    // game's root has y up under a z-up skeleton).
    {
        Rig rig;
        rig.nodes.push_back({"skel", -1, {0, 0, 0}, {0, 0, 0, 1}});
        rig.nodes.push_back({"root_trans", 0, {0, 0, 0}, {0.70710678f, 0, 0, 0.70710678f}});
        rig.nodes.push_back({"root_rot", 1, {0, 0, 0}, {0, 0, 0, 1}});
        rig.nodes.push_back({"spine", 2, {0, 1, 0.5f}, {0, 0, 0, 1}});
        auto animation = std::make_shared<Animation>();
        animation->duration = 1000;
        animation->channels.push_back(channel("root_trans", ts2::anim::ChannelKind::Translation, {0.3f, 0.9f, 0.1f}));
        animation->channels.push_back(channel("root_rot", ts2::anim::ChannelKind::Rotation, {20, 35, 10}));
        const auto bound = ts2::scene::bindAnimation(rig, animation);
        ts2::scene::LayeredClip clip{&bound, 0, 1, 0, 0};
        std::vector<ts2::scene::Transform> still, moved;
        ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{clip}, still);
        const auto move = ts2::scene::Transform::of({2, -1, 0}, {0, 0, 0.38268343f, 0.92387953f});
        clip.placeMove = move;
        ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{clip}, moved);
        for (const std::size_t i : {std::size_t{2}, std::size_t{3}}) {
            const auto want = move * still[i];
            float off = 0;
            for (std::size_t k = 0; k < 12; ++k) off = std::max(off, std::fabs(want.m[k] - moved[i].m[k]));
            CHECK(off < 1e-4f);
        }
        // Additive root channels are offsets on top of the other clips, so they don't move.
        for (auto& c : animation->channels) c.flags &= ~0x8000u;
        const auto added = ts2::scene::bindAnimation(rig, animation);
        ts2::scene::LayeredClip offset{&added, 0, 1, 0, 0};
        std::vector<ts2::scene::Transform> offsetStill, offsetMoved;
        ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{offset}, offsetStill);
        offset.placeMove = move;
        ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{offset}, offsetMoved);
        float off = 0;
        for (std::size_t i = 0; i < rig.nodes.size(); ++i) {
            for (std::size_t k = 0; k < 12; ++k) off = std::max(off, std::fabs(offsetStill[i].m[k] - offsetMoved[i].m[k]));
        }
        CHECK(off < 1e-6f);
    }

    // An overlay on a higher layer that reaches with one limb (an arm carrying something) leaves the
    // other limbs (the legs of a walk under it) to the clips below.
    {
        Rig rig;
        rig.nodes.push_back({"skel", -1, {0, 0, 0}, {0, 0, 0, 1}});
        for (int i = 0; i < 3; ++i) rig.nodes.push_back({"leg" + std::to_string(i), i, {0, i == 0 ? 1.0f : 0.25f, 0}, {0, 0, 0, 1}});
        for (int i = 0; i < 3; ++i) rig.nodes.push_back({"arm" + std::to_string(i), i == 0 ? 0 : 3 + i, {0, i == 0 ? 1.0f : 0.25f, 0}, {0, 0, 0, 1}});
        const auto clipReaching = [&](const char* first, const char* last, const char* ctr, float z) {
            auto animation = std::make_shared<Animation>();
            animation->duration = 1000;
            animation->channels.push_back(channel(ctr, ts2::anim::ChannelKind::Translation, {0.2f, 1.3f, z}));
            animation->channels.push_back(channel(std::string(ctr) + "pole", ts2::anim::ChannelKind::Translation, {0, 1.4f, 1}));
            ts2::anim::IkChain chain;
            chain.strategy = ts2::anim::kLimbStrategy;
            chain.begin = h(first);
            chain.end = h(last);
            chain.pole = h((std::string(ctr) + "pole").c_str());
            ts2::anim::IkTarget target;
            target.position = h(ctr);
            chain.targets.push_back(target);
            animation->chains.push_back(chain);
            return ts2::scene::bindAnimation(rig, animation);
        };
        const auto walk = clipReaching("leg0", "leg2", "footctr", 0.1f), carry = clipReaching("arm0", "arm2", "handctr", -0.1f);
        std::vector<ts2::scene::Transform> world;
        ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{{&walk, 0, 1, 0, 0}, {&carry, 0, 1, 3, 1}}, world);
        CHECK(std::fabs(world[3].m[11] - 0.1f) < 1e-4f && std::fabs(world[6].m[11] + 0.1f) < 1e-4f);

        // A clip that only turns the arm's joints (a walk's swinging arms) covers a reach with the arm
        // below it (the standing pose's): the arm keeps the turns.
        auto swing = std::make_shared<Animation>();
        swing->duration = 1000;
        swing->channels.push_back(channel("arm0", ts2::anim::ChannelKind::Rotation, {30, 0, 0}));
        const auto swinging = ts2::scene::bindAnimation(rig, swing);
        const auto stand = clipReaching("arm0", "arm2", "handctr", -0.1f);
        std::vector<ts2::scene::Transform> alone;
        ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{{&swinging, 0, 1, 0, 0}}, alone);
        ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{{&stand, 0, 1, -1, 0}, {&swinging, 0, 1, 0, 1}}, world);
        CHECK(std::fabs(world[6].m[3] - alone[6].m[3]) < 1e-4f && std::fabs(world[6].m[7] - alone[6].m[7]) < 1e-4f &&
              std::fabs(world[6].m[11] - alone[6].m[11]) < 1e-4f);
    }
}

// A clip whose limb targets are additive channels moves the others' target by its offset times its own
// weight, without taking a share of the layers.
void limbIkAddsAdditiveTargets() {
    using ts2::anim::Animation;
    const auto channel = [](const std::string& name, ts2::anim::ChannelKind kind, std::vector<float> values, bool additive) {
        ts2::anim::Channel c;
        c.name = name;
        c.flags = static_cast<std::uint32_t>(kind) << 22 | (additive ? 0u : 0x8000u);
        for (const float v : values) {
            ts2::anim::Curve curve;
            curve.keys.push_back({0, v, 0, 0});
            c.components.push_back(curve);
        }
        return c;
    };
    const auto h = [](const char* s) { return ts2::hash::crc32(s); };
    ts2::scene::Rig rig;
    rig.nodes.push_back({"skel", -1, {0, 0, 0}, {0, 0, 0, 1}});
    for (int i = 0; i < 3; ++i) rig.nodes.push_back({"a" + std::to_string(i), i, {0, i == 0 ? 1.0f : 0.25f, 0}, {0, 0, 0, 1}});
    const auto leg = [&](std::vector<float> at, std::vector<float> pole, bool additive) {
        auto animation = std::make_shared<Animation>();
        animation->channels.push_back(channel("ctr", ts2::anim::ChannelKind::Translation, at, additive));
        animation->channels.push_back(channel("pole", ts2::anim::ChannelKind::Translation, pole, additive));
        ts2::anim::IkChain chain;
        chain.strategy = ts2::anim::kLimbStrategy;
        chain.begin = h("a0");
        chain.end = h("a2");
        chain.pole = h("pole");
        ts2::anim::IkTarget target;
        target.node = h("ctr");
        target.position = h("ctr");
        chain.targets.push_back(target);
        animation->chains.push_back(chain);
        return ts2::scene::bindAnimation(rig, animation);
    };
    const auto base = leg({0.2f, 1.3f, 0.1f}, {0, 1.4f, 1}, false);
    const auto shift = leg({0.04f, 0, 0}, {0, 0, 0}, true);
    std::vector<ts2::scene::Transform> world;
    ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{{&base, 0, 1, 0}, {&shift, 0, 0.5f, 1}}, world);
    const auto& end = world.back().m;
    CHECK(std::fabs(end[3] - 0.22f) < 1e-4f && std::fabs(end[7] - 1.3f) < 1e-4f && std::fabs(end[11] - 0.1f) < 1e-4f);

    // An additive turn of the target is in the target's own frame: it comes after the target's turn.
    const auto turned = [&](std::vector<float> at, std::vector<float> turn, bool additive) {
        auto animation = std::make_shared<Animation>();
        animation->channels.push_back(channel("ctr", ts2::anim::ChannelKind::Translation, at, additive));
        animation->channels.push_back(channel("ctr", ts2::anim::ChannelKind::Rotation, turn, additive));
        animation->channels.push_back(channel("pole", ts2::anim::ChannelKind::Translation, {0, additive ? 0.0f : 1.4f, additive ? 0.0f : 1.0f}, additive));
        ts2::anim::IkChain chain;
        chain.strategy = ts2::anim::kLimbStrategy;
        chain.begin = h("a0");
        chain.end = h("a2");
        chain.pole = h("pole");
        ts2::anim::IkTarget target;
        target.node = h("ctr");
        target.position = h("ctr");
        chain.targets.push_back(target);
        animation->chains.push_back(chain);
        return ts2::scene::bindAnimation(rig, animation);
    };
    const auto facing = turned({0.2f, 1.3f, 0.1f}, {0, 90, 0}, false);
    const auto tilt = turned({0, 0, 0}, {30, 0, 0}, true);
    std::vector<ts2::scene::Transform> plain, tilted;
    ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{{&facing, 0, 1, 0}}, plain);
    ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{{&facing, 0, 1, 0}, {&tilt, 0, 1, 1}}, tilted);
    // tilted = plain's turn then 30 degrees about x, so tilted * plain^T turns about plain's x axis:
    // y turned 90 degrees takes x to -z.
    const auto& r0 = plain.back().m;
    const auto& r1 = tilted.back().m;
    const float c = std::cos(0.5235988f), s = std::sin(0.5235988f);
    const float expected[3][3] = {{c, s, 0}, {-s, c, 0}, {0, 0, 1}};
    bool same = true;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            float v = 0;
            for (int k = 0; k < 3; ++k) v += r1[4 * i + k] * r0[4 * j + k];
            same = same && std::fabs(v - expected[i][j]) < 1e-3f;
        }
    }
    CHECK(same);
}

// A target on a named bone of the clip's IK object (a hug's hand on the other Sim's back) is placed
// in that bone's frame. Without the bone it is left out and the limb keeps its own turns.
void limbIkReachesBonesOfTheIkObject() {
    using ts2::anim::Animation;
    const auto h = [](const char* s) { return ts2::hash::crc32(s); };
    ts2::scene::Rig rig;
    rig.nodes.push_back({"skel", -1, {0, 0, 0}, {0, 0, 0, 1}});
    for (int i = 0; i < 3; ++i) rig.nodes.push_back({"a" + std::to_string(i), i, {0, i == 0 ? 1.0f : 0.25f, 0}, {0, 0, 0, 1}});
    auto animation = std::make_shared<Animation>();
    for (const auto& [name, v] : {std::pair<std::string, std::array<float, 3>>{"ctr", {0.2f, 0.3f, 0.1f}}, {"pole", {0, 1.4f, 1}}}) {
        ts2::anim::Channel c;
        c.name = name;
        c.flags = static_cast<std::uint32_t>(ts2::anim::ChannelKind::Translation) << 22 | 0x8000u;
        for (const float x : v) {
            ts2::anim::Curve curve;
            curve.keys.push_back({0, x, 0, 0});
            c.components.push_back(curve);
        }
        animation->channels.push_back(c);
    }
    ts2::anim::IkChain chain;
    chain.strategy = ts2::anim::kLimbStrategy;
    chain.begin = h("a0");
    chain.end = h("a2");
    chain.pole = h("pole");
    ts2::anim::IkTarget target;
    target.kind = 2;
    target.frame = target.frameMirror = h("backtarget_surface");
    target.position = h("ctr");
    chain.targets.push_back(target);
    animation->chains.push_back(chain);
    const auto clip = ts2::scene::bindAnimation(rig, animation);
    ts2::scene::LayeredClip layered{&clip, 0, 1, 0};
    layered.objectBone = [&](std::uint32_t bone) -> std::optional<ts2::scene::Transform> {
        return bone == h("backtarget_surface") ? std::optional(ts2::scene::Transform::at(0.1f, 1, 0, 0)) : std::nullopt;
    };
    std::vector<ts2::scene::Transform> world;
    ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{layered}, world);
    const auto& end = world.back().m;
    CHECK(std::fabs(end[3] - 0.3f) < 1e-4f && std::fabs(end[7] - 1.3f) < 1e-4f && std::fabs(end[11] - 0.1f) < 1e-4f);
    layered.objectBone = [](std::uint32_t) -> std::optional<ts2::scene::Transform> { return std::nullopt; };
    ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{layered}, world);
    CHECK(std::fabs(world.back().m[3]) < 1e-4f && std::fabs(world.back().m[7] - 1.5f) < 1e-4f);
}

// Scenegraph resources are filed under name hashes: instance 0xFF000000 | crc24, high instance
// CRC-32/MPEG-2 of the lowercase name.
// DXT1 blocks hold two 5:6:5 colors and a 2-bit index per pixel.
void lightResourcesAreRead() {
    // A spot light as the game saves one (built by hand): container, the light's classes, its name,
    // then intensity, specular, colour, falloff rate, cone angle, penumbra and range.
    std::vector<std::uint8_t> b;
    auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    auto f32 = [&](float v) { u32(std::bit_cast<std::uint32_t>(v)); };
    auto str = [&](const std::string& s) {
        b.push_back(static_cast<std::uint8_t>(s.size()));
        b.insert(b.end(), s.begin(), s.end());
    };
    auto block = [&](const std::string& name, std::uint32_t version) {
        str(name);
        u32(0);
        u32(version);
    };
    u32(0xFFFF0001);
    u32(0);           // no links
    u32(1);
    u32(0xC9C81BAD);  // one block
    block("cSpotLight", 5);
    block("cStandardLightBase", 11);
    block("cSGResource", 2);
    str("window_light_lght");
    block("cLightT", 1);
    block("cSGResource", 2);
    str("window_light_lght");
    block("cReferentNode", 1);
    block("cObjectGraphNode", 4);
    u32(0);
    b.push_back(0);
    str("window_light");
    for (float v : {1.0f, 1.0f, 1.0f, 0.99362f, 0.772f, 0.4f, 1.65807f, 0.17453f, 2.35293f}) f32(v);
    const auto light = ts2::scenegraph::parseLight(b);
    CHECK(light.kind == "cSpotLight" && light.name == "window_light");
    CHECK((light.color == std::array<float, 3>{1.0f, 0.99362f, 0.772f}));
    CHECK(light.intensity == 1 && light.falloffRate == 0.4f && light.coneAngle == 1.65807f && light.range == 2.35293f);
    // Cut short or of another kind: refused.
    auto cut = b;
    cut.resize(cut.size() - 6);
    auto refused = [](const std::vector<std::uint8_t>& data) {
        try {
            ts2::scenegraph::parseLight(data);
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    CHECK(refused(cut));
    auto other = b;
    other[18] = 'X';  // "cXpotLight"
    CHECK(refused(other));
}

void scenegraphHashesAndTextures() {
    const auto key = ts2::hash::scenegraphKey("fridgeCheap_cres");
    CHECK(key.instance == 0xFF4F7CA0 && key.instanceHigh == 0x128A6A6A);
    const auto lifo = ts2::hash::scenegraphKey("afTopHalter-Dots0_lifo");
    CHECK(lifo.instance == 0x001780DC && lifo.instanceHigh == 0x26227C85);

    const std::vector<std::uint8_t> block = {0x00, 0xF8, 0x1F, 0x00, 0b11100100, 0, 0, 0};  // red, blue, then indices 0 1 2 3
    const auto rgba = ts2::scenegraph::decodeRgba(block, 4, 4, 4);
    auto px = [&](int i) { return std::array<int, 4>{rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3]}; };
    CHECK((px(0) == std::array<int, 4>{255, 0, 0, 255}) && (px(1) == std::array<int, 4>{0, 0, 255, 255}));
    CHECK((px(2) == std::array<int, 4>{170, 0, 85, 255}) && (px(3) == std::array<int, 4>{85, 0, 170, 255}));
    CHECK((px(4) == std::array<int, 4>{255, 0, 0, 255}));  // second row: index 0 again

    // Format 3 is alpha only: black, with each byte as the alpha.
    const std::vector<std::uint8_t> alphas = {0, 155, 255, 7};
    const auto shadow = ts2::scenegraph::decodeRgba(alphas, 2, 2, 3);
    CHECK(shadow[0] == 0 && shadow[3] == 0 && shadow[4] == 0 && shadow[7] == 155 && shadow[11] == 255 && shadow[15] == 7);
    // Format 6 (bump map heights): a byte per pixel, shown as grey.
    const auto bump = ts2::scenegraph::decodeRgba(alphas, 2, 2, 6);
    CHECK(bump[4] == 155 && bump[5] == 155 && bump[6] == 155 && bump[7] == 255 && bump[12] == 7);
    // A size whose pixel count would wrap around to nothing is refused, not decoded into no memory.
    bool refused = false;
    try {
        ts2::scenegraph::decodeRgba(alphas, 0x80000000u, 0x80000000u, 1);
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);
    // A few bytes claiming the largest DXT1 texture are refused before its pixels are allocated.
    refused = false;
    try {
        ts2::scenegraph::decodeRgba(block, ts2::tga::kMaxSide, ts2::tga::kMaxSide, 4);
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);

    // A lot's scene record names the object's model and its material overrides (group: material).
    std::vector<std::uint8_t> record(64, 0);
    auto str = [&](std::string_view t) {
        record.push_back(static_cast<std::uint8_t>(t.size()));
        record.insert(record.end(), t.begin(), t.end());
    };
    auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            record.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    u32(0xFA1C39F7);
    u32(0x11);
    str("cObject");
    str("couchModern");
    u32(1);
    str("couchModern");
    u32(12);
    u32(1);
    str("couchmodern_fabric_blue");
    str("fabric");
    const auto so = ts2::scenegraph::parseSceneObject(record);
    CHECK(so.model == "couchModern" && so.overrides.size() == 1 && so.overrides[0].material == "couchmodern_fabric_blue" &&
          so.overrides[0].group == "fabric");

    // Older records (cObject version 0x10, often inside a cAnimatable block) have no overrides, and
    // a record without entries has none either. The position that follows isn't read as a count.
    record.assign(64, 0);
    u32(0x7D4F2BB8);
    u32(0x0F);
    str("cAnimatable");
    u32(0xFA1C39F7);
    u32(0x10);
    str("cObject");
    str("windowtwotilewideloft");
    u32(1);
    str("windowtwotilewideloft");
    u32(0);
    u32(0x41780000);  // 15.5, the object's x
    const auto older = ts2::scenegraph::parseSceneObject(record);
    CHECK(older.model == "windowtwotilewideloft" && older.overrides.empty());
    record.assign(64, 0);
    u32(0xFA1C39F7);
    u32(0x11);
    str("cObject");
    str("noModel");
    u32(0);
    u32(0x41A14138);  // a position, not an override count
    const auto empty = ts2::scenegraph::parseSceneObject(record);
    CHECK(empty.model == "noModel" && empty.overrides.empty());
}

// Targa images: true color stored bottom row first, and run-length encoded with alpha.
void targaImagesDecode() {
    std::vector<std::uint8_t> plain(18, 0);
    plain[2] = 2;
    plain[12] = 2;
    plain[14] = 2;
    plain[16] = 24;
    // Bottom row then top row, blue-green-red per pixel.
    const std::uint8_t rows[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    plain.insert(plain.end(), std::begin(rows), std::end(rows));
    const auto a = ts2::tga::decode(plain);
    CHECK(a && a->width == 2 && a->height == 2);
    CHECK(!ts2::tga::decode(plain, 1) && !ts2::image::decode(plain, 1) && ts2::image::decode(plain, 2));  // a caller's size limit
    CHECK((std::vector<std::uint8_t>(a->rgba.begin(), a->rgba.begin() + 4) == std::vector<std::uint8_t>{9, 8, 7, 255}));
    CHECK((std::vector<std::uint8_t>(a->rgba.begin() + 8, a->rgba.begin() + 12) == std::vector<std::uint8_t>{3, 2, 1, 255}));

    std::vector<std::uint8_t> packed(18, 0);
    packed[2] = 10;
    packed[12] = 3;
    packed[14] = 1;
    packed[16] = 32;
    packed[17] = 0x28;  // top row first, 8 alpha bits
    const std::uint8_t runs[] = {0x81, 10, 20, 30, 40, 0x00, 50, 60, 70, 80};
    packed.insert(packed.end(), std::begin(runs), std::end(runs));
    const auto b = ts2::tga::decode(packed);
    CHECK(b && b->width == 3 && b->height == 1);
    CHECK((b->rgba == std::vector<std::uint8_t>{30, 20, 10, 40, 30, 20, 10, 40, 70, 60, 50, 80}));
    CHECK(!ts2::tga::decode(std::vector<std::uint8_t>(10, 0)));
    // A header claiming far more pixels than the data can hold is refused before any memory is set
    // aside for them, plain or run-length coded.
    auto huge = packed;
    huge[12] = huge[13] = huge[14] = huge[15] = 0xFF;  // 65535 x 65535
    CHECK(!ts2::tga::decode(huge));
    huge[13] = huge[15] = 0x30;  // 12543 x 12543: within the size limit, still far too few bytes
    CHECK(!ts2::tga::decode(huge));
    huge[2] = 2;  // plain
    CHECK(!ts2::tga::decode(huge));
}

// A neighborhood's moving picture: one 32 x 32 block, a pixel written out then runs of one color,
// added to a black start. The next frame leaves the block as it was, the one after adds to it.
void neighborhoodVideosDecode() {
    std::vector<std::uint8_t> file;
    auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            file.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    auto tag = [&](const char* t) { file.insert(file.end(), t, t + 4); };
    tag("RIFF");
    u32(0);
    tag("Reia");
    tag("head");
    u32(24);
    for (const std::uint32_t v : {1u, 32u, 32u, 10u, 1u, 2u}) {
        u32(v);
    }
    std::vector<std::uint8_t> frame{1, 0x00, 30, 20, 10};  // block changed, one pixel: blue 30, green 20, red 10
    for (int left = 1023; left > 0;) {
        const int n = std::min(left, 129);
        const std::uint8_t px[] = {static_cast<std::uint8_t>(257 - n), 3, 2, 1};
        frame.insert(frame.end(), std::begin(px), std::end(px));
        left -= n;
    }
    tag("frme");
    u32(static_cast<std::uint32_t>(frame.size()));
    file.insert(file.end(), frame.begin(), frame.end());
    if (frame.size() & 1) {
        file.push_back(0);
    }
    tag("frme");
    u32(1);
    file.push_back(0);  // unchanged
    file.push_back(0);
    tag("frme");
    u32(static_cast<std::uint32_t>(frame.size()));
    file.insert(file.end(), frame.begin(), frame.end());
    const auto video = ts2::reia::parse(file);
    CHECK(video.width == 32 && video.height == 32 && video.frames.size() == 3 && video.rateNumerator == 10);
    ts2::tga::Pixels picture;
    CHECK(ts2::reia::decodeFrame(video, 0, picture));
    CHECK((std::vector<std::uint8_t>(picture.rgba.begin(), picture.rgba.begin() + 8) == std::vector<std::uint8_t>{10, 20, 30, 255, 1, 2, 3, 255}));
    CHECK(ts2::reia::decodeFrame(video, 1, picture) && picture.rgba[0] == 10 && picture.rgba[4 * 1023] == 1);
    CHECK(ts2::reia::decodeFrame(video, 2, picture) && picture.rgba[0] == 20 && picture.rgba[2] == 60 && picture.rgba[4 * 1023] == 2);
}

// Animation curves run as cubic Hermite segments between keys, holding the end values outside.
void animationCurvesSample() {
    ts2::anim::Curve flat{{{0, 1, 0, 0}, {100, 1, 0, 0}}};
    CHECK(flat.sample(-5) == 1 && flat.sample(50) == 1 && flat.sample(500) == 1);
    // Slopes equal to the rise make the segment a straight line.
    ts2::anim::Curve line{{{0, 0, 0, 2}, {100, 2, 2, 0}}};
    CHECK(std::abs(line.sample(25) - 0.5f) < 1e-5f && std::abs(line.sample(50) - 1.0f) < 1e-5f);
    // Without slopes it eases in and out.
    ts2::anim::Curve eased{{{0, 0, 0, 0}, {100, 1, 0, 0}}};
    CHECK(std::abs(eased.sample(50) - 0.5f) < 1e-5f && eased.sample(10) < 0.1f);
}

// A tiny baseline JPEG writer for the decoder tests: one quantizer for every coefficient, a DC
// table whose code for size s is s in 4 bits, and an AC table whose code for symbol v is v in 8 bits.
struct JpegWriter {
    std::vector<std::uint8_t> scan;
    int bits = 0, count = 0;
    std::vector<int> predictors;

    void put(int value, int n) {
        for (int i = n - 1; i >= 0; --i) {
            bits = (bits << 1) | ((value >> i) & 1);
            if (++count == 8) {
                scan.push_back(static_cast<std::uint8_t>(bits));
                if (bits == 0xFF) scan.push_back(0);
                bits = count = 0;
            }
        }
    }
    static int sizeOf(int v) {
        int s = 0;
        for (int a = std::abs(v); a; a >>= 1) ++s;
        return s;
    }
    void putValue(int v, int s) {
        if (s) put(v < 0 ? v + (1 << s) - 1 : v, s);
    }
    // One block of a component, its coefficients in natural order.
    void block(std::size_t component, const std::map<int, int>& coefficients) {
        static constexpr int kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                            41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                            30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
        std::array<int, 64> c{};
        for (const auto& [k, v] : coefficients) c[static_cast<std::size_t>(k)] = v;
        predictors.resize(std::max(predictors.size(), component + 1));
        const int diff = c[0] - predictors[component];
        predictors[component] = c[0];
        put(sizeOf(diff), 4);
        putValue(diff, sizeOf(diff));
        int run = 0;
        for (int k = 1; k < 64; ++k) {
            const int v = c[static_cast<std::size_t>(kZigzag[k])];
            if (!v) {
                ++run;
                continue;
            }
            for (; run > 15; run -= 16) put(0xF0, 8);
            put(run << 4 | sizeOf(v), 8);
            putValue(v, sizeOf(v));
            run = 0;
        }
        if (run) put(0, 8);
    }
    // The whole file. Sampling holds each component's h << 4 | v. Its frame header starts at kFrame.
    static constexpr std::size_t kFrame = 2 + 69 + 33 + 276;
    std::vector<std::uint8_t> file(int width, int height, int quant, const std::vector<int>& sampling) {
        while (count) put(1, 1);
        std::vector<std::uint8_t> f{0xFF, 0xD8, 0xFF, 0xDB, 0, 67, 0};
        f.insert(f.end(), 64, static_cast<std::uint8_t>(quant));
        f.insert(f.end(), {0xFF, 0xC4, 0, 31, 0x00, 0, 0, 0, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
        for (int v = 0; v < 12; ++v) f.push_back(static_cast<std::uint8_t>(v));
        f.insert(f.end(), {0xFF, 0xC4, 0x01, 0x12, 0x10, 0, 0, 0, 0, 0, 0, 0, 255, 0, 0, 0, 0, 0, 0, 0, 0});
        for (int v = 0; v < 255; ++v) f.push_back(static_cast<std::uint8_t>(v));
        const auto n = static_cast<std::uint8_t>(sampling.size());
        f.insert(f.end(), {0xFF, 0xC0, 0, static_cast<std::uint8_t>(8 + 3 * n), 8, 0, static_cast<std::uint8_t>(height), 0,
                           static_cast<std::uint8_t>(width), n});
        for (std::uint8_t i = 0; i < n; ++i) f.insert(f.end(), {static_cast<std::uint8_t>(i + 1), static_cast<std::uint8_t>(sampling[i]), 0});
        f.insert(f.end(), {0xFF, 0xDA, 0, static_cast<std::uint8_t>(6 + 2 * n), n});
        for (std::uint8_t i = 0; i < n; ++i) f.insert(f.end(), {static_cast<std::uint8_t>(i + 1), 0x00});
        f.insert(f.end(), {0, 63, 0});
        f.insert(f.end(), scan.begin(), scan.end());
        f.insert(f.end(), {0xFF, 0xD9});
        return f;
    }
};

void jpegDecodesAsTheGame() {
    // Expected blocks come from running the game's JPEG library's own inverse DCT instructions.
    auto grey = [](int quant, const std::map<int, int>& coefficients) {
        JpegWriter w;
        w.block(0, coefficients);
        return ts2::jpeg::decode(w.file(8, 8, quant, {0x11}));
    };
    auto plane = [](const std::optional<ts2::tga::Pixels>& p) {
        std::vector<int> v;
        if (p) {
            for (std::size_t i = 0; i < p->rgba.size(); i += 4) v.push_back(p->rgba[i]);
        }
        return v;
    };
    const std::vector<int> dc{129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129, 129,
                              128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
                              128, 128, 128, 128, 128, 128, 128, 128, 129, 129, 129, 129, 129, 129, 129, 129,
                              128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128};
    CHECK(plane(grey(1, {{0, 4}})) == dc);
    const std::vector<int> busy{144, 91,  64,  68,  56,  11, 0,   19, 114, 128, 113, 48,  35,  41,  43,  8,
                                112, 139, 122, 68,  37,  55, 54,  32, 156, 113, 108, 102, 97,  42,  54,  83,
                                160, 116, 93,  112, 101, 71, 64,  92, 135, 142, 131, 78,  92,  110, 115, 74,
                                121, 141, 127, 86,  81,  112, 114, 86, 142, 109, 108, 118, 112, 73,  88,  126};
    CHECK(plane(grey(1, {{0, -300}, {1, 200}, {8, -150}, {9, 90}, {2, 60}, {16, -45}, {27, 33}, {63, -20}, {7, 15}, {56, -10}, {36, 120}})) ==
          busy);
    // Coefficients times quantizers past 16 bits wrap as the game's do.
    const std::vector<int> wrap{128, 128, 0,   0,   0,   0,   0,   0, 255, 255, 255, 0,   0,   0,   0,   0,
                                255, 255, 255, 0,   0,   0,   0,   0, 255, 255, 255, 0,   0,   0,   0,   0,
                                255, 255, 255, 12,  0,   0,   0,   0, 255, 255, 255, 166, 0,   0,   0,   0,
                                255, 255, 255, 0,   0,   0,   0,   0, 255, 255, 255, 128, 128, 128, 255, 255};
    CHECK(plane(grey(255, {{0, 200}, {1, -130}, {8, 129}, {18, 7}})) == wrap);

    // Colour: four flat luma blocks and one chroma pair spread over them, in a picture smaller than
    // its MCU, turned to RGB with the game's integer steps.
    JpegWriter w;
    for (int dcValue : {80, 160, -80, 0}) w.block(0, {{0, dcValue}});
    w.block(1, {{0, -160}});
    w.block(2, {{0, 240}});
    const auto colourFile = w.file(13, 11, 1, {0x22, 0x11, 0x11});
    const auto colour = ts2::jpeg::decode(colourFile);
    CHECK(colour && colour->width == 13 && colour->height == 11);
    if (colour) {
        const std::array<std::array<int, 3>, 4> expected{{{180, 124, 102}, {190, 134, 112}, {160, 104, 82}, {170, 114, 92}}};
        bool same = true;
        for (std::size_t y = 0; y < 11; ++y) {
            for (std::size_t x = 0; x < 13; ++x) {
                const auto& e = expected[(y >= 8 ? 2u : 0u) + (x >= 8 ? 1u : 0u)];
                const auto* p = &colour->rgba[(y * 13 + x) * 4];
                same = same && p[0] == e[0] && p[1] == e[1] && p[2] == e[2] && p[3] == 255;
            }
        }
        CHECK(same);
        const auto viaImage = ts2::image::decode(colourFile);
        CHECK(viaImage && viaImage->rgba == colour->rgba);
        // Over a caller's size limit: refused by both the exact decoder and stb.
        CHECK(!ts2::jpeg::decode(colourFile, 12) && !ts2::image::decode(colourFile, 12) && ts2::image::decode(colourFile, 13));
    }

    // Broken or unsupported files give nothing. Cut-off data never reads past its end.
    CHECK(!ts2::jpeg::decode({}));
    CHECK(!ts2::jpeg::decode(std::vector<std::uint8_t>{0xFF, 0xD8, 0xFF}));
    constexpr std::size_t frame = JpegWriter::kFrame;
    CHECK(colourFile[frame] == 0xFF && colourFile[frame + 1] == 0xC0);
    auto progressive = colourFile;
    progressive[frame + 1] = 0xC2;
    CHECK(!ts2::jpeg::decode(progressive));
    auto huge = colourFile;
    for (std::size_t i = frame + 5; i < frame + 9; ++i) huge[i] = 0xFF;
    CHECK(!ts2::jpeg::decode(huge));
    // A few hundred bytes claiming 8192 x 8192 are refused by both decoders before allocating.
    auto large = colourFile;
    large[frame + 5] = large[frame + 7] = 0x20;
    large[frame + 6] = large[frame + 8] = 0x00;
    CHECK(!ts2::jpeg::decode(large) && !ts2::image::decode(large));
    std::vector<std::uint8_t> bmp(54, 0);
    bmp[0] = 'B';
    bmp[1] = 'M';
    bmp[10] = 54;            // pixels start
    bmp[14] = 40;            // info header size
    bmp[19] = bmp[23] = 0x20;  // 8192 x 8192
    bmp[26] = 1;             // planes
    bmp[28] = 24;            // bits per pixel
    CHECK(!ts2::image::decode(bmp));
    // A 1 x 1 PNG decodes. The same header claiming 8192 x 8192 asks for more than its few bytes can
    // unpack to, and is refused before allocating.
    const std::vector<std::uint8_t> png{
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
        0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0x64, 0x60, 0xF8, 0x5F, 0x0F, 0x00, 0x02, 0x87, 0x01, 0x80,
        0xEB, 0x47, 0xBA, 0x92, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    const auto pngDecoded = ts2::image::decode(png);
    CHECK(pngDecoded && pngDecoded->width == 1 && pngDecoded->height == 1);
    auto largePng = png;
    largePng[18] = largePng[22] = 0x20;  // 8192 x 8192
    largePng[19] = largePng[23] = 0x00;
    CHECK(!ts2::image::decode(largePng));
    // A 1-bit BMP whose palette holds only white: the pixel naming the missing second colour is black.
    std::vector<std::uint8_t> shortPalette(62, 0);
    shortPalette[0] = 'B';
    shortPalette[1] = 'M';
    shortPalette[10] = 58;  // pixels start after one palette entry
    shortPalette[14] = 40;
    shortPalette[18] = 2;   // 2 x 1
    shortPalette[22] = 1;
    shortPalette[26] = 1;
    shortPalette[28] = 1;   // bits per pixel
    shortPalette[54] = shortPalette[55] = shortPalette[56] = 0xFF;
    shortPalette[58] = 0x40;  // first pixel colour 0, second colour 1
    const auto shortDecoded = ts2::image::decode(shortPalette);
    CHECK((shortDecoded && shortDecoded->rgba == std::vector<std::uint8_t>{255, 255, 255, 255, 0, 0, 0, 255}));
    bool safe = true;
    for (std::size_t n = 0; n < colourFile.size(); ++n) {
        const std::vector<std::uint8_t> cut(colourFile.begin(), colourFile.begin() + static_cast<std::ptrdiff_t>(n));
        const auto p = ts2::jpeg::decode(cut);
        safe = safe && (!p || p->rgba.size() == std::size_t{13} * 11 * 4);
    }
    CHECK(safe);
}

void barPaintingKeepsTheFillShape() {
    ts2::tga::Pixels fill{3, 2, {9, 9, 9, 0, 9, 9, 9, 128, 9, 9, 9, 255, 9, 9, 9, 255, 9, 9, 9, 64, 9, 9, 9, 0}};
    ts2::tga::Pixels gradient{3, 2, {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255, 13, 14, 15, 255, 16, 17, 18, 255}};
    const auto painted = ts2::content::paintBar(fill, gradient, 1);
    CHECK(painted.width == 3 && painted.height == 2);
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(painted.rgba[i * 4] == gradient.rgba[(3 + i % 3) * 4]);
        CHECK(painted.rgba[i * 4 + 3] == fill.rgba[i * 4 + 3]);
    }
    ts2::tga::Pixels broken{4, 4, {1, 2, 3}};
    CHECK(ts2::content::paintBar(broken, gradient, 0).rgba.size() == 3);  // left as it is
    CHECK(ts2::content::pixel(gradient, 9, 9) == (std::array<std::uint8_t, 3>{0, 0, 0}));
}

void moodColourFillsTheBackgroundShape() {
    ts2::tga::Pixels shape{2, 1, {255, 255, 0, 0, 200, 200, 10, 255}};
    const auto filled = ts2::content::fillShape(shape, {1, 203, 57});
    CHECK(filled.rgba == (std::vector<std::uint8_t>{1, 203, 57, 0, 1, 203, 57, 255}));
}

void portraitIsCroppedForThumbnails() {
    ts2::tga::Pixels portrait{256, 256, std::vector<std::uint8_t>(256 * 256 * 4, 0)};
    portrait.rgba[(38 * 256 + 38) * 4] = 7;
    const auto crop = ts2::content::portraitCrop(portrait);
    CHECK(crop.width == 180 && crop.height == 180 && crop.rgba[0] == 7);
    const auto half = ts2::content::portraitCrop(ts2::tga::Pixels{128, 128, std::vector<std::uint8_t>(128 * 128 * 4, 0)});
    CHECK(half.width == 90);
    CHECK(ts2::content::portraitCrop(ts2::tga::Pixels{4, 2, {1}}).width == 4);  // broken: left as it is
    CHECK(ts2::content::placed(ts2::tga::Pixels{1, 1, {1, 2, 3, 4}}, 100000, 100000, 0, 0).rgba.empty());  // too big: nothing
}

// Want target pictures load in the background: unknown until they arrive, at most four at once,
// a failed read leaves none, and forgetting them while loads run doesn't wait for those.
void targetPicturesLoadInTheBackground() {
    std::promise<void> open;
    const std::shared_future<void> gate = open.get_future().share();
    ts2::content::TargetPictures pictures([gate](const std::filesystem::path& file, std::uint32_t) -> std::optional<ts2::tga::Pixels> {
        gate.wait();
        if (file == "broken") {
            throw std::runtime_error("broken file");
        }
        if (file == "none") {
            return std::nullopt;
        }
        return ts2::tga::Pixels{256, 256, std::vector<std::uint8_t>(256 * 256 * 4, 255)};
    });
    CHECK(pictures.load(1, "good", 0x10) && pictures.load(2, "broken", 0x10) && pictures.load(3, "none", 0x10));
    CHECK(pictures.known(1) && pictures.find(1) == nullptr && !pictures.load(1, "good", 0x10));  // loading
    CHECK(pictures.load(4, "good", 0x10) && !pictures.load(5, "good", 0x10));  // four at most
    CHECK(!pictures.collect());  // nothing finished yet
    open.set_value();
    for (int tries = 0; tries < 500 && pictures.loading() > 0; ++tries) {
        pictures.collect();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(pictures.loading() == 0);
    const auto* good = pictures.find(1);
    CHECK(good && good->width == 44 && good->height == 44 && good->rgba[3] == 255);
    CHECK(pictures.known(2) && pictures.find(2) == nullptr && pictures.find(3) == nullptr);
    pictures.make(6, ts2::tga::Pixels{2048, 2048, std::vector<std::uint8_t>(2048u * 2048u * 4u, 255)});
    CHECK(pictures.known(6) && pictures.find(6) == nullptr);  // too large to be a portrait

    // Forgetting while a load runs returns at once, and the load finishes on its own.
    std::promise<void> later;
    const std::shared_future<void> slow = later.get_future().share();
    auto slowToken = std::make_shared<int>(0);
    const std::weak_ptr<int> slowGone = slowToken;
    {
        ts2::content::TargetPictures waiting([slow, slowToken](const std::filesystem::path&, std::uint32_t) -> std::optional<ts2::tga::Pixels> {
            slow.wait();
            return std::nullopt;
        });
        slowToken.reset();
        CHECK(waiting.load(1, "slow", 0x10));
        waiting.clear();
        CHECK(!waiting.known(1) && waiting.loading() == 0);
        later.set_value();  // let it end before the pictures go
    }

    // Going away while a load is stuck returns at once, and the load ends later on its own.
    std::promise<void> stuck;
    const std::shared_future<void> held = stuck.get_future().share();
    std::promise<void> ended;
    auto endedFuture = ended.get_future();
    std::weak_ptr<std::promise<void>> stuckGone;
    {
        auto endedShared = std::make_shared<std::promise<void>>(std::move(ended));
        stuckGone = endedShared;
        ts2::content::TargetPictures leaving([held, endedShared](const std::filesystem::path&, std::uint32_t) -> std::optional<ts2::tga::Pixels> {
            held.wait();
            endedShared->set_value();
            return std::nullopt;
        });
        CHECK(leaving.load(1, "stuck", 0x10));
    }
    stuck.set_value();
    CHECK(endedFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    // Both left-over threads free what they hold after their result is in. The test waits for that,
    // so the debug leak check after it doesn't count memory a finishing thread is about to free.
    for (int tries = 0; tries < 500 && !(slowGone.expired() && stuckGone.expired()); ++tries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(slowGone.expired() && stuckGone.expired());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // the thread's own last frees
}

// Bump heights become the game's normal map, texel for texel: the centre texel of each 3 x 3 image
// has the diagonal neighbours of a texel of wall-brickandmetal-bump, and the colour the game makes
// of it.
void bumpHeightsBecomeTheGamesNormals() {
    struct Case {
        std::uint8_t aboveLeft, aboveRight, belowLeft, belowRight;
        std::uint8_t r, g, b;
    };
    for (const Case& c : {Case{225, 183, 238, 236, 171, 62, 228}, Case{205, 240, 232, 237, 82, 100, 243},
                          Case{164, 179, 135, 168, 76, 170, 236}}) {
        const std::vector<std::uint8_t> heights{c.aboveLeft, 7, c.aboveRight, 9, 200, 3, c.belowLeft, 1, c.belowRight};
        const auto normals = ts2::scene::bumpNormals(heights, 3, 3, 10);
        CHECK(normals.size() == 36);
        CHECK(normals[16] == c.r && normals[17] == c.g && normals[18] == c.b && normals[19] == 255);
    }
    // Flat: straight up. The neighbours wrap around the edges.
    const auto flat = ts2::scene::bumpNormals(std::vector<std::uint8_t>(8, 90), 4, 2);
    CHECK(flat.size() == 32 && flat[0] == 128 && flat[1] == 128 && flat[2] == 255);
    CHECK(ts2::scene::bumpNormals(std::vector<std::uint8_t>(8, 90), 4, 2, 0).empty());
    const auto wrapped = ts2::scene::bumpNormals(std::vector<std::uint8_t>{0, 0, 0, 255}, 4, 1);
    CHECK(wrapped[0] == 253 && wrapped[1] == 128 && wrapped[2] == 153);  // the 255 on the far end is its left neighbour
    CHECK(wrapped[2 * 4] == 2 && wrapped[1 * 4] == 128);
    // Heights that don't fill the size give nothing.
    CHECK(ts2::scene::bumpNormals(std::vector<std::uint8_t>(5, 0), 2, 3).empty());
    CHECK(ts2::scene::bumpNormals({}, 0, 0).empty());
}

// A ground decal lies on the terrain's own triangles, cut to its turned square, its texture across
// the square only.
void groundDecalsAreCutToTheirSquare() {
    ts2::scene::PaintedTerrain t;
    t.columns = t.rows = 4;
    t.tileSize = 10;
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            t.heights.push_back(300 + 0.5f * static_cast<float>(x) * 10 + 0.25f * static_cast<float>(y) * 10);
        }
    }
    const auto p = ts2::scene::groundDecal(t, {15, 15}, 10, 0.125f, 0);
    CHECK(!p.indices.empty() && p.indices.size() % 3 == 0);
    double area = 0;
    for (std::size_t i = 0; i + 2 < p.indices.size(); i += 3) {
        const float* a = &p.positions[p.indices[i] * 3];
        const float* b = &p.positions[p.indices[i + 1] * 3];
        const float* c = &p.positions[p.indices[i + 2] * 3];
        area += std::abs((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])) / 2;
    }
    CHECK(std::abs(area - 100) < 1e-3);
    bool onGround = true, inside = true;
    for (std::size_t v = 0; v * 3 < p.positions.size(); ++v) {
        const float x = p.positions[v * 3], y = p.positions[v * 3 + 1], z = p.positions[v * 3 + 2];
        onGround = onGround && std::abs(z - (300 + 0.5f * x + 0.25f * y)) < 1e-3f;
        const float u = p.uvs[v * 2], w = p.uvs[v * 2 + 1];
        inside = inside && u > -1e-4f && u < 1 + 1e-4f && w > -1e-4f && w < 1 + 1e-4f;
    }
    CHECK(onGround && inside);
    // Past the terrain's edge nothing is left, and broken values give nothing.
    CHECK(ts2::scene::groundDecal(t, {-50, -50}, 10, 0, 0).indices.empty());
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    CHECK(ts2::scene::groundDecal(t, {nan, 15}, 10, 0, 0).indices.empty());
    CHECK(ts2::scene::groundDecal(t, {1e30f, 15}, 10, 0, 0).indices.empty());
    CHECK(ts2::scene::groundDecal(t, {15, 15}, 0, 0, 0).indices.empty());
    CHECK(ts2::scene::groundDecal(t, {15, 15}, inf, 0, 0).indices.empty());
    CHECK(ts2::scene::groundDecal(t, {15, 15}, 10, nan, 0).indices.empty());
    auto tiny = t;
    tiny.tileSize = 1e-30f;
    CHECK(ts2::scene::groundDecal(tiny, {0, 0}, 10, 0, 0).indices.size() <= 9 * 2 * 15);
    auto shortHeights = t;
    shortHeights.heights.pop_back();
    CHECK(ts2::scene::groundDecal(shortHeights, {15, 15}, 10, 0, 0).indices.empty());
}

} // namespace

std::vector<Test> modelAndImageTests() {
    return {
        {"ground decals are cut to their square", groundDecalsAreCutToTheirSquare},
        {"clips fit the skeleton's stretch", clipsFitTheSkeletonsStretch},
        {"Limb IK reaches channel targets", limbIkReachesChannelTargets},
        {"Limb IK adds additive targets", limbIkAddsAdditiveTargets},
        {"Limb IK reaches bones of the IK object", limbIkReachesBonesOfTheIkObject},
        {"JPEG pictures decode as the game decodes them", jpegDecodesAsTheGame},
        {"scenegraph hashes and textures", scenegraphHashesAndTextures},
        {"Light resources are read", lightResourcesAreRead},
        {"data list reads flags and two-float vectors", dataListReadsFlagsAndTwoFloatVectors},
        {"shape skips part extra names", shapeSkipsPartExtraNames},
        {"shape before version 8 links its geometry", shapeBeforeVersion8LinksItsGeometry},
        {"old mesh versions parse", oldMeshVersionsParse},
        {"old texture versions parse", oldTextureVersionsParse},
        {"bump heights become the game's normals", bumpHeightsBecomeTheGamesNormals},
        {"targa images decode", targaImagesDecode},
        {"neighborhood videos decode", neighborhoodVideosDecode},
        {"animation curves sample", animationCurvesSample},
        {"bar painting keeps the fill shape", barPaintingKeepsTheFillShape},
        {"mood colour fills the background shape", moodColourFillsTheBackgroundShape},
        {"portrait is cropped for thumbnails", portraitIsCroppedForThumbnails},
        {"target pictures load in the background", targetPicturesLoadInTheBackground},
    };
}
