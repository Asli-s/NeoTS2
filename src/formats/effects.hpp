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

// A value over an effect's life: evenly spaced samples from its start to its end. A "packed"
// curve keeps at most four values and the game's use of them is not known yet.
struct Curve {
    std::vector<float> values;
    bool packed = false;

    // The value at `t` (0 to 1): the samples blended linearly, the ends held. A packed curve gives
    // its first value, since how the game reads it is not known yet. 0 for an empty curve.
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
    std::uint64_t flags = 0;
    std::array<float, 2> life{};  // seconds, picked from the range
    float prewarm = 0;            // seconds played before it is first shown
    std::array<float, 3> spawnLow{}, spawnHigh{};  // the box particles start in
    Curve count;          // particles per burst, read at its start
    float interval = 0;   // seconds between bursts
    std::uint16_t burstLimit = 0;  // 0: endless
    Curve size;           // a sprite's height in world units, a shape's scale
    Curve aspect;         // narrows a sprite's width (its size is its height)
    std::array<float, 3> rotationAxis{0, 0, 1};
    std::array<float, 2> rotationScale{1, 1};  // multiplies the rotation curve
    float rotationOffset = 0;
    Curve rotation;       // full turns for sprites, radians for shapes, about `rotationAxis`
    Curve alpha;
    std::vector<std::array<float, 3>> colours;
    std::string texture;  // or a shape's name, with kModel
    std::uint8_t atlasColumns = 1, atlasRows = 1;
    std::uint8_t alignment = 0;  // 0: faces the camera
    std::uint8_t blend = 0;      // 4: added (stdPMAdditive), 0: blended
    float layer = 0;
};

// A particle system whose particles each play a visual effect, by name.
struct MetaParticleBlock {
    // A pull that wanders a particle about: every `retime` seconds (picked from the range) it
    // picks a new strength from `speed` and turns its heading by up to `turn` of a full turn,
    // at least `leastTurn`. `upward` (0 to 1) tilts the pull from flat to straight up.
    struct Wander {
        std::array<float, 2> retime{}, speed{};
        float turn = 0, leastTurn = 0, upward = 0;
    };
    std::uint64_t flags = 0;
    std::array<float, 2> life{};  // seconds, picked from the range
    float damping = 0;            // the share of its speed a particle loses each second
    std::array<float, 3> spawnLow{}, spawnHigh{};  // the box particles start in
    Curve size;  // scales the effects its particles play
    std::array<Wander, 2> wander;
    std::string effect;
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

// One part of a visual effect: a block, by its type and its place in that type's list, placed by
// `orientation` (3x3, rows), `offset` and `scale`. It plays only while the game's state matches `state`
// under `stateMask`.
struct Component {
    std::string name;  // an authoring label only
    std::uint8_t type = 0;
    std::int32_t index = 0;
    float scale = 1;
    std::array<float, 9> orientation{};
    std::array<float, 3> offset{};
    std::uint32_t state = 0, stateMask = 0;
    // Components of one group (a run of the same id, 0 for none) play one of them, picked at random by
    // their weights: a roll below 0x10000 against their running total.
    std::uint16_t group = 0, weight = 0;
};

struct VisualEffect {
    std::uint32_t flags = 0;
    std::vector<Component> components;
};

struct File {
    std::array<std::uint32_t, kBlockTypes> blockCounts{};  // every type's count, by type - 1
    std::vector<CameraBlock> cameras;
    std::vector<SequenceBlock> sequences;
    std::vector<ParticleBlock> particles;
    std::vector<MetaParticleBlock> metaParticles;
    std::vector<DecalBlock> decals;
    std::vector<ScreenBlock> screens;
    std::vector<VisualEffect> effects;
    std::unordered_map<std::string, std::uint32_t> names;  // lower-case name -> effect index

    // The visual effect with this name (the game ignores case), if any.
    const VisualEffect* find(std::string_view name) const;
};

// Reads a whole effects file. Throws on a block version the game would not read, on counts the
// data can't hold and on strings longer than 64 KiB.
File parse(std::span<const std::uint8_t> data);

} // namespace ts2::effects
