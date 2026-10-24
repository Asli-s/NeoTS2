#pragma once

#include "render/math.hpp"
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

// Where the camera is and what it looks at, in game coordinates (z up).
struct Camera {
    Vec3 eye{10, -10, 10};
    Vec3 target{0, 0, 0};
    float fovY = 0.7f;  // radians
    float fovX = 0;  // radians across the screen, used instead of fovY when set
    float farPlane = 500;  // nothing farther is drawn
    float nearPlane = 0;  // 0: a ten-thousandth of farPlane
};

// A textured rectangle drawn over the scene, in screen pixels from the top left. Texture -1 draws
// the plain color.
struct Sprite {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    int texture = -1;
    float color[4] = {1, 1, 1, 1};
};

// Draws a Scene with the GPU through SDL: opaque surfaces first, then alpha-tested ones, then
// blended ones. Shaders are loaded from files in `shaderFolder`.
class Renderer {
public:
    Renderer(SDL_Window* window, const std::filesystem::path& shaderFolder);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Uploads a scene's images and meshes in place of what was uploaded before.
    void upload(const scene::Scene& scene);
    // A scene uploaded but not drawn yet (see prepare).
    class Prepared;
    // Uploads a scene's images and meshes without drawing them. It can run on another thread (SDL's
    // GPU device makes textures and buffers on any thread), so a scene is uploaded as it is built.
    [[nodiscard]] Prepared prepare(const scene::Scene& scene) const;
    // Draws a prepared scene from now on, in place of what was uploaded before.
    void upload(Prepared prepared);
    // The same, handing back the scene drawn so far, to be drawn again later without uploading it anew.
    [[nodiscard]] Prepared swap(Prepared prepared);
    // Whether the GPU has done what the last upload gave it. A picture read back before then waits
    // for all of it (see drawInstancesToImage).
    [[nodiscard]] bool uploaded();

    // Which uploaded instances to draw, one flag each. All of them when empty (as after upload).
    void show(std::vector<std::uint8_t> visible);

    // Adds or replaces sprite images by index (a slot past the end grows the list).
    void setSprites(const std::vector<std::pair<int, const scene::Image*>>& images);
    // The color the frame starts from (RGB, 0 to 1).
    void setClearColor(float r, float g, float b) { clear_[0] = r; clear_[1] = g; clear_[2] = b; }
    // What to draw over the scene, in order, from the next frame on, and whether to draw the scene.
    void showSprites(std::vector<Sprite> sprites, bool scene = true);

    // Draws one frame. Returns false when the window can't be drawn to right now (minimized, or no
    // depth buffer).
    [[nodiscard]] bool draw(const Camera& camera);

    // Draws one frame off-screen and saves it as a PNG.
    [[nodiscard]] bool drawToFile(const Camera& camera, unsigned width, unsigned height, const std::filesystem::path& path);
    // Draws only the shown instances off-screen over a clear (transparent) background, without the
    // sky, ground, water or sprites, objects lit by `lighting`, and returns the picture (rows from
    // the top). Nullopt when it can't be drawn. For Sim portraits.
    [[nodiscard]] std::optional<scene::Image> drawInstancesToImage(const Camera& camera, unsigned width, unsigned height,
                                                     const scene::ObjectLighting& lighting);

    // The name of the GPU backend in use (e.g. "direct3d12").
    std::string backend() const;
    // The graphics card and its driver, as the driver names them (for the log).
    std::string device() const;
    // Draws `frames` frames off screen, each waited for, with timings on: their times go to the timing
    // report ("record frame" for building a frame, "gpu frame" for building and drawing it).
    void benchmark(const Camera& camera, unsigned width, unsigned height, int frames);

    // The joint matrices a skinned instance is drawn with from the next frame on (see
    // scene::Mesh::skin). Skinned instances without them aren't drawn.
    void setJoints(std::size_t instance, const std::vector<scene::Transform>& joints);
    // Moves an uploaded instance.
    void setPlace(std::size_t instance, const scene::Transform& transform);
    // Draws an uploaded instance with these materials, one per primitive of its scene mesh (indexes
    // into the scene's materials, scene::kHiddenMaterial for primitives not drawn). Empty for its mesh's own.
    void setMaterials(std::size_t instance, std::vector<int> materials);
    // Uploads the materials and images the drawn scene has gained since it was uploaded (both only
    // ever grow, in the scene's order).
    void addMaterials(const scene::Scene& scene);
    // The same for its meshes (after their materials).
    void addMeshes(const scene::Scene& scene);
    // Replaces an uploaded instance with the scene's, or adds it as the next one. Its materials are
    // its mesh's own again. Light sets the scene has gained are taken too.
    void setInstance(const scene::Scene& scene, std::size_t index);
    // Lights an uploaded instance with the light sets and sun the scene's has now.
    void setLights(const scene::Scene& scene, std::size_t index);
    // Uploads the scene's images at these indexes again, in place of the uploaded ones, without
    // smaller levels (as light maps are drawn). With `fade`, light maps are drawn fading from the
    // ones they replace (see setLightMapFade).
    void setImages(const scene::Scene& scene, const std::vector<int>& indexes, bool fade = false);
    // How far light maps have faded to the ones setImages put in (0 .. 1, at 1 the old ones are let go).
    void setLightMapFade(float fade);
    // Draws a lot's sky with these six faces from the next frame on (see scene::Scene::skybox). None
    // when they are not six faces of one size.
    void setSkybox(const std::vector<scene::Image>& faces);
    // The light lot imposters' walls, roofs and slices are drawn with from the next frame on: the
    // lighting state's environment colour in lot view, white in the neighborhood.
    void setImposterLight(const std::array<float, 3>& light) { imposterLight_ = light; }
    // Whether the lot is lit by a night state: unlit textured materials are darkened then.
    void setNight(bool night) { night_ = night; }
    // How objects are lit from the next frame on (see scene::Scene::objectLighting).
    void setObjectLighting(std::optional<scene::ObjectLighting> lighting) { objectLighting_ = std::move(lighting); }

    // The time animated surfaces (the neighborhood water) are drawn at, in seconds.
    void setTime(float seconds) { time_ = seconds; }

    // Whether materials with a normal map are drawn with it (the game's bumpMapping, on at its High
    // Material Detail). On by default.
    void setBumpMapping(bool on) { bumpMapping_ = on; }
    // Whether floors and walls are bump-mapped too (the game's floorAndWallNormalMapping, on at its
    // High Lighting Quality). It also needs bump mapping. On by default.
    void setFloorAndWallNormalMapping(bool on) { floorAndWallNormalMapping_ = on; }
    // Brightens a lot object (by its game object id, 0 for none) by adding `ambient` to its ambient light.
    void setHighlight(std::uint32_t object, float ambient) {
        highlightObject_ = object;
        highlightAmbient_ = ambient;
    }

private:
    // Where a primitive lies in its set of mesh buffers (see MeshBuffers). Meshes share a few
    // buffers, because binding a different buffer for every draw is slow.
    struct GpuPrimitive {
        std::uint32_t buffers = 0;  // index into SceneGpu::meshBuffers
        std::uint32_t firstVertex = 0;
        std::uint32_t firstIndex = 0;
        std::uint32_t firstSkin = 0;  // in meshSkin, for skinned primitives
        bool skinned = false;         // has joints and weights per vertex
        unsigned count = 0;
        int material = -1;
        bool lightMapped = false;  // has light-map coordinates
        bool tangents = false;     // has tangents (and texture coordinates), for normal maps
        std::size_t source = 0;    // its index among the scene mesh's primitives
        // The box around its vertices in model space. Skinned primitives move out of it, so they are
        // never culled.
        std::array<float, 3> low{}, high{};
        bool hasBox = false;  // the box is finite
        bool cullable = false;
    };
    struct GpuMesh {
        std::vector<GpuPrimitive> primitives;
        // The box around its cullable primitives' boxes, tested first so most primitives need no test.
        std::array<float, 3> low{}, high{};
        bool anyCullable = false;
    };
    // One vertex, one index and one skin buffer: the uploaded scene's meshes, then each batch of
    // meshes added later (see addMeshes).
    struct MeshBuffers {
        SDL_GPUBuffer* vertices = nullptr;
        SDL_GPUBuffer* indices = nullptr;
        SDL_GPUBuffer* skin = nullptr;
    };
    // Meshes laid out for one set of mesh buffers.
    struct MeshData {
        std::vector<float> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<std::uint8_t> skin;
        bool tooBig = false;  // some primitives were left out (a buffer's size is 32-bit)
    };
    static GpuMesh packMesh(const scene::Mesh& mesh, std::size_t materials, std::uint32_t buffers, MeshData& data);

    void release();
    // Takes the light sets the scene has gained (they only ever grow).
    void addLightSets(const scene::Scene& scene);
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
    struct Draw;
    struct DrawUniforms;
    [[nodiscard]] bool reflectionReady(unsigned width, unsigned height);
    void drawReflection(const Frame& f);
    void drawSky(const Frame& f, SDL_GPURenderPass* on, SDL_GPUTexture* cube, float up);
    void drawBoundary(const Frame& f);
    void drawTerrain(const Frame& f, SDL_GPURenderPass* on, const Mat4& viewProjection, bool reflected);
    void collectDraws(const Frame& f, std::vector<Draw>& opaque, std::vector<Draw>& layers, std::vector<Draw>& blended) const;
    void drawList(const Frame& f, const std::vector<Draw>& list, int blend);
    DrawUniforms uniformsFor(const Frame& f, const Draw& d, int blend) const;
    void setLights(DrawUniforms& u, const Draw& d, const scene::ObjectLighting& l, bool skirt, bool roof, bool bumped) const;
    void setMaterial(DrawUniforms& u, const Frame& f, const Draw& d, int blend, SDL_GPUTexture* normalMap,
                     const scene::ObjectLighting* lighting) const;
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
    // Skinned instances' joint matrices (3x4, by rows), all in one buffer: where each instance's
    // start (in matrices, -1 for none) and how many it has.
    std::vector<int> jointBase_, jointCount_;
    std::vector<float> jointData_;
    bool jointsChanged_ = false;
    SDL_GPUBuffer* jointBuffer_ = nullptr;
    std::size_t jointCapacity_ = 0;  // floats the buffer holds
    std::vector<scene::Material> materials_;
    std::vector<scene::Instance> instances_;
    std::vector<std::vector<int>> instanceMaterials_;  // see setMaterials
    std::optional<scene::ObjectLighting> objectLighting_;
    std::array<float, 3> imposterLight_{1, 1, 1};
    bool night_ = false;
    std::vector<scene::ObjectLighting> lightSets_;  // objects lit by their room
    std::vector<std::uint8_t> visible_;

    SDL_GPUGraphicsPipeline* spritePipeline_ = nullptr;
    SDL_GPUSampler* spriteSampler_ = nullptr;  // clamped, as sprites are drawn whole
    std::vector<SDL_GPUTexture*> spriteTextures_;
    std::vector<std::array<unsigned, 2>> spriteSizes_;
    std::vector<Sprite> sprites_;
    bool drawScene_ = true;
    bool instancesOnly_ = false;  // while drawing a portrait (see drawInstancesToImage)
    float clear_[3] = {0.17f, 0.18f, 0.19f};
    SDL_GPUBuffer* spriteVertices_ = nullptr;
    unsigned spriteCapacity_ = 0;  // vertices the buffer holds

    // A neighborhood's painted terrain (see scene::PaintedTerrain).
    struct TerrainUniforms {
        float alphaSize, textureRepeat, cellTiles, layers, cellsX, cellsY, lightMapScale, litFrom;
        float gradientDepth, reflected, padding[2];
    };
    struct WaterUniforms {
        float tint[4];
        float screen[2];
        float offset, scale, speed, time, padding[2];
    };
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
    float time_ = 0;
    bool bumpMapping_ = true;
    bool floorAndWallNormalMapping_ = true;
    std::uint32_t highlightObject_ = 0;
    float highlightAmbient_ = 0;

    // What a scene is drawn with on the GPU. It owns its textures and buffers until release(). The
    // renderer and a Prepared hold it by pointer, swapped in upload, never copied.
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

public:
    class Prepared {
    public:
        Prepared() = default;
        Prepared(Prepared&&) noexcept = default;
        Prepared& operator=(Prepared&& other) noexcept;
        ~Prepared();
        // Gives back up to `textures` of its textures, and the rest of it with the last ones. SDL
        // destroys what is given back in the next frame, which takes a while for hundreds at once.
        void releaseSome(std::size_t textures);

    private:
        friend class Renderer;
        void release();
        SDL_GPUDevice* device_ = nullptr;
        std::unique_ptr<SceneGpu> gpu_;
        std::vector<SDL_GPUTexture*> withMips_;  // their smaller levels are made in upload
        std::vector<scene::Material> materials_;
        std::vector<scene::Instance> instances_;
        std::optional<scene::ObjectLighting> objectLighting_;
        std::vector<scene::ObjectLighting> lightSets_;
    };
};

} // namespace ts2::render
