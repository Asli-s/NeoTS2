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
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct SDL_Window;
struct SDL_GPUDevice;
struct SDL_GPUBuffer;
struct SDL_GPUTexture;
struct SDL_GPUSampler;
struct SDL_GPUGraphicsPipeline;
struct SDL_GPUCommandBuffer;
struct SDL_GPURenderPass;
struct SDL_GPUFence;

namespace ts2::render {

// Draws a Scene with the GPU through SDL (Direct3D 12 or Vulkan): opaque surfaces first, then
// alpha-tested ones, then blended ones. Shaders are loaded from files in `shaderFolder`.
class SdlGpuRenderer final : public RendererState, private ResourceQuery {
public:
    SdlGpuRenderer(SDL_Window* window, const std::filesystem::path& shaderFolder);
    ~SdlGpuRenderer() override;

    using IRenderer::upload;
    [[nodiscard]] Capabilities capabilities() const override { return {}; }
    [[nodiscard]] Prepared prepare(const scene::Scene& scene) const override;
    void upload(Prepared prepared) override;
    [[nodiscard]] Prepared swap(Prepared prepared) override;
    [[nodiscard]] bool uploaded() override;
    void setSprites(const std::vector<std::pair<int, const scene::Image*>>& images) override;
    [[nodiscard]] bool draw(const Camera& camera) override;
    [[nodiscard]] bool drawToFile(const Camera& camera, unsigned width, unsigned height, const std::filesystem::path& path) override;
    [[nodiscard]] std::optional<scene::Image> drawInstancesToImage(const Camera& camera, unsigned width, unsigned height,
                                                                   const scene::ObjectLighting& lighting) override;
    std::string backend() const override;
    std::string device() const override;
    void benchmark(const Camera& camera, unsigned width, unsigned height, int frames) override;
    void addMaterials(const scene::Scene& scene) override;
    void addMeshes(const scene::Scene& scene) override;
    void setImages(const scene::Scene& scene, const std::vector<int>& indexes, bool fade = false) override;
    void setLightMapFade(float fade) override;
    void setSkybox(const std::vector<scene::Image>& faces) override;

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
    };

    void release();
    void create(const std::filesystem::path& shaderFolder);
    // The steps create makes the shaders, pipelines and samplers in (renderer_pipelines.cpp).
    void makeScenePipelines(const std::filesystem::path& shaders);
    void makeSpritePipeline(const std::filesystem::path& shaders);
    void makeSamplers();
    void makeTerrainPipeline(const std::filesystem::path& shaders);
    void makeRoadPipeline(const std::filesystem::path& shaders);
    void makeImposterPipelines(const std::filesystem::path& shaders);
    void makeClipPipeline(const std::filesystem::path& shaders);
    void makeSkyPipeline(const std::filesystem::path& shaders);
    void makeWaterPipeline(const std::filesystem::path& shaders);
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
    void drawSky(const Frame& f, SDL_GPURenderPass* on, SDL_GPUTexture* cube, float up);
    void drawBoundary(const Frame& f);
    void drawTerrain(const Frame& f, SDL_GPURenderPass* on, const Mat4& viewProjection, bool reflected);
    void drawList(const Frame& f, const std::vector<Draw>& list, int blend);
    void drawRoads(const Frame& f);
    void drawWater(const Frame& f);
    void drawSprites(const Frame& f);
    // Draws one frame into a new texture and reads it back as RGBA.
    [[nodiscard]] bool drawOffscreen(const Camera& camera, unsigned width, unsigned height, scene::Image& out);
    static constexpr unsigned kMaxOffscreen = 8192;
    void drawImposters(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, const Mat4& viewProjection);

    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    // A depth buffer, remade only when the size drawn at changes: one for the window (and shots),
    // one for portraits, so drawing a portrait doesn't remake the window's.
    struct DepthBuffer {
        SDL_GPUTexture* texture = nullptr;
        unsigned width = 0, height = 0;
    };
    SDL_GPUTexture* depthFor(DepthBuffer& depth, unsigned width, unsigned height);
    DepthBuffer windowDepth_, portraitDepth_;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUSampler* clampedSampler_ = nullptr;  // sampler_, clamped at the edges
    // Pipelines by [two-sided][blend]: opaque and alpha-tested surfaces write depth, blended ones don't.
    SDL_GPUGraphicsPipeline* pipelines_[2][3] = {};  // by two-sided, then opaque, blended or added
    SDL_GPUGraphicsPipeline* skinnedPipelines_[2][3] = {};  // the same for skinned primitives
    SDL_GPUBuffer* jointBuffer_ = nullptr;  // jointData_ on the GPU
    std::size_t jointCapacity_ = 0;  // floats the buffer holds

    SDL_GPUGraphicsPipeline* spritePipeline_ = nullptr;
    SDL_GPUSampler* spriteSampler_ = nullptr;  // clamped, as sprites are drawn whole
    std::vector<SDL_GPUTexture*> spriteTextures_;
    std::vector<std::array<unsigned, 2>> spriteSizes_;
    SDL_GPUBuffer* spriteVertices_ = nullptr;
    unsigned spriteCapacity_ = 0;  // vertices the buffer holds

    SDL_GPUGraphicsPipeline* terrainPipeline_ = nullptr;

    // Its water's pipelines and the reflection drawn each frame.
    SDL_GPUGraphicsPipeline* skyPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* waterPipeline_ = nullptr;
    // A lot skirt's boundary curtain: four walls written into depth only.
    SDL_GPUGraphicsPipeline* clipPipeline_ = nullptr;
    SDL_GPUTexture* reflection_ = nullptr;
    SDL_GPUTexture* reflectionDepth_ = nullptr;
    unsigned reflectionWidth_ = 0, reflectionHeight_ = 0;

    // Its roads.
    SDL_GPUGraphicsPipeline* roadPipeline_ = nullptr;
    SDL_GPUSampler* clampedMips_ = nullptr;  // clamped, with mip levels (road textures)

    // Its lots' imposters (see scene::PaintedTerrain::ImposterPart): pipelines for the terrain's
    // light map, the terrain's texture, walls and slices, and roofs.
    struct ImposterDraw {
        scene::PaintedTerrain::ImposterPart::Kind kind;
        SDL_GPUTexture* texture = nullptr;
        SDL_GPUTexture* lightMap = nullptr;
        float tint[3] = {1, 1, 1};
        std::uint32_t firstIndex = 0, indexCount = 0;
        std::int32_t vertexOffset = 0;
    };
    SDL_GPUGraphicsPipeline* imposterLightPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* imposterTerrainPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* imposterWallPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* imposterRoofPipeline_ = nullptr;
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
        void uploadRoads(Copy& c, const scene::PaintedTerrain& t);
        void uploadImposters(Copy& c, const scene::PaintedTerrain& t);
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
        // Its water: the sheet, the sky cube map and the ripple map.
        SDL_GPUBuffer* waterVertices = nullptr;
        SDL_GPUBuffer* waterIndices = nullptr;
        unsigned waterIndexCount = 0;
        SDL_GPUTexture* skyCube = nullptr;
        SDL_GPUTexture* ripples = nullptr;
        float waterLevel = 0;
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
    struct GpuPrepared final : PreparedBase {
        ~GpuPrepared() override { release(); }
        void releaseSome(std::size_t textures) override;
        void release();
        SDL_GPUDevice* device = nullptr;
        std::unique_ptr<SceneGpu> gpu;
        std::vector<SDL_GPUTexture*> withMips;  // their smaller levels are made in upload
    };
};

} // namespace ts2::render
