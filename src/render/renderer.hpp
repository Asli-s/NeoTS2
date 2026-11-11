#pragma once

#include "render/frame_plan.hpp"
#include "render/irenderer.hpp"
#include "render/math.hpp"
#include "render/mesh_pack.hpp"
#include "render/shared_state.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

struct SDL_Window;
struct SDL_GPUDevice;
struct SDL_GPUBuffer;
struct SDL_GPUTransferBuffer;
struct SDL_GPUTexture;
struct SDL_GPUSampler;
struct SDL_GPUGraphicsPipeline;
struct SDL_GPUCommandBuffer;
struct SDL_GPURenderPass;
struct SDL_GPUFence;

namespace ts2::render {

// Draws a Scene with the GPU through SDL (Direct3D 12 or Vulkan): opaque surfaces first, then
// alpha-tested ones, then blended ones. Shaders are loaded from files in `shaderFolder`.
// An upload buffer for data sent every frame, see mapStream.
struct StreamUpload {
    SDL_GPUTransferBuffer* buffer = nullptr;
    std::uint32_t capacity = 0;  // bytes
};

class SdlGpuRenderer final : public RendererState, private ResourceQuery {
public:
    SdlGpuRenderer(SDL_Window* window, const std::filesystem::path& shaderFolder);
    ~SdlGpuRenderer() override;

    using IRenderer::upload;
    [[nodiscard]] Capabilities capabilities() const override {
        Capabilities out;
        out.mostSmoothEdgeSamples = mostSmoothEdgeSamples();
        return out;
    }
    void setSmoothEdges(int samples) override;
    void setAmbientOcclusion(float strength) override;
    void setAmbientOcclusionHalfSize(bool half) override;
    [[nodiscard]] Prepared prepare(const scene::Scene& scene) const override;
    void upload(Prepared prepared) override;
    [[nodiscard]] Prepared swap(Prepared prepared) override;
    [[nodiscard]] bool uploaded() override;
    void setSprites(const std::vector<std::pair<int, const scene::Image*>>& images) override;
    [[nodiscard]] bool draw(const Camera& camera) override;
    [[nodiscard]] bool drawToFile(const Camera& camera, unsigned width, unsigned height, const std::filesystem::path& path) override;
    [[nodiscard]] std::optional<scene::Image> drawInstancesToImage(const Camera& camera, unsigned width, unsigned height,
                                                                   std::optional<scene::ObjectLighting> lighting) override;
    [[nodiscard]] bool makePreparedLevels(Prepared& scene, std::size_t textures) override;
    void releasePictureTargets() override;
    [[nodiscard]] Pictures startPreparedImages(Prepared& scene, const std::vector<PreparedShot>& shots) override;
    [[nodiscard]] std::optional<std::vector<scene::Image>> takePreparedImages(PicturesBase& pictures) override;
    std::string backend() const override;
    std::string device() const override;
    void benchmark(const Camera& camera, unsigned width, unsigned height, int frames) override;
    void addMaterials(const scene::Scene& scene) override;
    void addMeshes(const scene::Scene& scene) override;
    [[nodiscard]] Addition prepareAddition(const scene::Scene& add, const std::vector<int>& shared) const override;
    void leaveOutPrepared(Addition& addition, const std::vector<std::size_t>& positions) const override;
    void addPrepared(const scene::Scene& scene, Addition addition) override;
    void setImages(const scene::Scene& scene, const std::vector<int>& indexes, bool fade = false) override;
    void setLightMapFade(float fade) override;
    void setSkybox(const std::vector<scene::Image>& faces) override;
    void setTerrainLightMap(const scene::Image& lightMap) override;
    void setSeaLight(float light) override;
    void setNeighborhoodSky(const std::vector<scene::Image>& faces) override;
    void setSurroundings(const scene::Scene& scene) override;
    [[nodiscard]] ImposterTextures prepareImposterTextures(const scene::SharedImages& images) const override;
    void addImposters(const scene::Scene& scene, ImposterTextures ahead = {}) override;

private:
    // What the frame plan asks about the uploaded scene (see ResourceQuery).
    const std::vector<GpuMesh>& meshes() const override { return gpu_->meshes; }
    bool hasImage(int index) const override;
    bool hasEnvCube(int index) const override;
    bool hasFadeFrom(int index) const override;
    float lightMapFade() const override { return gpu_->lightMapFade; }
    bool skinReady(std::uint32_t buffers) const override;
    // The texture and sampler a plan's reference stands for.
    SDL_GPUTexture* textureFor(const TextureRef& ref) const;
    SDL_GPUSampler* samplerFor(SamplerKind kind) const { return kind == SamplerKind::Clamped ? clampedSampler_ : sampler_; }

    // One vertex, one index and one skin buffer: the uploaded scene's meshes, then each batch of
    // meshes added later (see addMeshes).
    struct MeshBuffers {
        SDL_GPUBuffer* vertices = nullptr;
        SDL_GPUBuffer* indices = nullptr;
        SDL_GPUBuffer* skin = nullptr;
        SDL_GPUBuffer* morphs = nullptr;  // the morphing vertices' deltas (see MeshData::morphs), none without
    };
    // Binds what a skinned draw from `buffers` reads besides its vertices: the joint matrices and
    // morph weights, and the set's morph deltas (a stand-in when it has none).
    void bindSkinStorage(SDL_GPURenderPass* pass, const MeshBuffers& buffers);

    void release();
    void create(const std::filesystem::path& shaderFolder);
    // The steps create makes the shaders, pipelines and samplers in (renderer_pipelines.cpp).
    // What the main view is drawn with. A pipeline only works on targets of its own sample count, so
    // smoothing edges needs a second set of them.
    struct PassPipelines {
        // By two-sided, then opaque, blended, added, summed, blended hair or opaque over a depth drawn
        // already. Opaque and alpha-tested surfaces write depth, blended ones don't, except a Sim's
        // blended hair. The last kind is only made with one sample (see kOpaqueOverDepth).
        SDL_GPUGraphicsPipeline* scene[2][6] = {};
        SDL_GPUGraphicsPipeline* skinnedScene[2][6] = {};  // the same for skinned primitives
        std::array<SDL_GPUGraphicsPipeline*, 8> particles{};  // by effects::ParticleBlock::Draw
        SDL_GPUGraphicsPipeline* sprite = nullptr;
        SDL_GPUGraphicsPipeline* terrain = nullptr;
        SDL_GPUGraphicsPipeline* road = nullptr;
        SDL_GPUGraphicsPipeline* sky = nullptr;
        SDL_GPUGraphicsPipeline* water = nullptr;  // the neighborhood's
        SDL_GPUGraphicsPipeline* clip = nullptr;   // a lot skirt's boundary curtain: four walls written into depth only
        SDL_GPUGraphicsPipeline* pond = nullptr;   // the water in the lot's ponds
        SDL_GPUGraphicsPipeline* pool = nullptr;   // the pools' water surface
        SDL_GPUGraphicsPipeline* sea = nullptr;    // a beach lot's sea
        // Its lots' imposters (see scene::PaintedTerrain::ImposterPart): the terrain's light map, the
        // terrain's texture, walls and slices, and roofs. A fading imposter's walls and roofs (see
        // setImposterAlpha) are blended by their alpha, without writing depth.
        SDL_GPUGraphicsPipeline* imposterLight = nullptr;
        SDL_GPUGraphicsPipeline* imposterTerrain = nullptr;
        SDL_GPUGraphicsPipeline* imposterWall = nullptr;
        SDL_GPUGraphicsPipeline* imposterRoof = nullptr;
        SDL_GPUGraphicsPipeline* imposterFadedWall = nullptr;
        SDL_GPUGraphicsPipeline* imposterFadedRoof = nullptr;
    };
    // Fills `out` for targets of `samples` samples. Throws when one can't be made.
    void makePassPipelines(PassPipelines& out, int samples);
    void releasePassPipelines(PassPipelines& pipelines);
    int mostSmoothEdgeSamples() const;
    // With `gameAlpha`, blending takes the colour's factors for alpha too, as Direct3D 9 blends
    // without a separate alpha blend, and no depth-clipped pipelines are made.
    void makeScenePipelines(const std::filesystem::path& shaders, PassPipelines& out, int samples, bool gameAlpha = false);
    void makeSpritePipeline(const std::filesystem::path& shaders, PassPipelines& out, int samples);
    void makeParticlePipelines(const std::filesystem::path& shaders, PassPipelines& out, int samples);
    void makeSamplers();
    void makeTerrainPipeline(const std::filesystem::path& shaders, PassPipelines& out, int samples);
    void makeRoadPipeline(const std::filesystem::path& shaders, PassPipelines& out, int samples);
    void makeImposterPipelines(const std::filesystem::path& shaders, PassPipelines& out, int samples);
    void makeClipPipeline(const std::filesystem::path& shaders, PassPipelines& out, int samples);
    void makeSkyPipeline(const std::filesystem::path& shaders, PassPipelines& out, int samples);
    void makeWaterPipeline(const std::filesystem::path& shaders, PassPipelines& out, int samples);
    void makePoolPipeline(const std::filesystem::path& shaders, PassPipelines& out, int samples, bool gameAlpha = false);
    void makeSeaPipeline(const std::filesystem::path& shaders, PassPipelines& out, int samples);
    void makePondPipeline(const std::filesystem::path& shaders, PassPipelines& out, int samples);
    void makeCausticsPipelines(const std::filesystem::path& shaders);
    void makeShadowPipelines(const std::filesystem::path& shaders);
    void destroy();
    void releaseSprites();
    void uploadSpriteVertices(SDL_GPUCommandBuffer* cmd, unsigned width, unsigned height);
    void uploadJoints(SDL_GPUCommandBuffer* cmd);
    // Records one frame. False when it couldn't (no depth buffer), with nothing recorded.
    [[nodiscard]] bool render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, unsigned width, unsigned height, const Camera& camera);
    // The steps render records a frame in, and what they share (renderer_draw.cpp).
    struct Frame;
    [[nodiscard]] bool reflectionReady(unsigned width, unsigned height);
    void drawReflection(const Frame& f);
    void drawSky(const Frame& f, SDL_GPURenderPass* on, SDL_GPUTexture* cube, const std::array<float, 12>& camera);
    void drawBoundary(const Frame& f);
    void drawTerrain(const Frame& f, SDL_GPURenderPass* on, const Mat4& viewProjection, bool reflected);
    // `ready` is each draw's uniforms when they were made already, or empty.
    // `overDepth` draws opaque surfaces whose depth is in the target already, without writing it.
    void drawList(const Frame& f, std::span<const Draw> list, int blend, std::span<const DrawUniforms> ready = {}, bool overDepth = false);
    // The blended surfaces and the particles together, by layer.
    void drawBlended(const Frame& f, const std::vector<Draw>& blended);
    void drawParticles(const Frame& f, const ParticleBatch& batch);
    void uploadParticleVertices(SDL_GPUCommandBuffer* cmd);
    // A mirror image's colour and depth textures, the size of the screen as the game's are.
    struct MirrorTarget {
        SDL_GPUTexture* color = nullptr;
        SDL_GPUTexture* depth = nullptr;
        unsigned width = 0, height = 0;
    };
    // Makes `target`'s textures at the size drawn at. False when they couldn't be made, to be tried
    // again next frame.
    [[nodiscard]] bool mirrorReady(MirrorTarget& target, unsigned width, unsigned height);
    // What a mirror image shows: the lot for a pool's, the beach sea's own list for the sea's, and
    // the neighborhood water's for the neighborhood's.
    enum class Mirrored { Pool, Sea, Neighborhood };
    // The scene mirrored in the plane at height `plane` (see poolMirror) into `target`, only within
    // `area` of its clip space.
    void clearMirror(const Frame& f, const MirrorTarget& target);
    void drawMirror(const Frame& f, float plane, float keepBelow, const MirrorTarget& target, Mirrored what, const std::array<float, 4>& area);
    void drawPoolSurface(const Frame& f, const Draw& d);
    void drawSkirtSea(const Frame& f, const Draw& d);
    void drawPondWater(const Frame& f, const Draw& d);
    // The pools' caustics tile and shadow map for the pool water draw `pool`, and the shadow
    // uniforms they are read with. False when they couldn't be drawn (no sun or no flipbook).
    // Without `passes` the caustics tile and the pool's shadow map are taken as they were drawn last.
    [[nodiscard]] bool drawPoolCaustics(const Frame& f, const Draw& pool, ShadowUniforms& shadows, bool passes = true);
    [[nodiscard]] std::size_t drawShadows(const Frame& f);
    void makeObjectShadowPipelines(const std::filesystem::path& shaders);
    // Makes the objects' pictures, list and grid again when the shadows' layout changed.
    void updateObjectShadows(const Frame& f);
    void drawObjectPictures(const Frame& f, const scene::objectshadow::Layout& layout);
    // False when the upload couldn't be staged, so the next frame tries again.
    bool uploadObjectPlaces(const Frame& f, const scene::objectshadow::Layout& layout);
    void drawRoads(const Frame& f);
    void drawWater(const Frame& f);
    void drawSprites(const Frame& f);
    // Ambient occlusion (renderer_occlusion.cpp).
    void makeOcclusionPipelines();
    void releaseOcclusion();
    [[nodiscard]] bool occlusionReady(unsigned width, unsigned height);
    void drawOcclusion(const Frame& f, std::span<const Draw> opaque, std::span<const DrawUniforms> uniforms);
    // Draws one frame into a new texture and reads it back as RGBA.
    [[nodiscard]] bool drawOffscreen(const Camera& camera, unsigned width, unsigned height, scene::Image& out);
    static constexpr unsigned kMaxOffscreen = 8192;
    void drawImposters(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, const Mat4& viewProjection);

    SDL_Window* window_ = nullptr;
public:
    void setWaitForScreen(bool on) override;
    [[nodiscard]] double screenRefreshRate() const override;
    void setKeepFrames(bool on) override;
    [[nodiscard]] bool showAgain() override;
private:
    SDL_GPUDevice* device_ = nullptr;
    // A depth buffer, remade only when the size drawn at changes: one for the window (and shots),
    // one for portraits, so drawing a portrait doesn't remake the window's.
    struct DepthBuffer {
        SDL_GPUTexture* texture = nullptr;
        unsigned width = 0, height = 0;
    };
    SDL_GPUTexture* depthFor(DepthBuffer& depth, unsigned width, unsigned height);
    DepthBuffer windowDepth_, portraitDepth_;
    // The main view's targets while edges are smoothed, resolved into the frame's own target as the
    // pass ends. Remade when the size drawn at changes.
    SDL_GPUTexture* smoothTarget(DepthBuffer& buffer, bool isDepth, unsigned width, unsigned height);
    DepthBuffer smoothColor_, smoothDepth_;
    // While frames are kept (see setKeepFrames), the window's frame is drawn here and copied to the
    // screen. `keptShown_` is whether it holds the last frame drawn.
    SDL_GPUTexture* keptTarget(unsigned width, unsigned height);
    void showKept(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* screen);
    DepthBuffer kept_;
    bool keepFrames_ = false, keptShown_ = false;
    int smoothSamples_ = 1;  // of the main view: 1, or 2, 4 or 8 when smoothing edges
    std::filesystem::path shaderFolder_;
    // Mirror images, shots of Sims and everything else keep one sample. The main view's pass uses
    // `smooth_` when it has more, and `drawing_` is the set of the pass being recorded.
    PassPipelines single_, smooth_;
    // Prepared scenes' pictures (see startPreparedImages) are drawn with only its scene and pool
    // pipelines, which blend alpha as the game's pictures were blended.
    PassPipelines picture_;
    bool picturing_ = false;  // while a prepared scene's pictures are drawn
    const PassPipelines* drawing_ = &single_;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUSampler* clampedSampler_ = nullptr;  // sampler_, clamped at the edges
    // The scene's pipelines clipping depth instead of clamping it, for the pools' reflection.
    SDL_GPUGraphicsPipeline* clippedPipelines_[2][6] = {};
    SDL_GPUGraphicsPipeline* clippedSkinnedPipelines_[2][6] = {};
    SDL_GPUBuffer* jointBuffer_ = nullptr;
    SDL_GPUBuffer* noMorphs_ = nullptr;  // bound in place of a mesh set's morph deltas when it has none  // jointData_ on the GPU
    std::size_t jointCapacity_ = 0;  // floats the buffer holds
    StreamUpload jointUpload_;

    SDL_GPUSampler* spriteSampler_ = nullptr;  // clamped, as sprites are drawn whole
    std::vector<SDL_GPUTexture*> spriteTextures_;
    std::vector<std::array<unsigned, 2>> spriteSizes_;
    SDL_GPUBuffer* spriteVertices_ = nullptr;
    unsigned spriteCapacity_ = 0;  // vertices the buffer holds
    StreamUpload spriteUpload_;

    SDL_GPUBuffer* particleBuffer_ = nullptr;
    std::uint32_t particleCapacity_ = 0;  // vertices the buffer holds
    StreamUpload particleUpload_;

    // The neighborhood water's reflection, drawn each frame.
    SDL_GPUTexture* reflection_ = nullptr;
    SDL_GPUTexture* reflectionDepth_ = nullptr;
    unsigned reflectionWidth_ = 0, reflectionHeight_ = 0;

    // The lot mirrored in the pools' water, each frame a pool is in view.
    MirrorTarget poolReflection_;
    bool poolReflected_ = false;  // poolReflection_ holds this frame's picture
    // While a prepared scene's picture is drawn: its pools (see PreparedShot::pools), none for none.
    const std::vector<std::uint8_t>* picturePools_ = nullptr;
    // The prepared scene whose pictures' caustics tile and pool shadow map were drawn last (see
    // GpuPrepared::serial), as they don't change from one picture of it to the next.
    std::uint64_t pictureScene_ = 0, pictureCausticsScene_ = 0;
    // A beach lot's sea mirror image, each frame the sea is in view.
    MirrorTarget seaReflection_;
    bool seaReflected_ = false;  // seaReflection_ holds this frame's picture
    bool seaFromWater_ = false;  // the sea shows the water sheet's reflection (a neighborhood's pool-style sea)
    // The pools' caustics: the tile drawn from the flipbook each frame, and the shadow map from the
    // sun (depth along the sun, with its depth buffer), made once.
    SDL_GPUGraphicsPipeline* causticsPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* poolShadowPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* poolShadowSkinnedPipeline_ = nullptr;
    SDL_GPUTexture* causticsTile_ = nullptr;
    SDL_GPUTexture* poolShadow_ = nullptr;
    SDL_GPUTexture* poolShadowDepth_ = nullptr;

    // The Sims' projected shadows: their pictures and the same blurred, an atlas each (see
    // kShadowTilesAcross), and how they are drawn and read.
    SDL_GPUGraphicsPipeline* shadowCasterPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* shadowBlurPipeline_ = nullptr;
    SDL_GPUTexture* shadowPictures_ = nullptr;
    SDL_GPUTexture* shadowBlurred_ = nullptr;
    SDL_GPUSampler* shadowSampler_ = nullptr;  // linear, clamped, no mip levels
    SDL_GPUSampler* shadowPoint_ = nullptr;    // nearest texel, clamped
    // The lot objects' shadows from the sun: each picture drawn into a scratch picture, then blurred
    // into its tile of an atlas (see kObjectTilesAcross), and the places' list and the grid saying
    // which places reach each tile. All made again only when the layout changes.
    SDL_GPUGraphicsPipeline* objectCasterPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* objectBlurPipeline_ = nullptr;
    SDL_GPUTexture* objectScratch_ = nullptr;
    SDL_GPUTexture* objectPictures_ = nullptr;
    SDL_GPUTexture* objectPlaces_ = nullptr;
    SDL_GPUTexture* objectGrid_ = nullptr;
    SDL_GPUTexture* objectFloors_ = nullptr;
    std::shared_ptr<const scene::objectshadow::Layout> objectLayout_;
    std::array<float, 4> objectGridArea_{};  // see ShadowUniforms::objectGrid
    std::array<float, 16> objectFloorsArea_{};  // see ShadowUniforms::objectFloorsToLot and objectFloors

    // Ambient occlusion: its pipelines while it is on (depth by skinned, then two-sided, for surfaces
    // with holes and then without), and its
    // targets at the size drawn at: the depth pass, how open each pixel is, and the same blurred.
    struct Occlusion {
        SDL_GPUGraphicsPipeline* depthPipelines[2][2] = {};
        SDL_GPUGraphicsPipeline* solidPipelines[2][2] = {};
        SDL_GPUGraphicsPipeline* occlusionPipeline = nullptr;
        SDL_GPUGraphicsPipeline* blurPipeline = nullptr;
        SDL_GPUGraphicsPipeline* upsamplePipeline = nullptr;
        SDL_GPUGraphicsPipeline* halfDepthPipeline = nullptr;
        SDL_GPUTexture* depth = nullptr;
        // At half size it is worked out from halfDepth and blurred into raw and soft, then brought up
        // to the view's size in blurred. At full size halfDepth and soft aren't made.
        SDL_GPUTexture* halfDepth = nullptr;
        SDL_GPUTexture* raw = nullptr;
        SDL_GPUTexture* soft = nullptr;
        SDL_GPUTexture* blurred = nullptr;
        unsigned width = 0, height = 0;
        bool half = false;
    };
    Occlusion occlusion_;
    float occlusionStrength_ = 0;
    bool occlusionHalf_ = true;
    SDL_GPUTexture* noOcclusion_ = nullptr;  // bound when there is none, as the scene shader asks for a texture
    SDL_GPUTexture* occlusionBound_ = nullptr;  // what the scene shader reads this frame

    SDL_GPUSampler* clampedMips_ = nullptr;  // clamped, with mip levels (road textures)

    // One of its lots' imposters' parts.
    struct ImposterDraw {
        scene::PaintedTerrain::ImposterPart::Kind kind;
        std::uint32_t lot = 0;  // the lot it shows
        SDL_GPUTexture* texture = nullptr;
        SDL_GPUTexture* lightMap = nullptr;
        float tint[3] = {1, 1, 1};
        std::uint32_t firstIndex = 0, indexCount = 0;
        std::int32_t vertexOffset = 0;
        std::array<float, 3> low{}, high{};  // the box around its vertices
    };
    SDL_GPUSampler* pointSampler_ = nullptr;  // nearest texel, no mip levels (imposter walls and slices)

    // What a scene is drawn with on the GPU. It owns its textures and buffers until release(). The
    // renderer and a prepared scene hold it by pointer, swapped in upload, never copied.
    struct SceneGpu {
        SceneGpu() = default;
        SceneGpu(const SceneGpu&) = delete;
        SceneGpu& operator=(const SceneGpu&) = delete;
        // Fills it from a scene. Textures that still need their smaller levels made are added to
        // `withMips` (made on the main thread, see upload).
        void upload(SDL_GPUDevice* device, const scene::Scene& scene, std::vector<SDL_GPUTexture*>& withMips);
        // The steps upload fills it in, in one copy pass (renderer_upload.cpp).
        struct Copy;
        void uploadImages(Copy& c, const scene::Scene& scene);
        void uploadMeshes(Copy& c, const scene::Scene& scene);
        void uploadTerrainGrid(Copy& c, const scene::PaintedTerrain& t);
        void uploadTerrainTextures(Copy& c, const scene::PaintedTerrain& t);
        void uploadWater(Copy& c, const scene::PaintedTerrain& t);
        static SDL_GPUTexture* uploadSkyCube(Copy& c, const std::vector<scene::Image>& sky);
        void uploadRoads(Copy& c, const scene::PaintedTerrain& t);
        void uploadImposters(Copy& c, const scene::PaintedTerrain& t);
        static SDL_GPUTexture* uploadImposterTexture(Copy& c, const scene::Image& image);
        void uploadBoundary(Copy& c, const scene::Scene& scene);
        void release(SDL_GPUDevice* device);
        // Releases up to `count` textures. True when none are left.
        bool releaseTextures(SDL_GPUDevice* device, std::size_t count);

        std::vector<SDL_GPUTexture*> textures;
        SDL_GPUTexture* white = nullptr;  // stands in for surfaces without a texture
        std::vector<GpuMesh> meshes;
        std::vector<MeshBuffers> meshBuffers;
        // A neighborhood's painted terrain (see scene::PaintedTerrain).
        SDL_GPUBuffer* terrainVertices = nullptr;
        SDL_GPUBuffer* terrainIndices = nullptr;
        unsigned terrainIndexCount = 0;
        SDL_GPUTexture* terrainPaints = nullptr;
        SDL_GPUTexture* terrainAlphas = nullptr;
        SDL_GPUTexture* terrainCells = nullptr;
        SDL_GPUTexture* terrainLight = nullptr;
        SDL_GPUTexture* terrainGradient = nullptr;
        TerrainUniforms terrainUniforms{};
        // The underwater gradient as the scene shader is given it (see ShadowUniforms::underwater).
        std::array<float, 4> underwater{};
        float seaLevel = 0;  // see PaintedTerrain::waterLevel
        float seaLight = 1;  // see PaintedTerrain::seaLight
        // PaintedTerrain::seaDepthMap, its points along x and y (0 for none) and its area.
        SDL_GPUBuffer* seaDepth = nullptr;
        std::array<float, 2> seaDepthSize{};
        std::array<float, 4> seaDepthArea{};
        std::array<std::array<float, 4>, kUnderwaterRows> underwaterRows{};
        // Its water: the sheet, the sky cube map and the ripple map.
        SDL_GPUBuffer* waterVertices = nullptr;
        SDL_GPUBuffer* waterIndices = nullptr;
        unsigned waterIndexCount = 0;
        SDL_GPUTexture* skyCube = nullptr;
        SDL_GPUTexture* ripples = nullptr;
        float waterLevel = 0;
        bool poolStyleSea = false;  // see PaintedTerrain::poolStyleSea
        WaterUniforms waterUniforms{};
        SDL_GPUTexture* skybox = nullptr;  // a lot's sky
        // Materials' environment cubes (scene::Scene::envCubes, null where broken), and a black one
        // bound for materials without.
        std::vector<SDL_GPUTexture*> envCubes;
        SDL_GPUTexture* blackCube = nullptr;
        // Light maps being faded from, by the image they are replacing (see setImages), and how far
        // the fade has gone (0 .. 1).
        std::map<int, SDL_GPUTexture*> fadingFrom;
        float lightMapFade = 1;
        // A lot skirt's boundary curtain.
        SDL_GPUBuffer* clipVertices = nullptr;
        SDL_GPUBuffer* clipIndices = nullptr;
        // Its roads.
        SDL_GPUBuffer* roadVertices = nullptr;
        SDL_GPUBuffer* roadIndices = nullptr;
        unsigned roadIndexCount = 0;
        SDL_GPUTexture* roadTextures = nullptr;
        float lightMapSize = 1280;
        // Its lots' imposters.
        SDL_GPUBuffer* imposterVertices = nullptr;
        SDL_GPUBuffer* imposterIndices = nullptr;
        std::vector<SDL_GPUTexture*> imposterTextures;
        std::vector<ImposterDraw> imposterDraws;
    };
    std::unique_ptr<SceneGpu> gpu_ = std::make_unique<SceneGpu>();
    SDL_GPUFence* uploadFence_ = nullptr;  // until the last upload is done

    // A scene uploaded but not drawn yet (see prepare).
    // Buffers pictures of prepared scenes are read back into, kept by the side of the square they
    // hold for the next pictures (see startPreparedImages). Pictures may be let go on any thread.
    struct TransferPool {
        std::mutex lock;
        SDL_GPUDevice* device = nullptr;  // none once the device is gone
        std::map<unsigned, std::vector<SDL_GPUTransferBuffer*>> free;
        SDL_GPUTransferBuffer* take(unsigned side);
        void giveBack(unsigned side, SDL_GPUTransferBuffer* buffer);
        // Lets go of the buffers kept, and with `forever` takes no more back (the device is going).
        void releaseAll(bool forever);
    };
    std::shared_ptr<TransferPool> transferPool_ = std::make_shared<TransferPool>();
    // The targets they are drawn into, a square of each power-of-two side, used by one picture after
    // another, and the depth buffer render() uses instead of its own while one is drawn.
    struct PictureTarget {
        SDL_GPUTexture* color = nullptr;
        DepthBuffer depth;
    };
    std::map<unsigned, PictureTarget> pictureTargets_;
    DepthBuffer* pictureDepth_ = nullptr;
    // Imposter textures uploaded ahead, let go unless taken.
    struct GpuImposterTextures final : ImposterTexturesBase {
        ~GpuImposterTextures() override;
        SDL_GPUDevice* device = nullptr;
        std::vector<SDL_GPUTexture*> textures;
    };
    // Pictures being drawn: a buffer to read each back into, done with the fence.
    struct GpuPictures final : PicturesBase {
        ~GpuPictures() override;
        SDL_GPUDevice* device = nullptr;
        SDL_GPUFence* fence = nullptr;
        std::shared_ptr<TransferPool> pool;
        bool swapRedBlue = false;
        struct Target {
            SDL_GPUTransferBuffer* transfer = nullptr;
            unsigned side = 0;  // of the square the buffer holds
            unsigned width = 0, height = 0;
        };
        std::vector<Target> targets;
    };
    struct GpuAddition final : AdditionBase {
        ~GpuAddition() override { release(); }
        void release();
        SDL_GPUDevice* device = nullptr;
        std::vector<SDL_GPUTexture*> textures, cubes;
        std::vector<SDL_GPUTexture*> withMips;  // their smaller levels are made in addPrepared
        MeshBuffers buffers;
        std::vector<GpuMesh> meshes;  // their primitives name the scene's materials from 0 and buffer set 0
        std::size_t materials = 0;    // how many materials the scene brought
    };
    struct GpuPrepared final : PreparedBase {
        ~GpuPrepared() override { release(); }
        void releaseSome(std::size_t textures) override;
        void release();
        SDL_GPUDevice* device = nullptr;
        std::unique_ptr<SceneGpu> gpu;
        std::vector<SDL_GPUTexture*> withMips;  // their smaller levels are made in upload
        std::uint64_t serial = 0;               // a number no other prepared scene has
    };
};

} // namespace ts2::render
