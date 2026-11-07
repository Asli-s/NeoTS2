// Building and viewing scenes: lots, Sims, rigs, roofs, walls and views.
#include "render/math.hpp"
#include "test_support.hpp"

#include "../app/object_materials.hpp"
#include "scene/rig_layers.hpp"
#include "scene/sim_builder.hpp"
#include "scene/neighborhood_scene.hpp"
#include "scene/prop_fade.hpp"
#include "scene/hood_drift.hpp"
#include "scene/effect_player.hpp"
#include "scene/object_effects.hpp"
#include "scene/particle_emitter.hpp"
#include "scene/particle_quads.hpp"
#include "scene/look_at.hpp"
#include "sim/look_at.hpp"
#include "scene/scene_ray.hpp"
#include "scene/terrain_water.hpp"

#include <cmath>
#include <filesystem>
#include <limits>
#include <random>

namespace {

// A particle system of one cycle a second that starts its particles still, at the origin, with a life of 10.
ts2::effects::ParticleBlock stillParticles(std::uint64_t flags, float count) {
    ts2::effects::ParticleBlock b;
    b.flags = flags;
    b.life = {10, 10};
    b.delay = {-1, -1};
    b.gap = {-1, -1};
    b.directionLow = b.directionHigh = {0, 0, 1};
    b.count.values = {count};
    b.interval = 1;
    return b;
}

bool near(float a, float b, float within = 1e-4f) { return std::fabs(a - b) <= within; }

// A part playing particle block `index`, facing along the axes.
ts2::effects::Component particlePart(std::int32_t index) {
    ts2::effects::Component c;
    c.type = 1;
    c.index = index;
    c.orientation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    return c;
}

// A file with one effect called `name`, as its levels' records.
void addEffect(ts2::effects::File& file, const std::string& name, ts2::effects::VisualEffect effect) {
    if (effect.level == 0) {
        effect.level = 1;
    }
    file.names[name].push_back(static_cast<std::uint32_t>(file.effects.size()));
    file.effects.push_back(std::move(effect));
}

std::size_t systemsPlaying(const ts2::scene::EffectPlayer& player) {
    std::size_t n = 0;
    player.forEachParticleSystem([&](const auto&, const auto& emitter, const auto&) { n += emitter.running(); });
    return n;
}

void effectsFaceTheCamera() {
    using namespace ts2;
    effects::File file;
    file.particles.push_back(stillParticles(effects::ParticleBlock::kHoldCount, 1));
    effects::VisualEffect facing, turning;
    facing.components = {particlePart(0)};
    facing.flags = effects::VisualEffect::kViewRelative | effects::VisualEffect::kCameraFacing;
    turning.components = {particlePart(0)};
    turning.flags = effects::VisualEffect::kViewRelative;
    addEffect(file, "facing", facing);
    addEffect(file, "turning", turning);

    const auto parentOf = [&](const char* name) {
        scene::EffectPlayer player(file, 1);
        player.setDetail(1);
        player.setCamera({0, -10, 0}, {0, 1, 0}, {0, 0, 1});
        auto effect = player.create(name);
        effect->start(false);
        player.update(0.1f, 0.1f);
        player.update(0.1f, 0.1f);
        // It keeps facing as the camera moves round to +x.
        player.setCamera({10, 0, 0}, {-1, 0, 0}, {0, 0, 1});
        player.update(0.1f, 0.1f);
        scene::EmitterPlace parent;
        player.forEachParticleSystem([&](const auto&, const auto&, const auto& p) { parent = p; });
        return parent;
    };
    // The effect's x points at the camera, z stays up and y goes along the side.
    const auto f = parentOf("facing");
    CHECK(near(f.rotation[0], 1) && near(f.rotation[3], 0) && near(f.rotation[6], 0));
    CHECK(near(f.rotation[2], 0) && near(f.rotation[5], 0) && near(f.rotation[8], 1));
    CHECK(near(f.rotation[4], 1));
    // viewRelative alone turns about the vertical by the camera's heading: looking along -x is half a turn.
    const auto t = parentOf("turning");
    CHECK(near(t.rotation[0], -1) && near(t.rotation[4], -1) && near(t.rotation[8], 1));
}

void effectsPickTheirRecordByLevel() {
    using namespace ts2;
    effects::File file;
    file.particles.push_back(stillParticles(effects::ParticleBlock::kHoldCount, 1));
    effects::VisualEffect low, high;
    low.level = 1;
    high.level = 3;
    high.components = {particlePart(0), particlePart(0)};
    low.components = {particlePart(0)};
    addEffect(file, "smoke", low);
    addEffect(file, "smoke", high);
    effects::VisualEffect onlyHigh;
    onlyHigh.level = 3;
    onlyHigh.components = {particlePart(0)};
    addEffect(file, "sparkle", onlyHigh);

    scene::EffectPlayer player(file, 1);
    player.setDetail(1);
    CHECK(player.create("no such effect") == nullptr);
    auto smoke = player.create("SMOKE");  // the game ignores case
    smoke->start(false);
    player.update(0.1f, 0.1f);
    CHECK(systemsPlaying(player) == 2);
    player.setLevel(1);
    CHECK(systemsPlaying(player) == 1 && smoke->running());
    // A name with no record at this level is taken, but never plays.
    auto sparkle = player.create("sparkle");
    sparkle->start(false);
    player.update(0.1f, 0.1f);
    CHECK(sparkle && !sparkle->running() && systemsPlaying(player) == 1);
}

void effectPartsFollowDetailAndState() {
    using namespace ts2;
    effects::File file;
    file.particles.push_back(stillParticles(effects::ParticleBlock::kHoldCount, 1));
    effects::VisualEffect e;
    e.stateBits = 0x10;
    auto always = particlePart(0);
    auto near_ = particlePart(0);
    near_.lowestDetail = 3;
    auto snowing = particlePart(0);
    snowing.state = 0x10;
    snowing.stateMask = 0x10;
    e.components = {always, near_, snowing};
    addEffect(file, "e", e);

    scene::EffectPlayer player(file, 1);
    player.setDetail(1);
    auto effect = player.create("e");
    effect->start(false);
    player.update(0.1f, 0.1f);
    CHECK(systemsPlaying(player) == 1);
    player.setDetail(3.5f);
    CHECK(systemsPlaying(player) == 2);
    player.setDetail(3.6f);
    player.setState(0x10);
    player.update(0.1f, 0.1f);
    CHECK(systemsPlaying(player) == 3);
    // A part no longer allowed finishes first: its one held particle has a life of 10.
    player.setState(0);
    player.update(0.1f, 0.1f);
    CHECK(systemsPlaying(player) == 3);
    player.update(11, 11);
    CHECK(systemsPlaying(player) == 2);
}

void effectGroupsPlayOneOfTheirParts() {
    using namespace ts2;
    effects::File file;
    file.particles.push_back(stillParticles(effects::ParticleBlock::kHoldCount, 1));
    file.particles.push_back(stillParticles(effects::ParticleBlock::kHoldCount, 2));
    effects::VisualEffect e;
    auto a = particlePart(0);
    a.group = 7;
    a.weight = 0x7FFF;
    auto b = particlePart(1);
    b.group = 7;
    b.weight = 0x7FFF;
    e.components = {a, b};
    addEffect(file, "either", e);

    int firstPicked = 0, secondPicked = 0;
    for (std::uint32_t seed = 1; seed <= 64; ++seed) {
        scene::EffectPlayer player(file, seed * 7919);
        player.setDetail(1);
        auto effect = player.create("either");
        effect->start(false);
        player.update(0.1f, 0.1f);
        CHECK(systemsPlaying(player) == 1);
        player.forEachParticleSystem([&](const auto& block, const auto&, const auto&) { (block.count.values[0] == 1 ? firstPicked : secondPicked)++; });
    }
    CHECK(firstPicked > 8 && secondPicked > 8);
}

void effectsPlacePartsInsideThemselves() {
    using namespace ts2;
    effects::File file;
    file.particles.push_back(stillParticles(effects::ParticleBlock::kHoldCount, 1));
    auto part = particlePart(0);
    part.offset = {1, 0, 0};
    part.scale = 2;
    effects::VisualEffect loose, rigid, detached;
    loose.components = {part};
    rigid.components = {part};
    rigid.flags = effects::VisualEffect::kRigid;
    detached.components = {part};
    detached.flags = effects::VisualEffect::kDetach;
    addEffect(file, "loose", loose);
    addEffect(file, "rigid", rigid);
    addEffect(file, "detached", detached);

    scene::EmitterPlace where;
    where.position = {10, 0, 0};
    where.rotation = {0, -1, 0, 1, 0, 0, 0, 0, 1};  // a quarter turn, so the part's +x goes to +y
    where.scale = 3;

    {
        scene::EffectPlayer player(file, 1);
        player.setDetail(1);
        auto effect = player.create("loose");
        effect->place(where);
        effect->start(false);
        player.update(0.1f, 0.1f);
        player.forEachParticleSystem([&](const auto&, const auto& emitter, const auto& parent) {
            CHECK(near(emitter.where().position[0], 10) && near(emitter.where().position[1], 3) && near(emitter.where().scale, 6));
            CHECK(near(emitter.where().rotation[1], -1));
            CHECK(near(parent.position[0], 0) && near(parent.scale, 1));
        });
    }
    {
        scene::EffectPlayer player(file, 1);
        player.setDetail(1);
        auto effect = player.create("rigid");
        effect->place(where);
        effect->start(false);
        player.update(0.1f, 0.1f);
        player.forEachParticleSystem([&](const auto&, const auto& emitter, const auto& parent) {
            CHECK(near(emitter.where().position[0], 1) && near(emitter.where().scale, 2));
            CHECK(near(parent.position[0], 10) && near(parent.scale, 3));
        });
    }
    {
        scene::EffectPlayer player(file, 1);
        player.setDetail(1);
        auto effect = player.create("detached");
        effect->start(false);
        effect->place(where);
        player.update(0.1f, 0.1f);
        scene::EmitterPlace elsewhere;
        elsewhere.position = {-50, 0, 0};
        effect->place(elsewhere);
        player.forEachParticleSystem([&](const auto&, const auto& emitter, const auto&) { CHECK(near(emitter.where().position[0], 10)); });
    }
}

void effectsEndAsTheirKeywordsSay() {
    using namespace ts2;
    using Block = effects::ParticleBlock;
    effects::File file;
    auto once = stillParticles(Block::kBursts, 1);
    once.life = {1, 1};
    once.burstLimit = 1;
    file.particles.push_back(once);
    file.particles.push_back(stillParticles(Block::kHoldCount, 1));
    effects::VisualEffect plain, endless, hard, side;
    plain.components = {particlePart(0)};
    endless.components = {particlePart(0)};
    endless.flags = effects::VisualEffect::kNoAutoStop;
    hard.components = {particlePart(1)};
    hard.flags = effects::VisualEffect::kHardStop;
    auto ignored = particlePart(1);
    ignored.flags = effects::Component::kIgnoreLength;
    side.components = {particlePart(0), ignored};
    addEffect(file, "plain", plain);
    addEffect(file, "endless", endless);
    addEffect(file, "hard", hard);
    addEffect(file, "side", side);

    scene::EffectPlayer player(file, 1);
    player.setDetail(1);
    auto a = player.create("plain");
    auto b = player.create("endless");
    auto c = player.create("hard");
    auto d = player.create("side");
    a->start(false);
    b->start(false);
    c->start(false);
    d->start(false);
    player.update(0.1f, 0.1f);
    CHECK(a->running() && b->running() && c->running() && d->running());
    player.update(2, 2);
    player.update(0.1f, 0.1f);
    CHECK(!a->running());
    CHECK(b->running());
    // The held particle's part doesn't count, so this ends with the burst, and hard stops take that part too.
    CHECK(!d->running());
    c->stop(false);
    CHECK(!c->running());
    b->stop(false);
    player.update(0.1f, 0.1f);
    CHECK(!b->running());
    // An effect nobody holds any more goes when finished, and one still playing stays.
    const std::size_t before = player.effectCount();
    a.reset();
    player.update(0.1f, 0.1f);
    CHECK(player.effectCount() == before - 1);
}

void effectsPlayOtherEffectsByName() {
    using namespace ts2;
    using Block = effects::ParticleBlock;
    effects::File file;
    auto dying = stillParticles(Block::kBursts, 1);
    dying.life = {0.5f, 0.5f};
    dying.burstLimit = 1;
    dying.deathEffect = "puff";
    file.particles.push_back(dying);
    file.particles.push_back(stillParticles(Block::kHoldCount, 1));

    effects::VisualEffect inner, outer, puff, loop;
    inner.components = {particlePart(1)};
    effects::Component named;
    named.name = "inner";
    named.orientation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    named.offset = {0, 0, 5};
    named.sizeByDetail = {2, 2};
    outer.components = {named};
    puff.components = {particlePart(1)};
    effects::Component self;
    self.name = "loop";
    self.orientation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    loop.components = {self};
    effects::VisualEffect dies;
    dies.components = {particlePart(0)};
    addEffect(file, "inner", inner);
    addEffect(file, "outer", outer);
    addEffect(file, "puff", puff);
    addEffect(file, "loop", loop);
    addEffect(file, "dies", dies);

    scene::EffectPlayer player(file, 1);
    player.setDetail(1);
    auto effect = player.create("outer");
    effect->start(false);
    player.update(0.1f, 0.1f);
    player.update(0.1f, 0.1f);
    CHECK(systemsPlaying(player) == 1);
    player.forEachParticleSystem([&](const auto&, const auto& emitter, const auto& parent) {
        CHECK(near(emitter.where().position[2], 5) && !emitter.particles().empty() && near(parent.scale, 2));
    });
    effect->stop(true);
    player.update(0.1f, 0.1f);

    // An effect that names itself stops at the nesting limit, over as many frames as it takes.
    auto endless = player.create("loop");
    endless->start(false);
    for (int i = 0; i < 40; ++i) {
        player.update(0.1f, 0.1f);
    }
    CHECK(player.effectCount() < 64);
    endless->stop(true);

    // A particle's death effect plays by itself where the particle died, with nobody holding it.
    auto dying2 = player.create("dies");
    dying2->start(false);
    player.update(0.1f, 0.1f);
    player.update(0.1f, 0.1f);
    player.update(1, 1);
    bool puffed = false;
    player.forEachParticleSystem([&](const auto& block, const auto& emitter, const auto&) { puffed = puffed || (block.count.values[0] == 1 && emitter.running()); });
    CHECK(puffed);
}

void metaParticlesPlayAnEffectEach() {
    using namespace ts2;
    using Meta = effects::MetaParticleBlock;
    effects::File file;
    file.particles.push_back(stillParticles(effects::ParticleBlock::kHoldCount, 1));
    effects::VisualEffect spark;
    spark.components = {particlePart(0)};
    addEffect(file, "spark", spark);

    Meta held;
    held.flags = Meta::kHoldCount;
    held.life = {0.5f, 0.5f};
    held.delay = {-1, -1};
    held.gap = {-1, -1};
    held.directionLow = held.directionHigh = {0, 0, 1};
    held.speed = {2, 2};
    held.spawnLow = held.spawnHigh = {0, 0, 1};
    held.count.values = {3};
    held.interval = 1;
    held.size.values = {2};
    held.effect = "spark";
    file.metaParticles.push_back(held);

    effects::Component part;
    part.type = 2;
    part.index = 0;
    part.orientation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    part.offset = {5, 0, 0};
    effects::VisualEffect swarm;
    swarm.components = {part};
    addEffect(file, "swarm", swarm);

    scene::EffectPlayer player(file, 1);
    player.setDetail(1);
    auto effect = player.create("swarm");
    effect->start(false);
    player.update(0.1f, 0.1f);
    player.update(0.1f, 0.1f);
    player.update(0.1f, 0.1f);
    int emitters = 0;
    player.forEachParticleSystem([&](const auto&, const auto& emitter, const auto&) {
        ++emitters;
        // The particle started 1 up, at the part's place, and rises by its speed. The effect takes its size.
        CHECK(near(emitter.where().position[0], 5) && emitter.where().position[2] > 1 && near(emitter.where().scale, 2));
    });
    CHECK(emitters == 3);
    // Hiding the effect hides its particles' effects, and those it makes while hidden.
    const auto showing = [&] {
        int count = 0;
        player.forEachParticleSystem([&](const auto&, const auto&, const auto&) { ++count; });
        return count;
    };
    effect->setHidden(true);
    CHECK(showing() == 0);
    player.update(0.5f, 0.5f);  // the first three die and three more are made
    player.update(0.1f, 0.1f);
    CHECK(showing() == 0);
    effect->setHidden(false);
    CHECK(showing() >= 3);  // the three new ones, and the old ones' effects that are still finishing
    // Each particle lives half a second, so a soft stop ends all of it once their effects finish.
    effect->stop(false);
    for (int i = 0; i < 12; ++i) {
        player.update(1, 1);
    }
    CHECK(!effect->running());
}

void metaParticlesFollowTheirPath() {
    using namespace ts2;
    using Meta = effects::MetaParticleBlock;
    effects::File file;
    file.particles.push_back(stillParticles(effects::ParticleBlock::kHoldCount, 1));
    effects::VisualEffect spark;
    spark.components = {particlePart(0)};
    addEffect(file, "spark", spark);

    // Like a speech balloon's: it rises to its place, stays there, and is gone past the path's end.
    Meta balloon;
    balloon.flags = Meta::kBursts | Meta::kFollowsPath;
    balloon.life = {10, 10};
    balloon.delay = {-1, -1};
    balloon.gap = {-1, -1};
    balloon.count.values = {1};
    balloon.interval = 100;
    balloon.burstLimit = 1;
    balloon.size.values = {1};
    balloon.effect = "spark";
    balloon.path = {{{0.2f, 0, 0.15f}, {}, 0}, {{0.2f, 0, 0.35f}, {}, 1}, {{0.2f, 0, 0.35f}, {}, 2}};
    file.metaParticles.push_back(balloon);
    effects::Component part;
    part.type = 2;
    part.index = 0;
    part.orientation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    effects::VisualEffect holder;
    holder.components = {part};
    addEffect(file, "balloon", holder);

    scene::EffectPlayer player(file, 1);
    player.setDetail(1);
    auto effect = player.create("balloon");
    scene::EmitterPlace at;
    at.position = {10, 20, 30};
    effect->place(at);
    effect->start(false);
    player.update(0.1f, 0.1f);
    player.update(0.1f, 0.1f);
    const auto where = [&] {
        std::array<float, 3> found{};
        player.forEachParticleSystem([&](const auto&, const auto& emitter, const auto&) { found = emitter.where().position; });
        return found;
    };
    player.update(0.4f, 0.4f);  // half way along the first stretch of the path
    player.update(0.1f, 0.1f);  // its effect is made by now
    const auto rising = where();
    CHECK(near(rising[0], 10.2f) && near(rising[1], 20) && rising[2] > 30.15f && rising[2] < 30.35f);
    player.update(1, 1);
    CHECK(near(where()[2], 30.35f));
    // The path comes along when the effect moves.
    at.position = {10, 20, 40};
    effect->place(at);
    player.update(0.1f, 0.1f);
    CHECK(near(where()[2], 40.35f));
    // Past the path's end the particle is gone, so its effect stays where it was left.
    player.update(1, 1);
    at.position = {10, 20, 50};
    effect->place(at);
    player.update(0.1f, 0.1f);
    CHECK(near(where()[2], 40.35f));
}

void attractorsHoldParticlesInPlace() {
    using namespace ts2;
    using Block = effects::ParticleBlock;
    // Like a thought balloon's little circles: a random push, drag, and a steady pull back to their place.
    Block circle = stillParticles(Block::kBursts | Block::kRandomWalk | Block::kAttractor, 1);
    circle.burstLimit = 1;
    circle.life = {40, 40};
    circle.drag = 3;
    circle.walk.retime = {0.55f, 0.65f};
    circle.walk.speed = {0.9f, 1.1f};
    circle.walk.turn = 0.5f;
    Block::Attractor back;
    back.type = 1;
    back.strength.values = {2};
    back.range = 0;
    circle.attractors = {back};
    Block free = circle;
    free.flags &= ~Block::kAttractor;
    free.attractors.clear();

    const auto furthest = [](const Block& block) {
        scene::ParticleEmitter emitter(block, 1);
        scene::EmitterPlace at;
        at.position = {10, 20, 30};
        emitter.place(at);
        emitter.start(false);
        scene::EffectConditions conditions;
        float most = 0;
        for (int i = 0; i < 300; ++i) {
            emitter.update(0.1f, conditions);
            for (const auto& p : emitter.particles()) {
                const float dx = p.position[0] - 10, dy = p.position[1] - 20, dz = p.position[2] - 30;
                most = std::max(most, std::sqrt(dx * dx + dy * dy + dz * dz));
            }
        }
        return most;
    };
    // The pull is twice the push, so a circle stays close and comes back. With no pull it wanders away.
    CHECK(furthest(circle) < 0.5f);
    CHECK(furthest(free) > 1);

    // A strength that runs over the range, the point moving with the emitter, and a kill radius.
    Block fall = stillParticles(Block::kBursts | Block::kAttractor, 1);
    fall.burstLimit = 1;
    fall.life = {40, 40};
    Block::Attractor point;
    point.type = 1;
    point.point = {0, 0, 1};
    point.strength.values = {5};
    point.killRadius = 0.2f;
    fall.attractors = {point};
    scene::ParticleEmitter emitter(fall, 1);
    scene::EmitterPlace at;
    at.scale = 2;
    emitter.place(at);
    emitter.start(false);
    scene::EffectConditions conditions;
    emitter.update(0.1f, conditions);
    CHECK(emitter.particles().size() == 1 && emitter.particles()[0].velocity[2] > 0);
    for (int i = 0; i < 40; ++i) {
        emitter.update(0.1f, conditions);
    }
    CHECK(emitter.particles().empty());
}

void metaParticlesPassOnAPicture() {
    using namespace ts2;
    using Block = effects::ParticleBlock;
    // Like a speech balloon: an effect whose metaparticle makes an effect holding an icon from a row of 16.
    effects::File file;
    Block icon = stillParticles(Block::kBursts | Block::kAcceptsPicture, 1);
    icon.burstLimit = 1;
    icon.life = {10, 10};
    icon.texture = "shared01";
    icon.atlasColumns = 16;
    icon.frameCycle = 1;
    Block plain = icon;
    plain.flags &= ~Block::kAcceptsPicture;
    plain.texture = "balloon";
    file.particles = {icon, plain};
    effects::VisualEffect bundle;
    bundle.components = {particlePart(0), particlePart(1)};
    addEffect(file, "bundle", bundle);

    effects::MetaParticleBlock meta;
    meta.flags = effects::MetaParticleBlock::kHoldCount;
    meta.life = {10, 10};
    meta.delay = {-1, -1};
    meta.gap = {-1, -1};
    meta.count.values = {1};
    meta.interval = 1;
    meta.size.values = {1};
    meta.effect = "bundle";
    file.metaParticles = {meta};
    effects::Component metaPart;
    metaPart.type = 2;
    metaPart.index = 0;
    metaPart.orientation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    effects::VisualEffect balloonEffect;
    balloonEffect.components = {metaPart};
    addEffect(file, "balloon", balloonEffect);

    scene::EffectPlayer player(file, 1);
    player.setDetail(1);
    const auto balloon = player.create("balloon");
    CHECK(balloon != nullptr);
    balloon->start(false);
    balloon->setPicture("shared04");
    balloon->setFirstFrame(9);
    player.update(0.1f, 0.1f);
    player.update(0.1f, 0.1f);
    int seen = 0;
    player.forEachParticleSystem([&](const Block& block, const scene::ParticleEmitter& emitter, const scene::EmitterPlace&) {
        ++seen;
        CHECK(emitter.texture() == (block.texture == "balloon" ? "balloon" : "shared04"));
        CHECK(emitter.firstFrame() == 9);
        for (const auto& p : emitter.particles()) {
            CHECK(p.firstFrame == 9);
        }
    });
    CHECK(seen == 2);
}

void aHardRestartLeavesNoGap() {
    using namespace ts2;
    effects::File file;
    auto held = stillParticles(effects::ParticleBlock::kHoldCount, 1);
    held.prewarm = 2.5f;
    file.particles.push_back(held);
    effects::VisualEffect glow;
    glow.components = {particlePart(0)};
    addEffect(file, "glow", glow);

    scene::EffectPlayer player(file, 1);
    player.setDetail(1);
    const auto showing = [&] {
        std::size_t n = 0;
        player.forEachParticleSystem([&](const auto&, const auto& emitter, const auto&) { n += emitter.particles().size(); });
        return n;
    };
    // As a plumbob's script does: a hard start, then later a hard stop and a new hard start between two updates.
    auto first = player.create("glow");
    first->start(true);
    player.update(0.1f, 0.1f);
    CHECK(showing() == 1);
    first->stop(true);
    auto second = player.create("glow");
    second->start(true);
    player.update(0.01f, 0.01f);
    CHECK(showing() == 1);
    // A hard start plays the prewarm, even with no time passing, and a soft start doesn't.
    second->stop(true);
    auto third = player.create("glow");
    third->start(true);
    player.update(0, 0);
    CHECK(showing() == 1);
    third->stop(true);
    auto fourth = player.create("glow");
    fourth->start(false);
    player.update(0, 0);
    CHECK(showing() == 0);
}

void sequencesPlayTheirItemsInTurn() {
    using namespace ts2;
    using Block = effects::ParticleBlock;
    effects::File file;
    file.particles.push_back(stillParticles(Block::kHoldCount, 1));
    file.particles.push_back(stillParticles(Block::kHoldCount, 2));
    effects::VisualEffect first, second;
    first.components = {particlePart(0)};
    second.components = {particlePart(1)};
    addEffect(file, "first", first);
    addEffect(file, "second", second);
    effects::SequenceBlock block;
    block.items = {{{1, 1}, "first"}, {{1, 1}, "second"}};
    block.flags = effects::SequenceBlock::kLetFinish;
    file.sequences.push_back(block);
    effects::Component part;
    part.type = 6;
    part.index = 0;
    part.orientation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    effects::VisualEffect both;
    both.components = {part};
    addEffect(file, "both", both);

    scene::EffectPlayer player(file, 1);
    player.setDetail(1);
    auto effect = player.create("both");
    effect->start(false);
    player.update(0.1f, 0.1f);
    player.update(0.1f, 0.1f);
    player.update(0.1f, 0.1f);
    const auto playing = [&](float count) {
        bool found = false;
        player.forEachParticleSystem([&](const auto& b, const auto& emitter, const auto&) { found = found || (b.count.values[0] == count && emitter.running() && !emitter.particles().empty()); });
        return found;
    };
    CHECK(playing(1) && !playing(2));
    player.update(1, 1);  // time's up: the second starts
    player.update(0.1f, 0.1f);
    CHECK(playing(2));
    // After the last item the sequence ends, here at once, its effect with it.
    player.update(1, 1);
    CHECK(!playing(2));
    player.update(0.1f, 0.1f);
    CHECK(!effect->running());
}

// A view's matrix and its inverse undo each other, as ambient occlusion finds places from depth with them.
void matrixInverseUndoesIt() {
    using ts2::render::Mat4;
    Mat4 turn;
    turn.m = {0, -1, 0, 3, 1, 0, 0, -2, 0, 0, 1, 5, 0, 0, 0, 1};
    const Mat4 view = Mat4::perspective(0.8f, 1.6f, 0.5f, 900) * turn;
    const Mat4 one = view * view.inverse();
    for (int i = 0; i < 16; ++i) {
        CHECK(std::abs(one.m[static_cast<std::size_t>(i)] - (i % 5 == 0 ? 1.0f : 0.0f)) < 0.001f);
    }
    // A view square on keeps a depth of 1 at the near plane and 0 at the far one.
    const Mat4 square = Mat4::orthographic(-32, 32, -32, 32, 50, 150);
    CHECK(std::abs(square.m[10] * -50 + square.m[11] - 1) < 0.001f && std::abs(square.m[10] * -150 + square.m[11]) < 0.001f);
    CHECK(std::abs(square.m[0] * 32 + square.m[3] - 1) < 0.001f);
    Mat4 flat;
    flat.m.fill(0);
    CHECK(flat.inverse().m[0] == 1 && flat.inverse().m[1] == 0);
}

// Over the particle target new particles thin out by target / count, and they come back as the count falls.
void particleTargetThinsNewParticles() {
    using namespace ts2;
    effects::File file;
    file.particles.push_back(stillParticles(effects::ParticleBlock::kHoldCount, 8));
    effects::VisualEffect e;
    e.components = {particlePart(0)};
    addEffect(file, "smoke", e);
    scene::EffectPlayer player(file, 1);
    player.setDetail(1);
    const auto effect = player.create("smoke");
    effect->start(false);
    player.update(0.1f, 0.1f);
    player.update(0.1f, 0.1f);
    CHECK(player.particleCount() == 8);
    CHECK(near(player.conditions().density, 1));
    player.setParticleTarget(4);
    player.update(0.1f, 0.1f);
    CHECK(near(player.conditions().density, 0.5f));
    player.setParticleTarget(8);
    player.update(0.1f, 0.1f);
    CHECK(near(player.conditions().density, 0.5f));  // 8 * 0.5 is still below the 8 there are
    player.setParticleTarget(100);
    player.update(0.1f, 0.1f);
    CHECK(near(player.conditions().density, 1));
}

// A queue plays one effect at a time, the highest priority next, and drops what has waited too long.
void objectEffectsPlayAQueueInTurn() {
    using namespace ts2;
    using scene::ObjectEffects;
    effects::File file;
    file.particles.push_back(stillParticles(effects::ParticleBlock::kHoldCount, 1));
    effects::VisualEffect e;
    e.components = {particlePart(0)};
    for (const char* name : {"first", "low", "high", "late", "cleared"}) {
        addEffect(file, name, e);
    }
    scene::EffectPlayer player(file, 1);
    player.setDetail(1);
    ObjectEffects objects(player);
    ObjectEffects::Where where;
    where.state = ObjectEffects::Where::State::Here;
    const auto placeOf = [&](std::int16_t, const std::string&) { return where; };
    const auto ask = [&](const char* name, int priority, std::uint64_t expires) {
        objects.startQueued(5, name, "", 0, false, false, priority, expires, "", 0, placeOf);
    };
    ask("first", 1, 100);
    CHECK(objects.recordCount() == 1);  // nothing queued: it starts at once
    ask("low", 1, 100);
    ask("high", 9, 100);
    ask("late", 5, 10);
    CHECK(objects.recordCount() == 1);
    objects.advanceQueues(50, placeOf);
    CHECK(objects.recordCount() == 1);  // the first plays on

    objects.stop(5, "first", "", 0, true);
    objects.advanceQueues(50, placeOf);
    objects.stop(5, "low", "", 0, true);  // not started yet: nothing to stop
    CHECK(objects.recordCount() == 1);
    objects.stop(5, "high", "", 0, true);
    CHECK(objects.recordCount() == 0);
    objects.advanceQueues(50, placeOf);  // "late" has waited too long, so "low" is next
    CHECK(objects.recordCount() == 1);
    objects.stop(5, "low", "", 0, true);
    CHECK(objects.recordCount() == 0);
    objects.advanceQueues(50, placeOf);

    ask("first", 1, 100);
    CHECK(objects.recordCount() == 1);
    ask("cleared", 1, 100);
    objects.clearQueue(5);
    objects.stop(5, "first", "", 0, true);
    objects.advanceQueues(50, placeOf);
    CHECK(objects.recordCount() == 0);
}

void objectEffectsStartStopAndFollow() {
    using namespace ts2;
    using scene::ObjectEffects;
    effects::File file;
    file.particles.push_back(stillParticles(effects::ParticleBlock::kHoldCount, 1));
    effects::VisualEffect e;
    e.components = {particlePart(0)};
    addEffect(file, "smoke", e);
    addEffect(file, "steam", e);
    scene::EffectPlayer player(file, 1);
    player.setDetail(1);
    ObjectEffects objects(player);

    ObjectEffects::Where where;
    where.state = ObjectEffects::Where::State::Here;
    where.place.position = {1, 2, 3};
    const auto placeOf = [&](std::int16_t, const std::string& bone) {
        if (bone == "nothing") {
            ObjectEffects::Where none;
            none.state = ObjectEffects::Where::State::NoBone;
            return none;
        }
        return where;
    };
    // A bone the object lacks starts nothing, and an unknown name neither.
    objects.start(5, "smoke", "nothing", 0, ObjectEffects::Primitive, false, false, placeOf);
    objects.start(5, "no such effect", "", 0, ObjectEffects::Primitive, false, false, placeOf);
    CHECK(objects.recordCount() == 0);

    objects.start(5, "smoke", "", 0, ObjectEffects::Primitive, false, false, placeOf);
    objects.start(5, "steam", "", 0, ObjectEffects::AnyKind, true, false, placeOf);
    objects.start(6, "smoke", "", 0, ObjectEffects::Primitive, false, false, placeOf);
    CHECK(objects.recordCount() == 3);
    player.update(0.1f, 0.1f);
    CHECK(systemsPlaying(player) == 3);

    // They follow their object.
    where.place.position = {7, 8, 9};
    objects.follow(placeOf);
    player.forEachParticleSystem([&](const auto&, const auto& emitter, const auto&) { CHECK(near(emitter.where().position[0], 7)); });

    // A hidden effect goes on but isn't shown.
    where.hidden = true;
    objects.follow(placeOf);
    int shown = 0;
    player.forEachParticleSystem([&](const auto&, const auto&, const auto&) { ++shown; });
    CHECK(shown == 0);
    where.hidden = false;
    objects.follow(placeOf);

    // Stopping one of a kind leaves those with the keep flag and other objects' alone, and a hard stop forgets them.
    objects.stopAll(5, ObjectEffects::AnyKind, true);
    CHECK(objects.recordCount() == 2);
    objects.stop(5, "steam", "", 0, true);
    CHECK(objects.recordCount() == 1);

    // A gone object's effects end.
    where.state = ObjectEffects::Where::State::Gone;
    objects.follow(placeOf);
    CHECK(objects.recordCount() == 0);

    // Effects on an object still loading wait, unseen.
    where.state = ObjectEffects::Where::State::NotReady;
    objects.start(9, "smoke", "", 0, ObjectEffects::Primitive, false, false, placeOf);
    CHECK(objects.recordCount() == 1);
    objects.follow(placeOf);
    CHECK(objects.recordCount() == 1);
}

void effectPartsScaleByDetail() {
    using namespace ts2;
    effects::File file;
    auto flowing = stillParticles(0, 100);
    file.particles.push_back(flowing);
    auto part = particlePart(0);
    part.lowestDetail = 1;
    part.detailEnd = 5;
    part.rateByDetail = {0, 1};  // none at detail 1, all at detail 5
    part.timeScale = 2;
    effects::VisualEffect e;
    e.components = {part};
    addEffect(file, "e", e);

    auto madeAt = [&](float zoom) {
        scene::EffectPlayer player(file, 1);
        player.setDetail(zoom);
        auto effect = player.create("e");
        effect->start(false);
        player.update(0.5f, 0.5f);
        std::size_t n = 0;
        player.forEachParticleSystem([&](const auto&, const auto& emitter, const auto&) { n += emitter.particles().size(); });
        return n;
    };
    CHECK(madeAt(1) == 0);
    // Detail 3 of the range 1 to 5 is half way: half of 100 a second, for one second of time scaled 2.
    const std::size_t half = madeAt(3);
    CHECK(half >= 48 && half <= 52);
    // Between 3 and 4 it blends towards the next level's rate.
    const std::size_t more = madeAt(3.5f);
    CHECK(more >= 60 && more <= 65);
}

void heldParticlesAreReplacedWhenTheyDie() {
    using Block = ts2::effects::ParticleBlock;
    auto b = stillParticles(Block::kHoldCount, 1.2f);  // any part of a particle makes a whole one
    b.life = {1, 1};
    ts2::scene::ParticleEmitter e(b, 1);
    e.start(false);
    e.update(0.5f, {});
    CHECK(e.particles().size() == 2 && near(e.particles()[0].age, 0.5f));
    e.update(0.5f, {});
    CHECK(e.particles().size() == 2 && near(e.particles()[1].age, 0.5f));
    // The density setting has no say.
    ts2::effects::ParticleBlock many = stillParticles(Block::kHoldCount, 50);
    ts2::scene::ParticleEmitter held(many, 1);
    held.start(false);
    ts2::scene::EffectConditions thin;
    thin.density = 0.1f;
    held.update(0.1f, thin);
    CHECK(held.particles().size() == 50);
}

void burstsComeEachCycleAgedByHowLateTheyAre() {
    using Block = ts2::effects::ParticleBlock;
    auto b = stillParticles(Block::kBursts, 3);
    b.life = {5, 5};
    b.interval = 2;
    b.burstLimit = 2;
    ts2::scene::ParticleEmitter e(b, 1);
    e.start(false);
    e.update(0.5f, {});
    CHECK(e.particles().size() == 3 && near(e.particles()[0].age, 0.5f));
    e.update(1.0f, {});
    CHECK(e.particles().size() == 3);
    e.update(0.5f, {});
    CHECK(e.particles().size() == 6 && near(e.particles()[3].age, 0.0f) && near(e.particles()[0].age, 2.0f));
    // The limit reached, it stops itself and ends when the last particle has gone.
    CHECK(e.running());
    e.update(3.0f, {});
    CHECK(e.particles().size() == 3 && e.running());
    e.update(2.0f, {});
    CHECK(e.particles().empty() && !e.running());

    // A burst of up to 20 is always played in full, a larger one follows the density.
    ts2::scene::EffectConditions half;
    half.density = 0.5f;
    auto small = stillParticles(Block::kBursts, 20);
    ts2::scene::ParticleEmitter full(small, 1);
    full.start(false);
    full.update(0.1f, half);
    CHECK(full.particles().size() == 20);
    auto large = stillParticles(Block::kBursts, 21);
    ts2::scene::ParticleEmitter thinned(large, 1);
    thinned.start(false);
    thinned.update(0.1f, half);
    CHECK(thinned.particles().size() == 11);
}

void flowingParticlesFollowTheRateOverACycle() {
    using Block = ts2::effects::ParticleBlock;
    auto b = stillParticles(0, 30);
    b.life = {100, 100};
    ts2::scene::ParticleEmitter e(b, 1);
    e.start(false);
    e.update(0.5f, {});
    CHECK(e.particles().size() >= 14 && e.particles().size() <= 15);
    ts2::scene::EffectConditions half;
    half.density = 0.5f;
    e.update(0.5f, half);
    CHECK(e.particles().size() >= 21 && e.particles().size() <= 23);

    // Once it has played its cycles it stops, unless it holds the last. A step of 1/15 second is a little
    // over that as a float, so the 15th step already ends the cycle, before making its particles.
    auto limited = b;
    limited.burstLimit = 1;
    ts2::scene::ParticleEmitter once(limited, 1);
    once.start(false);
    once.update(3.0f, {});
    CHECK(once.particles().size() >= 27 && once.particles().size() <= 28);
    limited.flags = Block::kSustain;
    ts2::scene::ParticleEmitter kept(limited, 1);
    kept.start(false);
    kept.update(3.0f, {});
    CHECK(kept.particles().size() >= 89 && kept.particles().size() <= 90);

    // New particles spread back along the way the emitter moved, unless it jumped.
    ts2::scene::ParticleEmitter moving(b, 1);
    moving.start(false);
    moving.place({{1, 0, 0, 0, 1, 0, 0, 0, 1}, 1, {0.9f, 0, 0}});
    moving.update(1.0f, {});
    float lowest = 1, highest = 0;
    for (const auto& p : moving.particles()) {
        lowest = std::min(lowest, p.position[0]);
        highest = std::max(highest, p.position[0]);
    }
    CHECK(lowest < 0.1f && highest > 0.8f);
    ts2::scene::ParticleEmitter jumping(b, 1);
    jumping.start(false);
    jumping.place({{1, 0, 0, 0, 1, 0, 0, 0, 1}, 1, {5, 0, 0}});
    jumping.update(1.0f, {});
    CHECK(!jumping.particles().empty() && near(jumping.particles().front().position[0], 5));
}

void particlesMoveAsTheGameMovesThem() {
    using Block = ts2::effects::ParticleBlock;
    // In one step: speed changes by the force and the drag, the position by the mean of the two speeds.
    auto b = stillParticles(Block::kHoldCount, 1);
    b.speed = {3, 3};
    b.gravity = {0, 0, -2};
    ts2::scene::ParticleEmitter fall(b, 1);
    fall.start(false);
    fall.update(1, {});
    CHECK(near(fall.particles()[0].velocity[2], 1) && near(fall.particles()[0].position[2], 2));
    fall.update(1, {});
    CHECK(near(fall.particles()[0].velocity[2], -1) && near(fall.particles()[0].position[2], 2));

    b.gravity = {};
    b.drag = 0.02f;
    ts2::scene::ParticleEmitter slowing(b, 1);
    slowing.start(false);
    slowing.update(1, {});
    CHECK(near(slowing.particles()[0].velocity[2], 2.94f) && near(slowing.particles()[0].position[2], 2.97f));

    // More than 5% of its speed lost in a step: steps of at most 1/15 second, position first.
    b.drag = 0.9f;
    ts2::scene::ParticleEmitter stepping(b, 1);
    stepping.start(false);
    stepping.update(0.1f, {});
    CHECK(near(stepping.particles()[0].position[2], 0.294f) && near(stepping.particles()[0].velocity[2], 2.7354f));
}

void newParticlesStartWhereTheBlockAndPlaceSay() {
    using Block = ts2::effects::ParticleBlock;
    // The place turns, scales and moves the start, and only turns the speed.
    auto b = stillParticles(Block::kHoldCount, 1);
    b.spawnLow = b.spawnHigh = {1, 0, 0};
    b.directionLow = b.directionHigh = {1, 0, 0};
    b.speed = {1, 1};
    ts2::scene::ParticleEmitter e(b, 1);
    e.place({{0, -1, 0, 1, 0, 0, 0, 0, 1}, 2, {10, 0, 0}});
    e.start(false);
    e.update(1e-4f, {});
    const auto& p = e.particles()[0];
    CHECK(near(p.position[0], 10) && near(p.position[1], 2, 1e-3f) && near(p.velocity[1], 1) && near(p.velocity[0], 0));

    // No direction at all flies along x.
    auto still = stillParticles(Block::kHoldCount, 1);
    still.directionLow = still.directionHigh = {0, 0, 0};
    still.speed = {2, 2};
    ts2::scene::ParticleEmitter along(still, 1);
    along.start(false);
    along.update(1e-4f, {});
    CHECK(near(along.particles()[0].velocity[0], 2));

    // A blast pushes away from its centre, on top of the speed.
    still.speed = {0, 0};
    still.spawnLow = still.spawnHigh = {3, 0, 0};
    still.blast = 5;
    ts2::scene::ParticleEmitter blasted(still, 1);
    blasted.start(false);
    blasted.update(1e-4f, {});
    CHECK(near(blasted.particles()[0].velocity[0], 5));

    // A particle flying from where it started heads away from the middle.
    auto outward = stillParticles(Block::kHoldCount | Block::kDirectionIsSpawn, 1);
    outward.spawnLow = outward.spawnHigh = {0, 4, 0};
    outward.speed = {1, 1};
    ts2::scene::ParticleEmitter away(outward, 1);
    away.start(false);
    away.update(1e-4f, {});
    CHECK(near(away.particles()[0].velocity[1], 1));

    // A ball and a torus fill the spawn box.
    auto ball = stillParticles(Block::kHoldCount | Block::kBall, 300);
    ball.spawnLow = {-1, -1, -1};
    ball.spawnHigh = {1, 1, 1};
    ts2::scene::ParticleEmitter inBall(ball, 1);
    inBall.start(false);
    inBall.update(1e-4f, {});
    CHECK(inBall.particles().size() == 300);
    for (const auto& q : inBall.particles()) {
        CHECK(std::hypot(q.position[0], q.position[1], q.position[2]) <= 1.001f);
    }
    auto torus = stillParticles(Block::kHoldCount, 300);
    torus.spawnLow = ball.spawnLow;
    torus.spawnHigh = ball.spawnHigh;
    torus.ring = 0.5f;
    ts2::scene::ParticleEmitter inTorus(torus, 1);
    inTorus.start(false);
    inTorus.update(1e-4f, {});
    for (const auto& q : inTorus.particles()) {
        const float across = std::hypot(q.position[0], q.position[1]);
        CHECK(across >= 0.499f && across <= 1.001f && std::fabs(q.position[2]) <= 0.5f);
    }
}

void particleStopsFollowTheirKind() {
    using Block = ts2::effects::ParticleBlock;
    auto b = stillParticles(Block::kHoldCount, 2);
    b.life = {1, 1};
    b.deathEffect = "smoke";
    // Soft: no new ones, the living finish and play their death effect.
    ts2::scene::ParticleEmitter soft(b, 1);
    soft.start(false);
    soft.update(0.1f, {});
    soft.stop(false);
    soft.update(0.1f, {});
    CHECK(soft.running() && soft.particles().size() == 2);
    soft.update(1, {});
    CHECK(!soft.running() && soft.takeDeaths().size() == 2 && soft.takeDeaths().empty());
    // Hard: gone at once, with no death effects.
    ts2::scene::ParticleEmitter hard(b, 1);
    hard.start(false);
    hard.update(0.1f, {});
    hard.stop(true);
    CHECK(!hard.running() && hard.particles().empty() && hard.takeDeaths().empty());
    // A cycle that kills ends the last cycle's particles, death effects and all.
    auto kills = stillParticles(Block::kBursts | Block::kCycleKill, 1);
    kills.deathEffect = "smoke";
    ts2::scene::ParticleEmitter cycles(kills, 1);
    cycles.start(false);
    cycles.update(1, {});
    CHECK(cycles.particles().size() == 1 && cycles.takeDeaths().size() == 1);
}

void particleStartsWaitAndPrewarm() {
    using Block = ts2::effects::ParticleBlock;
    // Prewarm is played by the first update, with emission a little short of it.
    auto b = stillParticles(Block::kHoldCount, 1);
    b.life = {100, 100};
    b.prewarm = 10;
    ts2::scene::ParticleEmitter warm(b, 1);
    warm.start(true);
    warm.update(0.1f, {});
    CHECK(near(warm.particles()[0].age, 9.99f, 1e-3f));
    ts2::scene::ParticleEmitter cold(b, 1);
    cold.start(false);
    cold.update(0.1f, {});
    CHECK(near(cold.particles()[0].age, 0.1f));
    // A start with prewarm doesn't restart a block with a cycle limit.
    auto limited = b;
    limited.burstLimit = 1;
    ts2::scene::ParticleEmitter refused(limited, 1);
    refused.start(true);
    CHECK(!refused.running());

    auto waits = stillParticles(Block::kHoldCount, 1);
    waits.delay = {1, 1};
    ts2::scene::ParticleEmitter later(waits, 1);
    later.start(false);
    later.update(0.5f, {});
    CHECK(later.particles().empty());
    later.update(1, {});
    CHECK(later.particles().size() == 1 && near(later.particles()[0].age, 0.5f));
}

void particleWalkPushesThemAbout() {
    using Block = ts2::effects::ParticleBlock;
    auto b = stillParticles(Block::kHoldCount | Block::kRandomWalk, 1);
    b.walk.retime = {100, 100};
    b.walk.speed = {2, 2};
    ts2::scene::ParticleEmitter e(b, 7);
    e.start(false);
    e.update(1, {});
    const auto& v = e.particles()[0].velocity;
    CHECK(near(std::hypot(v[0], v[1], v[2]), 2));
    // A retime of 0 in a broken file doesn't hang.
    b.walk.retime = {0, 0};
    ts2::scene::ParticleEmitter stuck(b, 7);
    stuck.start(false);
    stuck.update(1, {});
    CHECK(stuck.particles().size() == 1);
}

// The six corners of one sprite of a block, with a camera looking along +y with z up.
std::vector<ts2::scene::ParticleVertex> oneSprite(const ts2::effects::ParticleBlock& b, const ts2::scene::EmitterPlace& parent = {}) {
    ts2::scene::ParticleEmitter e(b, 1);
    e.start(false);
    e.update(1e-5f, {});
    std::vector<ts2::scene::ParticleVertex> out;
    ts2::scene::appendParticleQuads(b, e, parent, {}, out);
    return out;
}

ts2::effects::ParticleBlock spriteBlock(float size, float aspect = 1) {
    using Block = ts2::effects::ParticleBlock;
    auto b = stillParticles(Block::kHoldCount, 1);
    b.size.values = {size};
    b.aspect.values = {aspect};
    b.alpha.values = {1};
    return b;
}

bool cornerIs(const ts2::scene::ParticleVertex& v, float x, float y, float z) {
    return near(v.position[0], x, 1e-3f) && near(v.position[1], y, 1e-3f) && near(v.position[2], z, 1e-3f);
}

void particleSpritesFaceAndSizeAsTheGame() {
    using Block = ts2::effects::ParticleBlock;
    // Size is the height, the aspect narrows the width. The texture's top is at the sprite's top.
    auto sprite = oneSprite(spriteBlock(2, 0.5f));
    CHECK(sprite.size() == 6);
    CHECK(cornerIs(sprite[0], -0.5f, 0, 1) && sprite[0].uv == (std::array<float, 2>{0, 0}));
    CHECK(cornerIs(sprite[1], -0.5f, 0, -1) && sprite[1].uv == (std::array<float, 2>{0, 1}));
    CHECK(cornerIs(sprite[2], 0.5f, 0, -1) && sprite[2].uv == (std::array<float, 2>{1, 1}));
    CHECK(cornerIs(sprite[5], 0.5f, 0, 1) && sprite[5].uv == (std::array<float, 2>{1, 0}));

    // A quarter turn, in full turns: the top goes toward -x. The aspect narrows it first...
    auto turning = spriteBlock(2, 0.5f);
    turning.rotation.values = {0.25f};
    auto turned = oneSprite(turning);
    CHECK(cornerIs(turned[0], -0.5f, 0, -1));
    CHECK(near(turned[5].position[0], turned[0].position[0], 1e-3f) && near(turned[5].position[2] - turned[0].position[2], 2));
    // ...or after, along the turned width.
    turning.flags |= Block::kAspectAfterTurn;
    auto late = oneSprite(turning);
    CHECK(near(std::fabs(late[5].position[2] - late[0].position[2]), 1));

    // Pivots add to the middle, in shares of its size.
    auto moved = spriteBlock(2);
    moved.pivots = {{0.5f, 0, 0}};
    auto pivoted = oneSprite(moved);
    CHECK(cornerIs(pivoted[1], -2, 0, -1));

    // A ground sprite's top runs along the direction particles leave in, flat.
    auto ground = spriteBlock(2);
    ground.alignment = Block::Ground;
    ground.directionLow = ground.directionHigh = {1, 0, 0};
    auto flat = oneSprite(ground);
    CHECK(cornerIs(flat[0], 1, 1, 0) && cornerIs(flat[1], -1, 1, 0));

    // A sprite that stretches runs along its flight, longer past the block's speed.
    auto stretching = spriteBlock(1);
    stretching.directionLow = stretching.directionHigh = {1, 0, 0};
    stretching.speed = {4, 4};
    stretching.stretch = 2;
    auto long1 = oneSprite(stretching);
    CHECK(near(long1[0].position[0] - long1[1].position[0], 2, 1e-3f) && near(long1[0].position[2], long1[1].position[2], 1e-3f));

    // The parent's scale and place carry it.
    auto carried = oneSprite(spriteBlock(2), {{1, 0, 0, 0, 1, 0, 0, 0, 1}, 2, {10, 0, 0}});
    CHECK(cornerIs(carried[1], 8, 0, -2));

    CHECK(ts2::scene::particleDrawLayer(spriteBlock(1)) == 17);
    auto layered = spriteBlock(1);
    layered.layer = 7;
    CHECK(ts2::scene::particleDrawLayer(layered) == 73);
    layered.layer = 3;
    layered.flags |= Block::kDirectLayer;
    CHECK(ts2::scene::particleDrawLayer(layered) == 13);
    layered.layer = 0;
    CHECK(ts2::scene::particleDrawLayer(layered) == 0);
}

void particleSpritesTakeTheirFrameAndColour() {
    // Frames go along a row, then down, and wrap over the cycle from the first frame.
    auto b = spriteBlock(1);
    b.atlasColumns = 4;
    b.atlasRows = 2;
    b.framesPerSecond = 2;
    b.firstFrame = 1;
    ts2::scene::ParticleEmitter e(b, 1);
    e.start(false);
    e.update(2.5f, {});  // frame 2 * 2.5 + 1 = 6: the third of the second row
    std::vector<ts2::scene::ParticleVertex> out;
    ts2::scene::appendParticleQuads(b, e, {}, {}, out);
    CHECK(near(out[0].uv[0], 0.5f) && near(out[0].uv[1], 0.5f) && near(out[2].uv[0], 0.75f) && near(out[2].uv[1], 1));
    b.frameCycle = 3;
    out.clear();
    ts2::scene::appendParticleQuads(b, e, {}, {}, out);
    CHECK(near(out[0].uv[0], 0.75f) && near(out[0].uv[1], 0));  // (6 - 1) % 3 + 1 = 3, the last of the first row

    // Colour blends over the life, in 256ths kept below 256.
    auto colour = spriteBlock(1);
    colour.colours = {{1, 0, 0}, {0, 0, 1}};
    colour.alpha.values = {0.5f};
    ts2::scene::ParticleEmitter c(colour, 1);
    c.start(false);
    c.update(5, {});
    out.clear();
    ts2::scene::appendParticleQuads(colour, c, {}, {}, out);
    CHECK((out[0].colour == std::array<std::uint8_t, 4>{128, 0, 128, 128}));

    // Toward the screen's edge, by a slope and a base from the block.
    auto fading = spriteBlock(2);
    fading.screenEdge = {48, 51, 0, 255};
    auto middle = oneSprite(fading);
    CHECK(middle[0].colour[3] == 255);
    fading.spawnLow = fading.spawnHigh = {5, 0, 0};
    auto outside = oneSprite(fading);
    CHECK(outside[0].colour[3] == 51);
    CHECK(near(outside[0].position[2] - outside[1].position[2], 2));
}

void brokenParticleBlocksStayWithinLimits() {
    using Block = ts2::effects::ParticleBlock;
    auto notNumber = stillParticles(Block::kHoldCount, 1);
    notNumber.life[0] = std::numeric_limits<float>::quiet_NaN();
    ts2::scene::ParticleEmitter none(notNumber, 1);
    none.start(false);
    none.update(1, {});
    CHECK(none.particles().empty());

    auto flood = stillParticles(0, 1e30f);
    ts2::scene::ParticleEmitter flowing(flood, 1);
    flowing.start(false);
    flowing.update(1, {});
    CHECK(flowing.particles().size() <= 10000);

    auto rapid = stillParticles(Block::kBursts, 1);
    rapid.interval = 0;
    rapid.life = {1e6f, 1e6f};
    ts2::scene::ParticleEmitter bursting(rapid, 1);
    bursting.start(false);
    bursting.update(1000, {});
    CHECK(bursting.particles().size() <= 10000);
}

// The Fade Distance's rule, as the game works it out each frame in lot view. Nothing fades while the
// camera is over the lot (its far edges included). From beyond the lot, a prop fades when it is
// nearer than the lot's middle, to (distance squared over Fade Distance squared) to the fourth.
void propsFadeAsTheGameFadesThem() {
    namespace propfade = ts2::scene::propfade;
    const std::array<float, 2> lot{30, 40};
    CHECK(!propfade::reach({0, 0, 15}, lot) && !propfade::reach({30, 40, 15}, lot) && !propfade::reach({12, 20, 15}, lot));
    // South of the lot: only the distance along y to the lot's middle counts.
    const std::array<float, 3> eye{10, -20, 15};
    const auto reach = propfade::reach(eye, lot);
    CHECK(reach && *reach == 1600);
    if (!reach) {
        return;
    }
    const float fade = 50.0f * 50.0f;
    // To the box's middle on both axes (dx -4, dy 9), and the camera 11 above its 4 high top.
    const propfade::Box bush{{5, -12, 0}, {7, -10, 4}};
    const auto a = propfade::alpha(bush, eye, *reach, fade);
    CHECK(a && std::fabs(*a - static_cast<float>(std::pow(218.0 / 2500.0, 4.0))) < 1e-9f);
    // A camera within the box's span on an axis counts nothing along it. Below a tall prop's top
    // its height is left out.
    const propfade::Box tree{{8, -14, 0}, {12, -12, 25}};
    const auto b = propfade::alpha(tree, eye, *reach, fade);
    CHECK(b && std::fabs(*b - static_cast<float>(std::pow(49.0 / 2500.0, 4.0))) < 1e-9f);
    // Nothing fades beyond the lot's middle or the Fade Distance, and a prop below the lot's level
    // is left as it was.
    CHECK(propfade::alpha({{5, 30, 0}, {7, 32, 4}}, eye, *reach, fade) == 1.0f);
    CHECK(propfade::alpha({{5, -12, 0}, {7, -10, 4}}, eye, *reach, 100.0f) == 1.0f);
    CHECK(!propfade::alpha({{5, -12, -3}, {7, -10, -1}}, eye, *reach, fade));
    // The camera inside a prop's box makes it all but invisible.
    CHECK(propfade::alpha({{9, -21, 10}, {11, -19, 16}}, eye, *reach, fade) == 0.0f);
}

// Lot view fades only the plants and decorations taller than 15 or with ground within 3 tiles of
// the lot. These are the game's own trees around a 3 by 3 tile lot at tiles (56, 43): each side of a
// ground rectangle less than 0.07 of a tile short of the next tile line counts as on it.
void onlyPropsNearTheLotFade() {
    namespace propfade = ts2::scene::propfade;
    const std::array<int, 4> lot{56, 43, 59, 46};
    const propfade::Box low{{0, 0, -9}, {1, 1, 1}};
    // Its west side 0.06 short of line 63 starts there, past the area's last line (62).
    CHECK((propfade::groundTiles({629.386f, 403.201f, 637.829f, 410.671f}) == std::array<int, 4>{63, 40, 64, 41}));
    CHECK(!propfade::fadesInLotView(low, propfade::groundTiles({629.386f, 403.201f, 637.829f, 410.671f}), lot));
    CHECK(propfade::fadesInLotView(low, propfade::groundTiles({628.413f, 442.495f, 638.112f, 451.117f}), lot));
    // Its north side reaches line 40, the area's first.
    CHECK(propfade::fadesInLotView(low, propfade::groundTiles({576.221f, 377.921f, 592.808f, 394.323f}), lot));
    CHECK(!propfade::fadesInLotView(low, propfade::groundTiles({591.71f, 499.597f, 599.845f, 507.733f}), lot));
    // A tall one fades wherever it stands.
    CHECK(propfade::fadesInLotView({{0, 0, -9}, {1, 1, 16}}, propfade::groundTiles({0, 0, 10, 10}), lot));
    CHECK(propfade::groundTiles({std::nanf(""), 0, 1e30f, -1e30f})[0] == 0);
}

// A neighbor's imposter fades by the same reach, from its distance with the camera's height above
// the lot's level, less the camera's height under its top squared, at any height, curve squared.
void imposterFadeIsSquared() {
    namespace propfade = ts2::scene::propfade;
    const std::array<float, 3> eye{10, -20, 15};
    const float reach = 1600, fade = 50.0f * 50.0f;
    // To the middle on both axes (dx -4, dy 9), the camera 15 up, 10 under the 25 high top: 16 + 81
    // + 225 - 100.
    const propfade::Box house{{5, -12, 0}, {7, -10, 25}};
    const float a = propfade::imposterAlpha(house, eye, reach, fade);
    CHECK(std::fabs(a - static_cast<float>(std::pow(222.0 / 2500.0, 2.0))) < 1e-7f);
    CHECK(propfade::imposterAlpha({{5, 30, 0}, {7, 32, 4}}, eye, reach, fade) == 1.0f);
    // Below the lot's level an imposter still fades.
    CHECK(propfade::imposterAlpha({{5, -12, -3}, {7, -10, -1}}, eye, reach, fade) < 1.0f);
}

// A placed prop's box is its model's box turned and moved, and the box around that.
void placedPropBoxesTurnWithTheProp() {
    ts2::scene::Mesh mesh;
    ts2::scene::Primitive p;
    p.positions = {0, 0, 0, 2, 1, 3};
    mesh.primitives.push_back(p);
    const auto box = ts2::scene::propfade::placedBox(mesh, ts2::scene::Transform::at(10, 0, 0, ts2::scene::kPi / 2));
    CHECK(box.has_value());
    if (box) {
        const std::array<float, 3> low{9, 0, 0}, high{10, 2, 3};
        for (std::size_t k = 0; k < 3; ++k) {
            CHECK(std::fabs(box->low[k] - low[k]) < 1e-5f && std::fabs(box->high[k] - high[k]) < 1e-5f);
        }
    }
    CHECK(!ts2::scene::propfade::placedBox(ts2::scene::Mesh{}, {}));
}

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

    // A model a script has hidden stays off in every view, and comes back when the script clears it.
    CHECK(visibleInstances(scene, view)[1] == 1);
    scene.instances[1].hidden = true;
    CHECK(visibleInstances(scene, view)[1] == 0);
    view.level = 2;
    view.walls = WallView::Down;
    CHECK(visibleInstances(scene, view)[1] == 0);
    scene.instances[1].hidden = false;
    CHECK(visibleInstances(scene, view)[1] == 1);
}

// Floors' undersides, as the game shows the Curious brothers' rooftop platforms (floor 5.625, from
// the deck one storey down): with Toggle Ceiling on, seen from a camera 4.25 high but not from 5.77
// or 16.96, and never with the toggle off. The edges above walls show only with the toggle on.
void floorUndersidesShowFromBelowWithToggleCeiling() {
    using namespace ts2::scene;
    Scene scene;
    auto add = [&](Underside underside, int level) {
        Instance i;
        i.mesh = 0;
        i.part = Part::Floor;
        i.level = level;
        i.underside = underside;
        i.floorHeight = 5.625f;
        scene.instances.push_back(i);
    };
    add(Underside::Floor, 2);      // 0 the platforms
    add(Underside::OverWalls, 2);  // 1
    add(Underside::Floor, 1);      // 2 the deck's own
    add(Underside::OverWalls, 1);  // 3
    View view;
    view.level = 1;
    view.eyeHeight = 4.25f;
    CHECK((visibleInstances(scene, view) == std::vector<std::uint8_t>{0, 0, 1, 0}));
    view.ceilings = true;
    CHECK((visibleInstances(scene, view) == std::vector<std::uint8_t>{1, 1, 1, 1}));
    view.eyeHeight = 5.77f;
    CHECK((visibleInstances(scene, view) == std::vector<std::uint8_t>{0, 0, 1, 1}));
    view.eyeHeight = 16.96f;
    CHECK((visibleInstances(scene, view) == std::vector<std::uint8_t>{0, 0, 1, 1}));
}

// A stretched rig scales each joint's rest translation on its bone data's stretch axis by its
// weighted share, keeps rotations, and leaves joints marked unstretched alone. The numbers are the
// adult skeleton's (auskel) and the teen meshes' own copies of it at 0.94.
void stretchedRigScalesJointTranslations() {
    using ts2::scene::Rig;
    Rig rig;
    const auto joint = [&](const char* name, std::array<float, 3> t, int axis, float weight) {
        Rig::Node n;
        n.name = name;
        n.parent = rig.nodes.empty() ? -1 : 0;
        n.translation = t;
        n.stretchAxis = axis;
        n.stretchWeight = weight;
        rig.nodes.push_back(n);
    };
    joint("", {1, 1, 1}, 1, 1);
    joint("spine0", {0, 0.116f, -0.044f}, 1, 1);
    joint("neck", {0, 0.16f, 0}, 1, 0.25f);
    joint("r_clavicle", {-0.02f, 0.0826f, 0.0341f}, 1, 0.7f);
    joint("r_thigh", {-0.066f, -0.11f, 0}, 0, 1);
    joint("root_trans", {0, 0, 1.0122f}, 2, 1);
    joint("pelvis", {0, 0.111f, 0}, Rig::kStretchNone, 1);
    joint("all", {1, 2, 3}, Rig::kStretchAll, 1);
    rig.nodes[1].rotation = {0.1f, 0, 0, 0.995f};
    const auto teen = ts2::scene::stretched(rig, 0.94f);
    const auto near = [&](std::size_t i, std::array<float, 3> want) {
        for (std::size_t k = 0; k < 3; ++k) {
            if (std::fabs(teen.nodes[i].translation[k] - want[k]) > 1e-4f) {
                return false;
            }
        }
        return true;
    };
    CHECK(near(0, {1, 1, 1}));
    CHECK(near(1, {0, 0.109f, -0.044f}) && teen.nodes[1].rotation == rig.nodes[1].rotation);
    CHECK(near(2, {0, 0.1576f, 0}));
    CHECK(near(3, {-0.02f, 0.0791f, 0.0341f}));
    CHECK(near(4, {-0.062f, -0.11f, 0}));
    CHECK(near(5, {0, 0, 0.9515f}));
    CHECK(near(6, {0, 0.111f, 0}));
    CHECK(near(7, {0.94f, 1.88f, 2.82f}));
    CHECK(teen.stretch == 0.94f);
    // Upper arms are unstretched in the skeleton, but a teen human's take the y stretch (0.1316 in game).
    joint("r_upperarm", {0, 0.14f, 0}, Rig::kStretchNone, 1);
    CHECK(std::fabs(ts2::scene::stretched(rig, 0.94f).nodes[8].translation[1] - 0.14f) < 1e-6f);
    Rig teenArms = rig;
    ts2::scene::stretchTeenArms(teenArms);
    CHECK(std::fabs(ts2::scene::stretched(teenArms, 0.94f).nodes[8].translation[1] - 0.1316f) < 1e-6f);
    // Stretching again works from the present stretch, so going back to 1 gives the adult rig.
    const auto back = ts2::scene::stretched(teen, 1.0f);
    CHECK(std::fabs(back.nodes[3].translation[1] - 0.0826f) < 1e-6f && back.stretch == 1.0f);
}

// Clips weigh a mesh's morphs by blend group and name, whatever the case, mixing as on a joint: a
// full upper layer takes the morph whole. A morph no clip weighs keeps its weight.
void clipsWeighMorphs() {
    using ts2::scene::LayeredClip;
    ts2::scene::Mesh mesh;
    mesh.morphs = {{"faceblends", "y_nose"}, {"faceblends", "l_brow"}};
    auto clipWith = [](const std::string& group, const std::string& name, float value) {
        auto a = std::make_shared<ts2::anim::Animation>();
        ts2::anim::Animation::MorphTarget target;
        target.type = ts2::anim::kMorphTargetType;
        target.group = group;
        ts2::anim::Channel c;
        c.name = name;
        c.flags = 0x8000;
        ts2::anim::Curve curve;
        curve.keys.push_back({0, value, 0, 0});
        c.components.push_back(curve);
        target.channels.push_back(c);
        a->morphTargets.push_back(target);
        auto r = std::make_shared<ts2::scene::RigAnimation>();
        r->animation = a;
        return r;
    };
    const auto upper = clipWith("FaceBlends", "Y_Nose", 0.5f), lower = clipWith("*", "y_nose", 1.0f), brow = clipWith("other", "l_brow", 2.0f);
    std::vector<float> weights;
    std::vector<LayeredClip> clips(3);
    clips[0].animation = upper.get();
    clips[0].layer = 2;
    clips[1].animation = lower.get();
    clips[1].layer = 0;
    clips[2].animation = brow.get();
    ts2::scene::mixMorphWeights(mesh, clips, weights);
    CHECK(weights.size() == 2 && weights[0] == 0.5f && weights[1] == 0);
    // Half an upper layer leaves the other half to the one below.
    clips[0].weight = 0.5f;
    ts2::scene::mixMorphWeights(mesh, clips, weights);
    CHECK(std::abs(weights[0] - 0.75f) < 1e-6f);
    ts2::scene::mixMorphWeights(mesh, {}, weights);
    CHECK(std::abs(weights[0] - 0.75f) < 1e-6f);
    // Bound once per mesh and clip, with the same weights, and again for another mesh at that index.
    ts2::scene::MorphBindings bindings;
    std::vector<float> cached;
    ts2::scene::mixMorphWeights(mesh, clips, cached, &bindings, 0);
    ts2::scene::mixMorphWeights(mesh, clips, cached, &bindings, 0);
    CHECK(bindings.entries.size() == 3 && std::abs(cached[0] - 0.75f) < 1e-6f);
    ts2::scene::Mesh other;
    other.morphs = {{"other", "l_brow"}};
    cached.clear();
    ts2::scene::mixMorphWeights(other, clips, cached, &bindings, 0);
    CHECK(cached.size() == 1 && cached[0] == 2.0f);
    // An eye target's channels give the eyes' direction (two values) and each eye's stretch.
    auto eyes = std::make_shared<ts2::anim::Animation>();
    ts2::anim::Animation::MorphTarget target;
    target.type = ts2::anim::kEyeTargetType;
    target.group = "*";
    for (const auto& [name, values] : std::vector<std::pair<std::string, std::vector<float>>>{{"eyedirection", {0.05f, -0.02f}}, {"L_EyeStretch", {0.1f}}}) {
        ts2::anim::Channel c;
        c.name = name;
        c.flags = 0x8000;
        for (const float v : values) {
            ts2::anim::Curve curve;
            curve.keys.push_back({0, v, 0, 0});
            c.components.push_back(curve);
        }
        target.channels.push_back(c);
    }
    eyes->morphTargets.push_back(target);
    ts2::scene::RigAnimation looking;
    looking.animation = eyes;
    std::vector<LayeredClip> eyeClips(1);
    eyeClips[0].animation = &looking;
    std::array<float, 4> values{0, 0, 0, 0.3f};
    ts2::scene::mixEyes(eyeClips, values);
    CHECK(values[0] == 0.05f && values[1] == -0.02f && values[2] == 0.1f && values[3] == 0.3f);
    // The morph mixing leaves eye targets alone.
    mesh.morphs = {{"*", "eyedirection"}};
    weights.clear();
    ts2::scene::mixMorphWeights(mesh, eyeClips, weights);
    CHECK(weights.size() == 1 && weights[0] == 0);
}

// A worn part's bone_adjust lists set its joints' rest places, the later entry winning. Entries
// without a name or with values that aren't numbers are left out. Values from the teen body
// tmBodyShorts, as read on a live teen.
void partsAdjustJoints() {
    using ts2::scenegraph::DataValue;
    const auto value = [](const char* name, DataValue::Type type, std::vector<float> floats, std::string text = {}) {
        DataValue v;
        v.name = name;
        v.type = type;
        v.floats = std::move(floats);
        v.text = std::move(text);
        return v;
    };
    const auto entry = [&](const char* tag, std::vector<float> t, std::vector<float> q) {
        DataValue list;
        list.name = "bone_adjust";
        list.type = DataValue::Type::List;
        if (tag) {
            list.items.push_back(value("bone_adjust_bone_tag", DataValue::Type::String, {}, tag));
        }
        list.items.push_back(value("bone_adjust_translation", DataValue::Type::Vector, std::move(t)));
        if (!q.empty()) {
            list.items.push_back(value("bone_adjust_rotation", DataValue::Type::Rotation, std::move(q)));
        }
        return list;
    };
    const std::vector<DataValue> lists = {
        entry("l_shouldertarget_surface", {-0.05716f, 0.07715f, -0.00032f}, {-0.03657f, 0.06857f, -0.68218f, 0.72705f}),
        entry("neck_grip", {0, -0.0011f, 0.07657f}, {}),
        entry(nullptr, {1, 1, 1}, {}),
        entry("neck_grip", {0, 0, std::nanf("")}, {}),
        entry("no_such_joint", {1, 1, 1}, {}),
    };
    const auto adjusts = ts2::scene::jointAdjusts(lists);
    CHECK(adjusts.size() == 3);
    ts2::scene::Rig rig;
    rig.nodes.push_back({"l_shouldertarget_surface", -1, {-0.034f, 0.077f, 0}, {0, 0, 0, 1}});
    rig.nodes.push_back({"neck_grip", 0, {0, 0, 0.07f}, {0.1f, 0, 0, 0.995f}});
    ts2::scene::adjustJoints(rig, adjusts);
    CHECK((rig.nodes[0].translation == std::array<float, 3>{-0.05716f, 0.07715f, -0.00032f}));
    CHECK(rig.nodes[0].rotation[3] == 0.72705f);
    // An entry with no rotation keeps the joint's own.
    CHECK(rig.nodes[1].translation[2] == 0.07657f && rig.nodes[1].rotation[0] == 0.1f);
    const std::vector<ts2::scene::JointAdjust> later = {{"neck_grip", std::array<float, 3>{0, 0, 0.08f}, std::nullopt}};
    ts2::scene::adjustJoints(rig, later);
    CHECK(rig.nodes[1].translation[2] == 0.08f);
    // Parts apply by part bit whatever their order in the look: a bottom's hip target wins over a
    // top's. A joint the new parts don't name keeps what earlier parts gave it.
    rig.nodes.push_back({"l_hiptarget_surface", 0, {0, 0, 0}, {0, 0, 0, 1}});
    ts2::scene::SimLook look;
    look.parts.resize(2);
    look.parts[0].part = ts2::scene::kBottomPart;
    look.parts[0].jointAdjusts = {{"l_hiptarget_surface", std::array<float, 3>{0.107f, 0, 0}, std::nullopt}};
    look.parts[1].part = ts2::scene::kTopPart;
    look.parts[1].jointAdjusts = {{"l_hiptarget_surface", std::array<float, 3>{0.2f, 0, 0}, std::nullopt}};
    ts2::scene::wearPartJoints(rig, look);
    CHECK(rig.nodes[2].translation[0] == 0.107f && rig.nodes[1].translation[2] == 0.08f);
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
    ts2::scene::poseRigLayered(rig, std::vector<ts2::scene::LayeredClip>{{&bound, 0, 1, 0, 0, kQuarter}}, world);
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
    // An overlay layer's clips count by their weights, and share the layer by them when they weigh
    // 1 or more together. Layer 0 averages its clips.
    CHECK(std::fabs(armX({{&high, 0, 1, 1, 0}, {&other, 0, 0.25f, 1, 2}}) - 4.4f) < 1e-5f);
    CHECK(armX({{&other, 0, 1, 1, 2}, {&high, 0, 1, 1, 0}}) == 5.0f);
    CHECK(std::fabs(armX({{&high, 0, 1, 0, 0}, {&other, 0, 1, 0, 2}}) - 5.0f) < 1e-5f);
    CHECK(armX({{&low, 0, 1, 0}, {&high, 0, 0.0005f, 1}}) == 2.0f);
    // A clip fading out under a newer one fading in gives way only to it: the two sum to 1. Alone it
    // fades out to the layer below.
    CHECK(std::fabs(armX({{&other, 0, 0.75f, 1, 0}, {&high, 0, 0.25f, 1, 2}}) - 5.5f) < 1e-5f);
    CHECK(std::fabs(armX({{&other, 0, 0.5f, 1, 0}, {&low, 0, 1, -1}}) - 4.0f) < 1e-5f);

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
    CHECK(ts2::scene::surfaceQuarterOpen(surface, 0, 0, 0, 0) && !ts2::scene::surfaceQuarterOpen(surface, 0, 0, 0, 1));
    CHECK(!ts2::scene::surfaceQuarterOpen(surface, 1, 0, 0, 0) && !ts2::scene::surfaceQuarterOpen(surface, 0, 1, 0, 0));
    CHECK(!ts2::scene::surfaceQuarterOpen(surface, 0, 0, 0, 4) && !ts2::scene::surfaceQuarterOpen(surface, 5, 0, 0, 0));
}

// A counter top saved with a sink's hole keeps it whatever material a script gives that group.
void savedCutoutStaysOnItsGroup() {
    using ts2::scene::withSavedCutout;
    const std::vector<ts2::scenegraph::MaterialOverride> saved{{"top_clean-cutout-sink-mask", "Counter_Top"}, {"wood", "frame"}};
    CHECK(withSavedCutout("top_dirty", "counter_top", saved) == "top_dirty-cutout-sink-mask");
    // A material already cut takes the group's own mask.
    CHECK(withSavedCutout("top_dirty-CUTOUT-old", "counter_top", saved) == "top_dirty-cutout-sink-mask");
    CHECK(withSavedCutout("oak", "frame", saved) == "oak");
    CHECK(withSavedCutout("oak", "legs", saved) == "oak");
    CHECK(withSavedCutout("oak", "", saved) == "oak");
    CHECK(withSavedCutout("oak", "counter_top", {}) == "oak");
}

// The ponds' water as the game builds it on the Capps' lot: alpha 62 on a dry rim corner and 74 over
// a corner 2.7 deep (Terrain.ini's curve). A centre takes the depth of its corners' averages.
void pondWaterFollowsTheGamesCurve() {
    using namespace ts2::scene::terrainwater;
    const AlphaCurve curve = readAlphaCurve("[WaterRenderParameters]\r\nAlphaVsDepthCurve= .25,62,  10,110\r\n;AlphaVsDepthCurve= 0,128,  0.5,255\r\n");
    CHECK(curve.points.size() == 2 && curve.alpha(-1) == 62 && curve.alpha(20) == 110);
    CHECK(std::fabs(curve.alpha(2.7f) - 74.06f) < 0.01f);
    CHECK(readAlphaCurve("AlphaVsDepthCurve= 1,2, 0,5").points.empty());   // depths falling
    CHECK(readAlphaCurve("AlphaVsDepthCurve= 1,2, 3").points.empty());     // half a point
    CHECK(readAlphaCurve("AlphaVsDepthCurve= 1,x").points.empty());
    CHECK(readAlphaCurve(";AlphaVsDepthCurve= 1,2").points.empty());

    // Three by three corners: the middle one dug 3 under the water at 1.2, the rest dry at 1.5.
    ts2::terrain::Array2D water;
    water.width = water.height = 3;
    water.elementSize = 4;
    water.elements.resize(3 * 3 * 4);
    const float level = 1.2f;
    for (std::size_t i = 0; i < 9; ++i) std::memcpy(&water.elements[i * 4], &level, 4);
    const auto mesh = build(water, [](int x, int y) { return x == 1 && y == 1 ? -1.5f : 1.5f; }, curve);
    // All four tiles touch the wet corner: 9 corners and 4 centres, 16 triangles.
    CHECK(mesh.alphas.size() == 13 && mesh.indices.size() == 48);
    auto alphaAt = [&](float x, float y) {
        for (std::size_t i = 0; i < mesh.alphas.size(); ++i) {
            if (mesh.positions[i * 3] == x && mesh.positions[i * 3 + 1] == y) return std::lround(mesh.alphas[i] * 255);
        }
        return -1L;
    };
    CHECK(alphaAt(1, 1) == 74 && alphaAt(0, 0) == 62 && alphaAt(0.5f, 0.5f) == 62);  // 0.45 deep at the centre
    CHECK(mesh.positions[2] == level && mesh.uvs[0] == mesh.positions[0]);
    // A dry lot has none, and nor does a broken grid.
    CHECK(build(water, [](int, int) { return 2.0f; }, curve).indices.empty());
    water.elementSize = 1;
    CHECK(build(water, [](int, int) { return -2.0f; }, curve).indices.empty());
}

void floatingObjectsReadTheWaterByHalfTiles() {
    using namespace ts2::scene::terrainwater;
    // Corners (0, 0) 0, (1, 0) 1, (0, 1) 2 and (1, 1) 4.
    ts2::terrain::Array2D water;
    water.width = water.height = 2;
    water.elementSize = 4;
    water.elements.resize(2 * 2 * 4);
    const float corners[4] = {0, 1, 2, 4};
    std::memcpy(water.elements.data(), corners, sizeof(corners));
    auto near = [](std::optional<float> h, float want) { return h && std::fabs(*h - want) < 1e-5f; };
    CHECK(near(heightAt(water, 0.5f, 0.5f), 2));       // a centre: the diagonal's average
    CHECK(near(heightAt(water, 0.25f, 0.75f), 2.0f));  // the half with (0, 1): 0 + 0.25 * 2 + 0.75 * 2
    CHECK(near(heightAt(water, 0.75f, 0.25f), 1.5f));  // the half with (1, 0): 0 + 0.75 * 1 + 0.25 * 3
    CHECK(near(heightAt(water, 1, 1), 4));              // the far corner reads the last tile
    CHECK(!heightAt(water, 1.5f, 0) && !heightAt(water, -0.1f, 0));
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

// The surface: per quarter 8 triangles on 9 points, shared points once. The water lies 2.75 above the
// level below the ground: with the ground at 0.375, the water is at 0.125.
void poolWaterHasTheGamesSurface() {
    namespace poolwater = ts2::scene::poolwater;
    CHECK(poolwater::surfaceHeight(-2.625f) == 0.125f);
    const auto one = poolwater::surface({{0, 0, 0}}, [](float, float) { return 0.5f; });
    CHECK(one.positions.size() == 9 * 3 && one.indices.size() == 24);
    // The outer edge (x = 0, from y 1 to 0) in four steps, the centre, and every point at the water's height.
    auto has = [&](float x, float y) {
        for (std::size_t i = 0; i < one.positions.size(); i += 3) {
            if (one.positions[i] == x && one.positions[i + 1] == y && one.positions[i + 2] == 0.5f) return true;
        }
        return false;
    };
    CHECK(has(0, 1) && has(0, 0.75f) && has(0, 0.5f) && has(0, 0.25f) && has(0, 0) && has(0.5f, 0.5f) && has(0.25f, 0.5f));
    // The first triangle turns counter-clockwise seen from above.
    auto at = [&](std::uint32_t i) { return std::array<float, 2>{one.positions[i * 3], one.positions[i * 3 + 1]}; };
    const auto a = at(one.indices[0]), b = at(one.indices[1]), c = at(one.indices[2]);
    CHECK((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]) > 0);
    // A whole tile shares its corners and the centre between its quarters.
    std::vector<poolwater::Quarter> tile{{0, 0, 0}, {0, 0, 1}, {0, 0, 2}, {0, 0, 3}};
    CHECK(poolwater::surface(tile, [](float, float) { return 0.0f; }).positions.size() < 4 * 9 * 3);
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

// The head turns so that the eye (head_grip, up the head's Y) looks along the head's Z through the
// target, its X square to the target and the pole, whatever its parent's turn.
void headAimsItsEyeAtTheTarget() {
    using ts2::scene::Transform;
    ts2::scene::Rig rig;
    rig.nodes.push_back({"neck", -1, {0, 0, 1.5f}, {0, 0, 0.70710678f, 0.70710678f}});  // a quarter turn about z
    rig.nodes.push_back({"head", 0, {0, 0, 0.1f}, {0, 0, 0, 1}});
    rig.nodes.push_back({"head_grip", 1, {0, 0.075f, 0}, {0, 0, 0, 1}});
    const auto chain = ts2::scene::headChain(rig);
    CHECK(chain.head == 1 && chain.grip == 2);
    std::vector<Transform> local(3), world(3);
    for (std::size_t i = 0; i < 3; ++i) {
        local[i] = Transform::of(rig.nodes[i].translation, rig.nodes[i].rotation);
    }
    ts2::scene::placeRig(rig, local, world);
    const std::array<float, 3> target{1, 2, 1.7f}, pole{0, 0, 2.6f};
    CHECK(ts2::scene::aimHead(rig, chain, target, pole, local, world));
    ts2::scene::placeRig(rig, local, world);
    const auto& h = world[1].m;
    const auto& g = world[2].m;
    // From the eye along the head's Z to the target.
    const float to[3] = {target[0] - g[3], target[1] - g[7], target[2] - g[11]};
    const float along = to[0] * h[2] + to[1] * h[6] + to[2] * h[10];
    CHECK(along > 0);
    CHECK(std::abs(to[0] - along * h[2]) < 1e-4f && std::abs(to[1] - along * h[6]) < 1e-4f && std::abs(to[2] - along * h[10]) < 1e-4f);
    // X square to the head-target and head-pole lines.
    const float t[3] = {target[0] - h[3], target[1] - h[7], target[2] - h[11]}, p[3] = {pole[0] - h[3], pole[1] - h[7], pole[2] - h[11]};
    CHECK(std::abs(t[0] * h[0] + t[1] * h[4] + t[2] * h[8]) < 1e-4f && std::abs(p[0] * h[0] + p[1] * h[4] + p[2] * h[8]) < 1e-4f);
    // Y leans to the pole.
    CHECK(p[0] * h[1] + p[1] * h[5] + p[2] * h[9] > 0);
    // Too close to the eye: nothing to do.
    CHECK(!ts2::scene::aimHead(rig, chain, {h[3], h[7] + 0.01f, h[11]}, pole, local, world));
}

// A look's direction as angles, forward +Z and up +Y, clamped to the head's cone.
void lookAnglesClampToTheCone() {
    constexpr float degree = 3.14159265f / 180;
    auto a = ts2::sim::lookAngles({1, 0, 1});
    CHECK(std::abs(a.yaw - 45 * degree) < 1e-5f && std::abs(a.pitch) < 1e-6f);
    CHECK(!ts2::sim::clampLook(a));
    a = ts2::sim::lookAngles({0, -2, 1});  // well down
    CHECK(a.pitch > 36 * degree && ts2::sim::clampLook(a) && std::abs(a.pitch - 36 * degree) < 1e-6f);
    a = ts2::sim::lookAngles({-1, 1, -1});  // behind, up and to the other side
    CHECK(ts2::sim::clampLook(a) && std::abs(a.yaw + 70 * degree) < 1e-6f && a.pitch < 0);
}

namespace look {

using ts2::sim::LookController;
constexpr float degree = 3.14159265f / 180;

// A look turning at 80 degrees a second, held `holdMs` (negative: forever).
ts2::sim::LookRequest request(int id, double start, float holdMs) {
    ts2::sim::LookRequest r;
    r.id = id;
    r.startMs = start;
    r.towards = r.away = 80 * degree;
    r.remainingMs = holdMs;
    r.forever = holdMs < 0;
    return r;
}
// A target 1 m away, `yawDegrees` toward +X from straight ahead (+Z).
ts2::sim::LookPose pose(float yawDegrees) {
    ts2::sim::LookPose p;
    p.target = {std::sin(yawDegrees * degree), 0, std::cos(yawDegrees * degree)};
    return p;
}
// When each state came, stepping 33 ms frames up to `untilMs`.
std::vector<LookController::Change> run(LookController& c, const ts2::sim::LookPose& p, double fromMs, double untilMs,
                                        std::vector<float>* weights = nullptr) {
    std::vector<LookController::Change> changes;
    for (double now = fromMs; now <= untilMs; now += 33) {
        c.update(now, p, changes);
        if (weights != nullptr) {
            weights->push_back(c.current() ? c.current()->weightAt(now) : 0.0f);
        }
    }
    return changes;
}

} // namespace look

// A look turns the head over about 30 degrees' worth of time at its speed whatever its size under
// 30 degrees, the head IK coming in over half of it. It holds for its duration, then turns back as
// the IK goes out, and is done.
void lookTurnsHoldsAndTurnsBack() {
    using namespace look;
    LookController c;
    c.begin(request(1, 0, 1500));
    std::vector<float> weights;
    const auto changes = run(c, pose(20), 0, 3000, &weights);
    CHECK(changes.size() == 4);
    CHECK(changes[0].state == ts2::sim::kLookTurningTo && changes[0].atMs == 0);
    CHECK(changes[1].state == ts2::sim::kLookHolding && changes[1].atMs > 350 && changes[1].atMs < 480);
    CHECK(changes[2].state == ts2::sim::kLookTimeUp);
    CHECK(changes[2].atMs - changes[1].atMs > 1450 && changes[2].atMs - changes[1].atMs < 1560);
    CHECK(changes[3].state == ts2::sim::kLookDone && changes[3].atMs - changes[2].atMs > 330 && changes[3].atMs - changes[2].atMs < 450);
    CHECK(c.current() == nullptr);
    // In by 187.5 ms: 0.001 at the start, about 0.35 at 66 ms, whole from 198 ms.
    CHECK(std::abs(weights[2] - (0.001f + 0.999f * 66 / 187.5f)) < 0.01f && weights[6] == 1);
    // The target lies along the look at the target's distance, the pole a metre up the head from the eye.
    LookController held;
    held.begin(request(2, 0, -1));
    run(held, pose(20), 0, 1000);
    const auto t = held.target(), p = pose(20).target;
    CHECK(std::abs(t[0] - p[0]) < 1e-3f && std::abs(t[1] - p[1]) < 1e-3f && std::abs(t[2] - p[2]) < 1e-3f);
    CHECK(std::abs(held.pole()[1] - 1) < 1e-5f);
}

// A newer look cancels the one followed, STOP turns the head back, and a target outside the cone
// ends the look once the head reaches the cone's edge.
void looksAreReplacedStoppedAndEndAtTheCone() {
    using namespace look;
    LookController c;
    c.begin(request(1, 0, -1));
    run(c, pose(20), 0, 600);
    c.begin(request(2, 610, -1));
    auto changes = run(c, pose(-20), 610, 620);
    CHECK(changes.size() >= 2 && changes[0].request == 1 && changes[0].state == ts2::sim::kLookCancelled);
    CHECK(changes[1].request == 2 && changes[1].state == ts2::sim::kLookTurningTo);
    c.stop(700, 80 * degree);
    changes = run(c, pose(-20), 709, 2000);
    CHECK(!changes.empty() && changes[0].request == 2 && changes[0].state == ts2::sim::kLookEndedEarly);
    CHECK(changes.back().state == ts2::sim::kLookCancelled && c.current() == nullptr);
    LookController behind;
    behind.begin(request(3, 0, -1));
    changes = run(behind, pose(120), 0, 3000);
    CHECK(changes.size() >= 3 && changes[1].state == ts2::sim::kLookEndedEarly);
}

// A lot object's actor poses every joint from its own model's rig, which holds the joints as the
// lot saved them: its instance gets those joints and the actor's place, and no rig nodes.
void objectActorsPoseEveryJointFromTheirRig() {
    using ts2::scene::Transform;
    auto rig = std::make_shared<ts2::scene::Rig>();
    rig->nodes.push_back({"root", -1, {0, 0, 1}, {0, 0, 0, 1}});
    rig->nodes.push_back({"door", 0, {0, 0, 0}, {0.70710678f, 0, 0, 0.70710678f}});  // a quarter turn about x
    ts2::scene::Scene scene;
    auto& mesh = scene.meshes.emplace_back();
    mesh.skin.push_back({"door", Transform{}, Transform{}, Transform::of({0, 0, 0}, {1, 0, 0, 0})});  // the model's own rest
    scene.instances.emplace_back().mesh = 0;
    auto& actor = scene.objectActors.emplace_back();
    actor.object = 5;
    actor.rig = rig;
    actor.skinned.push_back({0, {1}});
    actor.place = Transform::at(1, 2, 3, 0);
    ts2::scene::ScenePose pose;
    ts2::scene::poseActors(scene, 0, pose);
    CHECK(pose.joints.size() == 1 && pose.joints[0].first == 0 && pose.joints[0].second.size() == 1);
    CHECK(pose.places.size() == 1 && pose.places[0].second.m == actor.place.m);
    CHECK(pose.nodes.empty() && pose.held.contains(5));
    float p[3] = {0, 1, 0};
    pose.joints[0].second[0].apply(p, true);
    CHECK(std::abs(p[1]) < 1e-5f && std::abs(p[2] - 2) < 1e-5f);
}

// A carried Sim hangs at the bone of its carrier's slot as the carrier is posed in the same call,
// even when it comes first in the scene. Carriers that carry each other still get posed.
void carriedSimsHangAtTheCarriersSlotBone() {
    using ts2::scene::Transform;
    auto rig = std::make_shared<ts2::scene::Rig>();
    rig->nodes.push_back({"root", -1, {0, 0, 0}, {0, 0, 0, 1}});
    rig->nodes.push_back({"r_hand_grip", 0, {0.25f, 0, 1}, {0, 0, 0, 1}});
    ts2::scene::Scene scene;
    auto& carried = scene.actors.emplace_back();
    carried.object = 2;
    carried.rig = rig;
    carried.carrier = 1;
    carried.carrierSlot = 0;
    carried.place = Transform::at(9, 9, 0, 0);
    auto& carrier = scene.actors.emplace_back();
    carrier.object = 1;
    carrier.rig = rig;
    carrier.slotNodes = {1};
    carrier.place = Transform::at(5, 0, 0, 0);
    ts2::scene::ScenePose pose;
    ts2::scene::poseActors(scene, 0, pose);
    CHECK(pose.carried.size() == 1 && pose.carried.contains(2));
    const Transform& at = pose.carried.at(2);
    CHECK(std::abs(at.m[3] - 5.25f) < 1e-5f && std::abs(at.m[7]) < 1e-5f && std::abs(at.m[11] - 1) < 1e-5f);
    CHECK(pose.drawnAt.at(2).m == at.m && pose.drawnAt.at(1).m == carrier.place.m);
    scene.actors[1].carrier = 2;
    ts2::scene::poseActors(scene, 0, pose);
    CHECK(pose.nodes.size() == 2);
}

// A Sim with no clips takes no eye or morph values from the clips of the Sim posed before it, nor
// from a lot shown before, whose clips are gone.
void simsWithoutClipsKeepTheirOwnEyes() {
    using ts2::scene::Transform;
    auto rig = std::make_shared<ts2::scene::Rig>();
    rig->nodes.push_back({"root", -1, {0, 0, 0}, {0, 0, 0, 1}});
    ts2::scene::ScenePose pose;
    const auto face = [](ts2::scene::Scene& scene) {
        auto& mesh = scene.meshes.emplace_back();
        mesh.primitives.emplace_back().eyeUvs = {0, 0};
        mesh.morphs = {{"*", "y_nose"}};
        scene.instances.emplace_back().mesh = static_cast<int>(scene.meshes.size() - 1);
    };
    {
        auto looking = std::make_shared<ts2::anim::Animation>();
        ts2::anim::Animation::MorphTarget eyes;
        eyes.type = ts2::anim::kEyeTargetType;
        eyes.group = "*";
        ts2::anim::Animation::MorphTarget nose;
        nose.type = ts2::anim::kMorphTargetType;
        nose.group = "*";
        for (const auto& [target, name] : {std::pair{&eyes, "eyedirection"}, std::pair{&nose, "y_nose"}}) {
            ts2::anim::Channel c;
            c.name = name;
            c.flags = 0x8000;
            ts2::anim::Curve curve;
            curve.keys.push_back({0, 0.5f, 0, 0});
            c.components.assign(2, curve);
            target->channels.push_back(c);
        }
        looking->morphTargets = {eyes, nose};
        auto clip = std::make_shared<ts2::scene::RigAnimation>();
        clip->animation = looking;
        ts2::scene::Scene first;
        face(first);
        face(first);
        auto& withClip = first.actors.emplace_back();
        withClip.object = 1;
        withClip.rig = rig;
        withClip.skinned.push_back({0, {}});
        withClip.clips.push_back({clip});
        withClip.clips.back().fadeIn = false;
        auto& without = first.actors.emplace_back();
        without.object = 2;
        without.rig = rig;
        without.skinned.push_back({1, {}});
        ts2::scene::poseActors(first, 1, pose);
        CHECK(pose.eyes.at(0)[0] == 0.5f && pose.morphs.at(0)[0] == 0.5f);
        CHECK(pose.eyes.at(1)[0] == 0 && pose.morphs.at(1)[0] == 0);
    }
    // The next lot's first Sim has no clips, and the last lot's clip is freed.
    ts2::scene::Scene second;
    face(second);
    auto& without = second.actors.emplace_back();
    without.object = 3;
    without.rig = rig;
    without.skinned.push_back({0, {}});
    pose = {};
    ts2::scene::poseActors(second, 0, pose);
    CHECK(pose.eyes.at(0)[0] == 0 && pose.morphs.at(0)[0] == 0);
}

// A clip played at a negative speed runs from its end back to its start (a chat's talk-to-idle
// transition plays its idle-to-talk clip backwards), on its own and among layered clips.
void backwardClipsPlayFromTheirEnd() {
    using ts2::scene::Transform;
    auto rig = std::make_shared<ts2::scene::Rig>();
    rig->nodes.push_back({"root", -1, {0, 0, 0}, {0, 0, 0, 1}});
    auto clip = std::make_shared<ts2::anim::Animation>();
    clip->duration = 1000;
    ts2::anim::Channel move;
    move.name = "root";
    move.flags = static_cast<std::uint32_t>(ts2::anim::ChannelKind::Translation) << 22 | 0x8000;
    move.components.resize(3);
    move.components[0].keys = {{0, 0, 1, 1}, {1000, 1, 1, 1}};  // x from 0 to 1 over the second
    move.components[1].keys = move.components[2].keys = {{0, 0, 0, 0}};
    clip->channels.push_back(move);
    const auto bound = std::make_shared<const ts2::scene::RigAnimation>(ts2::scene::bindAnimation(*rig, clip));
    for (const bool layered : {false, true}) {
        ts2::scene::Scene scene;
        auto& actor = scene.actors.emplace_back();
        actor.rig = rig;
        if (layered) {
            auto& c = actor.clips.emplace_back();
            c.animation = bound;
            c.speed = -1;
            c.fadeIn = c.fadeOut = false;
        } else {
            actor.animation = bound;
            actor.speed = -1;
            actor.loop = false;
        }
        ts2::scene::ScenePose pose;
        ts2::scene::poseActors(scene, 0.25, pose);
        CHECK(pose.nodes.size() == 1 && std::abs(pose.nodes[0].second[0].m[3] - 0.75f) < 1e-3f);
    }
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
    CHECK(std::isnan(sine(std::numeric_limits<double>::infinity())) && std::isnan(power(-1, 2)) && std::isnan(ts2::scene::portable::log(-1)));
    CHECK(ts2::scene::portable::exp(1000) == std::numeric_limits<double>::infinity() && ts2::scene::portable::exp(-1000) == 0 && power(0, 2) == 0);
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

// With a pose, a Sim is hit by its skinned triangles where the pose puts them, not at rest, and
// the hit names the Sim. A hidden Sim and one without joints in the pose are not hit.
void pickerHitsPosedSims() {
    using namespace ts2::scene;
    Scene scene;
    Mesh body;
    Primitive p;
    p.positions = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
    p.indices = {0, 1, 2, 0, 2, 3};
    p.joints.assign(16, 255);
    p.weights.assign(16, 0);
    for (std::size_t v = 0; v < 4; ++v) {
        p.joints[v * 4] = 0;
        p.weights[v * 4] = 1;
    }
    body.primitives.push_back(p);
    scene.meshes.push_back(body);
    Instance i;
    i.mesh = 0;
    i.transform = Transform::at(0, 0, 1, 0);
    scene.instances.push_back(i);
    Actor sim;
    sim.object = 42;
    sim.skinned.push_back({0, {}});
    scene.actors.push_back(sim);
    Picker picker;
    picker.prepare(scene);
    ScenePose pose;
    pose.joints.emplace_back(0, std::vector<Transform>{Transform::at(10, 0, 0, 0)});  // the joint moves it 10 along x
    pose.places.emplace_back(0, Transform::at(0, 0, 3, 0));
    const std::array<float, 3> down{0, 0, -1};
    CHECK(!picker.pick(scene, {}, {0, 0, 10}, down));  // a Sim isn't hit without a pose
    CHECK(!picker.pick(scene, {}, {0, 0, 10}, down, &pose));  // nor where it rests
    auto hit = picker.pick(scene, {}, {10, 0, 10}, down, &pose);
    CHECK(hit && hit->instance == 0 && hit->object == 42 && std::abs(hit->distance - 7) < 1e-5f);
    const std::vector<std::uint8_t> hidden{0};
    CHECK(!picker.pick(scene, hidden, {10, 0, 10}, down, &pose));
    ScenePose unposed;
    unposed.places = pose.places;
    CHECK(!picker.pick(scene, {}, {10, 0, 10}, down, &unposed));
}

// The box a posed Sim is first checked against never leaves out a triangle the pose puts under the
// ray: not when its weights add up to less than 1 (pulled toward the model's origin), and not when
// they add up to more (a broken file, tested in full).
void pickerBoxKeepsEveryPosedHit() {
    using namespace ts2::scene;
    const auto sceneWith = [](float weight0, float weight1) {
        Scene scene;
        Mesh body;
        Primitive p;
        p.positions = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
        p.indices = {0, 1, 2, 0, 2, 3};
        p.joints.assign(16, 255);
        p.weights.assign(16, 0);
        for (std::size_t v = 0; v < 4; ++v) {
            p.joints[v * 4] = 0;
            p.weights[v * 4] = weight0;
            p.joints[v * 4 + 1] = 1;
            p.weights[v * 4 + 1] = weight1;
        }
        body.primitives.push_back(p);
        scene.meshes.push_back(body);
        Instance i;
        i.mesh = 0;
        scene.instances.push_back(i);
        Actor sim;
        sim.object = 42;
        sim.skinned.push_back({0, {}});
        scene.actors.push_back(sim);
        return scene;
    };
    ScenePose pose;
    pose.joints.emplace_back(0, std::vector<Transform>{Transform::at(10, 0, 0, 0), Transform{}});
    pose.places.emplace_back(0, Transform::at(0, 0, 3, 0));
    const std::array<float, 3> down{0, 0, -1};
    // Half weight on the moved joint only: the quad sits at x 4.5 to 5.5, between the origin and the joint.
    const Scene half = sceneWith(0.5f, 0);
    Picker picker;
    picker.prepare(half);
    for (int twice = 0; twice < 2; ++twice) {  // the second pick uses the kept box
        const auto hit = picker.pick(half, {}, {5, 0, 10}, down, &pose);
        CHECK(hit && hit->object == 42 && std::abs(hit->distance - 7) < 1e-5f);
        CHECK(!picker.pick(half, {}, {30, 0, 10}, down, &pose));
    }
    // Whole weight on both joints: the quad is twice its size at x 8 to 12, beyond either joint's box.
    const Scene doubled = sceneWith(1, 1);
    picker.prepare(doubled);
    const auto hit = picker.pick(doubled, {}, {11.5f, 1.5f, 10}, down, &pose);
    CHECK(hit && hit->object == 42 && std::abs(hit->distance - 7) < 1e-5f);
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
        e.level = 1;
        Component c;
        c.type = static_cast<std::uint8_t>(type);
        e.components.push_back(c);
        f.names[name] = {static_cast<std::uint32_t>(f.effects.size())};
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
    // Look At finds no point yet for an object's routing or targeting slot. Ray Trace takes the middle, as it has so far.
    for (const int kind : {1, 2}) {
        bool slotMissing = false, objectMissing = false;
        CHECK(!rays.locate({20, kind, 0}, pose, slotMissing, objectMissing, true) && slotMissing && !objectMissing);
        slotMissing = false;
        CHECK(rays.locate({20, kind, 0}, pose, slotMissing, objectMissing) && !slotMissing);
    }
    bool slotMissing = false, objectMissing = false;
    CHECK(rays.locate({20, 0, 0}, pose, slotMissing, objectMissing, true) && !slotMissing);
    // A place Look At worked out in the object's model goes with the object.
    const auto placedPoint = rays.locate({20, 1, 0, ts2::scene::Transform::at(0.5f, 0, 1, 0)}, pose, slotMissing, objectMissing, true);
    CHECK(placedPoint && !slotMissing && std::abs(placedPoint->m[3] - 4.5f) < 1e-4f && std::abs(placedPoint->m[11] - 1) < 1e-4f);
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
        {"Worn parts adjust joints", partsAdjustJoints},
        {"Clips weigh morphs", clipsWeighMorphs},
        {"Layered clips mix like the game", layeredClipsMixLikeTheGame},
        {"Mirrored clips play on the other side", mirroredClipsPlayOnTheOtherSide},
        {"Clips turn to their own heading", clipsTurnToTheirOwnHeading},
        {"Skin under clothes is named as the game names it", skinUnderClothesNames},
        {"Room samples are gathered as the game gathers them", roomSamplesAreGatheredAsTheGame},
        {"Portable maths matches the C library", portableMathMatchesTheLibrary},
        {"Joints the animation leaves keep the mesh's own rest", unanimatedJointsKeepTheMeshRest},
        {"A head aims its eye at the target", headAimsItsEyeAtTheTarget},
        {"Look angles clamp to the head's cone", lookAnglesClampToTheCone},
        {"A look turns, holds and turns back", lookTurnsHoldsAndTurnsBack},
        {"Looks are replaced, stopped and end at the cone", looksAreReplacedStoppedAndEndAtTheCone},
        {"A lot object's actor poses every joint from its own rig", objectActorsPoseEveryJointFromTheirRig},
        {"Backward clips play from their end", backwardClipsPlayFromTheirEnd},
        {"Carried Sims hang at the carrier's slot bone", carriedSimsHangAtTheCarriersSlotBone},
        {"Sims without clips keep their own eyes", simsWithoutClipsKeepTheirOwnEyes},
        {"scene transforms turn and move", sceneTransformsTurnAndMove},
        {"scene quads face their normal", sceneQuadsFaceTheirNormal},
        {"scene hip roof rises to its ridge", sceneHipRoofRisesToItsRidge},
        {"scene greenhouse roof pieces match the game", sceneGreenhouseRoofPiecesMatchTheGame},
        {"scene grid roofs match the game", sceneGridRoofsMatchTheGame},
        {"scene cutaway cuts walls in front of rooms", sceneCutawayCutsWallsInFrontOfRooms},
        {"scene views show storeys, walls and roofs", sceneViewsShowStoreysWallsAndRoofs},
        {"floor undersides show from below with Toggle Ceiling", floorUndersidesShowFromBelowWithToggleCeiling},
        {"a saved cutout stays on its mesh group", savedCutoutStaysOnItsGroup},
        {"ground is open where it has no surface", groundIsOpenWhereItHasNoSurface},
        {"effect tiles leave out the road row", effectTilesLeaveOutTheRoadRow},
        {"pool water has the game's depth layers", poolWaterHasTheGamesDepthLayers},
        {"pond water follows the game's curve", pondWaterFollowsTheGamesCurve},
        {"floating objects read the water by half tiles", floatingObjectsReadTheWaterByHalfTiles},
        {"pool water has the game's surface", poolWaterHasTheGamesSurface},
        {"attic walls reach the roof's underside", atticWallsReachTheRoofsUnderside},
        {"held particles are replaced when they die", heldParticlesAreReplacedWhenTheyDie},
        {"bursts come each cycle, aged by how late they are", burstsComeEachCycleAgedByHowLateTheyAre},
        {"flowing particles follow the rate over a cycle", flowingParticlesFollowTheRateOverACycle},
        {"particles move as the game moves them", particlesMoveAsTheGameMovesThem},
        {"new particles start where the block and place say", newParticlesStartWhereTheBlockAndPlaceSay},
        {"particle stops follow their kind", particleStopsFollowTheirKind},
        {"particle starts wait and prewarm", particleStartsWaitAndPrewarm},
        {"particle walk pushes them about", particleWalkPushesThemAbout},
        {"effects face the camera", effectsFaceTheCamera},
        {"effects pick their record by level", effectsPickTheirRecordByLevel},
        {"effect parts follow detail and state", effectPartsFollowDetailAndState},
        {"effect groups play one of their parts", effectGroupsPlayOneOfTheirParts},
        {"effects place parts inside themselves", effectsPlacePartsInsideThemselves},
        {"effects end as their keywords say", effectsEndAsTheirKeywordsSay},
        {"effects play other effects by name", effectsPlayOtherEffectsByName},
        {"effect parts scale by detail", effectPartsScaleByDetail},
        {"object effects start, stop and follow", objectEffectsStartStopAndFollow},
        {"metaparticles play an effect each", metaParticlesPlayAnEffectEach},
        {"metaparticles follow their path", metaParticlesFollowTheirPath},
        {"a hard restart leaves no gap", aHardRestartLeavesNoGap},
        {"metaparticles pass on a picture", metaParticlesPassOnAPicture},
        {"object effects play a queue in turn", objectEffectsPlayAQueueInTurn},
        {"a particle target thins new particles", particleTargetThinsNewParticles},
        {"a matrix's inverse undoes it", matrixInverseUndoesIt},
        {"attractors hold particles in place", attractorsHoldParticlesInPlace},
        {"sequences play their items in turn", sequencesPlayTheirItemsInTurn},
        {"particle sprites face and size as the game", particleSpritesFaceAndSizeAsTheGame},
        {"particle sprites take their frame and colour", particleSpritesTakeTheirFrameAndColour},
        {"broken particle blocks stay within limits", brokenParticleBlocksStayWithinLimits},
        {"props fade as the game fades them", propsFadeAsTheGameFadesThem},
        {"placed prop boxes turn with the prop", placedPropBoxesTurnWithTheProp},
        {"imposter fade is squared", imposterFadeIsSquared},
        {"only props near the lot fade", onlyPropsNearTheLotFade},
        {"picker finds the nearest shown triangle", pickerFindsTheNearestShownTriangle},
        {"picker hits posed Sims", pickerHitsPosedSims},
        {"picker box keeps every posed hit", pickerBoxKeepsEveryPosedHit},
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
