// What drawing a frame needs from the game's rules, apart from any graphics API: what is in view, in
// which order it is drawn, and the numbers the shaders are given for each draw. A renderer carries
// the plan out with its own calls.
#pragma once

#include "render/math.hpp"
#include "render/mesh_pack.hpp"
#include "render/shared_state.hpp"
#include "render/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ts2::render {

constexpr std::size_t kObjectLights = 8;  // as many as the game's shaders take

struct VertexUniforms {
    float viewProjection[16];
    float model[16];
    float ambient[4];  // w: 1 when lit as the game lights objects (see scene::ObjectLighting), 3 as its lot skirt
    float lightDirections[kObjectLights][4];  // w: a point light's range squared (0: directional)
    float lightColors[kObjectLights][4];
    float lightPositions[kObjectLights][4];   // a point light's position and falloff
    float lightCones[kObjectLights][4];       // a spot light's edge cosine, 1 / (inner - edge) cosines, 1
    float thumbnail[4];  // the material's diffuse and the thumbnail scale (see scene::ObjectLighting)
    float pixelLights[4];         // how many lights are lit per pixel through the normal map, and which (whole numbers)
    float eye[4];                 // the camera's position
    float specular[4];            // the material's specular scale and power (0: none)
};

struct SkinnedUniforms {
    VertexUniforms frame;
    std::uint32_t jointBase;
    std::uint32_t morphRow;  // see RendererState::morphRow
    std::uint32_t padding[2];
    float eyes[4];  // see RendererState::eyesOf
};

struct FragmentUniforms {
    float color[4];
    float sunDirection[4];
    float hasTexture;
    float alphaCutoff;
    float unlit;
    float alphaReplicate;  // 1: the texture's alpha stands in for its colour
    float lightMap[4];  // x: the gain on the light map (0: none), y: 1 to leave the material's colour out, z: 1 for terrain
    float pixelLightColors[3][4];  // the colours of the lights lit per pixel
    float pixelLightCount;         // how many (0: none)
    float normalMapped;            // 1: the per-pixel normal from the normal map, 0: straight out
    float simLighting;             // 1: lit as the game's Sim materials (see scene::Material)
    float lightMapFade;            // how much of the light map is the new one (see IRenderer::setLightMapFade)
    float envCube[4];              // the environment cube's colour scale, w: 1 when on (see scene::Material)
    // The camera's right, up and forward, right's w 1 to look the cube up in the camera's space.
    float cameraRight[4];
    float cameraUp[4];
    float cameraForward[4];
    float emissive[4];  // added to the light before the texture (see scene::Material), w: 1 when on
    float uvTile[4];    // the texture coordinates times xy plus zw: one frame of a tiled texture
    float shadowReceiver[4];  // x: 1 when Sims' projected shadows fall on it, y: its level, z: 1 to fade them by the light map, w: 1 for a floor
    float caustics[4];        // x: 1 for a pool floor, 2 for a pool wall, 3 for the caustics alone over a pond's ground, 0 for none (see scene::Material), yzw: the texture light
    float neighborLight[4];   // rgb: what a neighbor's real house is tinted by, w: 1 when it is one (its ground too), 2 for the ground around the lot
    float wallMask[4];        // x: 1 when a wall mask cuts it, y: 1 when the mask runs the other way along u (see scene::Material::mask)
    float terrainTexture[4];  // x: 1 for terrain, y: 1 with a detail texture, z: its repeats (see scene::Material::terrain)
    float underwaterShade[4];  // x: 1 when darkened below the water (see scene::Material::partlyUnderwater), y: 1 when hazed under a pool-style sea, z: 1 for the ground lot view hazes
    float skirtLamps[3][4];    // a lot skirt's lamp light from the light map: its coordinates' rows, then x the gain (see scene::Instance::skirtLampMap)
};

// The shaders' constant buffers pack in 16-byte rows, so these must match them field for field.
static_assert(sizeof(VertexUniforms) % 16 == 0 && offsetof(VertexUniforms, eye) == 688 && offsetof(VertexUniforms, specular) == 704);
static_assert(offsetof(SkinnedUniforms, jointBase) == 720 && sizeof(SkinnedUniforms) % 16 == 0);
static_assert(sizeof(FragmentUniforms) % 16 == 0 && offsetof(FragmentUniforms, lightMap) == 48 &&
              offsetof(FragmentUniforms, envCube) == 128 && offsetof(FragmentUniforms, emissive) == 192 &&
              offsetof(FragmentUniforms, uvTile) == 208 && offsetof(FragmentUniforms, shadowReceiver) == 224 &&
              offsetof(FragmentUniforms, caustics) == 240);

// The Sims' shadow pictures share one atlas, a square of tiles each simshadows::kResolution texels
// wide.
constexpr int kShadowTilesAcross = 4;
constexpr std::size_t kMaxShadowCasters = kShadowTilesAcross * kShadowTilesAcross;

// The most neighbors drawn as real houses whose lots the ground around a lot leaves out. The scene
// shader's neighborGround array holds as many.
constexpr std::size_t kMaxNeighborGrounds = 32;
// The most rows of the neighborhood's underwater gradient the scene shader is given (the game's has 64).
constexpr std::size_t kUnderwaterRows = 64;

// What the scene's fragment shader is given once a frame about the Sims' projected shadows.
struct ShadowUniforms {
    // Each caster's picture camera (see simshadows::camera), indoors its quad (simshadows::indoorQuad).
    float cameras[kMaxShadowCasters][16];
    float casters[kMaxShadowCasters][4];   // x, y: its tile's corner in the atlas (0..1), z: its level, w: 1 indoors
    float shade[4];                        // rgb: what a shadow multiplies the ground by, w: how many casters
    // The lot objects' shadows: the grid's first tile (x, y) and its size in tiles (z by w, 0 for none).
    float objectGrid[4];
    // The pools' caustics: world to the pool shadow map's texture coordinates and depth, world to
    // the lot, and toward the sun.
    float poolShadow[16];
    float toLot[16];
    float poolSun[4];
    float ambientOcclusion[4];  // x: 1 when the surfaces drawn are multiplied by it
    // The lots of the neighbors drawn as real houses (x0, x1, y0, y1 in the world), which the ground around
    // the lot leaves out, and how many there are (the first number).
    float neighborGround[kMaxNeighborGrounds][4];
    float neighborGrounds[4];
    // The neighborhood's underwater gradient: x the water's height, y the depth the gradient spans,
    // z how many of its rows are given (0 for none, outside the neighborhood view), and the rows
    // from the surface down.
    float underwater[4];
    float underwaterGradient[kUnderwaterRows][4];
    // The lot's outdoor floor quarters, the only floor the objects' pictures fall on: the scene's
    // frame to the lot's (three rows), and the mask's tiles along x and y, its first level and how
    // many levels it holds (0 for none, when every floor takes them).
    float objectFloorsToLot[3][4];
    float objectFloors[4];
    float seaLevel[4];      // x: the pool-style sea's surface, which its haze is deepened from, yz: its depth map's points (0 for none), w: how bright its haze is (see scene::PaintedTerrain::seaLight)
    float seaDepthArea[4];  // the depth map's area: x0, y0, x1, y1 (see scene::PaintedTerrain::seaDepthMap)
};
static_assert(sizeof(ShadowUniforms) % 16 == 0 && offsetof(ShadowUniforms, shade) == 1280 && offsetof(ShadowUniforms, objectGrid) == 1296 &&
              offsetof(ShadowUniforms, poolShadow) == 1312 && offsetof(ShadowUniforms, poolSun) == 1440);

// The lot objects' pictures from the sun: an atlas of kObjectTilesAcross square tiles of
// simshadows::kResolution texels, each picture in the corner of its tile.
constexpr int kObjectTilesAcross = 16;
constexpr std::size_t kMaxObjectPictures = kObjectTilesAcross * kObjectTilesAcross;
// The places' list: a row of kObjectPlaceTexels texels each (the camera's first three rows, then
// the tile's corner, the picture's share of the atlas and the level, then the rect).
constexpr int kObjectPlaceTexels = 5;
constexpr std::size_t kMaxObjectPlaces = 1024;
// The grid over the lot: up to kObjectGridSlots places per tile, two texels of four each.
constexpr int kObjectGridSlots = 8;
constexpr int kMaxObjectGrid = 256;
// The outdoor floor mask: a texel per lot tile and level, a channel per quarter.
constexpr int kMaxObjectFloorTiles = 256;
constexpr int kMaxObjectFloorLevels = 8;

// What an object's picture is blurred into its tile with.
struct ObjectBlurUniforms {
    float weights[25][4];  // x only, see FramePlan::outdoorBlurWeights
    float tile[4];         // x, y: the tile's corner in the atlas (texels), z: the picture's size (texels)
};

// What the blur shader is given: each tap's weight (see simshadows::blurWeight), across then up.
struct ShadowBlurUniforms {
    // The taps' weights out to 3 texels, x by (across + 3) * 7 + (up + 3), for the outdoor blur
    // (index 0, nothing past 2) and the indoor one (index 1).
    float weights[2][49][4];
    float indoor[kMaxShadowCasters][4];  // x: 1 when the caster in that tile is indoors
};

// A neighborhood's painted terrain (see scene::PaintedTerrain).
struct TerrainUniforms {
    float alphaSize, textureRepeat, cellTiles, layers, cellsX, cellsY, lightMapScale, litFrom;
    float gradientDepth, reflected;
    float slice, passAlpha;  // OpenGL 2 draws a pass per paint texture (see terrain.frag.hlsl), unused elsewhere
    float seaHaze;           // 1 under the neighborhood view's pool-style sea, 2 around the lot in lot view's
    float seaLevel;          // its surface, which its haze is deepened from
    float seaLight;          // how bright its haze is now (see scene::PaintedTerrain::seaLight)
    float unused;
};

struct WaterUniforms {
    float tint[4];
    float screen[2];
    float offset, scale, speed, time, padding[2];
};

// What a pool's water surface is drawn with (see pool.vert.hlsl).
struct PoolUniforms {
    float viewProjection[16];
    float model[16];
    float eye[4];
    float waveX[4];
    float waveY[4];
    float waveNormal[4];
    float light[4];
    float lightColor[4];
    float night[4];
    float time[4];
    float tint[4];
};
static_assert(sizeof(PoolUniforms) == 272);

// What a beach lot's sea is drawn with (see sea.vert.hlsl).
struct SeaUniforms {
    float viewProjection[16];
    float model[16];
    float eye[4];
    float time[4];  // x: seconds, y: how much the ripples blur the mirror image
    float tint[4];
    float scale[4];  // rgb: what the mirror image is multiplied by, a: opacity added
};
static_assert(sizeof(SeaUniforms) == 192);

// What the water in a lot's ponds is drawn with (see pond.vert.hlsl).
struct PondUniforms {
    float viewProjection[16];
    float model[16];
    float eye[4];
    float light[4];   // rgb: the outdoors' texture light, which tints the sky's reflection
    float ripple[4];  // x: seconds the lot has run unpaused, y: the slide's turn rate (radians a second), z: repeats per tile
    float padding[4];
};
static_assert(sizeof(PondUniforms) == 192);

// How far under a pool's water the lot is mirrored for its reflection.
constexpr float kPoolMirrorDrop = 0.01f;

// The pools' caustics tile and shadow map, in texels a side (the game's causticsTile and
// poolRenderTarget).
constexpr unsigned kCausticsTile = 64;
constexpr unsigned kPoolShadowMap = 1024;

// The sun's view of a pool, as the game fits it for the caustics: looking along the sun, across the
// pool's box (see scene::Instance::poolBox) widened 2 tiles each way and raised 6, as deep as the
// box is along the sun and as tall as that depth, hanging from the box's top. `clip` takes the
// world to its clip space (depth 0 to 1), `texture` to the shadow map's texture coordinates and
// depth, `toLot` to the lot.
struct PoolLight {
    Mat4 clip, texture, toLot;
};
PoolLight poolLight(const std::array<float, 12>& lotPlace, const std::array<float, 6>& box, const Vec3& toSun);

// What draws the caustics tile: the flipbook's frame (column, row) out of 8 by 4, and its colour.
struct CausticsUniforms {
    float frame[4];  // xy: the frame's corner in the flipbook (0..1), zw: one frame's size
    float color[4];
};
CausticsUniforms causticsFrame(float seconds, float brightness);

// The view a pool's reflection is drawn with: `viewProjection` mirrored in the level plane at
// height `plane`, then flipped left to right as the game flips it, which keeps triangles' winding.
// Its depth is w minus the height above the plane, so nothing below the plane is drawn and the
// lowest surface in front wins, as in the game's reflection. `keepBelow` draws that much under the
// plane too (0 in the game).
Mat4 poolMirror(const Mat4& viewProjection, float plane, float keepBelow = 0);

enum class InView { Outside, Inside, Partly };

// Where a model-space box lies against the view of `clip` (projection * view * model). It is
// outside when all eight corners are outside the same side of the clip volume (-w <= x, y <= w,
// 0 <= z <= w), so none of its triangles can show, and inside when no corner is outside any side.
InView boxInView(const Mat4& clip, const std::array<float, 3>& low, const std::array<float, 3>& high);
// The same answer as boxInView giving Outside, cheaper as most boxes on screen settle at their first corner.
bool boxOutside(const Mat4& clip, const std::array<float, 3>& low, const std::array<float, 3>& high);

// How the game sorts a blended surface: the view depth of its box's nearest corner, a corner at or
// behind the camera counting as on the near plane. `clip`'s last row gives the view depth (the
// projection's w).
float nearestDepth(const Mat4& clip, const std::array<float, 3>& low, const std::array<float, 3>& high, float nearZ);

// A texture a draw samples, as a renderer finds it among what it uploaded.
struct TextureRef {
    enum class Kind : std::uint8_t {
        White,      // stands in for surfaces without a texture
        Image,      // the scene's image `index`
        BlackCube,  // stands in for materials without an environment cube
        EnvCube,    // the scene's environment cube `index`
        FadeFrom,   // the light map `index` is fading from (see IRenderer::setImages)
    };
    Kind kind = Kind::White;
    int index = 0;
    bool operator==(const TextureRef&) const = default;
};

// How a texture is sampled: repeating, or clamped at its edges. Both filter with mip levels.
enum class SamplerKind : std::uint8_t { Repeat, Clamped };

struct SamplerBinding {
    TextureRef texture;
    SamplerKind sampler = SamplerKind::Repeat;
    bool operator==(const SamplerBinding&) const = default;
};

// What a plan asks of the renderer about what it has uploaded.
class ResourceQuery {
public:
    virtual ~ResourceQuery() = default;
    // The uploaded scene's meshes, by scene mesh index.
    [[nodiscard]] virtual const std::vector<GpuMesh>& meshes() const = 0;
    // Whether the scene's image / environment cube / faded-from light map is uploaded (not broken or out of range).
    [[nodiscard]] virtual bool hasImage(int index) const = 0;
    [[nodiscard]] virtual bool hasEnvCube(int index) const = 0;
    [[nodiscard]] virtual bool hasFadeFrom(int index) const = 0;
    [[nodiscard]] virtual float lightMapFade() const = 0;
    // Whether skinned primitives of mesh buffer set `buffers` can be drawn: it has skin data, and
    // there are joint matrices on the GPU.
    [[nodiscard]] virtual bool skinReady(std::uint32_t buffers) const = 0;
};

// How a frame is seen.
struct FrameView {
    const Camera* camera = nullptr;
    float aspect = 1, fovY = 0, nearZ = 0;
    Mat4 viewProjection;
    Vec3 sun{};

    static FrameView make(const Camera& camera, unsigned width, unsigned height);
    // The view keeps a pointer to its camera, so a temporary one would be gone before it is read.
    static FrameView make(Camera&& camera, unsigned width, unsigned height) = delete;
};

// What the sky shader is given: the camera's right and up (scaled to the field of view) and forward,
// forward's w being `up`: 1, or -1 for the rays mirrored in the water.
std::array<float, 12> skyCamera(const FrameView& f, float up);

// skyCamera's mirrored rays, also flipped left to right as a mirror image drawn with poolMirror is.
std::array<float, 12> mirroredSkyCamera(const FrameView& f);

// A primitive to draw, with what its draw needs.
struct Draw {
    const GpuPrimitive* primitive;
    const scene::Instance* instance;
    const scene::Material* material;
    int layer;    // the material's draw layer
    float depth;  // blended: the game's sort depth (nearestDepth)
    float sun;    // how much of the sun reaches it
};

// The part of a pool's mirror image (see poolMirror) that the pool surfaces among `blended` read, as
// x0, x1, y0, y1 in the mirror's clip space (-1..1). It allows for the waves, the ripples' shift,
// murky water's blur and filtering, so nothing outside it is ever seen. The whole image when a
// surface can't be bounded.
std::array<float, 4> poolMirrorArea(const FrameView& f, std::span<const Draw> blended, unsigned width, unsigned height);
// `viewProjection` with that area stretched over the whole clip space, to cull what lies outside it.
// Depth is unchanged, so blended surfaces sort as before.
Mat4 narrowedTo(const Mat4& viewProjection, const std::array<float, 4>& area);

// What one draw pushes and binds.
struct DrawUniforms {
    SkinnedUniforms vertex{};
    FragmentUniforms fragment{};
    std::array<SamplerBinding, 8> samplers{};
};

struct FramePlan {
    // The shown instances' primitives in view, split by how they are drawn (opaque, layered, blended)
    // and in the game's order: by layer, and blended surfaces of a layer from the farthest to the nearest.
    static void collectDraws(const RendererState& state, const ResourceQuery& resources, const FrameView& f, std::vector<Draw>& opaque,
                             std::vector<Draw>& layers, std::vector<Draw>& blended);
    // Where the surfaces of layers up to `layer` end in `blended`, searching from `from`.
    static std::size_t endOfLayer(std::span<const Draw> blended, std::size_t from, int layer);
    // The least alpha a particle's pixel is drawn with: more for the kind that writes depth.
    static float particleAlphaReference(std::uint8_t draw);
    // One draw's uniforms and textures: its place, the lights it is lit by, and its material.
    static DrawUniforms uniformsFor(const RendererState& state, const ResourceQuery& resources, const FrameView& f, const Draw& d, int blend);
    // A pool's water surface's uniforms (see scene::Material::poolSurface).
    static PoolUniforms poolUniformsFor(const RendererState& state, const FrameView& f, const Draw& d);
    // The water in a lot's ponds' uniforms (see scene::Material::pondWater).
    static PondUniforms pondUniformsFor(const RendererState& state, const FrameView& f, const Draw& d);
    // The first pool water surface in `blended`, nullptr for none.
    static const Draw* poolWater(const std::vector<Draw>& blended);
    // A beach lot's sea's uniforms (see scene::Material::skirtSea).
    static SeaUniforms seaUniformsFor(const RendererState& state, const FrameView& f, const Draw& d);
    // The first beach sea in `blended`, nullptr for none.
    static const Draw* skirtSea(const std::vector<Draw>& blended);
    // What casts the pools' shadow for their caustics, as the game draws it: lot objects and Sims, and
    // the pool's own floor and walls, wherever they reach the sun's view `clip`.
    static void poolCasters(const RendererState& state, const ResourceQuery& resources, const Mat4& clip, std::vector<Draw>& out);

    // The Sims casting projected shadows this frame, at most kMaxShadowCasters (the rest cast none).
    // Each one's skinned primitives that can be drawn, in its tile's order.
    static std::vector<std::vector<Draw>> shadowCasters(const RendererState& state, const ResourceQuery& resources);
    // What a caster's primitive is drawn into its shadow picture with.
    static SkinnedUniforms shadowCasterUniforms(const RendererState& state, const Draw& d, std::size_t caster);
    // The shadows' camera and colour for the scene's fragment shader, for `casters` casters.
    static ShadowUniforms shadowUniforms(const RendererState& state, std::size_t casters);
    // The blur's weights, and which casters are indoors.
    static ShadowBlurUniforms shadowBlurUniforms(const RendererState& state);
    // The outdoor blur's 5 x 5 weights, x by (across + 2) * 5 + (up + 2), as the objects' pictures take them.
    static std::array<std::array<float, 4>, 25> outdoorBlurWeights();

private:
    static void setLights(const RendererState& state, DrawUniforms& u, const Draw& d, const scene::ObjectLighting& l, bool skirt, bool roof,
                          bool bumped);
    static void setMaterial(const RendererState& state, const ResourceQuery& resources, DrawUniforms& u, const FrameView& f, const Draw& d,
                            int blend, int normalMap, const scene::ObjectLighting* lighting);
};

} // namespace ts2::render
