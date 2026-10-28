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
    std::uint32_t padding[3];
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
    float shadowReceiver[4];  // x: 1 when Sims' projected shadows fall on it, y: its level, z: 1 to fade them by the light map
};

// The shaders' constant buffers pack in 16-byte rows, so these must match them field for field.
static_assert(sizeof(VertexUniforms) % 16 == 0 && offsetof(VertexUniforms, eye) == 688 && offsetof(VertexUniforms, specular) == 704);
static_assert(offsetof(SkinnedUniforms, jointBase) == 720 && sizeof(SkinnedUniforms) % 16 == 0);
static_assert(sizeof(FragmentUniforms) % 16 == 0 && offsetof(FragmentUniforms, lightMap) == 48 &&
              offsetof(FragmentUniforms, envCube) == 128 && offsetof(FragmentUniforms, emissive) == 192 &&
              offsetof(FragmentUniforms, uvTile) == 208 && offsetof(FragmentUniforms, shadowReceiver) == 224);

// The Sims' shadow pictures share one atlas, a square of tiles each simshadows::kResolution texels
// wide.
constexpr int kShadowTilesAcross = 4;
constexpr std::size_t kMaxShadowCasters = kShadowTilesAcross * kShadowTilesAcross;

// What the scene's fragment shader is given once a frame about the Sims' projected shadows.
struct ShadowUniforms {
    float cameras[kMaxShadowCasters][16];  // each caster's picture camera (see simshadows::camera)
    float casters[kMaxShadowCasters][4];   // x, y: its tile's corner in the atlas (0..1), z: its level
    float shade[4];                        // rgb: what a shadow multiplies the ground by, w: how many casters
    // The lot objects' shadows: the grid's first tile (x, y) and its size in tiles (z by w, 0 for none).
    float objectGrid[4];
};
static_assert(sizeof(ShadowUniforms) % 16 == 0 && offsetof(ShadowUniforms, shade) == 1280 && offsetof(ShadowUniforms, objectGrid) == 1296);

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

// What an object's picture is blurred into its tile with.
struct ObjectBlurUniforms {
    float weights[25][4];  // x only, as ShadowBlurUniforms
    float tile[4];         // x, y: the tile's corner in the atlas (texels), z: the picture's size (texels)
};

// What the blur shader is given: each tap's weight (see simshadows::blurWeight), across then up.
struct ShadowBlurUniforms {
    float weights[25][4];  // x only
};

// A neighborhood's painted terrain (see scene::PaintedTerrain).
struct TerrainUniforms {
    float alphaSize, textureRepeat, cellTiles, layers, cellsX, cellsY, lightMapScale, litFrom;
    float gradientDepth, reflected;
    float slice, passAlpha;  // OpenGL 2 draws a pass per paint texture (see terrain.frag.hlsl); unused elsewhere
};

struct WaterUniforms {
    float tint[4];
    float screen[2];
    float offset, scale, speed, time, padding[2];
};

enum class InView { Outside, Inside, Partly };

// Where a model-space box lies against the view of `clip` (projection * view * model). It is
// outside when all eight corners are outside the same side of the clip volume (-w <= x, y <= w,
// 0 <= z <= w), so none of its triangles can show, and inside when no corner is outside any side.
InView boxInView(const Mat4& clip, const std::array<float, 3>& low, const std::array<float, 3>& high);

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

// A primitive to draw, with what its draw needs.
struct Draw {
    const GpuPrimitive* primitive;
    const scene::Instance* instance;
    const scene::Material* material;
    int layer;    // the material's draw layer
    float depth;  // blended: the game's sort depth (nearestDepth)
    float sun;    // how much of the sun reaches it
};

// What one draw pushes and binds.
struct DrawUniforms {
    SkinnedUniforms vertex{};
    FragmentUniforms fragment{};
    std::array<SamplerBinding, 6> samplers{};
};

struct FramePlan {
    // The shown instances' primitives in view, split by how they are drawn (opaque, layered, blended)
    // and in the game's order: by layer, and blended surfaces of a layer from the farthest to the nearest.
    static void collectDraws(const RendererState& state, const ResourceQuery& resources, const FrameView& f, std::vector<Draw>& opaque,
                             std::vector<Draw>& layers, std::vector<Draw>& blended);
    // One draw's uniforms and textures: its place, the lights it is lit by, and its material.
    static DrawUniforms uniformsFor(const RendererState& state, const ResourceQuery& resources, const FrameView& f, const Draw& d, int blend);

    // The Sims casting projected shadows this frame, at most kMaxShadowCasters (the rest cast none).
    // Each one's skinned primitives that can be drawn, in its tile's order.
    static std::vector<std::vector<Draw>> shadowCasters(const RendererState& state, const ResourceQuery& resources);
    // What a caster's primitive is drawn into its shadow picture with.
    static SkinnedUniforms shadowCasterUniforms(const RendererState& state, const Draw& d, std::size_t caster);
    // The shadows' camera and colour for the scene's fragment shader, for `casters` casters.
    static ShadowUniforms shadowUniforms(const RendererState& state, std::size_t casters);
    static ShadowBlurUniforms shadowBlurUniforms();

private:
    static void setLights(const RendererState& state, DrawUniforms& u, const Draw& d, const scene::ObjectLighting& l, bool skirt, bool roof,
                          bool bumped);
    static void setMaterial(const RendererState& state, const ResourceQuery& resources, DrawUniforms& u, const FrameView& f, const Draw& d,
                            int blend, int normalMap, const scene::ObjectLighting* lighting);
};

} // namespace ts2::render
