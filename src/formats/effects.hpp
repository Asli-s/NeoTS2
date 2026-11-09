#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ts2::effects {

// The game's effects file (TSData/Res/Effects/effects.package, resource 0xEA5118B0). It holds 13
// lists of blocks, one per block type, then the visual effects built from them and the names the
// game starts them by.
constexpr std::uint32_t kEffectsType = 0xEA5118B0;

// The block types, numbered as visual effect components name them. The others (4, 5, 7, 9, 12
// and 13) are read but their meaning is not known yet.
enum class BlockType : std::uint8_t {
    Particles = 1,
    MetaParticles = 2,
    Decal = 3,
    Sequence = 6,
    Camera = 8,
    Model = 10,
    Screen = 11,
};
constexpr int kBlockTypes = 13;

// The highest effect level, which Graphic Effects at High gives (effectPriorityLevel in Graphics Rules).
inline constexpr std::uint16_t kHighestLevel = 3;

// A value over an effect's life: evenly spaced samples from its start to its end, or a "packed"
// curve of four values, the effect script's sin.
struct Curve {
    std::vector<float> values;
    bool packed = false;

    // The value at `t`. Samples blend linearly over 0 to 1 and give 0 past either end. A packed curve
    // gives v0 * sin((t - v2) * v1) + v3 at any `t`. 0 for an empty curve.
    float at(float t) const;
};

// Moves the camera while it plays. `curves` are the block's nine curves in file order. Curve 5
// is the yaw offset in degrees, curves 7 and 8 the near and far clip. The rest are not known yet.
struct CameraBlock {
    std::uint32_t flags = 0;
    float length = 0;  // seconds
    std::array<Curve, 9> curves;
    std::string name;
};

// Plays visual effects one after another: each item plays the named effect for a time picked
// from its range, in seconds (an empty name waits).
struct SequenceBlock {
    static constexpr std::uint32_t kLoops = 0x1;           // starts over after its last item
    static constexpr std::uint32_t kStartsPrewarmed = 0x2;
    static constexpr std::uint32_t kLetFinish = 0x4;       // a replaced item is left to finish by itself, and the last ends hard
    static constexpr std::uint32_t kKeepPlaying = 0x8;     // the next item doesn't stop the one before

    struct Item {
        std::array<float, 2> range{};
        std::string effect;
    };
    std::vector<Item> items;
    std::uint32_t flags = 0;
};

// A particle system. Every curve and the colour list run over a particle's life (age / life).
// What the fields not kept here do is not known yet, or not needed so far.
struct ParticleBlock {
    static constexpr std::uint64_t kBursts = 0x1;     // emits `count` particles every `interval`
    static constexpr std::uint64_t kHoldCount = 0x2;  // keeps `count` particles alive, replacing the dead
    static constexpr std::uint64_t kModel = 0x400000;  // `texture` names a shape to draw instead of a sprite
    static constexpr std::uint64_t kAcceptsPicture = 0x1000000;  // a script can give it another texture, as balloons do
    static constexpr std::uint64_t kSustain = 0x4;     // holds its last cycle after `burstLimit`, instead of stopping
    static constexpr std::uint64_t kDirectionIsSpawn = 0x40;  // a particle flies away from where it started
    static constexpr std::uint64_t kBall = 0x100;      // starts in a ball filling the spawn box
    static constexpr std::uint64_t kScaleWithSize = 0x800;    // emission times the effect's scale
    static constexpr std::uint64_t kScaleWithArea = 0x1000;   // emission times its square
    static constexpr std::uint64_t kScaleWithVolume = 0x2000;  // emission times its cube
    static constexpr std::uint64_t kScaleParticles = 0x4000;  // new particles' size times the effect's scale
    static constexpr std::uint64_t kRandomWalk = 0x100000;    // `walk` pushes all the particles about
    static constexpr std::uint64_t kAttractor = 0x4000000;    // `attractors` pull on the particles
    static constexpr std::uint64_t kOldAttractorPoint = 0x8000000;  // in an old file, its one attractor has a point
    static constexpr std::uint64_t kScaleExisting = 0x10000000;  // size messages also reach live particles
    static constexpr std::uint64_t kCycleKill = 0x40000000;   // each new cycle kills the live particles
    static constexpr std::uint64_t kDirectLayer = 0x80000000;  // `layer` is the material's layer as it stands
    static constexpr std::uint64_t kAspectAfterTurn = 0x100000000;  // the aspect narrows the turned sprite
    // Its speed range is 0. A still shape that isn't turned to the camera then lines up along a second
    // random direction instead of its flight. Sprites aren't changed by it.
    static constexpr std::uint64_t kNoSpeed = 0x2000000000;

    // Ways to line up a sprite (the effect script's align).
    enum Alignment : std::uint8_t { Camera = 0, Ground = 1, Source = 5, ZPole = 6 };
    // Ways to draw a sprite (the effect script's draw).
    enum Draw : std::uint8_t {
        Decal = 0,
        DecalInvertDepth = 1,
        DecalIgnoreDepth = 2,
        DepthDecal = 3,
        Additive = 4,
        AdditiveInvertDepth = 5,
        AdditiveIgnoreDepth = 6,
        Modulate = 7,
    };

    // A push that changes every so often. Each change waits a time from `retime`, picks a strength from
    // `speed` and turns two headings by a random share of `turn` plus `leastTurn`, in full turns.
    struct Walk {
        std::array<float, 2> retime{}, speed{};
        float turn = 0, leastTurn = 0;
    };

    // A pull on each particle. Type 0 pulls only when told to by a script, which isn't done yet, and such
    // attractors aren't kept. Type 1 pulls towards `point`, which moves with the effect. The others stay at
    // `point` in the world: 2 pulls towards the line through it along x, 3 towards the plane through it across x,
    // and 4 towards the sphere of radius 1 around it. `strength` runs over the distance as a share of `range`,
    // and is held past it. A particle closer than `killRadius` dies.
    struct Attractor {
        std::uint8_t type = 0;
        std::array<float, 3> point{};
        Curve strength;
        float range = 1, killRadius = -1;
    };

    std::uint64_t flags = 0;
    std::array<float, 2> life{};  // seconds, picked from the range
    float prewarm = 0;            // seconds played before it is first shown
    std::array<float, 2> delay{}, gap{};  // seconds, each used when its first value isn't negative
    std::array<float, 3> directionLow{}, directionHigh{};
    std::array<float, 2> speed{};
    std::array<float, 3> spawnLow{}, spawnHigh{};  // the box particles start in
    float ring = -1;      // 0 or more: the box holds a torus of this thickness
    Curve count;          // particles per burst or kept alive, or per second over a cycle of `interval`
    float interval = 0;   // seconds in a cycle
    std::uint16_t burstLimit = 0;  // cycles, 0: endless
    float speedScale = 0;  // above 0: emission also grows with how fast the effect moves
    Curve size;           // a sprite's height in world units, a shape's scale
    float sizeVary = 0;   // each of these varies a particle by a random factor in [1 - vary, 1 + vary]
    Curve aspect;         // narrows a sprite's width (its size is its height)
    float aspectVary = 0;
    std::array<float, 3> rotationAxis{0, 0, 1};
    std::array<float, 2> rotationScale{1, 1};  // multiplies the rotation curve
    float rotationOffset = 0;
    Curve rotation;       // full turns for sprites, radians for shapes, about `rotationAxis`
    std::vector<std::array<float, 3>> pivots;  // where the sprite turns, over its life, from its middle in shares of its size
    std::array<float, 3> pivotVary{};
    Curve alpha;
    float alphaVary = 0;
    std::vector<std::array<float, 3>> colours;
    std::array<float, 3> colourVary{};
    std::string texture;  // or a shape's name, with kModel
    std::uint8_t atlasColumns = 1, atlasRows = 1;
    std::uint8_t alignment = 0;  // see Alignment
    std::uint8_t blend = 0;      // see Draw
    float layer = 0;
    float framesPerSecond = 0;   // with an atlas
    std::uint8_t firstFrame = 0, frameCycle = 0;  // frameCycle 0: frames never wrap
    std::array<float, 3> gravity{};
    float windShare = 0;
    float blast = 0;  // pushes away from `blastCentre`
    std::array<float, 3> blastCentre{};
    float drag = 0;   // the share of its speed a particle loses each second
    float stretch = 0;  // above 0: a sprite stretches along its flight past this speed
    std::array<std::uint8_t, 4> screenEdge{};  // the effect script's screenBloom bytes
    Walk walk;
    std::vector<Attractor> attractors;
    std::string deathEffect;  // played where a particle dies
};

// A particle system whose particles each play a visual effect, by name.
struct MetaParticleBlock {
    static constexpr std::uint64_t kBursts = 0x1;     // makes `count` particles every `interval`
    static constexpr std::uint64_t kHoldCount = 0x2;  // keeps `count` particles alive, replacing the dead
    static constexpr std::uint64_t kSustain = 0x4;    // holds its last cycle after `burstLimit`, instead of stopping
    static constexpr std::uint64_t kScaleWithSize = 0x80000;     // emission times the effect's scale
    static constexpr std::uint64_t kScaleWithArea = 0x100000;    // emission times its square
    static constexpr std::uint64_t kScaleWithVolume = 0x200000;  // emission times its cube
    static constexpr std::uint64_t kScaleParticles = 0x400000;   // new particles' size times the effect's scale
    static constexpr std::uint64_t kWanderAll = 0x800000;        // `wander` pushes all the particles alike
    static constexpr std::uint64_t kWanderEach = 0x1000000;      // `wander` pushes each particle by itself
    static constexpr std::uint64_t kWanderAtOnce = 0x2000000;    // wandering takes its strength at the start without its first wait
    static constexpr std::uint64_t kFollowsPath = 0x40000000;    // particles fly along `path` instead of by speed and forces

    // A pull that wanders a particle about: every `retime` seconds (picked from the range) it
    // picks a new strength from `speed` and turns its heading by up to `turn` of a full turn,
    // at least `leastTurn`. `upward` (0 to 1) tilts the pull from flat to straight up.
    struct Wander {
        std::array<float, 2> retime{}, speed{};
        float turn = 0, leastTurn = 0, upward = 0;
    };
    std::uint64_t flags = 0;
    std::array<float, 2> life{};  // seconds, picked from the range
    float prewarm = 0;            // seconds played before it is first shown
    std::array<float, 2> delay{}, gap{};  // seconds, each used when its first value isn't negative
    std::array<float, 3> directionLow{}, directionHigh{};
    std::array<float, 2> speed{};
    float damping = 0;            // the share of its speed a particle loses each second
    std::array<float, 3> spawnLow{}, spawnHigh{};  // the box particles start in
    float ring = -1;      // 0 or more: the box holds a torus of this thickness
    Curve count;          // particles per burst or kept alive, or per second over a cycle of `interval`
    float interval = 0;   // seconds in a cycle
    std::uint32_t burstLimit = 0;  // cycles, 0: endless
    Curve size;  // scales the effects its particles play
    float sizeVary = 0;   // each of these varies a particle by a random factor in [1 - vary, 1 + vary]
    Curve rotation;       // full turns about the vertical
    float rotationVary = 0;  // above 0: the rotation times a random number in [-vary, vary]
    std::vector<std::array<float, 3>> colours;
    std::array<float, 3> colourVary{};
    Curve alpha;
    float alphaVary = 0;
    std::uint8_t alignment = 0;  // the effect script's align
    // Above 0, a particle aligned along its flight turns about the vertical only, easing toward its
    // heading by 1 / (15 * alignSmoothing) of the way each update.
    float alignSmoothing = 0;
    float alignSpin = 0;  // not 0: a turn of the particle's own is added, whose source is not known yet
    std::array<float, 3> gravity{};
    float windShare = 0;
    float blast = 0;  // pushes away from `blastCentre`
    std::array<float, 3> blastCentre{};
    float screw = 0;  // above 0: motion not played yet
    // Waves that move where each particle's effect is drawn, not the particle itself: each adds
    // amplitude * sin(t * frequency + phase . (position - the effect's place)), t the block's interval
    // times the seconds it has played, so particles close together move in step.
    struct Wave {
        float frequency = 0;  // radians per second
        std::array<float, 3> phase{}, amplitude{};
    };
    std::vector<Wave> waves;
    std::array<std::uint8_t, 4> screenEdge{};  // the effect script's screenBloom bytes
    std::array<float, 10> terrain{};  // how particles meet the terrain, see meta_particles.cpp
    std::array<Wander, 2> wander;
    std::string effect;
    // Where a particle should be and how it should move at a time in its life, in seconds. Time 0 on the last
    // point stands for the end of its life.
    struct PathPoint {
        std::array<float, 3> position{}, velocity{};
        float time = 0;
    };
    std::vector<PathPoint> path;
};

// A texture laid on the ground (or the water). Every curve and the colour list run over its life.
// What the "light" and "ring" flags do is not known yet.
struct DecalBlock {
    static constexpr std::uint32_t kLight = 0x02;
    static constexpr std::uint32_t kOnWater = 0x04;
    static constexpr std::uint32_t kRepeats = 0x08;      // repeats its texture `repeat` times
    static constexpr std::uint32_t kTerrainSized = 0x10;
    static constexpr std::uint32_t kRing = 0x20;
    static constexpr std::uint32_t kStatic = 0x40;       // only its alpha changes over its life
    // How it is drawn over what is below: the game's OverlayDecal, OverlayAdditive, OverlayModulate,
    // OverlayDecalInverseDepth and OverlayDrawNoOverlap materials.
    enum class Draw : std::uint8_t { Decal, Added, Multiplied, Behind, NoOverlap };
    enum class Life : std::uint8_t { Loops, Once, Holds };
    std::uint32_t flags = 0;
    std::string texture;
    Draw draw = Draw::Decal;
    Life lifeMode = Life::Loops;
    float life = 0;             // seconds
    Curve rotation;             // full turns
    Curve size;                 // world units
    Curve alpha;
    std::vector<std::array<float, 3>> colours;
    Curve aspect;
    float alphaVary = 0, sizeVary = 0;  // each a random factor in [1 - vary, 1 + vary]
    float rotationVary = 0;             // the rotation times a random number in [-vary, vary], 1 without one
    float repeat = 1;                   // texture repeats, with flag 0x08
    std::array<float, 2> textureOffset{};
};

// A full-screen layer (mode 0 additive, 1 blend, 2 tint, 3 brighten, 4 camera view, 5 skybox).
struct ScreenBlock {
    std::uint8_t mode = 0;
    std::uint32_t flags = 0;
    std::vector<std::array<float, 3>> colours;
    Curve strength;
    float length = 0, delay = 0;
    std::string texture;
};

// Shows a model from its start until it is stopped, where its effect places it. Its colour and
// alpha are times the effect's colour and alpha, and the model is drawn tinted and see-through by
// them unless all four are 1. How `size` scales a model is not known yet.
struct ModelBlock {
    std::string model;  // the model's resource name, "_cres" included
    float size = 1;
    std::array<float, 3> colour{1, 1, 1};
    float alpha = 1;
};

// One part of a visual effect: a block, by its type and its place in that type's list, placed by
// `orientation` (3x3, rows), `offset` and `scale`. It plays only while the game's state matches `state`
// under `stateMask`.
struct Component {
    static constexpr std::uint32_t kIgnoreLength = 0x1;  // the effect can finish while this part still plays
    static constexpr std::uint32_t kStateList = 0x8;     // one of a run of blocks that one part switches between
    static constexpr std::uint32_t kEndsStateList = 0x10;
    static constexpr std::uint32_t kIgnoreParams = 0x20;  // the effect's alpha and colour don't reach this part

    std::string name;  // the visual effect to play for type 0, otherwise an authoring label
    std::uint8_t type = 0;
    std::uint32_t flags = 0;
    std::int32_t index = 0;
    float scale = 1;
    std::array<float, 9> orientation{};
    std::array<float, 3> offset{};
    // Plays at levels of detail from lowestDetail up to, not including, detailEnd (the script's lodRange).
    std::uint8_t lowestDetail = 1, detailEnd = 5;
    // Each runs across that range of levels of detail, and multiplies the part's particles.
    std::array<float, 2> rateByDetail{1, 1}, sizeByDetail{1, 1}, alphaByDetail{1, 1};
    std::uint32_t state = 0, stateMask = 0;
    // Components of one group (a run of the same id, 0 for none) play one of them, picked at random by
    // their weights: a roll below 0x10000 against their running total.
    std::uint16_t group = 0, weight = 0;
    float timeScale = 1;  // the part's time runs this much faster
};

struct VisualEffect {
    // The effect script's keywords.
    static constexpr std::uint32_t kViewRelative = 0x1;
    static constexpr std::uint32_t kCameraFacing = 0x2;
    static constexpr std::uint32_t kCameraAttached = 0x4;
    static constexpr std::uint32_t kNoAutoStop = 0x8;   // plays until told to stop, even with nothing left
    static constexpr std::uint32_t kHardStop = 0x10;    // every stop is a hard stop
    static constexpr std::uint32_t kRigid = 0x20;       // living particles move with the effect
    static constexpr std::uint32_t kIgnoreParams = 0x40;  // takes no alpha or colour from outside
    static constexpr std::uint32_t kApplyCursor = 0x80;
    static constexpr std::uint32_t kIgnoreScale = 0x100;
    static constexpr std::uint32_t kIgnoreOrientation = 0x200;
    static constexpr std::uint32_t kOrientZPole = 0x400;  // keeps only its turn about the vertical
    static constexpr std::uint32_t kRealTime = 0x1000;    // plays on while the game is paused
    static constexpr std::uint32_t kDetach = 0x2000;      // stays where it was first placed after starting
    static constexpr std::uint32_t kClampScreenSize = 0x8000;

    std::uint32_t flags = 0;
    std::uint16_t level = 0;  // the lowest Graphic Effects level that may play this record
    std::uint32_t stateBits = 0;  // every state bit its components test
    std::vector<Component> components;
};

struct File {
    std::array<std::uint32_t, kBlockTypes> blockCounts{};  // every type's count, by type - 1
    std::vector<CameraBlock> cameras;
    std::vector<SequenceBlock> sequences;
    std::vector<ParticleBlock> particles;
    std::vector<MetaParticleBlock> metaParticles;
    std::vector<DecalBlock> decals;
    std::vector<ModelBlock> models;
    std::vector<ScreenBlock> screens;
    std::vector<VisualEffect> effects;
    // Lower-case name -> effect indices. A name can have one record per level.
    std::unordered_map<std::string, std::vector<std::uint32_t>> names;

    // The visual effect with this name (the game ignores case) to play at `level`: the record with the
    // highest level not above it, and never one of level 0. Null when there is none.
    const VisualEffect* find(std::string_view name, std::uint16_t level = kHighestLevel) const;

    // True when the file has this name at any level.
    bool knows(std::string_view name) const;
};

// Reads a whole effects file. Throws on a block version the game would not read, on counts the
// data can't hold and on strings longer than 64 KiB.
File parse(std::span<const std::uint8_t> data);

// Adds a file read later to `into`, as the game joins its products' files. A later record replaces an
// earlier one of the same name and level. Other levels of that name stay.
void merge(File& into, File&& later);

} // namespace ts2::effects
